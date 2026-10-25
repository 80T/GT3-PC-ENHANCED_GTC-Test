/* ee_kernel.cpp - the EE kernel calls of eekernel.h on the port's scheduler.
 *
 * The PS2 names stay so the game and engine code that calls them stays as
 * it was.  The i-variants (iSignalSema, iWakeupThread...) change the state
 * without switching threads, as on the EE, also when a thread calls them
 * (signal_sema_helper does, 43 callers).  This file must not include <windows.h>: several of these names
 * (CreateThread, GetThreadId, SuspendThread...) are Win32 functions too. */
#include "eekernel.h"
#include "platform/scheduler.h"
#include "platform/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

extern "C" {

/* crt0's $gp, which threads were created with (ignored). */
char _gp[4];

/* ---- threads ------------------------------------------------------------- */

int CreateThread(struct ThreadParam *param)
{
    return sched::create_thread(param->entry, param->initPriority, param->stackSize);
}

int DeleteThread(int tid) { return sched::delete_thread(tid); }
int StartThread(int tid, void *arg) { return sched::start_thread(tid, arg); }
void ExitThread(void) { sched::exit_thread(false); }
void ExitDeleteThread(void) { sched::exit_thread(true); }
int TerminateThread(int tid) { return sched::terminate_thread(tid); }
int ChangeThreadPriority(int tid, int priority) { return sched::change_priority(tid, priority); }
int iChangeThreadPriority(int tid, int priority) { return sched::change_priority(tid, priority, false); }
int RotateThreadReadyQueue(int priority) { return sched::rotate_ready_queue(priority); }
int iRotateThreadReadyQueue(int priority) { return sched::rotate_ready_queue(priority, false); }
int ReleaseWaitThread(int tid) { return sched::release_wait(tid); }
int iReleaseWaitThread(int tid) { return sched::release_wait(tid, false); }
int GetThreadId(void) { return sched::current_thread(); }

int ReferThreadStatus(int tid, struct ThreadParam *info)
{
    sched::ThreadInfo t;
    int r = sched::refer_thread(tid, &t);
    if (r < 0 || !info)
        return r;
    info->status = t.status;
    info->entry = t.entry;
    info->stack = nullptr;
    info->stackSize = 0;
    info->gpReg = _gp;
    info->initPriority = t.initPriority;
    info->currentPriority = t.currentPriority;
    info->attr = 0;
    info->option = 0;
    info->waitType = t.waitType;
    info->waitId = t.waitId;
    info->wakeupCount = t.wakeupCount;
    return r;
}

int iReferThreadStatus(int tid, struct ThreadParam *info) { return ReferThreadStatus(tid, info); }
int SleepThread(void) { return sched::sleep_thread(); }
int WakeupThread(int tid) { return sched::wakeup_thread(tid); }
int iWakeupThread(int tid) { return sched::wakeup_thread(tid, false); }
int CancelWakeupThread(int tid) { return sched::cancel_wakeup(tid); }
int iCancelWakeupThread(int tid) { return sched::cancel_wakeup(tid); }
int SuspendThread(int tid) { return sched::suspend_thread(tid); }
int iSuspendThread(int tid) { return sched::suspend_thread(tid, false); }
int ResumeThread(int tid) { return sched::resume_thread(tid); }
int iResumeThread(int tid) { return sched::resume_thread(tid, false); }
int DelayThread(int usec) { return sched::delay_thread(usec); }

/* ---- semaphores ------------------------------------------------------------ */

int CreateSema(struct SemaParam *param)
{
    return sched::create_sema(param->initCount, param->maxCount);
}

int DeleteSema(int sid) { return sched::delete_sema(sid); }
int SignalSema(int sid) { return sched::signal_sema(sid); }
int iSignalSema(int sid) { return sched::signal_sema(sid, false); }
int WaitSema(int sid) { return sched::wait_sema(sid); }
int PollSema(int sid) { return sched::poll_sema(sid); }
int iPollSema(int sid) { return sched::poll_sema(sid); }

int ReferSemaStatus(int sid, struct SemaParam *info)
{
    sched::SemaInfo s;
    int r = sched::refer_sema(sid, &s);
    if (r < 0 || !info)
        return r;
    info->currentCount = s.currentCount;
    info->maxCount = s.maxCount;
    info->initCount = s.initCount;
    info->numWaitThreads = s.numWaitThreads;
    info->attr = 0;
    info->option = 0;
    return r;
}

int iReferSemaStatus(int sid, struct SemaParam *info) { return ReferSemaStatus(sid, info); }

/* ---- interrupts ------------------------------------------------------------ */

int AddIntcHandler(int cause, int (*handler)(int cause), int next)
{
    return sched::add_intc_handler(cause, nullptr, nullptr, handler, next);
}

int AddIntcHandler2(int cause, int (*handler)(int cause, void *arg, void *addr), int next, void *arg)
{
    return sched::add_intc_handler(cause, handler, arg, nullptr, next);
}

int RemoveIntcHandler(int cause, int handler_id) { return sched::remove_intc_handler(cause, handler_id); }

/* No DMA channel interrupts exist in the port (no DMA): accepted, never called. */
int AddDmacHandler(int, int (*)(int), int) { return 1; }
int AddDmacHandler2(int, int (*)(int, void *, void *), int, void *) { return 1; }
int RemoveDmacHandler(int, int) { return 0; }

int EnableIntc(int cause) { return sched::enable_intc(cause, true); }
int DisableIntc(int cause) { return sched::enable_intc(cause, false); }
int iEnableIntc(int cause) { return sched::enable_intc(cause, true); }
int iDisableIntc(int cause) { return sched::enable_intc(cause, false); }
int EnableDmac(int) { return 0; }
int DisableDmac(int) { return 0; }
int iEnableDmac(int) { return 0; }
int iDisableDmac(int) { return 0; }

void DI(void) { sched::di(); }
void EI(void) { sched::ei(); }
int DIntr(void) { return sched::di(); }
int EIntr(void) { sched::ei(); return 1; }

/* ---- alarms ------------------------------------------------------------------ */

int SetAlarm(u16 time, void (*callback)(int alarm_id, u16 time, void *arg), void *arg)
{
    return sched::set_alarm(time, callback, arg);
}

int iSetAlarm(u16 time, void (*callback)(int alarm_id, u16 time, void *arg), void *arg)
{
    return sched::set_alarm(time, callback, arg);
}

int ReleaseAlarm(int alarm_id) { return sched::release_alarm(alarm_id); }
int iReleaseAlarm(int alarm_id) { return sched::release_alarm(alarm_id); }

/* ---- cache, misc ------------------------------------------------------------- */

/* There are no caches to keep coherent with DMA: the port has no DMA. */
void FlushCache(int) {}
void iFlushCache(int) {}
void SyncDCache(void *, void *) {}
void iSyncDCache(void *, void *) {}
void InvalidDCache(void *, void *) {}
void iInvalidDCache(void *, void *) {}

void Exit(int status)
{
    pclog::write("game: Exit(%d)", status);
    pclog::close();
    exit(status);
}

int scePrintf(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(buf) - 1)
        n = (int)sizeof(buf) - 1;
    if (n > 0)
        pclog::raw(buf, n);
    return n;
}

} // extern "C"
