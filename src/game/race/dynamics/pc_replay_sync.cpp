/* pc_replay_sync.cpp - a replay checked against its race (pc_replay_sync.h).
 * No original: new PC code. */
#include "game/race/dynamics/pc_replay_sync.h"
#include "game/race/dynamics/control_record_internal.h" /* DynamicsConductor, RaceCarState, RaceInput, DC_MODE_* */
#include "platform/settings.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <memory>
#include <new>
#include <vector>

namespace {

/* a car's state of a frame; compared bit for bit */
const char *const kFields[] = {
    "x", "y", "z", "vx", "vy", "vz", "yaw", "yaw rate", "steer", "throttle", "brake", "clutch", "engine speed",
    "slipstream", "time scale", "impact", "spin 0", "spin 1", "spin 2", "spin 3", "load 0", "load 1", "load 2",
    "load 3",
};
constexpr int kFieldCount = (int)(sizeof(kFields) / sizeof(kFields[0]));

struct CarSnap {
    f32 v[kFieldCount];
    u8  gear, ctrl, pad[2];
};

/* the player's input of a frame (CarInput without its unrecorded byte) */
struct InputSnap {
    u8  flags, device;
    s8  shift;
    u8  pad;
    s16 steer, steerMag, throttle, brake, throttle2, handbrake;
};

constexpr f32 kTicksPerSecond = 59.94f;
constexpr size_t kChunk = 16u << 20;
constexpr size_t kMaxBytes = 256u << 20;
constexpr int kProgressFrames = 1800;           /* a line while all stays the same */

enum Run { RUN_NONE, RUN_RECORD, RUN_COMPARE };

Run  g_run = RUN_NONE;
const RaceOrganization *g_org;                  /* the race kept */
int  g_cars;
int  g_frames;                                  /* the frames kept */
int  g_frame;                                   /* the replay's frame */
size_t g_frameSize;
int  g_perChunk;
std::vector<std::unique_ptr<u8[]>> g_chunks;
std::vector<u8> g_now;
bool g_differs[PC_RACE_CARS_MAX];
int  g_differCount;
int  g_inputFrame;                              /* the first frame the input differs, -1 none */
int  g_firstFrame;                              /* the first frame anything differs, -1 none */

bool sync_on()
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("GT3PC_REPLAY_SYNC");
        on = (e != NULL ? atoi(e) > 0 : settings::debug_replay_sync()) ? 1 : 0;
        if (on)
            printf("PC: replay sync: on: a race's cars kept frame by frame, its replay compared with them\n");
    }
    return on != 0;
}

u8 *frame_at(int frame, bool grow)
{
    const size_t c = (size_t)(frame / g_perChunk);
    if (c >= g_chunks.size()) {
        if (!grow || (c + 1) * kChunk > kMaxBytes)
            return nullptr;
        u8 *mem = new (std::nothrow) u8[kChunk];
        if (mem == nullptr)
            return nullptr;
        g_chunks.emplace_back(mem);
    }
    return g_chunks[c].get() + (size_t)(frame % g_perChunk) * g_frameSize;
}

void take_car(RaceCarState *car, CarSnap *s)
{
    const DynCarState *st = &car->st;
    f32 *v = s->v;

    memset(s, 0, sizeof(*s));
    for (int i = 0; i < 3; i++)
        *v++ = st->pos[i];
    for (int i = 0; i < 3; i++)
        *v++ = st->vel[i];
    *v++ = st->yaw;
    *v++ = st->yawRate;
    *v++ = st->steer;
    *v++ = st->throttle;
    *v++ = st->brake;
    *v++ = st->clutch;
    *v++ = st->engineSpeed;
    *v++ = st->slipBlend;
    *v++ = st->timeScale;
    *v++ = st->impact;
    for (int i = 0; i < 4; i++)
        *v++ = st->wheel[i].spin;
    for (int i = 0; i < 4; i++)
        *v++ = st->wheel[i].load;
    s->gear = st->gear;
    s->ctrl = st->ctrlMode;
}

