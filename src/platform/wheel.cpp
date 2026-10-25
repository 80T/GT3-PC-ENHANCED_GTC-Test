/* wheel.cpp - the USB steering wheels of GT3 PC (wheel.h). */
#include "platform/wheel.h"
#include "platform/debug_camera.h"
#include "platform/gamepad.h"
#include "platform/input_map.h"
#include "platform/settings_menu.h"
#include "platform/system_menu.h"
#include "platform/window.h"

#include <math.h>
#include <string.h>

namespace wheel {
namespace {

using namespace input_map;

float control_value(const Profile &p, int c, const Sources &s)
{
    float v = 0.0f;
    for (int slot = 0; slot < kSlots; slot++) {
        const float x = value(p.map[c][slot], s);
        if (x > v)
            v = x;
    }
    return v;
}

/* the mouse wheel's notches (window::mouse_wheel) since the last read */
uint32_t g_wheelUp[2], g_wheelDown[2];

} // namespace

bool read(int port, State *out)
{
    gamepad::Joystick joy;
    if (port < 0 || port > 1 || !gamepad::read_wheel(port, &joy))
        return false;
    memset(out, 0, sizeof(*out));
    out->x = 0x200;
    out->hat = 8;
    if (settings_menu::shown() || system_menu::shown() || debug_camera::holds_input())
        return true;                        /* the menus (or the debug camera, unlocked) have the input: at rest */
    const int prof = kWheel1 + port;
    const Profile &p = profile(prof);
    const JoySource js = {joy.buttonCount, joy.axisCount, joy.hatCount, joy.buttons, joy.axes, joy.hats};
    uint32_t up, down;
    window::mouse_wheel(&up, &down);
    const int notches = (up != g_wheelUp[port] ? 1 : 0) | (down != g_wheelDown[port] ? 2 : 0);
    g_wheelUp[port] = up;
    g_wheelDown[port] = down;
    const Sources s = {nullptr, &js, window::key_down, notches};

    /* the steering: right - left, its dead zone about the centre */
    float steer = control_value(p, WHEEL_STEER_RIGHT, s) - control_value(p, WHEEL_STEER_LEFT, s);
    const float mag = apply_deadzone(fabsf(steer), deadzone(prof, 0) / 100.0f);
    steer = steer < 0.0f ? -mag : mag;
    out->x = 0x200 + (int)lroundf(steer * 0x1FF);
    out->gas = (int)lroundf(apply_deadzone(control_value(p, WHEEL_GAS, s), deadzone(prof, 1) / 100.0f) * 255.0f);
    out->brake = (int)lroundf(apply_deadzone(control_value(p, WHEEL_BRAKE, s), deadzone(prof, 2) / 100.0f) * 255.0f);
    for (int i = 0; i < kWheelButtons; i++)
        out->buttons[i] = control_value(p, WHEEL_CROSS + i, s) >= 0.5f ? 1 : 0;
    /* the D-pad as a hat: 0 up, clockwise */
    const bool u = control_value(p, WHEEL_UP, s) >= 0.5f, d = control_value(p, WHEEL_DOWN, s) >= 0.5f;
    const bool l = control_value(p, WHEEL_LEFT, s) >= 0.5f, r = control_value(p, WHEEL_RIGHT, s) >= 0.5f;
    const int dy = (d ? 1 : 0) - (u ? 1 : 0), dx = (r ? 1 : 0) - (l ? 1 : 0);
    static const int kHat[3][3] = {{7, 0, 1}, {6, 8, 2}, {5, 4, 3}};    /* [dy + 1][dx + 1] */
    out->hat = kHat[dy + 1][dx + 1];
    return true;
}

bool connected(int port)
{
    gamepad::Joystick joy;
    return port >= 0 && port <= 1 && gamepad::read_wheel(port, &joy);
}

bool preferred(int port)
{
    return !plays_with_controller(port);
}

} // namespace wheel
