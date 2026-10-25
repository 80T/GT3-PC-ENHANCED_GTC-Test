/* T434 (0x00216BB8..0x00216E58): 44 best times of the save data,
 * GameStatus+0x23B08 (? name and meaning: one per entry of the product
 * database's table at +0x1C, found by name with ProductConfig::index1C).  The
 * by-name update / get have no callers; GameStatus only constructs, clears,
 * loads and saves the table.
 *
 * Retail's T434 has no .rodata (no literal, no type name): the original saw
 * no class with a type name. */
#include "game/save/game_options_internal.h"
#include "game/race/core/T128_race_lap_record.h"  /* RACE_TIME_INVALID (0x157529FF: "no time") */
#include "game/data/T435_product_config.h"   /* product_config (index1C) */
#include "game/save/T434_best_times.h"
#include "game/save/pc_save_ext.h"              /* PC: the times past 44, the keyed list */

/* ---- BestTime ------------------------------------------------------------ */

// FUNC 0x00216BB8 BestTime::BestTime
BestTime::BestTime()
{
    clear();
}

// FUNC 0x00216BE0 BestTime::clear
void BestTime::clear()
{
    time = RACE_TIME_INVALID;
}

/* Unsigned compare. */
// FUNC 0x00216BF0 BestTime::update
void BestTime::update(u32 t)
{
    if (time == RACE_TIME_INVALID || t < time)
        time = t;
}

// FUNC 0x00216C18 BestTime::get
u32 BestTime::get()
{
    return time;
}

// FUNC 0x00216C20 BestTime::getSize
int BestTime::getSize()
{
    return 4;
}

// FUNC 0x00216C28 BestTime::load
const u8 *BestTime::load(const u8 *src, int arg)
{
    time = *(const u32 *)src;
    return src + 4;
}

// FUNC 0x00216C38 BestTime::save
u8 *BestTime::save(u8 *dst)
{
    *(u32 *)dst = time;
    return dst + 4;
}

/* ---- CourseBestTimes ----------------------------------------------------- */

// FUNC 0x00216C48 CourseBestTimes::CourseBestTimes
CourseBestTimes::CourseBestTimes()
{
    /* implicit: BestTime (compiler-generated: bases, members, deleting form) */
    /* the 44 element constructors (BestTime::BestTime, a count-down loop),
     * then clear() */
    clear();
}

/* PC (ROADMAP 18): t[] holds the first 44 times (the PS2's layout); the
 * table 0x6 entries past them have theirs on the heap (pc_save_ext, sized
 * from the table at first use).  Every access goes through best_time_at /
 * find. */
#define BEST_TIMES_SAVED 44

/* PC: the times held: table 0x6's entries, at least 44. */
static u32 best_times_capacity(void)
{
    u32 n = product_config() != NULL ? (u32)product_config()->count1C() : 0;

    return n > BEST_TIMES_SAVED ? n : BEST_TIMES_SAVED;
}

/* PC: time i (below the capacity). */
static BestTime *best_time_at(CourseBestTimes *times, u32 i)
{
    u32 need, old, b;
    u8 *ext;

    if (i < BEST_TIMES_SAVED)
        return &times->t[i];
    need = (best_times_capacity() - BEST_TIMES_SAVED) * sizeof(BestTime);
    ext = pc_ext_get(times, PC_EXT_BEST_TIMES, need, &old);
    for (b = old; b < need; b += sizeof(BestTime))
        ((BestTime *)(ext + b))->clear();
    return (BestTime *)ext + (i - BEST_TIMES_SAVED);
}

// FUNC 0x00216CB0 CourseBestTimes::clear
void CourseBestTimes::clear()
{
    int i;

    for (i = 0; i < 44; i++)
        t[i].clear();
    /* PC: the heap times too, and the kept entries of the keyed list */
    for (u32 k = BEST_TIMES_SAVED; k < best_times_capacity(); k++)
        best_time_at(this, k)->clear();
    pc_kept_clear(this, PC_EXT_BEST_TIMES);
}

/* PC: the index range-checked: an entry the database lacks (index1C:
 * garbage, PC (u32)-1) gets a scratch time (none) that takes the reads and
 * drops the writes. */
// FUNC 0x00216CF0 CourseBestTimes::find
BestTime *CourseBestTimes::find(const char *name)
{
    u32 index = product_config()->index1C(name);

    if (index >= best_times_capacity()) {
        static BestTime scratch;
        static int logged = 0;
        if (logged == 0) {
            logged = 1;
            printf("PC: CourseBestTimes: %s is not in product.gcf: not recorded\n", name);
        }
        scratch.clear();
        return &scratch;
    }
    return best_time_at(this, index);
}

/* (No callers.) */
// FUNC 0x00216D38 CourseBestTimes::update
void CourseBestTimes::update(const char *name, u32 time)
{
    find(name)->update(time);
}

/* (No callers.) */
// FUNC 0x00216D68 CourseBestTimes::get
u32 CourseBestTimes::get(const char *name)
{
    return find(name)->get();
}

// FUNC 0x00216D90 CourseBestTimes::getSize
int CourseBestTimes::getSize()
{
    return BestTime::getSize() * 44;
}

/* arg is not passed on: every time gets 0.  (PC: a 7.x save's, by
 * position; the heap times are cleared first.) */
// FUNC 0x00216DB8 CourseBestTimes::load
const u8 *CourseBestTimes::load(const u8 *src, int arg)
{
    int i;

    clear();                                    /* PC: the heap times, the kept entries */
    for (i = 0; i < 44; i++)
        src = t[i].load(src, 0);
    return src;
}

// FUNC 0x00216E08 CourseBestTimes::save
//   (PC: the 7.1 stream's; this build writes saveKeyed.)
u8 *CourseBestTimes::save(u8 *dst)
{
    int i;

    for (i = 0; i < 44; i++)
        dst = t[i].save(dst);
    return dst;
}

/* PC: the 8.0 keyed list: (table 0x6 name, BestTime) of the entries with a
 * time, then the entries kept from the last load. */
void CourseBestTimes::saveKeyed(PcSaveWriter *w)
{
    PcKeyedList list;
    int n = product_config()->count1C();

    pc_keyed_begin(&list, w);
    for (int i = 0; i < n; i++) {
        BestTime *bt = best_time_at(this, i);
        if (bt->time != RACE_TIME_INVALID) {
            const char *name = product_config()->name1C(i);
            pc_keyed_put(&list, name, (u32)strlen(name), &bt->time, PC_REC_BEST_TIME);
        }
    }
    pc_keyed_end(&list, this, PC_EXT_BEST_TIMES);
}

static int best_time_place(void *ctx, const char *key, u32 keyLen, const u8 *rec)
{
    u32 index = product_config()->index1C(key);

    if (index >= best_times_capacity())
        return 0;
    memcpy(&best_time_at((CourseBestTimes *)ctx, index)->time, rec, PC_REC_BEST_TIME);
    return 1;
}

const u8 *CourseBestTimes::loadKeyed(const u8 *src, const u8 *end)
{
    clear();
    return pc_keyed_load(src, end, PC_REC_BEST_TIME, this, PC_EXT_BEST_TIMES, best_time_place, this,
                         "course best times");
}
