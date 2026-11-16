/* sample_prof.cpp - the sampling profiler (sample_prof.h). */
#include "platform/sample_prof.h"
#include "platform/log.h"

#include <windows.h>
#include <dbghelp.h>
#include <mmsystem.h>                       /* timeBeginPeriod */

#include <algorithm>
#include <atomic>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace sample_prof {
namespace {

constexpr int kMaxThreads = 8;
constexpr int kTopFunctions = 30;

constexpr int kChain = 3;                   /* (the callers kept of a sample outside the program) */
constexpr size_t kStackCopy = 4096;

/* PC (2026-11-03): a sample outside the program (a system DLL: the heap, a
 * lock, a wait) by the program's functions that called it - the first
 * kChain return addresses into the program's code found on the stack */
struct Chain {
    uint32_t ip;                    /* where in the DLL */
    uint32_t ra[kChain];
    uint32_t samples;
};

struct Sampled {
    char name[32];
    HANDLE handle;
    uintptr_t stackHi;              /* (the thread's stack: its top) */
    std::unordered_map<uint32_t, uint32_t> hits;    /* instruction pointer -> samples (the sampler's) */
    std::unordered_map<uint64_t, Chain> chains;     /* outside the program: by DLL function and callers */
    /* PC (2026-11-05): a sample in the program by its instruction pointer
     * and the first return address into the program's code found on the
     * stack (its caller, mostly): the hottest functions' callers */
    std::unordered_map<uint64_t, uint32_t> callers;
    uint64_t total;
};

std::mutex g_lock;                  /* g_threads' registration */
Sampled g_threads[kMaxThreads];
std::atomic<int> g_count{0};
int g_every = -1;                   /* seconds between reports; 0 off */

/* A function's name by an instruction pointer in it (gt3pc.exe's: its PDB),
 * else the module's; the source line. */
std::string module_of(uint32_t ip)
{
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>((uintptr_t)ip), &m) ||
        m == nullptr)
        return "?";
    char path[MAX_PATH];
    if (GetModuleFileNameA(m, path, MAX_PATH) == 0)
        return "?";
    const char *s = strrchr(path, '\\');
    return s != nullptr ? s + 1 : path;
}

std::string function_of(uint32_t ip, bool own)
{
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = reinterpret_cast<SYMBOL_INFO *>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 disp = 0;
    if (!own) {
        /* PC: a system DLL's export nearest below (its symbols loaded once:
         * those loaded after SymInitialize were not) */
        static std::unordered_map<uintptr_t, bool> loaded;
        HMODULE m = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>((uintptr_t)ip), &m) &&
            m != nullptr && !loaded[(uintptr_t)m]) {
            loaded[(uintptr_t)m] = true;
            char path[MAX_PATH];
            if (GetModuleFileNameA(m, path, MAX_PATH) != 0)
                SymLoadModuleEx(GetCurrentProcess(), nullptr, path, nullptr, (DWORD64)(uintptr_t)m, 0, nullptr, 0);
        }
        if (SymFromAddr(GetCurrentProcess(), ip, &disp, sym))
            return "[" + module_of(ip) + "!" + sym->Name + "]";
        return "[" + module_of(ip) + "]";
    }
    if (!SymFromAddr(GetCurrentProcess(), ip, &disp, sym))
        return "?";
    return sym->Name;
}

/* PC: whether ra is a return address in the program's code: the bytes before
 * it a call (E8 rel32; FF /2 through a register, [reg], [reg + disp8],
 * [reg + disp32] or [abs32]) */
bool after_call(uint32_t ra, uintptr_t codeLo, uintptr_t codeHi)
{
    if (ra < codeLo + 8 || ra >= codeHi)
        return false;
    const uint8_t *p = reinterpret_cast<const uint8_t *>((uintptr_t)ra);
    if (p[-5] == 0xE8)
        return true;
    if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10 && (p[-1] & 0xC0) != 0x40 && (p[-1] & 0xC0) != 0x80)
        return true;                        /* call reg / [reg] */
    if (p[-3] == 0xFF && (p[-2] & 0xF8) == 0x50)
        return true;                        /* call [reg + disp8] */
    if (p[-6] == 0xFF && ((p[-5] & 0xF8) == 0x90 || p[-5] == 0x15))
        return true;                        /* call [reg + disp32], [abs32] */
    return false;
}

