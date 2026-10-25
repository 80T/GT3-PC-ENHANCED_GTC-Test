/* T424 (0x00216308..0x00216BB8): the arcade race records of the save data,
 * GameStatus+0x23BB8.  For each difficulty (the second letter of the race
 * code: 'E' easy, 'N' normal, 'H' hard, 'P' professional) and each course of
 * the product database's "SINGLE_RACE" list: the 4 best times and the best
 * place (BestPlace, T418: 0 = won, 8 = never raced).
 *
 * Retail's .rodata (0x003401E8..0x003402E0): "SINGLE_RACE", then the
 * GameStatus's garage type names (GarageWheelList .. Cell<GarageCar>): the
 * original saw the GameStatus and nothing else with a type name. */
#include "game/save/game_options_internal.h"
#include "game/race/core/T128_race_lap_record.h"  /* RACE_TIME_INVALID (0x157529FF: "no time") */
#include "game/data/T435_product_config.h"   /* product_config, the course tables */
#include "game/data/T481_car_data_base.h"    /* nameToCode */
#include "game/save/T429_game_status.h"      /* getGameStatus (updateArcadeProgress); the garage */
#include "game/save/T424_arcade_records.h"
#include "game/save/pc_save_ext.h"              /* PC: the records past 144, the keyed list */

/* ---- ArcadeRecord -------------------------------------------------------- */

// FUNC 0x00216308 ArcadeRecord::ArcadeRecord
ArcadeRecord::ArcadeRecord()
{
    /* the BestPlace member constructor (inline: place = 8), then clear() */
    clear();
}

// FUNC 0x00216338 ArcadeRecord::clear
void ArcadeRecord::clear()
{
    int slot;

    for (slot = 3; slot >= 0; slot--)
        times[slot] = RACE_TIME_INVALID;
    place.clear();
}

/* 1 if p is better than the best place (it is then taken). */
// FUNC 0x00216370 ArcadeRecord::updatePlace
int ArcadeRecord::updatePlace(int p)
{
    return place.update(p);
}

// FUNC 0x00216390 ArcadeRecord::getPlace
int ArcadeRecord::getPlace()
{
    return place.get();
}

/* Inserts time t into the 4 best (unsigned compares; an empty slot or a
 * slower-or-equal time makes room): returns the slot it went to, or -1 if t
 * is slower than all four. */
// FUNC 0x00216398 ArcadeRecord::addTime
int ArcadeRecord::addTime(u32 t)
{
    int rank, slot;

    for (rank = 0; rank < 4; rank++) {
        if (times[rank] == RACE_TIME_INVALID || !(times[rank] < t))
            break;
    }
    if (rank < 4) {
        for (slot = 3; rank < slot; slot--)
            times[slot] = times[slot - 1];
        times[rank] = t;
        return rank;
    }
    return -1;
}

// FUNC 0x00216460 ArcadeRecord::getTime
u32 ArcadeRecord::getTime(int i)
{
    return times[i];
}

// FUNC 0x00216470 ArcadeRecord::getSize
int ArcadeRecord::getSize()
{
    return BestPlace::getSize() + 0x10;         /* + times[4] */
}

// FUNC 0x00216490 ArcadeRecord::load
const u8 *ArcadeRecord::load(const u8 *src, int arg)
{
    u32 n = sizeof(times);                      /* in a variable, egcs calls memcpy */

    memcpy(times, src, n);
    return place.load(src + n, arg);
}

// FUNC 0x002164E0 ArcadeRecord::save
u8 *ArcadeRecord::save(u8 *dst)
{
    u32 n = sizeof(times);

    memcpy(dst, times, n);
    return place.save(dst + n);
}

/* ---- ArcadeRecords ------------------------------------------------------- */

/* PC (ROADMAP 18): rec[] holds the first 144 records (the PS2's layout);
 * with more "SINGLE_RACE" courses (4 x 36 fit), the records past them are
 * on the heap (pc_save_ext, sized from the course list at first use).  The
 * index is difficulty * n + course (n the courses).  Every access goes
 * through arcade_record, range-checked: an unknown difficulty or course
 * (was rec[-1] / the previous difficulty's last record) gets a scratch
 * record (never raced, no times) that takes the reads and drops the
 * writes. */
#define ARCADE_RECORDS_SAVED 144

/* PC: the records held: 4 x the courses, at least 144. */
static int arcade_capacity(void)
{
    int n = product_config() != NULL ? ARCADE_DIFFICULTIES * product_config()->courseCount(ARCADE_COURSE_LIST) : 0;

    return n > ARCADE_RECORDS_SAVED ? n : ARCADE_RECORDS_SAVED;
}

