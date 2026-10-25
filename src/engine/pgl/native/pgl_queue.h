/* pgl_queue.h - the renderer's D3D thread (docs/renderer.md "The D3D
 * thread", ROADMAP item 15).
 *
 * The executor (exec_list, on the game thread that kicks the frame) works
 * out the draws - the GS state, the registry, the targets, the models - and
 * hands their Direct3D calls, in order, to a thread of their own that alone
 * uses the immediate context while it has calls to run: the two work side by
 * side.  A call is a closure (run) copied into a ring of 32 MB (the executor
 * waits when it is full); it holds what it needs by value - Direct3D objects
 * as pointers, numbers, data copied along (run_with) - never the executor's
 * own structures, which go on changing.  A Direct3D object the executor lets
 * go (Com::reset) is released on the D3D thread after the calls queued
 * before it, so none of them uses a freed one.
 *
 * Any other use of the context - gpu::context() (pgl's dc() outside a
 * closure), the swap chain (Present, the back buffer) - first waits until
 * the D3D thread has run everything queued (sync: counted and timed in the
 * frame monitor's verbose view; GT3PC_SYNC_LOG=1 or the launcher's Debug
 * tab logs who asked, each caller once).  So the code that still calls the
 * context directly stays right; the
 * frequent calls are queued.  Device calls (resources made) are
 * free-threaded and stay on the executor's thread. */
#ifndef PGL_NATIVE_PGL_QUEUE_H
#define PGL_NATIVE_PGL_QUEUE_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <utility>

struct IUnknown;

namespace pglr {
namespace q {

/* A queued call: its header (16 bytes), the closure, then its data. */
struct Rec {
    void (*fn)(Rec *);              /* nullptr: the ring wraps here */
    uint32_t size;                  /* the record's bytes */
    uint32_t data;                  /* run_with: the data's offset in it */
};

extern thread_local bool t_d3dThread;

/* The ring's bytes (a power of two) and the records published together. */
constexpr uint64_t kSize = 32ull << 20;
constexpr uint32_t kBatch = 256;

/* The executor's side (one executor at a time): what it committed, how far
 * the D3D thread had run when it last looked (it looks again only when the
 * ring seems full, or at a sync: the D3D thread's counter is written after
 * every call - read at every one, each read was a cache miss across the
 * cores), the records not yet published. */
struct Producer {
    uint8_t *buf = nullptr;
    uint64_t head = 0;
    uint64_t tailSeen = 0;
    uint32_t pending = 0;
    bool running = false;
};
extern Producer g_prod;

/* The D3D thread started (exec_init); stop: everything run, the thread
 * ended (calls run at once after). */
void start();
void stop();
inline bool running() { return g_prod.running; }
inline bool on_d3d_thread() { return t_d3dThread; }
/* (reserve's other cases: the ring wraps, or seems full) */
void *reserve_slow(uint64_t need);
void publish();
/* Space for a record of `bytes' (16-aligned, its size set) and its queueing
 * (published to the D3D thread in batches, at a sync at once). */
inline void *reserve(size_t bytes)
{
    const uint64_t need = ((uint64_t)bytes + 15) & ~15ull;
    const uint64_t idx = g_prod.head & (kSize - 1);
    if (idx + need <= kSize && g_prod.head + need - g_prod.tailSeen <= kSize) {
        Rec *r = reinterpret_cast<Rec *>(g_prod.buf + idx);
        r->size = (uint32_t)need;
        r->data = 0;
        return r;
    }
    return reserve_slow(need);
}
inline void commit(Rec *r)
{
    g_prod.head += r->size;
    if (++g_prod.pending >= kBatch)
        publish();
}
/* Everything queued run, the D3D thread idle (the caller may use the
 * context).  Nothing on the D3D thread itself or without it. */
void sync();
/* For the frame monitor: the D3D thread's busy time (ms), the executor's
 * time waiting in syncs (ms) and the syncs that waited, since the last
 * call. */
void take_stats(double *busyMs, double *syncMs, uint32_t *syncs);
/* Com::reset: true if p's release was queued (the executor's thread while
 * the D3D thread runs), false to release it now. */
bool release_queued(IUnknown *p);
/* The calling thread releases its objects at once: none of them was used by
 * a queued call (the pre-decoding worker's, pgl_prewarm.cpp - it never
 * queues). */
void release_here();

template <class F> struct Call {
    Rec h;
    F f;
    static void run(Rec *r)
    {
        Call *c = reinterpret_cast<Call *>(r);
        c->f();
        c->f.~F();
    }
};

template <class F> struct CallData {
    Rec h;
    F f;
    static void run(Rec *r)
    {
        CallData *c = reinterpret_cast<CallData *>(r);
        c->f(reinterpret_cast<const uint8_t *>(r) + r->data);
        c->f.~F();
    }
};

/* f() on the D3D thread after what was queued before (at once without the
 * thread, or on it). */
template <class F> void run(F f)
{
    if (!running() || on_d3d_thread()) {
        f();
        return;
    }
    static_assert(alignof(F) <= 16, "a closure aligned beyond 16 bytes");
    Call<F> *c = static_cast<Call<F> *>(reserve(sizeof(Call<F>)));
    new (&c->f) F(std::move(f));
    c->h.fn = &Call<F>::run;
    commit(&c->h);
}

/* f(copy) likewise, copy: `bytes' of data copied along (16-aligned). */
template <class F> void run_with(const void *data, size_t bytes, F f)
{
    if (!running() || on_d3d_thread()) {
        f(static_cast<const uint8_t *>(data));
        return;
    }
    static_assert(alignof(F) <= 16, "a closure aligned beyond 16 bytes");
    const size_t off = (sizeof(CallData<F>) + 15) & ~(size_t)15;
    uint8_t *p = static_cast<uint8_t *>(reserve(off + bytes));
    CallData<F> *c = reinterpret_cast<CallData<F> *>(p);
    new (&c->f) F(std::move(f));
    c->h.fn = &CallData<F>::run;
    c->h.data = (uint32_t)off;
    if (bytes != 0)
        memcpy(p + off, data, bytes);
    commit(&c->h);
}

}  /* namespace q */
}  /* namespace pglr */

#endif /* PGL_NATIVE_PGL_QUEUE_H */
