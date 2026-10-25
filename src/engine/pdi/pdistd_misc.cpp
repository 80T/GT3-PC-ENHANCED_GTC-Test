/* pdistd_misc.cpp - the start of the no-FDE stretch G003,
 * .text 0x0026E310..0x0026E3E0: a CreateThread-with-retry helper, then the
 * small pdistd helpers that are the same machine code as the bootstrap's
 * src/pdistd/pdistd_misc.cpp (0x01012F68..0x01012FD8).
 *
 * PC: pdistd_misc.c of the decomp in C++.  The break traps log instead of
 * trapping, and the debug TTY is the port's log. */
#include "engine/pdi/G003_pdi1394_rpc.h"
#include "engine/media/G002_mpeg_frameq.h"    /* create_mutex_sema, signal_sema_helper, yield_low_priority */
#include "eekernel.h"
#include "runtime/startup/G000_collect2_ctors.h"   /* _gp */

/* Semaphore id of the global lock; -1 until the first global_lock_acquire. */
static int s_globalLockSema = -1;               /* 0x003531D8 */

// FUNC 0x0026E310 thread_create_retry
// Fills a ThreadParam and calls CreateThread until it does not return -1,
// yielding (yield_low_priority) between tries.  Returns the thread id.  Only
// caller: thread_start_retry (0x00269C30), which then starts the thread the
// same way.
int thread_create_retry(void (*entry)(void *), int priority, void *stack,
                        int stackSize)
{
    struct ThreadParam param = {};
    int tid;

    param.entry = entry;
    param.stack = stack;
    param.stackSize = stackSize;
    param.initPriority = priority;
    param.gpReg = _gp;
    param.option = 0;
    while ((tid = CreateThread(&param)) == -1)
        yield_low_priority();
    return tid;
}

// FUNC 0x0026E370 global_lock_acquire
// If the lock semaphore does not exist yet (id == -1), creates it with
// create_mutex_sema(1) and stores the id; then WaitSema(id), whose result
// is returned.  Callers: the "host:" fileio wrappers.
int global_lock_acquire(void)
{
    int sema = s_globalLockSema;

    if (sema == -1) {
        sema = create_mutex_sema(1);
        s_globalLockSema = sema;
    }
    return WaitSema(sema);
}

// FUNC 0x0026E3A8 global_lock_release
void global_lock_release(void)
{
    signal_sema_helper(s_globalLockSema);
}

// FUNC 0x0026E3B0 break_stub_0
// `break' (code 0) trap, then return 0.  PC: logged.
int break_stub_0(void)
{
    scePrintf("break_stub_0: break trap\n");
    return 0;
}

// FUNC 0x0026E3C0 break_stub_1
// `break' (code 0) trap, then return 0; abort() calls it with 6 (SIGABRT).
// PC: logged.
int break_stub_1(int code)
{
    scePrintf("break_stub_1(%d): break trap\n", code);
    return 0;
}

// FUNC 0x0026E3D0 printf_to_deci2
// PrintTarget_flush's backend (0x0026C5A8): the kernel TTY printf,
// sce_printf_deci2("%s", s).  PC: the log.
void printf_to_deci2(const char *s)
{
    scePrintf("%s", s);
}
