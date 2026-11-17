/* pc_debugcam.cpp - the debug camera (pc_debugcam.h).  No original: new PC
 * code. */
#include "game/app/pc_debugcam.h"
#include "engine/pgl/G001_flare.h"              /* pgl_get_matrix, vu0_scale */
#include "game/course/T457_runway.h"            /* the road's triangles (pc_debugcam_ground) */
#include "platform/debug_camera.h"
#include "platform/frame_step.h"
#include "platform/scheduler.h"
#include "platform/settings.h"
#include "platform/ui_text.h"                   /* PC (2026-11-16): the interface's languages */

#include <math.h>
#include <stdio.h>
#include <string.h>

#define DEBUGCAM_YAW_RATE    120.0f         /* degrees a second at full deflection */
#define DEBUGCAM_PITCH_RATE  90.0f
#define DEBUGCAM_PITCH_MAX   89.0f
#define DEBUGCAM_SPEED_START 15.0f          /* m/s */
#define DEBUGCAM_SPEED_MIN   0.5f
#define DEBUGCAM_SPEED_MAX   500.0f
#define DEBUGCAM_SPEED_HOLD  4.0f           /* x this a second, held */
#define DEBUGCAM_SPEED_STEP  1.25f          /* x this a mouse wheel notch */
#define DEBUGCAM_FOV_RATE    45.0f          /* degrees a second, held */
#define DEBUGCAM_FOV_STEP    5.0f           /* degrees a notch */
#define DEBUGCAM_FOV_MIN     10.0f
#define DEBUGCAM_FOV_MAX     150.0f
#define DEBUGCAM_DEAD_ZONE   0.1f
#define DEBUGCAM_MOUSE_DEG   0.05f          /* degrees a count of the mouse's movement (raw: 40 an inch at 800 dpi) */
#define DEBUGCAM_GROUND_MIN  0.3f           /* m: the eye above the road at the least (as the chase camera's, pc_freecam) */
#define DEBUGCAM_GROUND_RISE 1.0f           /* m: the road rising this, and the step moved, is still the same road */

static const f32 kDegToRad = 0.017453292f;

/* The listener's eye at the last sound tick (pc_debugcam_listener_tick) and
 * when; have: 0 none yet (the debug camera just started). */
static struct {
    int have;
    f32 eye[3];
    double at;
} s_listen;
#define DEBUGCAM_SOUND_STEP_MAX (0.9f * 5.524167f)  /* m a tick: 0.9 of the speed of sound a tick
                                                       (race_entry_internal.h SOUND_SPEED_PER_FRAME) */
#define DEBUGCAM_SOUND_GAP      0.25                /* s: a longer gap since the last tick, no motion */
static const void *s_forced;                /* the onboard camera put in the chase view (pc_debugcam_mount) */
static int s_forcedMount;                   /* its mount before */
static int s_stepFrames;                    /* it was on in this tick step pause (pc_debugcam_step_frames) */

/* 0 off; 1 on, to start from the camera at its next view; 2 flying */
static struct {
    int state;
    f32 eye[3];
    f32 yaw, pitch;                         /* degrees: the heading (atan2 of forward's x, z), up + */
    f32 sR, sU;                             /* the sides of the camera's right / up axes (its handedness) */
    f32 len[3];                             /* the lengths of the camera matrix's axes */
    f32 base[16];                           /* the camera's matrix at the start (its w column) */
    f32 fov;                                /* degrees, vertical; 0: the camera's (pc_debugcam_projection) */
    f32 speed;                              /* m/s */
    double last;                            /* the last frame's time */
    int onGround;                           /* the ground collision has found the road */
    f32 groundY, height;                    /* the road's height, the eye's above it */
    f32 groundEye[3];                       /* the eye as pc_debugcam_ground left it */
} s = {0, {0, 0, 0}, 0, 0, 1, 1, {1, 1, 1}, {0}, 0, DEBUGCAM_SPEED_START, 0.0};

