/* T420 (0x00215048..0x00215D00): the licence records of the save data
 * (GameStatus+0x1F550): 6 licences x 8 tests, a medal and 6 places each. */
#include "game/save/prize_records_internal.h"
#include "game/save/T429_garage_car_list.h"      /* (seen: GarageCarList, List<GarageCar>, GarageCar,
                                               Cell<GarageCar>: the type names after the dashes
                                               0x00340178..0x003401E8; no wheel list) */
#include "game/save/T420_license_records.h"

/* LICENSE_PLACES, LICENSE_TESTS, LICENSE_COUNT: T420. */

// FUNC 0x00215048 LicenseTest::clear
void LicenseTest::clear()
{
    medal = LICENSE_MEDAL_NONE;
    for (int i = 0; i < LICENSE_PLACES; i++) {
        names[i][0] = 0;
        times[i] = RACE_TIME_INVALID;
        dates[i] = 0;
        strcpy(codes[i].s, RANKING_CODE_EMPTY);  /* expanded to a 31-byte copy */
    }
}

// FUNC 0x002150F8 LicenseTest::getMedal
int LicenseTest::getMedal()
{
    return medal;
}

// FUNC 0x00215100 LicenseTest::setMedal
void LicenseTest::setMedal(int newMedal)
{
    if (medal == LICENSE_MEDAL_NONE || newMedal < medal)
        medal = newMedal;
}

// FUNC 0x00215128 LicenseTest::insertTime
int LicenseTest::insertTime(u32 time)
{
    int place;
    for (place = 0; place < LICENSE_PLACES; place++) {
        if (times[place] == RACE_TIME_INVALID || !(times[place] < time))
            break;
    }
    if (place < LICENSE_PLACES) {
        for (int i = LICENSE_PLACES - 1; place < i; i--)
            times[i] = times[i - 1];
        times[place] = time;
        return place;
    }
    return -1;
}

// FUNC 0x002151F0 LicenseTest::getTime
u32 LicenseTest::getTime(int i)
{
    return times[i];
}

// FUNC 0x00215200 LicenseTest::insertName
void LicenseTest::insertName(const char *name, int place)
{
    for (int i = LICENSE_PLACES - 1; place < i; i--)
        strcpy(names[i], names[i - 1]);
    int len = strlen(name);
    /* (sic) len bytes: strcpy writes len + 1 (in retail the extra byte
     * lands in the 16-byte rounding or the frame's outgoing-argument
     * area; the GCC 14 build allocates it).  alloca, not char[len]: a
     * VLA's size goes through bits (len * 8 / 8: sll 3; srl 3), retail
     * rounds len itself */
    char *shortName = (char *)__builtin_alloca(len + 1);
    strcpy(shortName, name);
    if (len >= 0x10)
        shortName[0xF] = 0;
    strcpy(names[place], shortName);
    insertDate(place);
}

// FUNC 0x002152F0 LicenseTest::getName
char *LicenseTest::getName(int i)
{
    return names[i];
}

// FUNC 0x00215300 LicenseTest::insertDate
void LicenseTest::insertDate(int place)
{
    for (int i = LICENSE_PLACES - 1; place < i; i--)
        dates[i] = dates[i - 1];
    dates[place] = clock_seconds();             /* 0x0022BB08 (F_0022BB08) */
}

// FUNC 0x00215388 LicenseTest::insertCode
void LicenseTest::insertCode(const RankingCode *code, int place)
{
    for (int i = LICENSE_PLACES - 1; place < i; i--)
        codes[i] = codes[i - 1];
    codes[place] = *code;
}

// FUNC 0x00215448 LicenseTest::getCode
RankingCode *LicenseTest::getCode(int i)
{
    return &codes[i];
}

// FUNC 0x00215458 LicenseTest::getDate
u32 LicenseTest::getDate(int i)
{
    return dates[i];
}

/* Unlike TimeTrialCourse::remove the last ranking code is left as it is. */
// FUNC 0x00215468 LicenseTest::remove
void LicenseTest::remove(int i)
{
    for (; i < LICENSE_PLACES - 1; i++) {
        times[i] = times[i + 1];
        strcpy(names[i], names[i + 1]);
        dates[i] = dates[i + 1];
        codes[i] = codes[i + 1];
    }
    dates[LICENSE_PLACES - 1] = 0;
    names[LICENSE_PLACES - 1][0] = 0;
    times[LICENSE_PLACES - 1] = RACE_TIME_INVALID;
}