static ArcadeRecord *arcade_record(ArcadeRecords *records, int index)
{
    if (index < 0 || index >= arcade_capacity()) {
        static ArcadeRecord scratch;
        static int logged = 0;
        if (logged == 0) {
            logged = 1;
            printf("PC: ArcadeRecords: record %d is no SINGLE_RACE course / difficulty: not recorded\n", index);
        }
        scratch.clear();
        return &scratch;
    }
    if (index < ARCADE_RECORDS_SAVED)
        return &records->rec[index];
    u32 old, b;
    u32 need = (u32)(arcade_capacity() - ARCADE_RECORDS_SAVED) * sizeof(ArcadeRecord);
    u8 *ext = pc_ext_get(records, PC_EXT_ARCADE, need, &old);
    for (b = old; b < need; b += sizeof(ArcadeRecord))
        ((ArcadeRecord *)(ext + b))->clear();
    return (ArcadeRecord *)ext + (index - ARCADE_RECORDS_SAVED);
}

// FUNC 0x00216528 ArcadeRecords::ArcadeRecords
ArcadeRecords::ArcadeRecords()
{
    /* implicit: ArcadeRecord (compiler-generated: bases, members, deleting form) */
    /* the 144 element constructors (ArcadeRecord::ArcadeRecord, a
     * count-down loop), then clear() */
    clear();
}

// FUNC 0x00216590 ArcadeRecords::clear
void ArcadeRecords::clear()
{
    int i;

    for (i = 0; i < 144; i++)
        rec[i].clear();
    /* PC: the heap records too, and the kept entries of the keyed list */
    for (int k = ARCADE_RECORDS_SAVED; k < arcade_capacity(); k++)
        arcade_record(this, k)->clear();
    pc_kept_clear(this, PC_EXT_ARCADE);
}

/* The difficulty of an arcade race code (its second character). */
// FUNC 0x002165D0 ArcadeRecords::difficultyOf
u32 ArcadeRecords::difficultyOf(const char *race)
{
    switch (race[1]) {
    case 'E': return ARCADE_EASY;
    case 'N': return ARCADE_NORMAL;
    case 'H': return ARCADE_HARD;
    case 'P': return ARCADE_PROFESSIONAL;
    default:  return ARCADE_DIFFICULTIES;
    }
}

/* The record of course `course' (a nameToCode code) at the difficulty of
 * `race'.  Quirks kept: an unknown difficulty gives rec[-1], an unknown
 * course rec[difficulty * n - 1].  PC: both give the scratch record (the
 * second wrote into the previous difficulty's last course). */
// FUNC 0x00216640 ArcadeRecords::find
ArcadeRecord *ArcadeRecords::find(const char *race, s64 course)
{
    u32 difficulty = difficultyOf(race);
    int index = -1;

    if (difficulty < ARCADE_DIFFICULTIES) {
        int found = -1;
        const char *list = ARCADE_COURSE_LIST;  /* one register for both calls, as retail */
        int i = 0;
        int courses = product_config()->courseCount(list);

        for (; i < courses; i++) {
            if ((s64)nameToCode(product_config()->courseName(i, list)) == course) {
                found = i;
                break;
            }
        }
        index = difficulty * courses + found;
        if (found < 0)
            index = -1;                         /* PC: the scratch record */
    }
    return arcade_record(this, index);          /* PC: range-checked */
}

/* (No callers.) */
// FUNC 0x00216720 ArcadeRecords::at
ArcadeRecord *ArcadeRecords::at(int difficulty, int course)
{
    /* PC: range-checked */
    return arcade_record(this, difficulty * product_config()->courseCount(ARCADE_COURSE_LIST) + course);
}

/* Records place p; if it is a new best, the GameStatus updates the arcade
 * progress. */
// FUNC 0x00216780 ArcadeRecords::updatePlace
void ArcadeRecords::updatePlace(const char *race, s64 course, int p)
{
    if (find(race, course)->updatePlace(p) != 0)
        getGameStatus()->updateArcadeProgress();
}

// FUNC 0x002167C8 ArcadeRecords::getPlace
int ArcadeRecords::getPlace(const char *race, s64 course)
{
    return find(race, course)->getPlace();
}

/* (No callers.) */
// FUNC 0x002167F0 ArcadeRecords::addTime
int ArcadeRecords::addTime(const char *race, s64 course, u32 t)
{
    return find(race, course)->addTime(t);
}

/* (No callers.) */
// FUNC 0x00216820 ArcadeRecords::getTime
u32 ArcadeRecords::getTime(const char *race, s64 course, int i)
{
    return find(race, course)->getTime(i);
}

/* 1 if the first `count' courses (all of them if count < 0) have been won
 * at `difficulty'.  (A result variable and `break': retail's loop is entered
 * by a jump to its test and returns the variable's register.) */
// FUNC 0x00216850 ArcadeRecords::allWon
int ArcadeRecords::allWon(int difficulty, int count)
{
    int result = 1;
    int i = 0;
    int courses = product_config()->courseCount(ARCADE_COURSE_LIST);
    int first = courses * difficulty;

    if (!(-1 < count))
        count = courses;
    for (; i < count; i++) {
        if (arcade_record(this, first + i)->getPlace() != PLACE_WON) {     /* PC: range-checked */
            result = 0;
            break;
        }
    }
    return result;
}

// FUNC 0x00216900 ArcadeRecords::allWonEasy
int ArcadeRecords::allWonEasy()
{
    return allWon(ARCADE_EASY, -1);
}