void take_input(DynamicsConductor *dc, InputSnap *s)
{
    const CarInput &in = dc->getCar(0)->input->rec.rec.cur;     /* (both the record's and the playing's last frame) */

    memset(s, 0, sizeof(*s));
    s->flags = in.flags;
    s->device = in.device;
    s->shift = in.shift;
    s->steer = in.steer;
    s->steerMag = in.steerMag;
    s->throttle = in.throttle;
    s->brake = in.brake;
    s->throttle2 = in.throttle2;
    s->handbrake = in.handbrake;
}

void take(DynamicsConductor *dc, u8 *out)
{
    CarSnap *cars = reinterpret_cast<CarSnap *>(out + sizeof(InputSnap));

    take_input(dc, reinterpret_cast<InputSnap *>(out));
    for (int i = 0; i < g_cars; i++)
        take_car(dc->getCar(i), &cars[i]);
}

void keep_frame(DynamicsConductor *dc)
{
    u8 *out = frame_at(g_frames, true);

    if (out == nullptr) {
        printf("PC: replay sync: the race's first %d frames kept (%.1f s): the rest is not checked\n", g_frames,
               (f32)g_frames / kTicksPerSecond);
        g_run = RUN_NONE;
        return;
    }
    take(dc, out);
    g_frames++;
}

/* the cars within 10 m of car i (the ones it can be touching) */
void near_list(DynamicsConductor *dc, int i, char *out, size_t n)
{
    const DynCarState *a = &dc->getCar(i)->st;
    size_t at = 0;

    out[0] = '\0';
    for (int j = 0; j < g_cars && at + 32 < n; j++) {
        if (j == i)
            continue;
        const DynCarState *b = &dc->getCar(j)->st;
        const f32 dx = b->pos[0] - a->pos[0], dy = b->pos[1] - a->pos[1];
        const f32 d = sqrtf(dx * dx + dy * dy);
        if (d < 10.0f)
            at += (size_t)snprintf(out + at, n - at, " %d (%.1f m)", j, d);
    }
}

void log_car(DynamicsConductor *dc, int frame, int i, const CarSnap &was, const CarSnap &now)
{
    const DynCarState *st = &dc->getCar(i)->st;
    char list[512], close[256];
    size_t at = 0;
    int fields = 0;

    list[0] = '\0';
    for (int k = 0; k < kFieldCount; k++) {
        if (memcmp(&was.v[k], &now.v[k], sizeof(f32)) == 0)
            continue;
        if (++fields <= 8)
            at += (size_t)snprintf(list + at, sizeof(list) - at, " %s %.9g / %.9g", kFields[k], was.v[k], now.v[k]);
    }
    if (was.gear != now.gear)
        at += (size_t)snprintf(list + at, sizeof(list) - at, " gear %d / %d", was.gear, now.gear);
    if (was.ctrl != now.ctrl)
        at += (size_t)snprintf(list + at, sizeof(list) - at, " control %d / %d", was.ctrl, now.ctrl);
    near_list(dc, i, close, sizeof(close));
    printf("PC: replay sync: frame %d (%.2f s): car %d differs (driver %d, control %d, rank %d, lap %d at %.1f m; "
           "cars near:%s): %d fields, race / replay:%s\n",
           frame, (f32)frame / kTicksPerSecond, i, st->driver, st->ctrlMode, st->rank, st->lap, st->lapDistance,
           close[0] != '\0' ? close : " none", fields, list);
}

