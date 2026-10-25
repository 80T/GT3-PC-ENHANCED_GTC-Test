/* crash.cpp - the crash report of GT3 PC (crash.h). */
#include "platform/crash.h"
#include "platform/log.h"
#include "platform/scheduler.h"

#include <windows.h>
#include <dbghelp.h>

#include <atomic>
#include <stdio.h>
#include <string.h>

namespace crash {
namespace {

std::atomic<bool> g_inCrash{false};
bool g_symbols = false;
uintptr_t g_imageLo, g_imageHi;     /* gt3pc.exe in memory */

/* "function+0xNN (file:line)" for a code address of the program. */
void describe(uintptr_t addr, char *out, size_t outSize)
{
    if (!g_symbols) {
        snprintf(out, outSize, "gt3pc+0x%X", (unsigned)(addr - g_imageLo));
        return;
    }
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 disp = 0;
    if (!SymFromAddr(GetCurrentProcess(), addr, &disp, sym)) {
        snprintf(out, outSize, "gt3pc+0x%X", (unsigned)(addr - g_imageLo));
        return;
    }
    IMAGEHLP_LINE64 line = {};
    line.SizeOfStruct = sizeof(line);
    DWORD lineDisp = 0;
    if (SymGetLineFromAddr64(GetCurrentProcess(), addr, &lineDisp, &line)) {
        const char *file = strrchr(line.FileName, '\\');
        snprintf(out, outSize, "%s+0x%X (%s:%u)", sym->Name, (unsigned)disp,
                 file ? file + 1 : line.FileName, (unsigned)line.LineNumber);
    } else {
        snprintf(out, outSize, "%s+0x%X", sym->Name, (unsigned)disp);
    }
}

bool in_image(uintptr_t a)
{
    return a >= g_imageLo && a < g_imageHi;
}

/* A word on the stack that points just after a call instruction of the
 * program: a return address (the frame-pointer chain is not kept by the
 * optimised build, so the stack is scanned). */
bool is_return_address(uintptr_t a)
{
    if (!in_image(a) || a < g_imageLo + 8)
        return false;
    const uint8_t *p = (const uint8_t *)a;
    if (IsBadReadPtr(p - 7, 7))
        return false;
    if (p[-5] == 0xE8)                                  /* call rel32 */
        return true;
    if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10)        /* call reg / [reg] */
        return true;
    if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10)        /* call [reg+d8] */
        return true;
    if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10)        /* call [reg+d32] / [abs] */
        return true;
    if (p[-7] == 0xFF && (p[-6] & 0x38) == 0x10)        /* call [sib+d32] */
        return true;
    return false;
}

void write_dump(EXCEPTION_POINTERS *ep)
{
    char path[MAX_PATH * 2];
    GetModuleFileNameA(nullptr, path, sizeof(path));
    char *slash = strrchr(path, '\\');
    if (!slash)
        return;
    strcpy(slash + 1, "gt3pc_crash.dmp");
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return;
    MINIDUMP_EXCEPTION_INFORMATION mei = {};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;
    MINIDUMP_TYPE type = (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs |
                                         MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
    BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, type, &mei, nullptr, nullptr);
    CloseHandle(f);
    pclog::write(ok ? "crash: minidump written to %s" : "crash: could not write %s", path);
}

