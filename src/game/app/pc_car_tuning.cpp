/* pc_car_tuning.cpp - the car multipliers (pc_car_tuning.h).  No original:
 * new PC code. */
#include "game/app/pc_car_tuning.h"
#include "game/car/physics/T302_car_state.h"            /* RaceCarState, DynCarState (DRIVER_COMPUTER) */
#include "game/race/core/T124_race_information.h"       /* RaceInformation */
#include "game/race/core/T129_race_base.h"              /* RaceInfoBlock */
#include "game/race/core/T149_race_replay_information.h"    /* RaceReplayInformation */
#include "game/race/modes/T135_race_arcade_demo.h"      /* RaceArcadeDemoInformation */
#include "platform/settings.h"
#include "game/app/pc_challenge.h"              /* PC (2026-11-14): Car Decay's power */

#include <stdio.h>
#include <string.h>

#include <unordered_map>

namespace {

/* A race's car multipliers, percent: [what][0 the players', 1 the
 * enemies']. */
struct Tuning {
    u16 pct[3][2];
};

/* A race's surface values, the raw values in hundredths, -1
 * (settings::kSurfaceOwn) the game's own: [surface code][0 grip, 1
 * resistance]. */
struct SurfaceTuning {
    s16 raw[16][2];
};

/* A race's both (the car part all 100, the surface part all own, when not
 * set). */
struct RaceTuning {
    Tuning car;
    SurfaceTuning surf;
    /* PC (2026-11-14): the challenge Car Decay's power for the players' cars,
     * hundredths of a percent, in place of car.pct[PC_TUNE_POWER][0] (100
     * then); 0 none */
    u16 powerC;
};

/* The races with any multiplier not 100 (bound by race_organization_create
 * for every organization: it sets or clears the entry). */
std::unordered_map<const RaceOrganization *, RaceTuning> g_tunings;

/* pc_car_tuning's last organization and its entry (NULL: none), so the
 * physics' per-wheel calls do not hash; reset with every change. */
const RaceOrganization *g_lastOrg;
const RaceTuning *g_lastTuning;

/* The race whose cars are being built (pc_surface_build_begin; NULL:
 * none) and its entry. */
const RaceTuning *g_building;

/* A RaceInfoBlock's in its spare bytes (unk_080, after the course scale's
 * 10 bytes and the laps' / time's 8 each), each part when set: "TUNE" then
 * the six car percents (u16: tyre wear, power, grip; players', enemies'
 * each); "SRAW" then the 16 codes' grip and resistance raw values (s16
 * pairs, hundredths, -1 own).  (The first build's "SURF" percents are not
 * read.) */
constexpr u32 kTuneTag = 0x454E5554;                    /* "TUNE" */
/* PC (2026-11-14): "TUNC", TUNE with Car Decay's power: the players' power
 * (pct[PC_TUNE_POWER][0]) in hundredths of a percent */
constexpr u32 kTuneDecayTag = 0x434E5554;
constexpr int kTuneAt = 0x20;
constexpr u32 kSurfTag = 0x57415253;                    /* "SRAW" */
constexpr int kSurfAt = 0x30;
static_assert(kTuneAt + 4 + sizeof(Tuning) <= kSurfAt, "the car part");
static_assert(kSurfAt + 4 + sizeof(SurfaceTuning) <= sizeof(((RaceInfoBlock *)0)->unk_080), "the spare bytes");

void forget(const RaceOrganization *org)
{
    g_tunings.erase(org);
    g_lastOrg = NULL;
    g_lastTuning = NULL;
    g_building = NULL;
}

void set_100(RaceTuning *t)
{
    for (int w = 0; w < 3; w++)
        for (int e = 0; e < 2; e++)
            t->car.pct[w][e] = 100;
    for (int c = 0; c < 16; c++)
        for (int k = 0; k < 2; k++)
            t->surf.raw[c][k] = settings::kSurfaceOwn;
    t->powerC = 0;
}

bool all_100(const Tuning &t)
{
    for (int w = 0; w < 3; w++)
        for (int e = 0; e < 2; e++)
            if (t.pct[w][e] != 100)
                return false;
    return true;
}

bool all_own(const SurfaceTuning &t)
{
    for (int c = 0; c < 16; c++)
        for (int k = 0; k < 2; k++)
            if (t.raw[c][k] != settings::kSurfaceOwn)
                return false;
    return true;
}

/* "1.25", or "own" */
void raw_text(char *out, size_t n, int raw)
{
    if (raw == settings::kSurfaceOwn)
        snprintf(out, n, "own");
    else
        snprintf(out, n, "%.2f", raw / 100.0);
}

void log_tuning(const char *what, const RaceTuning &rt)
{
    const Tuning &t = rt.car;
    if (!all_100(t))
        printf("PC: car multipliers (%s): tyre wear %u%% / %u%%, power %u%% / %u%%, grip %u%% / %u%% (players / "
               "enemies)\n", what, t.pct[0][0], t.pct[0][1], t.pct[1][0], t.pct[1][1], t.pct[2][0], t.pct[2][1]);
    if (rt.powerC != 0)                         /* PC (2026-11-14) */
        printf("PC: car multipliers (%s): Car Decay: the players' power %u.%02u%%\n", what, rt.powerC / 100u,
               rt.powerC % 100u);
    if (all_own(rt.surf))
        return;
    char line[640];
    size_t at = 0;
    line[0] = '\0';
    for (int c = 0; c < 16 && at < sizeof(line); c++) {
        if (rt.surf.raw[c][0] == settings::kSurfaceOwn && rt.surf.raw[c][1] == settings::kSurfaceOwn)
            continue;
        const char *key = "?";
        for (int s = 0; s < settings::kSurfaces; s++)
            if (settings::surface_code(s) == c)
                key = settings::surface_key(s);
        char grip[16], resist[16];
        raw_text(grip, sizeof(grip), rt.surf.raw[c][0]);
        raw_text(resist, sizeof(resist), rt.surf.raw[c][1]);
        at += (size_t)snprintf(line + at, sizeof(line) - at, "%s%s %s / %s", at != 0 ? ", " : "", key, grip, resist);
    }
    printf("PC: surfaces (%s): %s (grip / resistance)\n", what, line);
}

} // namespace

