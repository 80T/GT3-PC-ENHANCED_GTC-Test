/* T519 - pdicdvd.cxx.  0x0025B490..0x0025BF38.
 *
 * Client of the pdicdvd IOP module: two RPC objects in the file's anonymous
 * namespace (_GLOBAL_$N$.._.._pdicdvd.cxxs1Kop7):
 *   RPC  @0x004A6B00, server 'PCDV' (0x50434456): disc reads (fno 3), the
 *        disc identity (fno 2), fno 1 (sent when an iso9660 lookup fails);
 *   RPC2 @0x004A6A80, server 'Pcdv' (0x50636476): the clock (fno 2), fno 1.
 * Reads were serialised by the semaphore g_003530E8 and ended
 * asynchronously: RPC::onCallEnd signalled doneSema, pdicdvd_read_wait took
 * it.
 *
 * PC: the RPC objects are gone.  A read is a read of gt3.vol (the port's
 * disc, platform/disc.h), done at once: pdicdvd_read_start reads and keeps
 * the result, pdicdvd_read_wait returns it.  The clock is the PC's clock,
 * counted in Japan time as the PS2's RTC (libscf converts it to GMT / local
 * time).  The date conversion below is the original's. */
#include "engine/pdi/T519_pdicdvd.h"
#include "engine/pdi/pdi_internal.h"
#include "platform/disc.h"
#include "platform/pc_time.h"

/* Lock of the disc reads, a semaphore made by pdicdvd_init; -1 before.
 * .sdata (-G8). */
int g_003530E8 = -1;            /* 0x003530E8 */

/* The result of the last pdicdvd_read_start (PC: the read is already done). */
static int s_readOk = 1;

/* Caller: RaceIOPLoadModules::cmdBindClients, after the pdi modules are
 * loaded. */
// FUNC 0x0025B5D8 pdicdvd_init
void pdicdvd_init(void)
{
    g_003530E8 = create_mutex_sema(1);
}

/* Caller: iso9660_find, when the lookup fails.  ('PCDV' fno 1: PC nothing.) */
// FUNC 0x0025B620 F_0025B620
void F_0025B620(void)
{
}

/* Caller: iso9660_read_pvd.  ('PCDV' fno 2: PC nothing.) */
// FUNC 0x0025B648 pdicdvd_set_disc_id
void pdicdvd_set_disc_id(const PdiDiscId *id)
{
    (void)id;
}

/* Callers: vol_mount (0x0023DE10), sector_cache_read.  Blocking read;
 * returns 1 when it succeeded. */
// FUNC 0x0025B6A8 pdicdvd_read
int pdicdvd_read(void *buf, u32 lsn, u32 size, int mode)
{
    pdicdvd_read_start(buf, lsn, size, mode);
    return pdicdvd_read_wait();
}

// FUNC 0x0025B6D0 pdicdvd_read_start
void pdicdvd_read_start(void *buf, u32 lsn, u32 size, int mode)
{
    (void)mode;
    if (g_003530E8 >= 0)
        WaitSema(g_003530E8);
    s_readOk = disc::read_sectors(lsn, buf, size) ? 1 : 0;
    if (g_003530E8 >= 0)
        signal_sema_helper(g_003530E8);
}

// FUNC 0x0025B780 pdicdvd_read_wait
int pdicdvd_read_wait(void)
{
    return s_readOk;
}

/* No callers. */
// FUNC 0x0025B7B8 pdicdvd_read_pending
int pdicdvd_read_pending(void)
{
    return 0;
}

/* No callers.  ('Pcdv' fno 1.) */
// FUNC 0x0025B800 F_0025B800
void F_0025B800(void)
{
}

/* Caller: clock_update (0x0022BA48).  The clock: low word the seconds since
 * 2000-01-01 00:00 (Japan time, as the PS2's RTC), high word the stat byte
 * (0: the clock is fine). */
// FUNC 0x0025B828 pdicdvd_get_time
u64 pdicdvd_get_time(void)
{
    return (u64)pctime::rtc_seconds_since_2000_jst();
}

#define SECONDS_PER_DAY  86400
#define DAYS_PER_4_YEARS 1461           /* 3 x 365 + 366 */

