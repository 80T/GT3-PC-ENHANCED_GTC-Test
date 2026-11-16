/* pgl_queue.h - the renderer's D3D thread and, PC (2026-11-05), its submit
 * thread (docs/renderer.md "The D3D thread", "The submit thread"; ROADMAP
 * item 15).
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
 * free-threaded and stay on the executor's thread.
 *
 * PC (2026-11-05): the submit stage (sq).  Between the executor and the D3D
 * thread a third thread does the draws' submission - a VU1 draw's record,
 * constants, lighting block, the merged draws, the passes, the binding cache
 * and the closures for the D3D thread (pgl_exec.cpp sub_vu1, sub_gs: what
 * vu1_submit and draw_passes did on the executor) - from records the
 * executor queues into a ring of its own (sq::run: the same closures), one
 * per draw, with the draw state when it changes.  While a frame pipelines
 * (sq::g_on, set by exec_begin_frame) everything the executor's thread sends
 * toward the GPU goes through that ring too, in order with the draws
 * (q::t_forward: q::run, q::run_with, Com::reset's release, the binding
 * calls of pgl_d3d.cpp) - the ring of the D3D thread has one producer at a
 * time, the submit thread while it runs.  q::sync from the executor drains
 * the submit thread first; a frame ends with such a drain (exec_end_frame),
 * so the game thread's uses of the context between frames find both threads
 * idle.  Without the thread (GT3PC_PGL_SUBMIT_THREAD=0, the debug aids that
 * look at each draw, the GPU sections) the records run at once on the
 * executor, as before. */
#ifndef PGL_NATIVE_PGL_QUEUE_H
#define PGL_NATIVE_PGL_QUEUE_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <utility>
#include <vector>

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

/* The producer's side of a ring (one producer at a time): what it committed,
 * how far the consumer had run when it last looked (it looks again only
 * when the ring seems full, or at a sync: the consumer's counter is written
 * after every call - read at every one, each read was a cache miss across
 * the cores), the records not yet published. */
struct Producer {
    uint8_t *buf = nullptr;
    uint64_t head = 0;
    uint64_t tailSeen = 0;
    uint32_t pending = 0;
    bool running = false;
};
extern Producer g_prod;
/* PC (2026-11-05): set on the executor's thread while the submit stage
 * pipelines the frame (sq::g_on): the executor's calls go through the
 * submit ring (run, run_with, release_queued) - see the file comment. */
extern thread_local bool t_forward;

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
 * context).  Nothing on the D3D thread itself or without it.  PC
 * (2026-11-05): the submit thread drained first (its held draw, everything
 * the executor sent through it). */
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

/* PC (CPU and GPU time, 2026-11-03): a call its producer holds back - the
 * executor's last draw, which the next one may extend (pgl_exec.cpp,
 * draw_passes: a draw that goes on where the last one ended, nothing queued
 * between, is the same draw) - queued, by its function, before anything else
 * is (run, run_with, sync, release_held at a frame's end) - by the thread
 * that holds it (t_holder: the submit stage's), never another. */
extern thread_local void (*g_held)();   /* PC (2026-11-05): per thread (the submit workers hold their own) */
extern thread_local bool t_holder;
inline void release_held()
{
    if (t_holder) {
        void (*f)() = g_held;
        g_held = nullptr;
        t_holder = false;
        f();
    }
}
inline void hold(void (*f)())
{
    release_held();
    g_held = f;
    t_holder = true;
}

/* PC (2026-11-05, ROADMAP item 15 "The parallel executor", step 2): a
 * thread that captures its calls instead of queueing them - the submit
 * workers (pgl_exec.cpp run_work), which build a replayed run's submission
 * each on a thread of its own: the records as the ring holds them, into a
 * buffer of the run's own (t_capture), spliced into the D3D thread's ring in
 * the runs' order by the submit thread (splice).  A record holds what it
 * needs by value (run_with's data at an offset inside it), so its bytes
 * move as they are. */
struct alignas(16) CaptureBlock {
    uint8_t b[16];
};
struct Capture {
    std::vector<CaptureBlock> buf;          /* (16-aligned, as the ring's records are) */
    uint32_t count = 0;
    void clear()
    {
        buf.clear();
        count = 0;
    }
    size_t bytes() const { return buf.size() * 16; }
};
extern thread_local Capture *t_capture;     /* (null: the calls are queued) */
void *capture_reserve(size_t bytes);        /* a record's place in the capture (its size set) */
/* The ring's producer appends captured records as they are (bytes of them,
 * count records); without the D3D thread they run here. */
void splice(const void *recs, size_t bytes, uint32_t count);

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

}  /* namespace q */

/* PC (2026-11-05): the submit stage's ring and thread (the file comment).
 * The same records as the D3D thread's; its consumer runs them on the
 * submit thread, where they do the draws' submission (and queue the D3D
 * thread's calls).  g_on: this frame pipelines - else run / run_with run
 * their closure at once (the executor is the submit stage then, as before;
 * the thread idle). */
