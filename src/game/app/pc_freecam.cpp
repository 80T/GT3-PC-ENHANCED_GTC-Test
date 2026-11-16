/* pc_freecam.cpp - the chase camera's free orbit (pc_freecam.h).  No
 * original: new PC code. */
#include "game/app/pc_freecam.h"
#include "platform/pad_pc.h"
#include "platform/settings.h"

#include <math.h>

#define FREECAM_YAW_MAX     180.0f          /* degrees, each side (at the stops in front, looking back) */
#define FREECAM_PITCH_MAX   45.0f           /* degrees, up and down */
#define FREECAM_YAW_RATE    360.0f          /* degrees a second at full deflection */
#define FREECAM_PITCH_RATE  180.0f
#define FREECAM_TICK        (1.0f / 60.0f)  /* the camera's update, once a tick */
#define FREECAM_EASE        0.35f           /* of the way to the aimed angles each tick */
#define FREECAM_PULL        1.8f            /* the pull back behind, always on: the angle times this a second
                                               (a spring: weak near the middle, strong far out; full
                                               deflection holds the camera at rate / pull, 200 / 100
                                               degrees, beyond the stops; let go, 1/e in 0.56 s) */
#define FREECAM_DEAD_ZONE   0.15f           /* of a stick axis */
#define FREECAM_EXPO        3.0f            /* the stick's curve (e^(k a) - 1) / (e^k - 1): half way 18% */
#define FREECAM_PIVOT_UP    0.5f            /* the pivot above the car's origin (its middle), metres */
#define FREECAM_CLEARANCE   0.3f            /* the eye above the road, metres */
#define FREECAM_SLOTS       16              /* cameras at once (the split screen's two, their orbits and looks;
                                               PC 2026-11-14: the custom cameras' own, 8 before) */
#define LOOK_YAW_MAX        180.0f          /* the cockpit camera's look round, degrees each side (straight back) */
#define LOOK_PITCH_MAX      60.0f           /* up and down */

static const f32 kDegToRad = 0.017453292f;

/* The swing of each camera: the angles the stick has turned it to (aim),
 * and the ones shown, eased toward them. */
static struct {
    const void *key;
    f32 yaw, pitch;                         /* degrees */
    f32 aimYaw, aimPitch;
} s_cams[FREECAM_SLOTS];

static int slot(const void *key)
{
    int free = -1;
    for (int i = 0; i < FREECAM_SLOTS; i++) {
        if (s_cams[i].key == key)
            return i;
        if (s_cams[i].key == NULL && free < 0)
            free = i;
    }
    if (free < 0)
        free = 0;
    s_cams[free].key = key;
    s_cams[free].yaw = 0.0f;
    s_cams[free].pitch = 0.0f;
    s_cams[free].aimYaw = 0.0f;
    s_cams[free].aimPitch = 0.0f;
    return free;
}

