/* debug_camera.cpp - the debug camera's switch and controls (debug_camera.h). */
#include "platform/debug_camera.h"
#include "platform/gamepad.h"
#include "platform/input_map.h"
#include "platform/log.h"
#include "platform/settings_menu.h"
#include "platform/system_menu.h"
#include "platform/window.h"

#include <atomic>
#include <math.h>
#include <stdint.h>
#include <string.h>

namespace debug_camera {
namespace {

using namespace input_map;

std::atomic<bool> g_inRace{false};
std::atomic<bool> g_active{false};
std::atomic<bool> g_locked{false};
bool g_lockHeld;                            /* the Lock control down at the last read (a press: its edge) */
/* the mouse wheel's notches and the mouse's movement seen (window::
 * mouse_wheel, mouse_motion) */
uint32_t g_wheelUp, g_wheelDown, g_motionX, g_motionY;
bool g_mouseSeen;

/* The profile's dead zones on the gamepad's raw state (as pad_pc's): each
 * stick's radially, the triggers' at the start of their travel. */
void apply_deadzones(PadSource *raw)
{
    for (int s = 0; s < 2; s++) {
        const float d = deadzone(kDebugCamera, s) / 100.0f;
        if (d <= 0.0f)
            continue;
        int16_t *x = &raw->axes[s * 2], *y = &raw->axes[s * 2 + 1];
        const float fx = *x / 32767.0f, fy = *y / 32767.0f;
        const float m = sqrtf(fx * fx + fy * fy);
        if (m <= d) {
            *x = *y = 0;
            continue;
        }
        const float k = apply_deadzone(m > 1.0f ? 1.0f : m, d) / m;
        *x = (int16_t)lroundf(fmaxf(-32768.0f, fminf(32767.0f, fx * k * 32767.0f)));
        *y = (int16_t)lroundf(fmaxf(-32768.0f, fminf(32767.0f, fy * k * 32767.0f)));
    }
    const float d = deadzone(kDebugCamera, 2) / 100.0f;
    if (d > 0.0f)
        for (int a = kPadAxisLeftTrigger; a <= kPadAxisRightTrigger; a++)
            raw->axes[a] = (int16_t)lroundf(apply_deadzone(raw->axes[a] / 32767.0f, d) * 32767.0f);
}

} // namespace

void race(bool on)
{
    g_inRace = on;
    g_active = false;
    g_locked = false;
}

void key()
{
    if (!g_inRace.load())
        return;
    const bool on = !g_active.load();
    g_locked = false;
    g_active = on;
    pclog::write("debug camera: %s", on ? "on" : "off");
    settings_menu::notice(on ? "DEBUG CAMERA ENABLED" : "DEBUG CAMERA DISABLED", 2.0);
}

bool active()
{
    return g_active.load(std::memory_order_relaxed);
}

bool holds_input()
{
    return g_active.load(std::memory_order_relaxed) && !g_locked.load(std::memory_order_relaxed);
}

bool wants_mouse()
{
    return holds_input() && !settings_menu::shown() && !system_menu::shown();
}

void read(Controls *out)
{
    memset(out, 0, sizeof(*out));
    uint32_t up, down, mx, my;
    window::mouse_wheel(&up, &down);
    window::mouse_motion(&mx, &my);
    const int notchUp = g_mouseSeen ? (int)(up - g_wheelUp) : 0, notchDown = g_mouseSeen ? (int)(down - g_wheelDown) : 0;
    const int dx = g_mouseSeen ? (int32_t)(mx - g_motionX) : 0, dy = g_mouseSeen ? (int32_t)(my - g_motionY) : 0;
    g_wheelUp = up;
    g_wheelDown = down;
    g_motionX = mx;
    g_motionY = my;
    g_mouseSeen = true;
    if (settings_menu::shown() || system_menu::shown())
        return;
    const Profile &p = profile(kDebugCamera);
    PadSource raw;
    const bool pad = gamepad::read_raw(0, &raw);
    if (pad)
        apply_deadzones(&raw);
    const Sources s = {pad ? &raw : nullptr, nullptr, window::key_down, 0};
    /* a control: the larger of its slots' (a mouse wheel binding: its
     * notches, counted in *steps; the mouse's movement: its counts that way,
     * in *counts) */
    auto get = [&](int c, int *steps, int *counts = nullptr) {
        float v = 0.0f;
        for (int slot = 0; slot < kSlots; slot++) {
            const Binding &b = p.map[c][slot];
            if (b.source == Source::mouse_wheel) {
                if (steps != nullptr)
                    *steps += b.dir > 0 ? notchUp : notchDown;
                continue;
            }
            if (b.source == Source::mouse_motion) {
                const int d = (b.index == 0 ? dx : dy) * (b.dir < 0 ? -1 : 1);
                if (counts != nullptr && d > 0)
                    *counts += d;
                continue;
            }
            v = fmaxf(v, value(b, s));
        }
        return v;
    };
    /* the lock: a press each way (the players' input back, the camera held) */
    const bool lockHeld = get(DEBUG_LOCK, nullptr) >= 0.5f;
    if (lockHeld && !g_lockHeld) {
        const bool locked = !g_locked.load();
        g_locked = locked;
        pclog::write("debug camera: %s", locked ? "locked (the players drive)" : "unlocked");
        settings_menu::notice(locked ? "DEBUG CAMERA LOCKED" : "DEBUG CAMERA UNLOCKED", 2.0);
    }
    g_lockHeld = lockHeld;
    if (g_locked.load())
        return;
    int lookR = 0, lookL = 0, lookU = 0, lookD = 0;
    out->lookRight = get(DEBUG_LOOK_RIGHT, nullptr, &lookR) - get(DEBUG_LOOK_LEFT, nullptr, &lookL);
    out->lookUp = get(DEBUG_LOOK_UP, nullptr, &lookU) - get(DEBUG_LOOK_DOWN, nullptr, &lookD);
    out->lookRightCounts = lookR - lookL;
    out->lookUpCounts = lookU - lookD;
    int fasterSteps = 0, slowerSteps = 0, fovUpSteps = 0, fovDownSteps = 0;
    out->forward = get(DEBUG_FORWARD, nullptr) - get(DEBUG_BACK, nullptr);
    out->right = get(DEBUG_RIGHT, nullptr) - get(DEBUG_LEFT, nullptr);
    out->up = get(DEBUG_UP, nullptr) - get(DEBUG_DOWN, nullptr);
    out->faster = get(DEBUG_FASTER, &fasterSteps) - get(DEBUG_SLOWER, &slowerSteps);
    out->fov = get(DEBUG_FOV_UP, &fovUpSteps) - get(DEBUG_FOV_DOWN, &fovDownSteps);
    out->fasterSteps = fasterSteps - slowerSteps;
    out->fovSteps = fovUpSteps - fovDownSteps;
}

void notice(const char *text, double seconds)
{
    settings_menu::notice(text, seconds);
}

} // namespace debug_camera