LONG WINAPI on_crash(EXCEPTION_POINTERS *ep)
{
    if (g_inCrash.exchange(true))
        return EXCEPTION_CONTINUE_SEARCH;
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    const CONTEXT *c = ep->ContextRecord;
    char where[512];

    pclog::write("crash: exception 0x%08X at 0x%08X, game thread %d (Windows thread %u)",
                 (unsigned)er->ExceptionCode, (unsigned)(uintptr_t)er->ExceptionAddress,
                 sched::current_thread(), (unsigned)GetCurrentThreadId());
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        pclog::write("crash: %s address 0x%08X",
                     er->ExceptionInformation[0] == 0 ? "reading" :
                     er->ExceptionInformation[0] == 8 ? "executing" : "writing",
                     (unsigned)er->ExceptionInformation[1]);
    pclog::write("crash: eax %08X ebx %08X ecx %08X edx %08X esi %08X edi %08X ebp %08X esp %08X eip %08X",
                 (unsigned)c->Eax, (unsigned)c->Ebx, (unsigned)c->Ecx, (unsigned)c->Edx,
                 (unsigned)c->Esi, (unsigned)c->Edi, (unsigned)c->Ebp, (unsigned)c->Esp, (unsigned)c->Eip);
    if (in_image(c->Eip)) {
        describe(c->Eip, where, sizeof(where));
        pclog::write("crash: in %s", where);
    }

    /* The return addresses on the stack, nearest first. */
    const uintptr_t *sp = (const uintptr_t *)c->Esp;
    int found = 0;
    for (int i = 0; i < 4096 && found < 24; i++) {
        if (IsBadReadPtr(sp + i, sizeof(uintptr_t)))
            break;
        uintptr_t a = sp[i];
        if (!is_return_address(a))
            continue;
        describe(a, where, sizeof(where));
        pclog::write("crash:   [esp+0x%03X] %s", (unsigned)(i * 4), where);
        found++;
    }
    write_dump(ep);
    pclog::close();
    return EXCEPTION_CONTINUE_SEARCH;       /* Windows Error Reporting as before */
}

} // namespace

void log_stack(void *thread, const char *prefix)
{
    /* The stack copied while the thread is held (no lock taken meanwhile:
     * it may hold the log's or the heap's), then described. */
    enum { kMaxWords = 64 * 1024, kMaxFound = 40 };
    static uintptr_t words[kMaxWords];      /* (one report at a time: the window thread's) */
    HANDLE h = (HANDLE)thread;
    CONTEXT c = {};
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    size_t count = 0;
    if (SuspendThread(h) == (DWORD)-1) {
        pclog::write("%scould not hold the thread (error %u)", prefix, (unsigned)GetLastError());
        return;
    }
    const bool haveContext = GetThreadContext(h, &c) != FALSE;
    if (haveContext) {
        /* the committed stack from esp up (one region below the stack base) */
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((const void *)(uintptr_t)c.Esp, &mbi, sizeof(mbi)) == sizeof(mbi) &&
            mbi.State == MEM_COMMIT) {
            const uintptr_t end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
            count = (end - (uintptr_t)c.Esp) / sizeof(uintptr_t);
            if (count > kMaxWords)
                count = kMaxWords;
            memcpy(words, (const void *)(uintptr_t)c.Esp, count * sizeof(uintptr_t));
        }
    }
    ResumeThread(h);
    if (!haveContext) {
        pclog::write("%sno context (error %u)", prefix, (unsigned)GetLastError());
        return;
    }

    char where[512];
    if (in_image(c.Eip)) {
        describe(c.Eip, where, sizeof(where));
        pclog::write("%sat %s", prefix, where);
    }
    int found = 0;
    for (size_t i = 0; i < count && found < kMaxFound; i++) {
        if (!is_return_address(words[i]))
            continue;
        describe(words[i], where, sizeof(where));
        pclog::write("%s[esp+0x%05X] %s", prefix, (unsigned)(i * 4), where);
        found++;
    }
}

void describe_address(const void *addr, char *out, unsigned outSize)
{
    describe((uintptr_t)addr, out, outSize);
}

void install()
{
    HMODULE self = GetModuleHandleW(nullptr);
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)self;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const uint8_t *)self + dos->e_lfanew);
    g_imageLo = (uintptr_t)self;
    g_imageHi = g_imageLo + nt->OptionalHeader.SizeOfImage;

    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS);
    g_symbols = SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
    SetUnhandledExceptionFilter(on_crash);
}

} // namespace crash
