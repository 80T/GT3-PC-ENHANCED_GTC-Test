/* thread_util.cpp - interrupt / semaphore / thread helpers
 * (0x00269B00..0x00269D80, and busy_wait_delay at 0x00269F88 after the
 * hostfs wrappers, as in the bootstrap's link order).
 *
 * intr_suspend, create_mutex_sema, yield_low_priority, signal_sema_helper and
 * busy_wait_delay are the same machine code as the bootstrap's
 * (src/pdistd/pdistd_misc.cpp, src/thread_util.cpp): their source is reused.
 * The worker-thread helpers (thread_start_retry .. worker_thread_exit) are
 * new in the main executable.
 *
 * PC: thread_util.c of the decomp in C++.  The kernel calls are the port's
 * (eekernel.h); the COP0 Status reads and the calibrated nop loop are gone.
 */
#define G002_WORKER_JOIN_INT            /* worker_thread_join returns int here (see the header) */
#include "engine/media/G002_mpeg_frameq.h"
#include "engine/pdi/G003_pdi1394_rpc.h"      /* thread_create_retry */
#include "eekernel.h"
#include "platform/scheduler.h"

#define YIELD_PRIORITY      0x7E        /* yield_low_priority's temporary priority */
#define YIELD_PASSES        100
#define YIELD_SPIN          10          /* busy_wait_delay argument per pass */
#define WORKER_EXIT_PRIORITY 1          /* worker_thread_exit: not preempted before it exits */

void busy_wait_delay(s64 n);

// FUNC 0x00269B00 intr_suspend
// Polyphony's own DIntr: disables interrupts and returns 1 if they were
// enabled.  Callers pair it with `if (r) ei' (116 callers: the allocator,
// the sound code, GameObjectController, ...).
// PC: the port's interrupt lock (DIntr).
int intr_suspend(void)
{
    return DIntr();
}

// FUNC 0x00269B30 create_mutex_sema
// Creates a binary semaphore (maxCount 1) with the given initial count.
// CreateSema == -1 (no free semaphore) -> yield and retry forever.  Quirk:
// an id of 0 is rejected by recursing once more, which leaks semaphore 0.
// 16 callers (global_lock_acquire, vol_init, worker_thread_start, ...).
int create_mutex_sema(int initCount)
{
    struct SemaParam param = {};

    param.maxCount  = 1;
    param.initCount = initCount;
    param.option    = 0;

    int sid;
    while ((sid = CreateSema(&param)) == -1)
        yield_low_priority();

    if (sid == 0)
        return create_mutex_sema(initCount);
    return sid;
}

// FUNC 0x00269BA0 yield_low_priority
// Cooperative yield used by every polling loop (MC wrappers, CreateSema /
// CreateThread / StartThread retries): lower the calling thread to priority
// 0x7E, then 100 times spin busy_wait_delay(10) and rotate the 0x7E ready
// queue so lower/equal-priority threads get CPU time, then restore the
// priority read by ReferThreadStatus.  Return values are ignored.
void yield_low_priority(void)
{
    struct ThreadParam status;
    int tid = GetThreadId();

    ReferThreadStatus(tid, &status);
    ChangeThreadPriority(tid, YIELD_PRIORITY);
    for (int pass = YIELD_PASSES - 1; pass >= 0; pass--) {
        busy_wait_delay(YIELD_SPIN);
        RotateThreadReadyQueue(YIELD_PRIORITY);
    }
    ChangeThreadPriority(tid, status.currentPriority);
}

// FUNC 0x00269C10 signal_sema_helper
// Shared unlock helper (43 callers, also global_lock_release).  It calls
// iSignalSema even though callers run in thread context: the kernel signals
// the semaphore but does not reschedule immediately.
int signal_sema_helper(int semaId)
{
    return iSignalSema(semaId);
}

// FUNC 0x00269C30 thread_start_retry
/* Create a thread (thread_create_retry retries CreateThread) and start it
 * with arg, yielding and retrying while StartThread does not return the
 * thread id.  Returns the thread id.  Callers
 * MemorycardFileManager::createThread, F_0022D870 (T473), padStart,
 * worker_thread_start. */
int thread_start_retry(void (*entry)(void *), void *arg, int priority, void *stack, int stackSize)
{
    int tid = thread_create_retry(entry, priority, stack, stackSize);

    while (StartThread(tid, arg) != tid)
        yield_low_priority();
    return tid;
}

// FUNC 0x00269C90 worker_thread_start
/* sema = a fresh binary semaphore at count 0 (created first), then the
 * thread.  Callers logger_record_start, music_init, mc_manager_init,
 * vol_init. */
void worker_thread_start(ThreadCtx *w, void (*entry)(void *), void *arg, int priority,
                         void *stack, int stackSize)
{
    w->sema = create_mutex_sema(0);
    w->thread = thread_start_retry(entry, arg, priority, stack, stackSize);
}

// FUNC 0x00269D18 worker_thread_join
/* Wait for the worker's completion signal, then delete the semaphore and
 * return DeleteSema's result. */
int worker_thread_join(ThreadCtx *w)
{
    WaitSema(w->sema);
    return DeleteSema(w->sema);
}

// FUNC 0x00269D48 worker_thread_exit
/* Called by the worker itself at the end (music_thread, ...): raise its
 * priority to 1 (so it is not preempted between signalling and exiting),
 * signal sema and ExitDeleteThread (does not return). */
void worker_thread_exit(ThreadCtx *w)
{
    ChangeThreadPriority(GetThreadId(), WORKER_EXIT_PRIORITY);
    signal_sema_helper(w->sema);
    ExitDeleteThread();
}

// FUNC 0x00269F88 busy_wait_delay
// A calibrated spin of 75 * n + 2 passes of six nops: about 1.8 us per unit
// of n on the 294.912 MHz EE.  Only caller: yield_low_priority (n = 10).
// PC: waits the same time on the high-resolution clock.
void busy_wait_delay(s64 n)
{
    double until = sched::now_seconds() + (double)(n * 75 + 2) * 7.0 / 294912000.0;
    while (sched::now_seconds() < until) {
    }
}