namespace sq {

constexpr uint64_t kSize = 16ull << 20;
extern q::Producer g_prod;
extern thread_local bool t_thread;
extern bool g_on;
/* (pgl_exec.cpp: what the thread sets up for itself as it starts - its
 * profile and stats) */
extern void (*g_threadInit)();

void start();
void stop();
inline bool running() { return g_prod.running; }
inline bool on_thread() { return t_thread; }
void *reserve_slow(uint64_t need);
void publish();
inline void *reserve(size_t bytes)
{
    const uint64_t need = ((uint64_t)bytes + 15) & ~15ull;
    const uint64_t idx = g_prod.head & (kSize - 1);
    if (idx + need <= kSize && g_prod.head + need - g_prod.tailSeen <= kSize) {
        q::Rec *r = reinterpret_cast<q::Rec *>(g_prod.buf + idx);
        r->size = (uint32_t)need;
        r->data = 0;
        return r;
    }
    return reserve_slow(need);
}
inline void commit(q::Rec *r)
{
    g_prod.head += r->size;
    if (++g_prod.pending >= q::kBatch)
        publish();
}
/* Everything queued run (the held draw released first), the thread idle:
 * the executor's frame end, and q::sync from the executor.  Nothing when
 * the frame does not pipeline (the thread is idle) or on the thread. */
void sync();
/* For the frame monitor: the thread's busy time (ms), the executor's time
 * waiting for it (ms: the ring full, the syncs) and those waits, since the
 * last call. */
void take_stats(double *busyMs, double *waitMs, uint32_t *waits);
/* PC (2026-11-05, the submit workers): the producer's position (what the
 * executor has queued, published or not), what the thread has run (its
 * ring's tail), and a wait by another thread until that reaches `want'. */
inline uint64_t head() { return g_prod.head; }
uint64_t processed();
void wait_processed(uint64_t want);

/* f() on the submit thread after what was queued before (at once when the
 * frame does not pipeline, or on the thread). */
template <class F> void run(F f)
{
    if (!g_on || t_thread) {
        f();
        return;
    }
    static_assert(alignof(F) <= 16, "a closure aligned beyond 16 bytes");
    q::Call<F> *c = static_cast<q::Call<F> *>(sq::reserve(sizeof(q::Call<F>)));
    new (&c->f) F(std::move(f));
    c->h.fn = &q::Call<F>::run;
    sq::commit(&c->h);                      /* (qualified: q::commit is found by the record's type too) */
}

/* f(copy) likewise, copy: `bytes' of data copied along (16-aligned). */
template <class F> void run_with(const void *data, size_t bytes, F f)
{
    if (!g_on || t_thread) {
        f(static_cast<const uint8_t *>(data));
        return;
    }
    static_assert(alignof(F) <= 16, "a closure aligned beyond 16 bytes");
    const size_t off = (sizeof(q::CallData<F>) + 15) & ~(size_t)15;
    uint8_t *p = static_cast<uint8_t *>(sq::reserve(off + bytes));
    q::CallData<F> *c = reinterpret_cast<q::CallData<F> *>(p);
    new (&c->f) F(std::move(f));
    c->h.fn = &q::CallData<F>::run;
    c->h.data = (uint32_t)off;
    if (bytes != 0)
        memcpy(p + off, data, bytes);
    sq::commit(&c->h);
}

}  /* namespace sq */

namespace q {

/* f() on the D3D thread after what was queued before (at once without the
 * thread, or on it).  PC (2026-11-05): from the executor while the frame
 * pipelines, through the submit thread (t_forward: it queues the call from
 * there, in order with the draws). */
template <class F> void run(F f)
{
    if (t_forward) {
        sq::run([f = std::move(f)]() mutable { run(std::move(f)); });
        return;
    }
    release_held();                         /* PC: (a held draw first) */
    static_assert(alignof(F) <= 16, "a closure aligned beyond 16 bytes");
    if (t_capture != nullptr) {             /* PC: (a submit worker: captured for the splice) */
        Call<F> *c = static_cast<Call<F> *>(capture_reserve(sizeof(Call<F>)));
        new (&c->f) F(std::move(f));
        c->h.fn = &Call<F>::run;
        t_capture->count++;
        return;
    }
    if (!running() || on_d3d_thread()) {
        f();
        return;
    }
    Call<F> *c = static_cast<Call<F> *>(reserve(sizeof(Call<F>)));
    new (&c->f) F(std::move(f));
    c->h.fn = &Call<F>::run;
    commit(&c->h);
}

/* f(copy) likewise, copy: `bytes' of data copied along (16-aligned). */
template <class F> void run_with(const void *data, size_t bytes, F f)
{
    if (t_forward) {
        sq::run_with(data, bytes, [f = std::move(f), bytes](const uint8_t *p) mutable {
            run_with(p, bytes, std::move(f));
        });
        return;
    }
    release_held();                         /* PC: (a held draw first) */
    static_assert(alignof(F) <= 16, "a closure aligned beyond 16 bytes");
    if (t_capture != nullptr) {             /* PC: (a submit worker: captured for the splice) */
        const size_t off = (sizeof(CallData<F>) + 15) & ~(size_t)15;
        uint8_t *p = static_cast<uint8_t *>(capture_reserve(off + bytes));
        CallData<F> *c = reinterpret_cast<CallData<F> *>(p);
        new (&c->f) F(std::move(f));
        c->h.fn = &CallData<F>::run;
        c->h.data = (uint32_t)off;
        if (bytes != 0)
            memcpy(p + off, data, bytes);
        t_capture->count++;
        return;
    }
    if (!running() || on_d3d_thread()) {
        f(static_cast<const uint8_t *>(data));
        return;
    }
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