// FUNC 0x00215580 LicenseTest::getSize
int LicenseTest::getSize()
{
    return 0x154;
}

// FUNC 0x00215588 LicenseTest::load
const u8 *LicenseTest::load(const u8 *src, int arg)
{
    u32 size = 0x154;                           /* a variable: retail calls memcpy */

    memcpy(this, src, size);
    return src + size;
}

// FUNC 0x002155B8 LicenseTest::save
u8 *LicenseTest::save(u8 *dst)
{
    u32 size = 0x154;

    memcpy(dst, this, size);
    return dst + size;
}

/* PC: 1 if licence `lic' is one of the save's 6 (else logged once). */
static int license_saved(int lic)
{
    static int logged = 0;

    if ((u32)lic < LICENSE_COUNT)
        return 1;
    if (logged == 0) {
        logged = 1;
        printf("PC: LicenseRecords: licence %d outside the save's %d: not recorded\n", lic, LICENSE_COUNT);
    }
    return 0;
}

/* PC: tests[lic][test] range-checked.  lic / test come from the race code
 * (race_name_split; lic -1 for an unknown kind): a test past the save's 8
 * per licence (a transplanted race) used tests[] out of bounds, into
 * EventResults and the save.  Past it: a scratch test (no medal, empty
 * ranking) that takes the reads and drops the writes. */
static LicenseTest *license_test(LicenseRecords *records, int lic, int test)
{
    if (license_saved(lic) && (u32)test < LICENSE_TESTS)
        return &records->tests[lic][test];
    static LicenseTest scratch;
    static int logged = 0;
    if (logged == 0) {
        logged = 1;
        printf("PC: LicenseRecords: test %d of licence %d outside the save's %d x %d: not recorded\n",
               test, lic, LICENSE_COUNT, LICENSE_TESTS);
    }
    scratch.clear();
    return &scratch;
}

// FUNC 0x002155F0 LicenseRecords::LicenseRecords
LicenseRecords::LicenseRecords()
{
    reset();
}

/* (sic) clears 7 licences: the 7th (0xAA0 bytes) is past the object, the
 * next members of GameStatus (EventResults at +0x23510, CourseBestTimes,
 * the start of ArcadeRecords); GameStatus::init clears them right after. */
// FUNC 0x00215618 LicenseRecords::reset
void LicenseRecords::reset()
{
    for (int lic = 0; lic < LICENSE_COUNT + 1; lic++) {
        LicenseTest *test = (LicenseTest *)((u8 *)this + lic * (LICENSE_TESTS * sizeof(LicenseTest)));
        for (int i = 0; i < LICENSE_TESTS; i++, test++)
            test->clear();
    }
}

// FUNC 0x002156A0 LicenseRecords::getMedal
int LicenseRecords::getMedal(int lic, int test)
{
    return license_test(this, lic, test)->getMedal();      /* PC: range-checked */
}

// FUNC 0x002156D0 LicenseRecords::setMedal
void LicenseRecords::setMedal(int lic, int test, int medal)
{
    license_test(this, lic, test)->setMedal(medal);        /* PC: range-checked */
}

/* first7: only the first 7 tests count (check_go_license, T045: the 8th,
 * the licence's last exam, needs the other 7 passed). */
// FUNC 0x00215708 LicenseRecords::isPassed
int LicenseRecords::isPassed(int lic, int first7)
{
    if (!license_saved(lic))                    /* PC: lic range-checked (-1: an unknown kind) */
        return 0;
    for (int i = 0; first7 ? i < LICENSE_TESTS - 1 : i < LICENSE_TESTS; i++) {
        if (tests[lic][i].getMedal() == LICENSE_MEDAL_NONE)
            return 0;
    }
    return 1;
}

// FUNC 0x00215790 LicenseRecords::isAllGold
int LicenseRecords::isAllGold(int lic)
{
    if (!license_saved(lic))                    /* PC: range-checked */
        return 0;
    LicenseTest *test = tests[lic];
    for (int i = 0; i < LICENSE_TESTS; i++, test++) {
        if (test->getMedal() != LICENSE_MEDAL_GOLD)
            return 0;
    }
    return 1;
}