std::string line_of(uint32_t ip)
{
    IMAGEHLP_LINE64 line = {};
    line.SizeOfStruct = sizeof(line);
    DWORD disp = 0;
    if (!SymGetLineFromAddr64(GetCurrentProcess(), ip, &disp, &line) || line.FileName == nullptr)
        return "";
    const char *f = strrchr(line.FileName, '\\');
    char out[160];
    snprintf(out, sizeof(out), "%s:%lu", f != nullptr ? f + 1 : line.FileName, (unsigned long)line.LineNumber);
    return out;
}

/* Thread t's samples since the last report into the log (then cleared). */
void report(Sampled &t, uintptr_t imageLo, uintptr_t imageHi)
{
    if (t.total == 0)
        return;
    struct Fn {
        std::string name;
        uint64_t samples = 0;
        std::unordered_map<std::string, uint64_t> lines;
    };
    std::unordered_map<std::string, Fn> fns;
    for (const auto &h : t.hits) {
        const bool own = h.first >= imageLo && h.first < imageHi;
        std::string name = function_of(h.first, own);
        Fn &f = fns[name];
        f.name = name;
        f.samples += h.second;
        if (own)
            f.lines[line_of(h.first)] += h.second;
    }
    /* PC (2026-11-05): each function's callers (by the return address found
     * on the stack: the function and its line) */
    std::unordered_map<std::string, std::unordered_map<std::string, uint64_t>> callers;
    for (const auto &c : t.callers) {
        const uint32_t ip = (uint32_t)(c.first >> 32), ra = (uint32_t)c.first;
        if (ra == 0)
            continue;
        callers[function_of(ip, true)][function_of(ra, true) + " (" + line_of(ra) + ")"] += c.second;
    }
    std::vector<const Fn *> order;
    for (const auto &f : fns)
        order.push_back(&f.second);
    std::sort(order.begin(), order.end(), [](const Fn *a, const Fn *b) { return a->samples > b->samples; });
    pclog::write("sample prof: %s: %llu samples; the hottest functions (own time, %% of the thread's samples):", t.name,
                 (unsigned long long)t.total);
    for (size_t i = 0; i < order.size() && i < (size_t)kTopFunctions; i++) {
        const Fn &f = *order[i];
        /* (its three hottest lines) */
        std::vector<std::pair<uint64_t, std::string>> lines;
        for (const auto &l : f.lines)
            if (!l.first.empty())
                lines.push_back({l.second, l.first});
        std::sort(lines.begin(), lines.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        std::string hottest;
        for (size_t k = 0; k < lines.size() && k < 3; k++) {
            char part[128];
            snprintf(part, sizeof(part), "%s%s %.1f%%", k ? ", " : "", lines[k].second.c_str(),
                     100.0 * lines[k].first / t.total);
            hottest += part;
        }
        if (!hottest.empty())
            pclog::write("sample prof:   %5.1f%%  %s  (hottest lines %s)", 100.0 * f.samples / t.total,
                         f.name.c_str(), hottest.c_str());
        else
            pclog::write("sample prof:   %5.1f%%  %s", 100.0 * f.samples / t.total, f.name.c_str());
        /* PC (2026-11-05): its three most frequent callers */
        auto ci = callers.find(f.name);
        if (ci != callers.end() && !ci->second.empty()) {
            std::vector<std::pair<uint64_t, std::string>> cs;
            for (const auto &c : ci->second)
                cs.push_back({c.second, c.first});
            std::sort(cs.begin(), cs.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
            std::string from;
            for (size_t k = 0; k < cs.size() && k < 3; k++) {
                char part[256];
                snprintf(part, sizeof(part), "%s%s %.1f%%", k ? ", " : "", cs[k].second.c_str(),
                         100.0 * cs[k].first / t.total);
                from += part;
            }
            pclog::write("sample prof:           from %s", from.c_str());
        }
    }
    /* PC: the samples outside the program by the DLL function and the
     * program's callers (the 12 most) */
    struct Out {
        std::string text;
        uint64_t samples = 0;
    };
    std::unordered_map<std::string, Out> outs;
    for (const auto &c : t.chains) {
        std::string s = function_of(c.second.ip, false);
        for (int k = 0; k < kChain && c.second.ra[k] != 0; k++)
            s += (k ? " < " : " from ") + function_of(c.second.ra[k], true) + " (" + line_of(c.second.ra[k]) + ")";
        Out &o = outs[s];
        o.text = s;
        o.samples += c.second.samples;
    }
    std::vector<const Out *> list;
    for (const auto &o : outs)
        list.push_back(&o.second);
    std::sort(list.begin(), list.end(), [](const Out *a, const Out *b) { return a->samples > b->samples; });
    for (size_t i = 0; i < list.size() && i < 12; i++)
        pclog::write("sample prof:   outside %5.1f%%  %s", 100.0 * list[i]->samples / t.total, list[i]->text.c_str());
    t.hits.clear();
    t.chains.clear();
    t.callers.clear();
    t.total = 0;
}

void sampler_main()
{
    timeBeginPeriod(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    const HMODULE self = GetModuleHandleW(nullptr);
    const IMAGE_DOS_HEADER *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(self);
    const IMAGE_NT_HEADERS *nt =
        reinterpret_cast<const IMAGE_NT_HEADERS *>(reinterpret_cast<const uint8_t *>(self) + dos->e_lfanew);
    const uintptr_t imageLo = (uintptr_t)self, imageHi = imageLo + nt->OptionalHeader.SizeOfImage;
    ULONGLONG nextReport = GetTickCount64() + (ULONGLONG)g_every * 1000;
    static uint32_t stack[kStackCopy / 4];
    for (;;) {
        Sleep(1);
        const int n = g_count.load();
        for (int i = 0; i < n; i++) {
            Sampled &t = g_threads[i];
            /* (nothing between the suspend and the resume but the context
             * read and a copy of the stack's top: the thread may hold any
             * lock, the heap's too) */
            if (SuspendThread(t.handle) == (DWORD)-1)
                continue;
            CONTEXT c = {};
            c.ContextFlags = CONTEXT_CONTROL;
            const BOOL ok = GetThreadContext(t.handle, &c);
            size_t words = 0;
            const bool outside = ok && !(c.Eip >= imageLo && c.Eip < imageHi);
            if (ok && c.Esp != 0 && (uintptr_t)c.Esp < t.stackHi) {
                /* (PC, 2026-11-05: the stack's top for every sample - a
                 * sample in the program keeps its caller too) */
                const size_t bytes = std::min<size_t>(outside ? kStackCopy : 512, t.stackHi - (uintptr_t)c.Esp) &
                                     ~(size_t)3;
                memcpy(stack, reinterpret_cast<const void *>((uintptr_t)c.Esp), bytes);
                words = bytes / 4;
            }
            ResumeThread(t.handle);
            if (!ok)
                continue;
            t.hits[(uint32_t)c.Eip]++;
            t.total++;
            if (!outside) {
                uint32_t ra = 0;
                for (size_t w = 0; w < words && ra == 0; w++)
                    if (after_call(stack[w], imageLo, imageHi))
                        ra = stack[w];
                t.callers[(uint64_t)(uint32_t)c.Eip << 32 | ra]++;
            }
            if (outside) {
                Chain ch = {};
                ch.ip = (uint32_t)c.Eip;
                int k = 0;
                for (size_t w = 0; w < words && k < kChain; w++)
                    if (after_call(stack[w], imageLo, imageHi))
                        ch.ra[k++] = stack[w];
                uint64_t key = ch.ip;
                for (int j = 0; j < kChain; j++)
                    key = (key ^ ch.ra[j]) * 0x100000001B3ull;
                Chain &e = t.chains[key];
                if (e.samples == 0)
                    e = ch;
                e.samples++;
            }
        }
        if (GetTickCount64() >= nextReport) {
            for (int i = 0; i < n; i++)
                report(g_threads[i], imageLo, imageHi);
            nextReport = GetTickCount64() + (ULONGLONG)g_every * 1000;
        }
    }
}

} // namespace

void register_thread(const char *name)
{
    std::lock_guard<std::mutex> hold(g_lock);
    if (g_every < 0) {
        const char *e = getenv("GT3PC_SAMPLE");
        g_every = (e != nullptr && atoi(e) > 0) ? atoi(e) : 0;
        if (g_every > 0)
            pclog::write("sample prof: GT3PC_SAMPLE=%d: the renderer's threads sampled, reported every %d s", g_every,
                         g_every);
    }
    const int n = g_count.load();
    if (g_every == 0 || n >= kMaxThreads)
        return;
    Sampled &t = g_threads[n];
    snprintf(t.name, sizeof(t.name), "%s", name);
    ULONG_PTR lo = 0, hi = 0;
    GetCurrentThreadStackLimits(&lo, &hi);
    t.stackHi = (uintptr_t)hi;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &t.handle,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
        return;
    t.total = 0;
    g_count = n + 1;                        /* (the sampler reads it after the slot is filled) */
    static bool started = false;
    if (!started) {
        started = true;
        std::thread(sampler_main).detach();
    }
}

} // namespace sample_prof