static f32 clampf(f32 v, f32 lo, f32 hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* An angle step degrees nearer 0 (0 once within it: the spring's tail). */
static f32 pull_back(f32 angle, f32 step)
{
    if (angle > step)
        return angle - step;
    if (angle < -step)
        return angle + step;
    return 0.0f;
}

static f32 dot3(const f32 *a, const f32 *b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* The orbit's axes in the world: right (the screen's), up (the screen's, the
 * side of the world's +y), back (from the pivot toward the eye). */
struct Orbit {
    f32 r[3], u[3], b[3];
};

/* v turned as the orbit turns: pitch degrees in the up / back plane (back
 * toward up), then yaw degrees in the right / back plane (back toward
 * right). */
static void orbit_turn(const Orbit &o, const f32 *v, f32 pitch, f32 yaw, f32 *out)
{
    const f32 p = pitch * kDegToRad, y = yaw * kDegToRad;
    const f32 a = dot3(v, o.r), c = dot3(v, o.u), e = dot3(v, o.b);
    const f32 c1 = c * cosf(p) + e * sinf(p);
    const f32 e1 = -c * sinf(p) + e * cosf(p);
    const f32 a2 = a * cosf(y) + e1 * sinf(y);
    const f32 e2 = -a * sinf(y) + e1 * cosf(y);
    for (int i = 0; i < 3; i++)
        out[i] = a2 * o.r[i] + c1 * o.u[i] + e2 * o.b[i];
}

/* A camera axis (-1 .. 1) with its dead zone, on an exponential curve (half
 * way about 0.18). */
static f32 curve(f32 v)
{
    if (v > 1.0f)
        v = 1.0f;
    if (v < -1.0f)
        v = -1.0f;
    const f32 a = fabsf(v);
    if (a <= FREECAM_DEAD_ZONE)
        return 0.0f;
    /* exponential (user request): fine near the middle, the full swing at
     * the stops */
    const f32 lin = (a - FREECAM_DEAD_ZONE) / (1.0f - FREECAM_DEAD_ZONE);
    const f32 curved = (expf(FREECAM_EXPO * lin) - 1.0f) / (expf(FREECAM_EXPO) - 1.0f);
    return v > 0.0f ? curved : -curved;
}

void pc_freecam_stick(int player, int wheel, int takenX, int takenY, f32 *stickX, f32 *stickY)
{
    float x, y;
    pad_pc::camera(player, wheel != 0, takenX != 0, takenY != 0, &x, &y);
    *stickX = -curve(x);                    /* (right: round the left side) */
    *stickY = -curve(y);                    /* (up +) */
}

f32 pc_freecam_chase_rest(void)
{
    return (f32)settings::chase_angle();
}

void pc_freecam_apply(const void *key, f32 *m, const f32 *pivot, f32 stickX, f32 stickY, PcGroundFn ground,
                      void *ctx, f32 groundDefault, f32 rest, int ownAxes)
{
    /* the stick turns the aim (its deflection the speed) against a pull back
     * behind that grows with the angle (user request): a held deflection
     * settles where the two meet, let go it returns - to the angle set
     * (`rest', the swing's rest: the aim is kept from it.  PC 2026-11-14: the
     * caller's - the chase camera's [Graphics] ChaseAngle, a custom
     * camera's own; user: "The existing chase cam angle setting should only
     * apply to the base chase cam.  The user-made ones get their own") */
    const int i = slot(key);
    /* (the swing stays within 45 degrees either way, or as far as a rest
     * angle beyond that - a custom camera's, up to 90) */
    const f32 pitchMax = fabsf(rest) > FREECAM_PITCH_MAX ? fabsf(rest) : FREECAM_PITCH_MAX;
    const f32 keep = expf(-FREECAM_PULL * FREECAM_TICK);
    s_cams[i].aimYaw = pull_back(s_cams[i].aimYaw * keep, 0.05f);
    s_cams[i].aimPitch = pull_back(s_cams[i].aimPitch * keep, 0.05f);
    s_cams[i].aimYaw = clampf(s_cams[i].aimYaw + stickX * FREECAM_YAW_RATE * FREECAM_TICK, -FREECAM_YAW_MAX,
                              FREECAM_YAW_MAX);
    s_cams[i].aimPitch = clampf(s_cams[i].aimPitch + stickY * FREECAM_PITCH_RATE * FREECAM_TICK,
                                -pitchMax - rest, pitchMax - rest);
    s_cams[i].yaw += (s_cams[i].aimYaw - s_cams[i].yaw) * FREECAM_EASE;
    s_cams[i].pitch += (s_cams[i].aimPitch - s_cams[i].pitch) * FREECAM_EASE;
    if (s_cams[i].aimYaw == 0.0f && fabsf(s_cams[i].yaw) < 0.05f)
        s_cams[i].yaw = 0.0f;
    if (s_cams[i].aimPitch == 0.0f && fabsf(s_cams[i].pitch) < 0.05f)
        s_cams[i].pitch = 0.0f;
    if (s_cams[i].yaw == 0.0f && s_cams[i].pitch == 0.0f && rest == 0.0f)
        return;                             /* behind: the camera's own view */

    /* the camera's axes in the world (the rotation's columns) and its eye
     * (t = -eye A) */
    f32 axis[3][3], len[3];
    for (int k = 0; k < 3; k++) {
        for (int j = 0; j < 3; j++)
            axis[k][j] = m[4 * j + k];
        len[k] = sqrtf(dot3(axis[k], axis[k]));
        if (len[k] <= 0.0f)
            return;
        for (int j = 0; j < 3; j++)
            axis[k][j] /= len[k];
    }
    f32 eye[3];
    for (int j = 0; j < 3; j++)
        eye[j] = -(m[12] * axis[0][j] / len[0] + m[13] * axis[1][j] / len[1] + m[14] * axis[2][j] / len[2]);

    Orbit o;
    const f32 upSign = axis[1][1] >= 0.0f ? 1.0f : -1.0f;
    f32 centre[3], d[3];
    for (int j = 0; j < 3; j++) {
        o.r[j] = axis[0][j];
        o.u[j] = axis[1][j] * upSign;
        centre[j] = pivot[j] + o.u[j] * FREECAM_PIVOT_UP;
        d[j] = eye[j] - centre[j];
    }
    /* PC (2026-11-14): with ownAxes the view's own back axis, whatever side
     * of the centre the eye is on: from the eye's side, an eye over the
     * centre (along ~0) flipped the swing tick by tick - a custom camera's
     * ANGLE twitching to its opposite on cars whose bumper eye sits over the
     * car's middle (scratchpad camreplay: 3 of the US demos' cars, 2,629
     * flips in 129,284 ticks; user report) */
    const f32 backSign = ownAxes ? 1.0f : (dot3(d, axis[2]) >= 0.0f ? 1.0f : -1.0f);
    for (int j = 0; j < 3; j++)
        o.b[j] = axis[2][j] * backSign;

    /* below the default height, the eye kept above the road: the pitch the
     * road lets it go down to (the eye's height grows with the pitch) */
    f32 pitch = s_cams[i].pitch + rest;
    const f32 yaw = s_cams[i].yaw;
    if (pitch < 0.0f && ground != NULL) {
        f32 lo = pitch, hi = 0.0f;
        for (int step = 0; step < 12; step++) {
            const f32 p = step == 0 ? pitch : 0.5f * (lo + hi);
            f32 e[3], g;
            orbit_turn(o, d, p, yaw, e);
            for (int j = 0; j < 3; j++)
                e[j] += centre[j];
            if (!ground(ctx, e[0], e[1], e[2], &g))
                g = groundDefault;
            const int clear = e[1] >= g + FREECAM_CLEARANCE;
            if (step == 0) {
                if (clear)
                    break;
                continue;
            }
            if (clear)
                hi = p;
            else
                lo = p;
            pitch = hi;
        }
        /* held at the road: the aim goes no lower (pushing on would only
         * have to be wound back) */
        const f32 held = pitch - rest;
        if (held > s_cams[i].pitch) {
            s_cams[i].pitch = held;
            if (s_cams[i].aimPitch < held)
                s_cams[i].aimPitch = held;
        }
    }

    /* the view swung: its axes and its eye turned round the centre */
    f32 newEye[3];
    orbit_turn(o, d, pitch, yaw, newEye);
    for (int j = 0; j < 3; j++)
        newEye[j] += centre[j];
    for (int k = 0; k < 3; k++) {
        f32 a[3];
        orbit_turn(o, axis[k], pitch, yaw, a);
        for (int j = 0; j < 3; j++)
            m[4 * j + k] = a[j] * len[k];
    }
    for (int k = 0; k < 3; k++)
        m[12 + k] = -(newEye[0] * m[k] + newEye[1] * m[4 + k] + newEye[2] * m[8 + k]);
}

/* PC (2026-11-07): the cockpit camera's look round (pc_freecam.h). */
void pc_freecam_look(const void *key, f32 *m, f32 stickX, f32 stickY)
{
    /* the aim as the orbit's: the stick turns it, the pull brings it back to
     * the front when let go (no rest angle: the front is straight ahead) */
    const int i = slot(key);
    const f32 keep = expf(-FREECAM_PULL * FREECAM_TICK);
    s_cams[i].aimYaw = pull_back(s_cams[i].aimYaw * keep, 0.05f);
    s_cams[i].aimPitch = pull_back(s_cams[i].aimPitch * keep, 0.05f);
    /* (the stick as pc_freecam_stick gives it, with the orbit's signs - user,
     * 2026-11-07: "the Y axis should match the chase cam look controls": x
     * negated, so a negative yaw turns the view's back toward its left, its
     * front toward its right: stick right looks right, as the orbit goes
     * round the left side; y up positive, and the pitch turns the back
     * toward up, the front down: stick up looks down, as the orbit's eye
     * rises over the car and looks down on it) */
    s_cams[i].aimYaw = clampf(s_cams[i].aimYaw + stickX * FREECAM_YAW_RATE * FREECAM_TICK, -LOOK_YAW_MAX, LOOK_YAW_MAX);
    s_cams[i].aimPitch = clampf(s_cams[i].aimPitch + stickY * FREECAM_PITCH_RATE * FREECAM_TICK, -LOOK_PITCH_MAX,
                                LOOK_PITCH_MAX);
    s_cams[i].yaw += (s_cams[i].aimYaw - s_cams[i].yaw) * FREECAM_EASE;
    s_cams[i].pitch += (s_cams[i].aimPitch - s_cams[i].pitch) * FREECAM_EASE;
    if (s_cams[i].aimYaw == 0.0f && fabsf(s_cams[i].yaw) < 0.05f)
        s_cams[i].yaw = 0.0f;
    if (s_cams[i].aimPitch == 0.0f && fabsf(s_cams[i].pitch) < 0.05f)
        s_cams[i].pitch = 0.0f;
    if (s_cams[i].yaw == 0.0f && s_cams[i].pitch == 0.0f)
        return;                             /* straight ahead: the camera's own view */

    /* the camera's axes in the world (the rotation's columns: right, up and
     * back - the projection looks down -z) and its eye (t = -eye A) */
    f32 axis[3][3], len[3];
    for (int k = 0; k < 3; k++) {
        for (int j = 0; j < 3; j++)
            axis[k][j] = m[4 * j + k];
        len[k] = sqrtf(dot3(axis[k], axis[k]));
        if (len[k] <= 0.0f)
            return;
        for (int j = 0; j < 3; j++)
            axis[k][j] /= len[k];
    }
    f32 eye[3];
    for (int j = 0; j < 3; j++)
        eye[j] = -(m[12] * axis[0][j] / len[0] + m[13] * axis[1][j] / len[1] + m[14] * axis[2][j] / len[2]);

    /* the axes turned about the eye: the pitch about the right axis, then
     * the yaw about the up axis (the world's +y side of it: looking up is
     * toward the sky whichever way the car leans) */
    Orbit o;
    const f32 upSign = axis[1][1] >= 0.0f ? 1.0f : -1.0f;
    for (int j = 0; j < 3; j++) {
        o.r[j] = axis[0][j];
        o.u[j] = axis[1][j] * upSign;
        o.b[j] = axis[2][j];
    }
    for (int k = 0; k < 3; k++) {
        f32 a[3];
        orbit_turn(o, axis[k], s_cams[i].pitch, s_cams[i].yaw, a);
        for (int j = 0; j < 3; j++)
            m[4 * j + k] = a[j] * len[k];
    }
    for (int k = 0; k < 3; k++)
        m[12 + k] = -(eye[0] * m[k] + eye[1] * m[4 + k] + eye[2] * m[8 + k]);
}
