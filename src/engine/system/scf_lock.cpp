/* scf_lock.cpp - two libscf clock conversions serialised by a lock
 * (same pattern as the memory-card wrappers).  Callers: scf_lock_init from
 * game_init (0x00102C00); the conversions from T519's
 * pdicdvd_time_to_clock_gmt / pdicdvd_time_to_clock_local.
 *
 * PC: scf_lock.c of the decomp in C++, with the libscf conversions it
 * called (sceScfGetGMTfromRTC / sceScfGetLocalTimefromRTC, which used the
 * console's time zone settings) done on the PC's clock settings
 * (platform/pc_time.h).  libscf's sceScfGetAspect is here too. */
#include "engine/media/G002_mpeg_frameq.h"    /* create_mutex_sema, signal_sema_helper, scf_clock_to_* */
#include "eekernel.h"
#include "platform/pc_time.h"

/* .sdata: the lock, -1 until scf_lock_init creates it. */
extern "C" int g_scf_lock_sema = -1;        /* 0x003531A4 */

// FUNC 0x00267A58 scf_lock_init
/* Creates the lock (count 1) if it does not exist yet. */
void scf_lock_init(void)
{
    if (g_scf_lock_sema < 0)
        g_scf_lock_sema = create_mutex_sema(1);
}

// FUNC 0x00267A80 scf_clock_to_gmt
/* libscf RTC (JST) -> GMT conversion in place, under the lock. */
void scf_clock_to_gmt(sceCdCLOCK *rtc)
{
    WaitSema(g_scf_lock_sema);
    pctime::bcd_jst_to_gmt((u8 *)rtc);
    signal_sema_helper(g_scf_lock_sema);
}

// FUNC 0x00267AB8 scf_clock_to_local
/* libscf RTC (JST) -> local time conversion in place, under the lock. */
void scf_clock_to_local(sceCdCLOCK *rtc)
{
    WaitSema(g_scf_lock_sema);
    pctime::bcd_jst_to_local((u8 *)rtc);
    signal_sema_helper(g_scf_lock_sema);
}

// FUNC 0x00274720 sceScfGetAspect
/* libscf: the console's screen type (0 4:3, 1 full, 2 16:9).  PC: 4:3; the
 * aspect follows the window in a later pass (docs/ROADMAP.md). */
extern "C" int sceScfGetAspect(void)
{
    return 0;
}