void compare_frame(DynamicsConductor *dc)
{
    const u8 *was = frame_at(g_frame, false);

    if (g_frame >= g_frames || was == nullptr) {
        printf("PC: replay sync: the replay ran past the race's %d frames kept\n", g_frames);
        g_run = RUN_NONE;
        return;
    }
    u8 *now = g_now.data();
    take(dc, now);
    if (g_inputFrame < 0 && memcmp(was, now, sizeof(InputSnap)) != 0) {
        const InputSnap *a = reinterpret_cast<const InputSnap *>(was);
        const InputSnap *b = reinterpret_cast<const InputSnap *>(now);

        g_inputFrame = g_frame;
        if (g_firstFrame < 0)
            g_firstFrame = g_frame;
        printf("PC: replay sync: frame %d (%.2f s): the player's input differs (race / replay): flags %d / %d, device "
               "%d / %d, shift %d / %d, steer %d / %d, throttle %d / %d, brake %d / %d, handbrake %d / %d\n",
               g_frame, (f32)g_frame / kTicksPerSecond, a->flags, b->flags, a->device, b->device, a->shift, b->shift,
               a->steer, b->steer, a->throttle, b->throttle, a->brake, b->brake, a->handbrake, b->handbrake);
    }
    const CarSnap *carsWas = reinterpret_cast<const CarSnap *>(was + sizeof(InputSnap));
    const CarSnap *carsNow = reinterpret_cast<const CarSnap *>(now + sizeof(InputSnap));
    for (int i = 0; i < g_cars; i++) {
        if (g_differs[i] || memcmp(&carsWas[i], &carsNow[i], sizeof(CarSnap)) == 0)
            continue;
        g_differs[i] = true;
        g_differCount++;
        if (g_firstFrame < 0)
            g_firstFrame = g_frame;
        log_car(dc, g_frame, i, carsWas[i], carsNow[i]);
    }
    if (g_firstFrame < 0 && g_frame > 0 && g_frame % kProgressFrames == 0)
        printf("PC: replay sync: frame %d (%.2f s): every car the same so far\n", g_frame,
               (f32)g_frame / kTicksPerSecond);
    g_frame++;
}

/* what the last run came to */
void report()
{
    if (g_run == RUN_RECORD) {
        printf("PC: replay sync: the race kept %d frames (%.1f s) of %d cars\n", g_frames,
               (f32)g_frames / kTicksPerSecond, g_cars);
    } else if (g_run == RUN_COMPARE) {
        if (g_firstFrame < 0)
            printf("PC: replay sync: %d of the race's %d frames compared: every car exactly the same\n", g_frame,
                   g_frames);
        else
            printf("PC: replay sync: %d of the race's %d frames compared: from frame %d %d of %d cars differ, the "
                   "input %s\n", g_frame, g_frames, g_firstFrame, g_differCount, g_cars,
                   g_inputFrame >= 0 ? "too" : "not");
    }
}

}  // namespace

void pc_replay_sync_begin(DynamicsConductor *dc)
{
    if (!sync_on())
        return;
    report();
    g_run = RUN_NONE;
    RaceOrganization *org = dc->org;
    if (org->backgroundMode != 0 || org->racePreviewMode != 0 || org->loggerMode != 0)
        return;
    if (dc->mode == DC_MODE_RACE) {
        g_org = org;
        g_cars = dc->carCount;
        g_frames = 0;
        g_chunks.clear();
        g_frameSize = sizeof(InputSnap) + (size_t)g_cars * sizeof(CarSnap);
        g_perChunk = (int)(kChunk / g_frameSize);
        g_now.resize(g_frameSize);
        g_run = RUN_RECORD;
        keep_frame(dc);
    } else if (dc->mode == DC_MODE_REPLAY) {
        if (org != g_org || g_cars != dc->carCount || g_frames == 0) {
            printf("PC: replay sync: this replay's race was not run in this session: not compared\n");
            return;
        }
        memset(g_differs, 0, sizeof(g_differs));
        g_differCount = 0;
        g_inputFrame = -1;
        g_firstFrame = -1;
        g_frame = 0;
        g_run = RUN_COMPARE;
        compare_frame(dc);
    }
}

void pc_replay_sync_frame(DynamicsConductor *dc)
{
    if (g_run == RUN_RECORD)
        keep_frame(dc);
    else if (g_run == RUN_COMPARE)
        compare_frame(dc);
}
