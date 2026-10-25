/* pc_debugcam.cpp - the debug camera (pc_debugcam.h).  No original: new PC
 * code. */
#include "game/app/pc_debugcam.h"
#include "engine/pgl/G001_flare.h"              /* pgl_get_matrix, vu0_scale */
#include "platform/debug_camera.h"
#include "platform/scheduler.h"

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

static const f32 kDegToRad = 0.017453292f;

static int s_bypass;                        /* PcDebugcamBypass scopes alive */
static const void *s_forced;                /* the onboard camera put in the chase view (pc_debugcam_mount) */
static int s_forcedMount;                   /* its mount before */

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
    return 1;
}

int pc_debugcam_on(void)
{
    return debug_camera::active() ? 1 : 0;
}

int pc_debugcam_owns(int view)
{
    return view == 0 && s.state != 0 && s_bypass == 0 && debug_camera::active();
}

PcDebugcamBypass::PcDebugcamBypass()
{
    s_bypass++;
}

PcDebugcamBypass::~PcDebugcamBypass()
{
    s_bypass--;
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
    char text[64];
    const f32 speed = s.speed;
    if (c.faster != 0.0f)
        s.speed *= powf(DEBUGCAM_SPEED_HOLD, shape(c.faster) * dt);
    if (c.fasterSteps != 0)
        s.speed *= powf(DEBUGCAM_SPEED_STEP, (f32)c.fasterSteps);
    s.speed = clampf(s.speed, DEBUGCAM_SPEED_MIN, DEBUGCAM_SPEED_MAX);
    if (s.speed != speed) {
        snprintf(text, sizeof(text), "DEBUG CAMERA SPEED %.1f M/S", s.speed);
        debug_camera::notice(text, 1.0);
    }
    if (s.fov > 0.0f && (c.fov != 0.0f || c.fovSteps != 0)) {
        const f32 fov = s.fov;
        s.fov = clampf(s.fov + shape(c.fov) * DEBUGCAM_FOV_RATE * dt + (f32)c.fovSteps * DEBUGCAM_FOV_STEP,
                       DEBUGCAM_FOV_MIN, DEBUGCAM_FOV_MAX);
        if (s.fov != fov) {
            snprintf(text, sizeof(text), "DEBUG CAMERA FIELD OF VIEW %.0f", s.fov);
            debug_camera::notice(text, 1.0);
        }
    }
    /* the move: forward / right in the heading's horizontal plane, up
     * vertically */
    const f32 y = s.yaw * kDegToRad;
    const f32 fwd[3] = {sinf(y), 0.0f, cosf(y)}, right[3] = {s.sR * cosf(y), 0.0f, -s.sR * sinf(y)};
    const f32 mf = shape(c.forward), mr = shape(c.right), mu = shape(c.up);
    for (int j = 0; j < 3; j++)
        s.eye[j] += (fwd[j] * mf + right[j] * mr) * s.speed * dt;
    s.eye[1] += mu * s.speed * dt;
}

void pc_debugcam_race(int on)
{
    debug_camera::race(on != 0);
    s.state = 0;
    s_forced = NULL;                        /* (a camera of the race gone, or not yet made) */
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
