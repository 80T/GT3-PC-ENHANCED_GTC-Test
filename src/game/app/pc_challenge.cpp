/* pc_challenge.cpp - the challenge modes (pc_challenge.h).  No original: new
 * PC code. */
#include "game/app/pc_challenge.h"
#include "game/app/app_internal.h"              /* MAIN_MODE_* */
#include "game/save/T429_game_status.h"          /* gt2k_game::GameStatus, getGameStatus */
#include "game/save/pc_save_ext.h"               /* PcSaveWriter, pc_put_u32, PC_EXT_CHALLENGE */
#include "game/data/T516_country.h"              /* g_00352FD8, COUNTRY_JP */
#include "platform/challenge_osd.h"
#include "platform/settings.h"

#include <stdio.h>
#include <string.h>

#include <atomic>
#include <unordered_map>

namespace {

/* Money decay: days 2 .. 9 (day - 1) x 50, from day 10 (day - 9) x 500, at
 * most kMoneyCap a day; a licence earned: the next kPauseDays free. */
constexpr int kMoneyRampEnd = 9;
constexpr s64 kMoneyRamp = 50, kMoneyStep = 500, kMoneyCap = 200000;
constexpr int kPauseDays = 10;
/* Car decay: 0.25 % (25 hundredths) a day, at least 5 %. */
constexpr int kPowerFull = 10000, kPowerStep = 25, kPowerMin = 500;
/* Course shrinkage: 0.2 % (2 tenths) a day, at least 25 %. */
constexpr int kCourseFull = 1000, kCourseStep = 2, kCourseMin = 250;

std::atomic<int> g_scene{PC_SCENE_OTHER};

/* The money decay's pauses: the last free day of each GameStatus (the
 * game's, and a guest garage loaded for the trade).  The game's thread. */
std::unordered_map<const gt2k_game::GameStatus *, int> g_pausedThrough;

/* "1,234,567" */
void with_commas(char *out, size_t n, s64 v)
{
    char digits[32];
    int len = 0;
    unsigned long long u = v < 0 ? (unsigned long long)(-v) : (unsigned long long)v;
    do {
        digits[len++] = (char)('0' + (int)(u % 10));
        u /= 10;
    } while (u != 0 && len < (int)sizeof(digits));
    size_t at = 0;
    if (v < 0 && at + 1 < n)
        out[at++] = '-';
    for (int i = len - 1; i >= 0 && at + 1 < n; i--) {
        out[at++] = digits[i];
        if (i != 0 && i % 3 == 0 && at + 1 < n)
            out[at++] = ',';
    }
    out[at < n ? at : n - 1] = '\0';
}

/* hundredths -> "74.50%", tenths -> "79.6%" */
void hundredths_text(char *out, size_t n, int v)
{
    snprintf(out, n, "%d.%02d%%", v / 100, v % 100);
}
void tenths_text(char *out, size_t n, int v)
{
    snprintf(out, n, "%d.%d%%", v / 10, v % 10);
}

bool is_game(const gt2k_game::GameStatus *gs)
{
    return gs != NULL && gs == getGameStatus();
}

} // namespace

s64 pc_challenge_money_decay(int day)
{
    if (day < 2)
        return 0;
    const s64 d = day <= kMoneyRampEnd ? (s64)(day - 1) * kMoneyRamp : (s64)(day - kMoneyRampEnd) * kMoneyStep;
    return d < kMoneyCap ? d : kMoneyCap;
}

int pc_challenge_power(int day)
{
    const int passed = day > 1 ? day - 1 : 0;
    const int v = passed >= (kPowerFull - kPowerMin) / kPowerStep ? kPowerMin : kPowerFull - passed * kPowerStep;
    return v;
}

int pc_challenge_course(int day)
{
    const int passed = day > 1 ? day - 1 : 0;
    return passed >= (kCourseFull - kCourseMin) / kCourseStep ? kCourseMin : kCourseFull - passed * kCourseStep;
}

s64 pc_challenge_money_scale(void)
{
    return g_00352FD8.value == COUNTRY_JP ? 100 : 1;
}