void pc_car_tuning_bind(RaceOrganization *org, RaceInformation *info)
{
    forget(org);
    if (org == NULL || info == NULL)
        return;
    if (dynamic_cast<RaceReplayInformation *>(info) != NULL || dynamic_cast<RaceArcadeDemoInformation *>(info) != NULL)
        return;                                         /* (a saved race's: pc_car_tuning_load) */
    RaceTuning t;
    set_100(&t);
    for (int w = 0; w < 3; w++)
        for (int e = 0; e < 2; e++)
            t.car.pct[w][e] = (u16)settings::car_tune((settings::CarTune)w, e != 0);
    for (int s = 0; s < settings::kSurfaces; s++)
        for (int k = 0; k < 2; k++)
            t.surf.raw[settings::surface_code(s)][k] = (s16)settings::surface_tune(s, (settings::SurfaceTune)k);
    /* PC (2026-11-14): the challenge Car Decay in a GT mode race (game/app/
     * pc_challenge.h): the players' power by the day, in place of
     * PowerPlayer */
    const int decay = pc_challenge_race_power();
    if (decay != 0) {
        t.car.pct[PC_TUNE_POWER][0] = 100;
        t.powerC = (u16)(decay < 10000 ? decay : 0);
    }
    if (all_100(t.car) && all_own(t.surf) && t.powerC == 0)
        return;
    g_tunings[org] = t;
    log_tuning("the settings'", t);
}

void pc_car_tuning_store(RaceInfoBlock *blk, const RaceOrganization *org)
{
    auto it = g_tunings.find(org);
    if (it == g_tunings.end())
        return;
    if (it->second.powerC != 0) {
        /* PC (2026-11-14): with Car Decay's power, "TUNC" */
        Tuning c = it->second.car;
        c.pct[PC_TUNE_POWER][0] = it->second.powerC;
        memcpy(blk->unk_080 + kTuneAt, &kTuneDecayTag, 4);
        memcpy(blk->unk_080 + kTuneAt + 4, &c, sizeof(Tuning));
    } else if (!all_100(it->second.car)) {
        memcpy(blk->unk_080 + kTuneAt, &kTuneTag, 4);
        memcpy(blk->unk_080 + kTuneAt + 4, &it->second.car, sizeof(Tuning));
    }
    if (!all_own(it->second.surf)) {
        memcpy(blk->unk_080 + kSurfAt, &kSurfTag, 4);
        memcpy(blk->unk_080 + kSurfAt + 4, &it->second.surf, sizeof(SurfaceTuning));
    }
}