/* Two BCD digits.  The tens digit is a signed `% 10' of the unsigned
 * quotient (div after divu in the listing). */
#define PDI_BCD(v) ((u8)((((int)((v) / 10) % 10) << 4) | ((v) % 10)))

/* First day of each month: [0] normal, [1] leap year.  Not const: retail
 * has it in .data (a const table would be in .rodata). */
u16 g_002FC4B0[2][12] = {       /* 0x002FC4B0 */
    { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 },
    { 0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335 },
};

/* First day of each year of a 4-year group (2000 is a leap year).  .sdata
 * (-G8), after g_003530E8. */
const u16 g_003530F0[4] = { 0, 366, 731, 1096 };   /* 0x003530F0 */

/* Converts t (low word: seconds since 2000-01-01 00:00; high word: the stat
 * byte) into a CdClock and returns its stat.  Callers: clock_update
 * (0x0022BA48), pdicdvd_time_to_clock_gmt / _local.
 * Not exact (81.8%, the same with the reference compiler 991111-01): the
 * register assignment differs from the first division on (retail's seconds
 * in $a2, the 60 / 1461 divisor copies in other registers), and the year
 * loop's `yearInGroup = next' sits at the loop label instead of in a
 * `beql' delay slot (loop.c calls that loop phony).  Tried: the year and
 * month loops as for / while / do / break / goto forms, an explicit `next',
 * int counters, a table pointer, the BCD stores with day / month computed
 * in place or month++ first (worse: 52-74%). */
// FUNC 0x0025B848 pdicdvd_time_to_clock
u8 pdicdvd_time_to_clock(CdClock *out, u64 t)
{
    u32 seconds     = (u32)t;
    u32 days        = seconds / SECONDS_PER_DAY;
    u32 secOfDay    = seconds % SECONDS_PER_DAY;
    u32 minOfDay    = secOfDay / 60;
    u32 second      = secOfDay % 60;
    u32 dayInGroup  = days % DAYS_PER_4_YEARS;      /* day in the 4-year group */
    u32 groups      = days / DAYS_PER_4_YEARS;
    u32 hour        = minOfDay / 60;
    u32 minute      = minOfDay % 60;
    u32 yearInGroup, year, dayOfYear, month, day;
    const u16 *monthStart;

    for (yearInGroup = 0;
         yearInGroup < 3 && dayInGroup >= g_003530F0[yearInGroup + 1];
         yearInGroup++)
        ;
    dayOfYear = dayInGroup - g_003530F0[yearInGroup];
    year = yearInGroup + groups * 4;            /* since 2000 */
    /* Quirk: only year 0 (2000) uses the leap-year month table; 2004, 2008...
     * use the normal one although their group year is 0 as well (movz on the
     * full year). */
    monthStart = (year == 0) ? g_002FC4B0[1] : g_002FC4B0[0];
    for (month = 0; month < 11 && dayOfYear >= monthStart[month + 1]; month++)
        ;
    day = dayOfYear - monthStart[month] + 1;

    out->stat   = (u8)(t >> 32);
    out->second = PDI_BCD(second);
    out->minute = PDI_BCD(minute);
    out->pad    = 0;
    out->hour   = PDI_BCD(hour);
    out->day    = PDI_BCD(day);
    out->month  = PDI_BCD(month + 1);
    out->year   = PDI_BCD(year);
    return out->stat;
}

/* Caller: clock_update (0x0022BA48). */
// FUNC 0x0025BB20 pdicdvd_time_to_clock_gmt
u8 pdicdvd_time_to_clock_gmt(CdClock *out, u64 t)
{
    u8 stat = pdicdvd_time_to_clock(out, t);

    scf_clock_to_gmt((sceCdCLOCK *)out);        /* libscf's type, the same layout */
    return stat;
}

/* Callers: gt2k_menu::RecordsButton::virtual_2 (0x00173D20), clock_update
 * (0x0022BA48). */
// FUNC 0x0025BB60 pdicdvd_time_to_clock_local
u8 pdicdvd_time_to_clock_local(CdClock *out, u64 t)
{
    u8 stat = pdicdvd_time_to_clock(out, t);

    scf_clock_to_local((sceCdCLOCK *)out);      /* libscf's type, the same layout */
    return stat;
}