void pc_challenge_scene(int scene)
{
    g_scene = scene;
    challenge_osd::set_shown(scene != PC_SCENE_OTHER);
    if (scene != PC_SCENE_OTHER)
        pc_challenge_refresh(getGameStatus());
}

int pc_challenge_scene_of_mode(int mode)
{
    switch (mode) {
    case MAIN_MODE_LICENSE_TEST:
        return PC_SCENE_GT_LICENSE;             /* PC (2026-11-15): no car decay (pc_challenge.h) */
    case MAIN_MODE_CHAMPIONSHIP:
    case MAIN_MODE_GTMODE_RACE:
    case MAIN_MODE_GTMODE_RACE2:
    case MAIN_MODE_MACHINE_TEST:
    case MAIN_MODE_FREE_RUN:
        return PC_SCENE_GT_RACE;
    case MAIN_MODE_ARCADE:
    case MAIN_MODE_CARD_REPLAY:
    case MAIN_MODE_DEMO_REPLAY:
    case MAIN_MODE_LOGGER:
        return PC_SCENE_OTHER;
    default:                                    /* the race previews, options, memory card screens */
        return PC_SCENE_GT_MENUS;
    }
}

PcChallengeRace::PcChallengeRace() : before(g_scene.load())
{
    pc_challenge_scene(PC_SCENE_GT_RACE);
}

PcChallengeRace::~PcChallengeRace()
{
    pc_challenge_scene(before);
}

void pc_challenge_day_passed(void)
{
    gt2k_game::GameStatus *gs = getGameStatus();
    if (gs == NULL)
        return;
    const int day = gs->getDay();
    if (settings::challenge(settings::Challenge::moneyDecay)) {
        const int through = pc_challenge_paused_through(gs);
        if (day <= through) {
            printf("PC: challenge: money decay: day %d free (a licence's pause, through day %d)\n", day, through);
        } else {
            const s64 decay = pc_challenge_money_decay(day) * pc_challenge_money_scale();
            const s64 before = gs->getMoney();
            const s64 after = before <= 0 ? before : (before > decay ? before - decay : 0);
            gs->setMoney(after);
            char d[32], b[32], a[32];
            with_commas(d, sizeof(d), decay);
            with_commas(b, sizeof(b), before);
            with_commas(a, sizeof(a), after);
            printf("PC: challenge: money decay: day %d takes %s (%s -> %s)\n", day, d, b, a);
        }
    }
    pc_challenge_refresh(gs);
}

void pc_challenge_licence_earned(int licence)
{
    gt2k_game::GameStatus *gs = getGameStatus();
    if (gs == NULL)
        return;
    const int day = gs->getDay();
    g_pausedThrough[gs] = day + kPauseDays;     /* (again from today: never more than 10 days) */
    printf("PC: challenge: licence %d earned on day %d: no money decay through day %d%s\n", licence, day,
           day + kPauseDays, settings::challenge(settings::Challenge::moneyDecay) ? "" : " (money decay off)");
    pc_challenge_refresh(gs);
}

int pc_challenge_race_power(void)
{
    if (!settings::challenge(settings::Challenge::carDecay))
        return 0;
    if (g_scene.load() == PC_SCENE_GT_LICENSE) {
        /* PC (2026-11-15): not in a licence test (pc_challenge.h) */
        printf("PC: challenge: car decay: none in a licence test\n");
        return 0;
    }
    if (g_scene.load() != PC_SCENE_GT_RACE)
        return 0;
    const gt2k_game::GameStatus *gs = getGameStatus();
    return gs != NULL ? pc_challenge_power(gs->getDay()) : 0;
}

int pc_challenge_race_course(void)
{
    const int scene = g_scene.load();
    if ((scene != PC_SCENE_GT_RACE && scene != PC_SCENE_GT_LICENSE) ||  /* (PC 2026-11-15: licence tests too) */
        !settings::challenge(settings::Challenge::courseShrinkage))
        return 0;
    const gt2k_game::GameStatus *gs = getGameStatus();
    return gs != NULL ? pc_challenge_course(gs->getDay()) : 0;
}

void pc_challenge_forget(const gt2k_game::GameStatus *gs)
{
    g_pausedThrough.erase(gs);
}