// FUNC 0x00216920 ArcadeRecords::allWonNormal
int ArcadeRecords::allWonNormal()
{
    return allWon(ARCADE_NORMAL, -1);
}

// FUNC 0x00216940 ArcadeRecords::allWonHard
int ArcadeRecords::allWonHard()
{
    return allWon(ARCADE_HARD, -1);
}

// FUNC 0x00216960 ArcadeRecords::allWonProfessional
int ArcadeRecords::allWonProfessional()
{
    return allWon(ARCADE_PROFESSIONAL, -1);
}

/* The arcade completion score: per course 3 if won at hard, else 2 if won
 * at normal, else 1 if won at easy (professional does not count).  Called by
 * GameStatus::getCompletionRatio.  (Each loop indexes rec[] itself: retail
 * computes its pointers per loop.  PC: through arcade_record, range-checked.) */
// FUNC 0x00216980 ArcadeRecords::score
int ArcadeRecords::score()
{
    int s = 0;
    int courses = product_config()->courseCount(ARCADE_COURSE_LIST);
    int i;

    for (i = 0; i < courses; i++) {
        if (arcade_record(this, i + ARCADE_EASY * courses)->getPlace() == PLACE_WON ||
            arcade_record(this, i + ARCADE_NORMAL * courses)->getPlace() == PLACE_WON ||
            arcade_record(this, i + ARCADE_HARD * courses)->getPlace() == PLACE_WON)
            s++;
    }
    for (i = 0; i < courses; i++) {
        if (arcade_record(this, i + ARCADE_NORMAL * courses)->getPlace() == PLACE_WON ||
            arcade_record(this, i + ARCADE_HARD * courses)->getPlace() == PLACE_WON)
            s++;
    }
    for (i = 0; i < courses; i++) {
        if (arcade_record(this, i + ARCADE_HARD * courses)->getPlace() == PLACE_WON)
            s++;
    }
    return s;
}

/* The maximum of score(): a constant (34 courses x 3).  PC: the
 * "SINGLE_RACE" courses x 3 (102 with retail's 34), so another course
 * count can still reach 100 %. */
// FUNC 0x00216AE8 ArcadeRecords::maxScore
int ArcadeRecords::maxScore()
{
    return 3 * product_config()->courseCount(ARCADE_COURSE_LIST);     /* PC: was 102 */
}

// FUNC 0x00216AF0 ArcadeRecords::getSize
int ArcadeRecords::getSize()
{
    return ArcadeRecord::getSize() * 144;
}

/* arg is not passed on: every record gets 0.  (PC: a 7.x save's, by
 * position; the heap records are cleared first.) */
// FUNC 0x00216B18 ArcadeRecords::load
const u8 *ArcadeRecords::load(const u8 *src, int arg)
{
    int i;

    clear();                                    /* PC: the heap records, the kept entries */
    for (i = 0; i < 144; i++)
        src = rec[i].load(src, 0);
    return src;
}

/* PC: the 8.0 keyed list: (SINGLE_RACE course name; the difficulty, then
 * the ArcadeRecord) of the records raced or timed, then the entries kept
 * from the last load. */
void ArcadeRecords::saveKeyed(PcSaveWriter *w)
{
    PcKeyedList list;
    const char *courseList = ARCADE_COURSE_LIST;
    int courses = product_config()->courseCount(courseList);
    u8 recBytes[PC_REC_ARCADE];

    pc_keyed_begin(&list, w);
    for (int c = 0; c < courses; c++) {
        for (int d = 0; d < ARCADE_DIFFICULTIES; d++) {
            ArcadeRecord *r = arcade_record(this, d * courses + c);
            if (r->getPlace() == PLACE_NONE && r->times[0] == RACE_TIME_INVALID)
                continue;
            const char *name = product_config()->courseName(c, courseList);
            recBytes[0] = (u8)d;
            memcpy(recBytes + 1, r, sizeof(ArcadeRecord));
            pc_keyed_put(&list, name, (u32)strlen(name), recBytes, PC_REC_ARCADE);
        }
    }
    pc_keyed_end(&list, this, PC_EXT_ARCADE);
}

static int arcade_record_place(void *ctx, const char *key, u32 keyLen, const u8 *rec)
{
    int c = product_config()->courseIndex(key, ARCADE_COURSE_LIST);

    if (c < 0 || rec[0] >= ARCADE_DIFFICULTIES)
        return 0;
    int courses = product_config()->courseCount(ARCADE_COURSE_LIST);
    memcpy(arcade_record((ArcadeRecords *)ctx, rec[0] * courses + c), rec + 1, sizeof(ArcadeRecord));
    return 1;
}

const u8 *ArcadeRecords::loadKeyed(const u8 *src, const u8 *end)
{
    clear();
    return pc_keyed_load(src, end, PC_REC_ARCADE, this, PC_EXT_ARCADE, arcade_record_place, this,
                         "arcade records");
}

// FUNC 0x00216B70 ArcadeRecords::save
u8 *ArcadeRecords::save(u8 *dst)
{
    int i;

    for (i = 0; i < 144; i++)
        dst = rec[i].save(dst);
    return dst;
}