/* The highest of the licences B .. S (0..4) passed; R is not looked at. */
// FUNC 0x002157E8 LicenseRecords::highestPassed
int LicenseRecords::highestPassed()
{
    for (int lic = 4; lic >= 0; lic--) {
        if (isPassed(lic, 0))
            return lic;
    }
    return -1;
}

/* No caller. */
// FUNC 0x00215840 LicenseRecords::passAll
void LicenseRecords::passAll(int lic)
{
    if (!license_saved(lic))                    /* PC: range-checked */
        return;
    LicenseTest *test = tests[lic];
    for (int i = 0; i < LICENSE_TESTS; i++, test++)
        test->setMedal(LICENSE_MEDAL_BRONZE);
}

// FUNC 0x00215890 LicenseRecords::countMedal
int LicenseRecords::countMedal(int medal)
{
    int count = 0;
    for (int lic = 0; lic < LICENSE_COUNT; lic++) {
        LicenseTest *test = tests[lic];
        for (int i = 0; i < LICENSE_TESTS; i++, test++) {
            if (test->getMedal() == medal)
                count++;
        }
    }
    return count;
}

/* (PC: the eight below through license_test, range-checked.) */
// FUNC 0x00215940 LicenseRecords::insertTime
int LicenseRecords::insertTime(int lic, int test, u32 time)
{
    return license_test(this, lic, test)->insertTime(time);
}

// FUNC 0x00215978 LicenseRecords::getTime
u32 LicenseRecords::getTime(int lic, int test, int i)
{
    return license_test(this, lic, test)->getTime(i);
}

// FUNC 0x002159B0 LicenseRecords::insertName
void LicenseRecords::insertName(int lic, int test, const char *name, int place)
{
    license_test(this, lic, test)->insertName(name, place);
}

// FUNC 0x002159E8 LicenseRecords::getName
char *LicenseRecords::getName(int lic, int test, int i)
{
    return license_test(this, lic, test)->getName(i);
}

// FUNC 0x00215A20 LicenseRecords::insertCode
void LicenseRecords::insertCode(int lic, int test, const RankingCode *code, int place)
{
    license_test(this, lic, test)->insertCode(code, place);
}

// FUNC 0x00215A58 LicenseRecords::getCode
RankingCode *LicenseRecords::getCode(int lic, int test, int i)
{
    return license_test(this, lic, test)->getCode(i);
}

// FUNC 0x00215A90 LicenseRecords::getDate
u32 LicenseRecords::getDate(int lic, int test, int i)
{
    return license_test(this, lic, test)->getDate(i);
}

// FUNC 0x00215AC8 LicenseRecords::remove
void LicenseRecords::remove(int lic, int test, int i)
{
    license_test(this, lic, test)->remove(i);
}

// FUNC 0x00215B00 LicenseRecords::countPassed
int LicenseRecords::countPassed()
{
    int count = 0;
    for (int lic = 0; lic < LICENSE_COUNT; lic++) {
        LicenseTest *test = tests[lic];
        for (int i = 0; i < LICENSE_TESTS; i++, test++) {
            if (test->getMedal() != LICENSE_MEDAL_NONE)
                count++;
        }
    }
    return count;
}

// FUNC 0x00215BA0 LicenseRecords::numTests
int LicenseRecords::numTests()
{
    return LICENSE_COUNT * LICENSE_TESTS;            /* 0x30 */
}

// FUNC 0x00215BA8 LicenseRecords::getSize
int LicenseRecords::getSize()
{
    return LicenseTest::getSize() * (LICENSE_COUNT * LICENSE_TESTS);
}

// FUNC 0x00215BD0 LicenseRecords::load
const u8 *LicenseRecords::load(const u8 *src, int arg)
{
    for (int lic = 0; lic < LICENSE_COUNT; lic++) {
        for (int i = 0; i < LICENSE_TESTS; i++)
            src = tests[lic][i].load(src, arg);
    }
    return src;
}

// FUNC 0x00215C70 LicenseRecords::save
u8 *LicenseRecords::save(u8 *dst)
{
    for (int lic = 0; lic < LICENSE_COUNT; lic++) {
        for (int i = 0; i < LICENSE_TESTS; i++)
            dst = tests[lic][i].save(dst);
    }
    return dst;
}