void pc_car_tuning_load(RaceOrganization *org, const RaceInfoBlock *blk)
{
    forget(org);
    RaceTuning t;
    set_100(&t);
    u32 tag;
    memcpy(&tag, blk->unk_080 + kTuneAt, 4);
    if (tag == kTuneTag || tag == kTuneDecayTag) {
        memcpy(&t.car, blk->unk_080 + kTuneAt + 4, sizeof(Tuning));
        if (tag == kTuneDecayTag) {
            /* PC (2026-11-14): Car Decay's power, hundredths */
            const u16 c = t.car.pct[PC_TUNE_POWER][0];
            t.powerC = c >= 1 && c < 10000 ? c : 0;
            t.car.pct[PC_TUNE_POWER][0] = 100;
        }
        for (int w = 0; w < 3; w++)
            for (int e = 0; e < 2; e++) {
                const int lo = settings::car_tune_min((settings::CarTune)w);
                if (t.car.pct[w][e] < lo || t.car.pct[w][e] > settings::kCarTuneMax)
                    t.car.pct[w][e] = 100;              /* (not a value the settings give) */
            }
    }
    memcpy(&tag, blk->unk_080 + kSurfAt, 4);
    if (tag == kSurfTag) {
        memcpy(&t.surf, blk->unk_080 + kSurfAt + 4, sizeof(SurfaceTuning));
        for (int c = 0; c < 16; c++)
            for (int k = 0; k < 2; k++) {
                const int lo = settings::surface_tune_min((settings::SurfaceTune)k);
                const int hi = settings::surface_tune_max((settings::SurfaceTune)k);
                if (t.surf.raw[c][k] < lo || t.surf.raw[c][k] > hi)
                    t.surf.raw[c][k] = settings::kSurfaceOwn;   /* (own, or not a value the settings give) */
            }
    }
    if (all_100(t.car) && all_own(t.surf) && t.powerC == 0)
        return;
    g_tunings[org] = t;
    log_tuning("the saved race's", t);
}

/* car's race's multipliers (NULL: none). */
static const RaceTuning *race_tuning(const RaceCarState *car)
{
    const RaceOrganization *org = car->org;
    if (org != g_lastOrg) {
        auto it = g_tunings.find(org);
        g_lastOrg = org;
        g_lastTuning = it != g_tunings.end() ? &it->second : NULL;
    }
    return g_lastTuning;
}

f32 pc_car_tuning(const RaceCarState *car, PcCarTune what)
{
    const RaceTuning *t = race_tuning(car);
    if (t == NULL)
        return 1.0f;
    const int enemy = car->st.driver == DRIVER_COMPUTER ? 1 : 0;
    if (what == PC_TUNE_POWER && enemy == 0 && t->powerC != 0)
        return (f32)t->powerC * 0.0001f;        /* PC (2026-11-14): Car Decay */
    return (f32)t->car.pct[what][enemy] * 0.01f;
}

void pc_surface_build_begin(const RaceOrganization *org)
{
    auto it = g_tunings.find(org);
    g_building = it != g_tunings.end() ? &it->second : NULL;
}

void pc_surface_build_end(void)
{
    g_building = NULL;
}

int pc_surface_grip_raw(int code)
{
    if (g_building == NULL || code < 0 || code >= 16)
        return settings::kSurfaceOwn;
    return g_building->surf.raw[code][0];
}

f32 pc_surface_resistance(const RaceCarState *car, int code, f32 own)
{
    const RaceTuning *t = race_tuning(car);
    if (t == NULL || code < 0 || code >= 16 || t->surf.raw[code][1] == settings::kSurfaceOwn)
        return own;
    return (f32)t->surf.raw[code][1] * 0.01f;
}
