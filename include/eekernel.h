/* eekernel.h - the EE kernel calls the game and the engine make (GT3 PC).
 *
 * On the PS2 these were syscalls of Sony's libkernl.  The port implements
 * them natively in src/platform/ee_kernel.cpp on top of its scheduler, with
 * the EE's rules: one game thread runs at a time, the highest-priority ready
 * thread (lowest number) runs, a running thread keeps the CPU until it
 * blocks or a higher-priority thread becomes ready; interrupt handlers run
 * asynchronously and are kept out of DI / EI sections.  Ids are small
 * integers as on the PS2.  Cache and DMA calls are no-ops.
 *
 * Only what the program calls is declared; the decomp's header lists every
 * libkernl syscall with its number. */
#ifndef BOOT_EEKERNEL_H
#define BOOT_EEKERNEL_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Thread parameter / status block (CreateThread, ReferThreadStatus). */
struct ThreadParam {
    int           status;           /* +0x00 THS_*                              */
    void        (*entry)(void *);   /* +0x04                                    */
    void         *stack;            /* +0x08 (PC: unused, threads get their own stacks) */
    int           stackSize;        /* +0x0C (PC: a minimum for the native stack) */
    void         *gpReg;            /* +0x10 (PC: unused)                       */
    int           initPriority;     /* +0x14                                    */
    int           currentPriority;  /* +0x18                                    */
    unsigned int  attr;             /* +0x1C                                    */
    unsigned int  option;           /* +0x20                                    */
    int           waitType;         /* +0x24 TSW_*                              */
    int           waitId;           /* +0x28                                    */
    int           wakeupCount;      /* +0x2C                                    */
};

/* Semaphore parameter / status block (CreateSema, ReferSemaStatus). */
struct SemaParam {
    int           currentCount;     /* +0x00 */
    int           maxCount;         /* +0x04 */
    int           initCount;        /* +0x08 */
    int           numWaitThreads;   /* +0x0C */
    unsigned int  attr;             /* +0x10 */
    unsigned int  option;           /* +0x14 */
};

/* ThreadParam.status */
#define THS_RUN          0x01
#define THS_READY        0x02
#define THS_WAIT         0x04
#define THS_SUSPEND      0x08
#define THS_WAITSUSPEND  0x0C
#define THS_DORMANT      0x10

/* ThreadParam.waitType */
#define TSW_SLEEP        1
#define TSW_SEMA         2

/* INTC interrupt causes (AddIntcHandler / EnableIntc).  The port delivers
 * INTC_VBLANK_S and INTC_VBLANK_E (59.94 Hz); the others never fire. */
#define INTC_GS          0
#define INTC_SBUS        1
#define INTC_VBLANK_S    2
#define INTC_VBLANK_E    3
#define INTC_VIF0        4
#define INTC_VIF1        5
#define INTC_VU0         6
#define INTC_VU1         7
#define INTC_IPU         8
#define INTC_TIM0        9
#define INTC_TIM1        10
#define INTC_TIM2        11
#define INTC_TIM3        12
#define INTC_SFIFO       13
#define INTC_VU0WD       14

/* DMAC channels (AddDmacHandler): accepted, never fire. */
#define DMAC_VIF0        0
#define DMAC_VIF1        1
#define DMAC_GIF         2
#define DMAC_FROM_IPU    3
#define DMAC_TO_IPU      4
#define DMAC_SIF0        5
#define DMAC_SIF1        6
#define DMAC_SIF2        7
#define DMAC_FROM_SPR    8
#define DMAC_TO_SPR      9

/* FlushCache modes (no-ops). */
#define WRITEBACK_DCACHE   0
#define INVALIDATE_DCACHE  1
#define INVALIDATE_ICACHE  2
#define INVALIDATE_CACHE   3

/* ---- Threads ----------------------------------------------------------- */
int   CreateThread(struct ThreadParam *param);
int   DeleteThread(int tid);
int   StartThread(int tid, void *arg);
void  ExitThread(void);
void  ExitDeleteThread(void);
int   TerminateThread(int tid);
int   ChangeThreadPriority(int tid, int priority);
int   iChangeThreadPriority(int tid, int priority);
int   RotateThreadReadyQueue(int priority);
int   iRotateThreadReadyQueue(int priority);
int   ReleaseWaitThread(int tid);
int   iReleaseWaitThread(int tid);
int   GetThreadId(void);
int   ReferThreadStatus(int tid, struct ThreadParam *info);
int   iReferThreadStatus(int tid, struct ThreadParam *info);
int   SleepThread(void);
int   WakeupThread(int tid);
int   iWakeupThread(int tid);
int   CancelWakeupThread(int tid);
int   iCancelWakeupThread(int tid);
int   SuspendThread(int tid);
int   iSuspendThread(int tid);
int   ResumeThread(int tid);
int   iResumeThread(int tid);
/* Libkernl's DelayThread waits in microseconds (an alarm per hsync). */
int   DelayThread(int usec);

/* ---- Semaphores ---------------------------------------------------------- */
int   CreateSema(struct SemaParam *param);
int   DeleteSema(int sid);
int   SignalSema(int sid);
int   iSignalSema(int sid);
int   WaitSema(int sid);
int   PollSema(int sid);
int   iPollSema(int sid);
int   ReferSemaStatus(int sid, struct SemaParam *info);
int   iReferSemaStatus(int sid, struct SemaParam *info);

/* ---- Interrupts ------------------------------------------------------------ */
int   AddIntcHandler(int cause, int (*handler)(int cause), int next);
int   AddIntcHandler2(int cause, int (*handler)(int cause, void *arg, void *addr), int next, void *arg);
int   RemoveIntcHandler(int cause, int handler_id);
int   AddDmacHandler(int channel, int (*handler)(int channel), int next);
int   AddDmacHandler2(int channel, int (*handler)(int channel, void *arg, void *addr), int next, void *arg);
int   RemoveDmacHandler(int channel, int handler_id);
int   EnableIntc(int cause);
int   DisableIntc(int cause);
int   iEnableIntc(int cause);
int   iDisableIntc(int cause);
int   EnableDmac(int channel);
int   DisableDmac(int channel);
int   iEnableDmac(int channel);
int   iDisableDmac(int channel);

/* Interrupts off / on for the calling thread: a recursive lock shared with
 * the interrupt dispatcher (a handler never runs inside a DI section).
 * DIntr returns 1 if interrupts were on. */
void  DI(void);
void  EI(void);
int   DIntr(void);
int   EIntr(void);
/* The end of an interrupt handler (a no-op: the dispatcher returns). */
#define ExitHandler() ((void)0)

/* ---- Alarms (in hsyncs: 15734 Hz) --------------------------------------- */
int   SetAlarm(u16 time, void (*callback)(int alarm_id, u16 time, void *arg), void *arg);
int   iSetAlarm(u16 time, void (*callback)(int alarm_id, u16 time, void *arg), void *arg);
int   ReleaseAlarm(int alarm_id);
int   iReleaseAlarm(int alarm_id);

/* ---- Cache, misc --------------------------------------------------------- */
void  FlushCache(int mode);
void  iFlushCache(int mode);
void  SyncDCache(void *start, void *end);
void  iSyncDCache(void *start, void *end);
void  InvalidDCache(void *start, void *end);
void  iInvalidDCache(void *start, void *end);
void  Exit(int status);

/* The kernel's own printf (libkernl scePrintf): to the port's log. */
int   scePrintf(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* BOOT_EEKERNEL_H */