int pc_challenge_paused_through(const gt2k_game::GameStatus *gs)
{
    auto it = g_pausedThrough.find(gs);
    return it != g_pausedThrough.end() ? it->second : 0;
}

/* The section: u32 PC_EXT_CHALLENGE, u32 4, s32 the pause's last free day;
 * only while a pause is ahead (after the day saved). */
void pc_challenge_save_section(const gt2k_game::GameStatus *gs, PcSaveWriter *w)
{
    const int through = pc_challenge_paused_through(gs);
    if (through <= gs->getDay())
        return;
    pc_put_u32(w, PC_EXT_CHALLENGE);
    pc_put_u32(w, 4);
    pc_put_u32(w, (u32)through);
}

void pc_challenge_load_section(const gt2k_game::GameStatus *gs, const u8 *src, u32 bytes)
{
    if (bytes < 4)
        return;
    s32 through;
    memcpy(&through, src, 4);
    const int day = gs->getDay();
    if (through <= day)
        return;
    if (through > day + kPauseDays)
        through = day + kPauseDays;             /* (not a value a licence gives) */
    g_pausedThrough[gs] = through;
    printf("PC: challenge: the save's licence pause: no money decay through day %d\n", (int)through);
}

void pc_challenge_refresh(const gt2k_game::GameStatus *gs)
{
    if (!is_game(gs))
        return;
    const int day = gs->getDay();
    char title[48];
    snprintf(title, sizeof(title), "CHALLENGE MODES - DAY %d", day);
    challenge_osd::set_title(title);

    /* money decay: the next day's take, the pause's days left */
    {
        char line[2][64], amount[32];
        const int through = pc_challenge_paused_through(gs);
        const int left = through > day ? through - day : 0;
        if (left > 0) {
            snprintf(line[0], sizeof(line[0]), "NEXT DAY: PAUSED");
            snprintf(line[1], sizeof(line[1]), "LICENSE PAUSE: %d DAY%s LEFT", left, left == 1 ? "" : "S");
        } else {
            const s64 next = pc_challenge_money_decay(day + 1);
            with_commas(amount, sizeof(amount), next * pc_challenge_money_scale());
            snprintf(line[0], sizeof(line[0]), "NEXT DAY: -%s %s%s", amount,
                     pc_challenge_money_scale() == 100 ? "YEN" : "CR.", next >= kMoneyCap ? " (MAX)" : "");
            line[1][0] = '\0';
        }
        const char *lines[challenge_osd::kStatusLines] = {line[0], line[1]};
        challenge_osd::set_status((int)settings::Challenge::moneyDecay, lines);
    }
    /* car decay: today's power, the next day's */
    {
        char line[2][64], now[16], next[16];
        hundredths_text(now, sizeof(now), pc_challenge_power(day));
        hundredths_text(next, sizeof(next), pc_challenge_power(day + 1));
        if (g_scene.load() == PC_SCENE_GT_LICENSE)
            snprintf(line[0], sizeof(line[0]), "LICENSE TEST: NO DECAY");   /* PC (2026-11-15) */
        else
            snprintf(line[0], sizeof(line[0]), "POWER: %s%s", now, pc_challenge_power(day) == kPowerMin ? " (MIN)" : "");
        snprintf(line[1], sizeof(line[1]), "NEXT DAY: %s", next);
        const char *lines[challenge_osd::kStatusLines] = {line[0], line[1]};
        challenge_osd::set_status((int)settings::Challenge::carDecay, lines);
    }
    /* course shrinkage: today's size, the next day's */
    {
        char line[2][64], now[16], next[16];
        tenths_text(now, sizeof(now), pc_challenge_course(day));
        tenths_text(next, sizeof(next), pc_challenge_course(day + 1));
        snprintf(line[0], sizeof(line[0]), "COURSE SIZE: %s%s", now,
                 pc_challenge_course(day) == kCourseMin ? " (MIN)" : "");
        snprintf(line[1], sizeof(line[1]), "NEXT DAY: %s", next);
        const char *lines[challenge_osd::kStatusLines] = {line[0], line[1]};
        challenge_osd::set_status((int)settings::Challenge::courseShrinkage, lines);
    }
}
