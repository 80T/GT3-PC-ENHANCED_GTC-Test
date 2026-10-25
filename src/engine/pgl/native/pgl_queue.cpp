/* pgl_queue.cpp - the renderer's D3D thread (pgl_queue.h).
 *
 * One producer at a time (the game thread executing a frame; game threads
 * run one at a time, platform/scheduler.h) and the D3D thread: positions in
 * the ring only grow (an index is the position modulo its size).  The
 * executor publishes its records in batches (g_head), the D3D thread runs
 * them and publishes how far it got after each (g_tail; the executor reads
 * it only when the ring seems full or at a sync).  Idle, the D3D thread
 * polls a little, then sleeps on g_head (WaitOnAddress); a sync or a full
 * ring waits likewise on g_tail. */
#include "engine/pgl/native/pgl_queue.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>

#include <intrin.h>
#include <windows.h>
#include <unknwn.h>

#include "platform/crash.h"
#include "platform/gpu.h"
#include "platform/log.h"
#include "platform/settings.h"

#pragma comment(lib, "Synchronization.lib")

namespace pglr {
namespace q {

thread_local bool t_d3dThread = false;
Producer g_prod;

namespace {

constexpr int kSpin = 4000;                 /* polls before sleeping */

alignas(64) std::atomic<uint64_t> g_head{0};    /* published by the executor */
alignas(64) std::atomic<uint64_t> g_tail{0};    /* run by the D3D thread */
alignas(64) std::atomic<int> g_sleeping{0};     /* the D3D thread sleeps on g_head */
std::atomic<int> g_waiting{0};                  /* the executor sleeps on g_tail */
std::atomic<bool> g_stop{false};
std::atomic<int64_t> g_busy{0};                 /* QPC ticks the D3D thread ran calls */
HANDLE g_thread;
uint32_t g_syncs;
int64_t g_syncTicks;                            /* QPC ticks the executor waited in syncs */
double g_qpcFreq;
int g_syncLog = -1;

int64_t qpc()
{
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

/* The executor waits until the D3D thread has run up to `want'. */
void wait_tail(uint64_t want)
{
    publish();
    int spins = 0;
    for (;;) {
        uint64_t t = g_tail.load(std::memory_order_acquire);
        if (t >= want) {
            g_prod.tailSeen = t;
            return;
        }
        if (spins < kSpin) {
            _mm_pause();
            spins++;
            continue;
        }
        g_waiting.store(1, std::memory_order_seq_cst);
        t = g_tail.load(std::memory_order_seq_cst);
        if (t < want)
            WaitOnAddress((volatile void *)&g_tail, &t, sizeof(t), 50);
        g_waiting.store(0, std::memory_order_seq_cst);
    }
}

/* GT3PC_SYNC_LOG=1 (or the launcher's Debug tab, [Debug] SyncLog): who made
 * the executor wait for the D3D thread, each call chain once (at most 64). */
void log_sync_caller()
{
    if (g_syncLog < 0) {
        const char *e = getenv("GT3PC_SYNC_LOG");
        g_syncLog = (e != nullptr && atoi(e) > 0) || settings::debug_sync_log() ? 1 : 0;
        if (g_syncLog)
            pclog::write("pgl: logging the waits for the D3D thread (pgl sync: ..., each caller once)");
    }
    if (g_syncLog == 0)
        return;
    static std::unordered_set<uint64_t> seen;
    void *frames[8] = {};
    const USHORT n = CaptureStackBackTrace(2, 8, frames, nullptr);
    uint64_t h = 0xCBF29CE484222325ull;
    for (USHORT i = 0; i < n; i++)
        h = (h ^ (uint64_t)(uintptr_t)frames[i]) * 0x100000001B3ull;
    if (seen.size() >= 64 || !seen.insert(h).second)
        return;
    char line[1024];
    size_t used = (size_t)snprintf(line, sizeof(line), "pgl sync:");
    for (USHORT i = 0; i < n && i < 6 && used < sizeof(line) - 8; i++) {
        char name[160];
        crash::describe_address(frames[i], name, sizeof(name));
        used += (size_t)snprintf(line + used, sizeof(line) - used, "%s %s", i ? " <" : "", name);
    }
    pclog::write("%s", line);
}

DWORD WINAPI d3d_thread(void *)
{
    t_d3dThread = true;
    uint64_t tail = 0;
    int64_t busyFrom = 0;
    for (;;) {
        uint64_t head = g_head.load(std::memory_order_acquire);
        if (head == tail) {
            if (busyFrom != 0) {
                g_busy.fetch_add(qpc() - busyFrom, std::memory_order_relaxed);
                busyFrom = 0;
            }
            if (g_stop.load(std::memory_order_acquire))
                break;
            for (int i = 0; i < kSpin && (head = g_head.load(std::memory_order_acquire)) == tail; i++)
                _mm_pause();
            if (head == tail) {
                g_sleeping.store(1, std::memory_order_seq_cst);
                head = g_head.load(std::memory_order_seq_cst);
                if (head == tail && !g_stop.load(std::memory_order_seq_cst))
                    WaitOnAddress((volatile void *)&g_head, &tail, sizeof(tail), 100);
                g_sleeping.store(0, std::memory_order_seq_cst);
                continue;
            }
        }
        if (busyFrom == 0)
            busyFrom = qpc();
        while (tail != head) {
            Rec *r = reinterpret_cast<Rec *>(g_prod.buf + (tail & (kSize - 1)));
            const uint32_t size = r->size;
            if (r->fn != nullptr)
                r->fn(r);
            tail += size;
            g_tail.store(tail, std::memory_order_release);
        }
        if (g_waiting.load(std::memory_order_seq_cst) != 0)
            WakeByAddressAll((void *)&g_tail);
    }
    return 0;
}

}  /* namespace */

void start()
{
    if (g_prod.running)
        return;
    const char *e = getenv("GT3PC_D3D_THREAD");
    if (e != nullptr && atoi(e) == 0) {
        pclog::write("pgl: the D3D thread off (GT3PC_D3D_THREAD=0): the executor makes its Direct3D calls itself");
        return;
    }
    g_prod.buf = static_cast<uint8_t *>(VirtualAlloc(nullptr, (SIZE_T)kSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (g_prod.buf == nullptr) {
        pclog::write("pgl: no memory for the D3D thread's queue: the executor makes its Direct3D calls itself");
        return;
    }
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpcFreq = (double)f.QuadPart;
    g_thread = CreateThread(nullptr, 0, d3d_thread, nullptr, 0, nullptr);
    if (g_thread == nullptr) {
        pclog::write("pgl: the D3D thread could not start: the executor makes its Direct3D calls itself");
        return;
    }
    SetThreadPriority(g_thread, GetThreadPriority(GetCurrentThread()));
    SetThreadDescription(g_thread, L"pgl D3D");
    g_prod.running = true;
    gpu::set_context_guard(&sync);
    atexit(&stop);
    pclog::write("pgl: the D3D thread runs the Direct3D calls (a %u MB queue)", (unsigned)(kSize >> 20));
}

void stop()
{
    if (!g_prod.running || t_d3dThread)
        return;
    sync();
    g_stop.store(true, std::memory_order_seq_cst);
    WakeByAddressSingle((void *)&g_head);
    WaitForSingleObject(g_thread, 2000);
    g_prod.running = false;
}

void publish()
{
    g_prod.pending = 0;
    g_head.store(g_prod.head, std::memory_order_seq_cst);
    if (g_sleeping.load(std::memory_order_seq_cst) != 0)
        WakeByAddressSingle((void *)&g_head);
}

void *reserve_slow(uint64_t need)
{
    if (need > kSize / 4)
        pclog::fatal("pgl: a queued call of %llu bytes (the D3D thread's queue holds %llu)",
                     (unsigned long long)need, (unsigned long long)kSize);
    uint64_t idx = g_prod.head & (kSize - 1);
    const uint64_t pad = idx + need > kSize ? kSize - idx : 0;
    if (g_prod.head + pad + need - g_prod.tailSeen > kSize) {
        /* full as last seen: how far the D3D thread is now, or wait */
        g_prod.tailSeen = g_tail.load(std::memory_order_acquire);
        if (g_prod.head + pad + need - g_prod.tailSeen > kSize)
            wait_tail(g_prod.head + pad + need - kSize);
    }
    if (pad != 0) {
        /* the ring wraps: the rest of it skipped */
        Rec *w = reinterpret_cast<Rec *>(g_prod.buf + idx);
        w->fn = nullptr;
        w->size = (uint32_t)pad;
        w->data = 0;
        g_prod.head += pad;
        idx = 0;
    }
    Rec *r = reinterpret_cast<Rec *>(g_prod.buf + idx);
    r->size = (uint32_t)need;
    r->data = 0;
    return r;
}

void sync()
{
    if (!g_prod.running || t_d3dThread)
        return;
    if (g_prod.pending != 0)
        publish();
    const uint64_t t = g_tail.load(std::memory_order_acquire);
    g_prod.tailSeen = t;
    if (t == g_prod.head)
        return;
    g_syncs++;
    log_sync_caller();
    const int64_t t0 = qpc();
    wait_tail(g_prod.head);
    g_syncTicks += qpc() - t0;
}

void take_stats(double *busyMs, double *syncMs, uint32_t *syncs)
{
    const int64_t ticks = g_busy.exchange(0, std::memory_order_relaxed);
    *busyMs = g_qpcFreq > 0.0 ? (double)ticks * 1000.0 / g_qpcFreq : 0.0;
    *syncMs = g_qpcFreq > 0.0 ? (double)g_syncTicks * 1000.0 / g_qpcFreq : 0.0;
    *syncs = g_syncs;
    g_syncs = 0;
    g_syncTicks = 0;
}

namespace {
thread_local bool t_releaseHere;            /* (release_here) */
}  /* namespace */

void release_here()
{
    t_releaseHere = true;
}

bool release_queued(IUnknown *p)
{
    if (!g_prod.running || t_d3dThread || t_releaseHere)
        return false;
    run([p] { p->Release(); });
    return true;
}

}  /* namespace q */
}  /* namespace pglr */