static f32 clampf(f32 v, f32 lo, f32 hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static f32 dot3(const f32 *a, const f32 *b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* An analog input past its dead zone, squared (a key's 1 stays 1). */
static f32 shape(f32 v)
{
    const f32 a = fabsf(v);
    if (a <= DEBUGCAM_DEAD_ZONE)
        return 0.0f;
    const f32 l = clampf((a - DEBUGCAM_DEAD_ZONE) / (1.0f - DEBUGCAM_DEAD_ZONE), 0.0f, 1.0f);
    return v > 0.0f ? l * l : -l * l;
}

/* The axes of the heading and pitch: forward, right, up (world). */
static void axes(f32 *f, f32 *r, f32 *u)
{
    const f32 y = s.yaw * kDegToRad, p = s.pitch * kDegToRad;
    f[0] = cosf(p) * sinf(y);
    f[1] = sinf(p);
    f[2] = cosf(p) * cosf(y);
    r[0] = s.sR * cosf(y);
    r[1] = 0.0f;
    r[2] = -s.sR * sinf(y);
    u[0] = s.sU * (r[1] * f[2] - r[2] * f[1]);
    u[1] = s.sU * (r[2] * f[0] - r[0] * f[2]);
    u[2] = s.sU * (r[0] * f[1] - r[1] * f[0]);
}

/* Started from the camera's view matrix cam (its axes: the rotation's
 * columns, right, up and back - the projection looks down -z; the eye from
 * the translation, t = -eye A).  0: a degenerate matrix. */
static int start_from(const f32 *cam)
{
    f32 a[3][3];
    for (int k = 0; k < 3; k++) {
        for (int j = 0; j < 3; j++)
            a[k][j] = cam[4 * j + k];
        s.len[k] = sqrtf(dot3(a[k], a[k]));
        if (!(s.len[k] > 0.0f))
            return 0;
        for (int j = 0; j < 3; j++)
            a[k][j] /= s.len[k];
    }
    for (int j = 0; j < 3; j++)
        s.eye[j] = -(cam[12] * a[0][j] / s.len[0] + cam[13] * a[1][j] / s.len[1] + cam[14] * a[2][j] / s.len[2]);
    const f32 fwd[3] = {-a[2][0], -a[2][1], -a[2][2]};
    s.yaw = atan2f(fwd[0], fwd[2]) / kDegToRad;
    s.pitch = clampf(asinf(clampf(fwd[1], -1.0f, 1.0f)) / kDegToRad, -DEBUGCAM_PITCH_MAX, DEBUGCAM_PITCH_MAX);
    s.sR = 1.0f;
    s.sU = 1.0f;
    f32 f[3], r[3], u[3];
    axes(f, r, u);
    s.sR = dot3(a[0], r) >= 0.0f ? 1.0f : -1.0f;
    axes(f, r, u);
    s.sU = dot3(a[1], u) >= 0.0f ? 1.0f : -1.0f;
    memcpy(s.base, cam, sizeof(s.base));
    s.fov = 0.0f;
    s.onGround = 0;
    return 1;
}

/* The road at (x, z): of its triangles there the highest not above limit, else
 * the lowest above it (runway_query's highest would be a bridge overhead).
 * 0: no road there. */
static int ground_at(const Runway *r, f32 x, f32 z, f32 limit, f32 *height)
{
    const u16 *cell = runway_map_find(r->triMap, x, z);
    int found = 0;
    f32 best = 0.0f;
    for (; cell != NULL && cell[0] != 0; cell += cell[0] + 2) {
        const RunwaySector *sector = &r->sectors[cell[1]];
        for (int i = 0; i < cell[0]; i++) {
            const RunwayTri *t = &sector->tris[cell[2 + i]];
            if (!runway_tri_contains(t, sector->verts, x, z))
                continue;
            const f32 h = runway_tri_height(t, sector->verts, x, z);
            if (!found || (h <= limit ? best > limit || h > best : best > limit && h < best)) {
                best = h;
                found = 1;
            }
        }
    }
    *height = best;
    return found;
}

int pc_debugcam_on(void)
{
    return debug_camera::active() ? 1 : 0;
}

int pc_debugcam_owns(int view)
{
    return view == 0 && s.state != 0 && debug_camera::active();
}

int pc_debugcam_step_frames(void)
{
    if (debug_camera::active())
        s_stepFrames = 1;
    return s_stepFrames;
}

int pc_debugcam_listener_prev(int view, f32 *xyz)
{
    f32 eye[3];
    if (!pc_debugcam_eye(view, eye))
        return 0;
    memcpy(xyz, eye, sizeof(eye));          /* (no motion: the first tick, after a gap) */
    if (!s_listen.have || sched::now_seconds() - s_listen.at > DEBUGCAM_SOUND_GAP)
        return 1;
    f32 d[3] = {eye[0] - s_listen.eye[0], eye[1] - s_listen.eye[1], eye[2] - s_listen.eye[2]};
    const f32 len = sqrtf(dot3(d, d));
    const f32 k = len > DEBUGCAM_SOUND_STEP_MAX ? DEBUGCAM_SOUND_STEP_MAX / len : 1.0f;
    for (int j = 0; j < 3; j++)
        xyz[j] = eye[j] - d[j] * k;
    return 1;
}

int pc_debugcam_side(int view, f32 *xyz)
{
    if (!pc_debugcam_owns(view) || s.state != 2)
        return 0;
    f32 f[3], r[3], u[3];
    axes(f, r, u);
    for (int j = 0; j < 3; j++)
        xyz[j] = r[j] * s.len[0];           /* (as the view matrix's column 0, pc_debugcam_view) */
    return 1;
}

void pc_debugcam_listener_tick(void)
{
    f32 eye[3];
    s_listen.have = pc_debugcam_eye(0, eye);
    if (s_listen.have) {
        memcpy(s_listen.eye, eye, sizeof(eye));
        s_listen.at = sched::now_seconds();
    }
}

int pc_debugcam_view(int view, const f32 *cam, f32 *out)
{
    if (!pc_debugcam_owns(view))
        return 0;
    if (s.state == 1) {
        if (!start_from(cam))
            return 0;
        s.state = 2;
    }
    f32 f[3], r[3], u[3];
    axes(f, r, u);
    memcpy(out, s.base, 16 * sizeof(f32));
    for (int j = 0; j < 3; j++) {
        out[4 * j + 0] = r[j] * s.len[0];
        out[4 * j + 1] = u[j] * s.len[1];
        out[4 * j + 2] = -f[j] * s.len[2];
    }
    for (int k = 0; k < 3; k++)
        out[12 + k] = -(s.eye[0] * out[k] + s.eye[1] * out[4 + k] + s.eye[2] * out[8 + k]);
    return 1;
}

int pc_debugcam_eye(int view, f32 *xyz)
{
    if (!pc_debugcam_owns(view) || s.state != 2)
        return 0;
    xyz[0] = s.eye[0];
    xyz[1] = s.eye[1];
    xyz[2] = s.eye[2];
    return 1;
}

void pc_debugcam_projection(int view)
{
    if (!pc_debugcam_owns(view) || s.state != 2)
        return;
    f32 m[16];
    pgl_get_matrix(m);
    const f32 sy = fabsf(m[5]);
    if (!(sy > 0.0f))
        return;
    const f32 half = atanf(1.0f / sy);
    if (s.fov <= 0.0f) {
        s.fov = clampf(2.0f * half / kDegToRad, DEBUGCAM_FOV_MIN, DEBUGCAM_FOV_MAX);     /* the camera's */
        return;
    }
    const f32 k = tanf(half) / tanf(0.5f * s.fov * kDegToRad);
    vu0_scale(k, k, 1.0f);
}

void pc_debugcam_frame(void)
{
    const double now = sched::now_seconds();
    const f32 dt = clampf((f32)(now - s.last), 0.0f, 0.1f);
    s.last = now;
    if (!frame_step::paused())
        s_stepFrames = 0;
    if (!debug_camera::active()) {
        s.state = 0;
        return;
    }
    if (s.state == 0) {
        s.state = 1;                        /* (from the camera at the next view) */
        return;
    }
    debug_camera::Controls c;
    debug_camera::read(&c);
    if (s.state != 2)
        return;
    /* the look: a rate (the sticks, keys), and the mouse's movement */
    s.yaw += s.sR * (shape(c.lookRight) * DEBUGCAM_YAW_RATE * dt + (f32)c.lookRightCounts * DEBUGCAM_MOUSE_DEG);
    if (s.yaw > 180.0f)
        s.yaw -= 360.0f;
    if (s.yaw < -180.0f)
        s.yaw += 360.0f;
    s.pitch = clampf(s.pitch + shape(c.lookUp) * DEBUGCAM_PITCH_RATE * dt + (f32)c.lookUpCounts * DEBUGCAM_MOUSE_DEG,
                     -DEBUGCAM_PITCH_MAX, DEBUGCAM_PITCH_MAX);
    /* the speed and the field of view, said as they change */
    char text[256];                         /* (PC 2026-11-16: 64 before - UTF-8 translations) */
    const f32 speed = s.speed;
    if (c.faster != 0.0f)
        s.speed *= powf(DEBUGCAM_SPEED_HOLD, shape(c.faster) * dt);
    if (c.fasterSteps != 0)
        s.speed *= powf(DEBUGCAM_SPEED_STEP, (f32)c.fasterSteps);
    s.speed = clampf(s.speed, DEBUGCAM_SPEED_MIN, DEBUGCAM_SPEED_MAX);
    if (s.speed != speed) {
        snprintf(text, sizeof(text), tr("DEBUG CAMERA SPEED %.1f M/S"), s.speed);
        debug_camera::notice(text, 1.0);
    }
    if (s.fov > 0.0f && (c.fov != 0.0f || c.fovSteps != 0)) {
        const f32 fov = s.fov;
        s.fov = clampf(s.fov + shape(c.fov) * DEBUGCAM_FOV_RATE * dt + (f32)c.fovSteps * DEBUGCAM_FOV_STEP,
                       DEBUGCAM_FOV_MIN, DEBUGCAM_FOV_MAX);
        if (s.fov != fov) {
            snprintf(text, sizeof(text), tr("DEBUG CAMERA FIELD OF VIEW %.0f"), s.fov);
            debug_camera::notice(text, 1.0);
        }
    }
    /* the move: forward / right in the heading's horizontal plane (with the
     * Y-axis look forward is along the look, the pitch too), up vertically */
    const f32 y = s.yaw * kDegToRad, p = settings::debug_camera_y_look() ? s.pitch * kDegToRad : 0.0f;
    const f32 fwd[3] = {cosf(p) * sinf(y), sinf(p), cosf(p) * cosf(y)}, right[3] = {s.sR * cosf(y), 0.0f, -s.sR * sinf(y)};
    const f32 mf = shape(c.forward), mr = shape(c.right), mu = shape(c.up);
    for (int j = 0; j < 3; j++)
        s.eye[j] += (fwd[j] * mf + right[j] * mr) * s.speed * dt;
    s.eye[1] += mu * s.speed * dt;
}

void pc_debugcam_ground(const Runway *runway)
{
    if (s.state != 2 || !settings::debug_camera_ground()) {
        s.onGround = 0;
        return;
    }
    const f32 dx = s.eye[0] - s.groundEye[0], dz = s.eye[2] - s.groundEye[2];
    const f32 limit = s.onGround ? s.groundY + DEBUGCAM_GROUND_RISE + sqrtf(dx * dx + dz * dz) : s.eye[1];
    f32 h;
    const int found = ground_at(runway, s.eye[0], s.eye[2], limit, &h);
    if (s.onGround)
        s.height += s.eye[1] - s.groundEye[1];  /* the vertical moves since the last frame */
    else if (found) {
        s.height = s.eye[1] - h;            /* the height it is at */
        s.onGround = 1;
    } else
        return;                             /* off the road, none yet: it flies freely */
    if (found)
        s.groundY = h;                      /* (off the road: the last road's height kept) */
    if (s.height < DEBUGCAM_GROUND_MIN)
        s.height = DEBUGCAM_GROUND_MIN;
    s.eye[1] = s.groundY + s.height;
    memcpy(s.groundEye, s.eye, sizeof(s.eye));
}

void pc_debugcam_race(int on)
{
    debug_camera::race(on != 0);
    s.state = 0;
    s_forced = NULL;                        /* (a camera of the race gone, or not yet made) */
    s_stepFrames = 0;
}

int pc_debugcam_mount(const void *key, int live, int mount, int chase)
{
    if (live && debug_camera::active()) {
        if (s_forced != key) {
            s_forced = key;
            s_forcedMount = mount;
        }
        return chase;
    }
    if (s_forced == key) {
        s_forced = NULL;
        return s_forcedMount;               /* the player's view back */
    }
    return mount;
}
