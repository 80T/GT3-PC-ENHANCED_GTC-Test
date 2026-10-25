/* pad_pc.cpp - the controller library of GT3 PC (Sony's libpad API).
 *
 * On the PS2, libpad talked to the padman IOP module, which polled the
 * controllers over SIO2 and DMA'd their answers to the EE; the engine's pad
 * thread (engine/input T477) then set the DualShock mode, pressure mode and
 * motors, and read the 32-byte answer of each pad.  Here the answers come
 * from the PC: each port (slot 0) is a DualShock 2 in pressure mode, built
 * through the mapping of gt3pc.ini [Controller1] / [Controller2]
 * (platform/input_map.h, the launcher's Controls tab) from the port's game
 * controller (platform/gamepad.h, SDL3) and the keyboard and mouse, their
 * values combined.  Port 1 is always plugged in (by default it has the
 * keyboard); port 2 while its controller is connected, or always when
 * [Controller2] maps keys.  The game's motors drive the controller's rumble.
 *
 * Controller 1's default keys:
 *   arrows        D-pad (menus; steering in a race)
 *   Z / X / A / S Cross / Circle / Square / Triangle
 *   Q / W         L1 / R1        E / R   L2 / R2
 *   Enter         Start          Backspace  Select
 *   numpad 4/6/8/2  left stick   numpad 7/9   L3 / R3
 *   J / L / I / K right stick (left / right / up / down); also the chase
 *                 camera's controls (its free orbit, game/app/pc_freecam.h:
 *                 camera, not the DualShock 2's), with the right stick
 * A button from an analog source (a trigger, a stick) is pressed from
 * halfway (a trigger from the slightest press), its travel the pressure;
 * the sticks' and triggers' dead zones apply first.
 *
 * The answer (mode id 0x79, DualShock 2 with pressure): byte 0 = 0 (valid),
 * 1 = 0x79, 2..3 = the buttons, active low (byte 2: Select, L3, R3, Start,
 * Up, Right, Down, Left; byte 3: L2, R2, L1, R1, Triangle, Circle, Cross,
 * Square), 4..7 = right stick x / y, left stick x / y (0x80 centred), 8..19
 * = the pressures of Right, Left, Up, Down, Triangle, Circle, Cross, Square,
 * L1, R1, L2, R2 (0xFF pressed). */
#include "libpad.h"
#include "platform/debug_camera.h"
#include "platform/gamepad.h"
#include "platform/input_map.h"
#include "platform/pad_pc.h"
#include "platform/settings_menu.h"
#include "platform/system_menu.h"
#include "platform/window.h"

#include <windows.h>
#include <math.h>
#include <string.h>

#include <algorithm>

namespace {

using namespace input_map;

enum {
    MODE_DUALSHOCK2 = 0x79,         /* 7 = DualShock, 9 halfwords */
    TYPE_DIGITAL = 4,
    TYPE_DUALSHOCK = 7
};

bool g_open[2];
/* scePadSetActAlign: which motor each byte of scePadSetActDirect drives
 * (0 small, 1 large, 0xFF none). */
unsigned char g_actAlign[2][6] = {{0, 1, 0xFF, 0xFF, 0xFF, 0xFF}, {0, 1, 0xFF, 0xFF, 0xFF, 0xFF}};

/* The port's profile maps a key or the mouse (port 2 is then always
 * plugged in). */
bool maps_input(int port)
{
    const Profile &p = profile(kController1 + port);
    for (int c = 0; c < kPadControlCount; c++)
        if (p.map[c][1].source != Source::none)
            return true;
    return false;
}

bool connected(int port, int slot)
{
    if (port < 0 || port > 1 || slot != 0 || !g_open[port])
        return false;
    PadSource raw;
    return port == 0 || gamepad::read_raw(port, &raw) || maps_input(port);   /* port 1 has the keyboard */
}

/* The buttons of the answer (gamepad.h's bits: bytes 2..3, active high) and
 * their pressure index (-1 none), of PAD_UP .. PAD_START. */
struct ButtonOut {
    int bit;
    int pressure;
};
const ButtonOut kButtonOut[PAD_START + 1] = {
    {4, 2}, {6, 3}, {7, 1}, {5, 0},         /* Up, Down, Left, Right */
    {14, 6}, {13, 5}, {15, 7}, {12, 4},     /* Cross, Circle, Square, Triangle */
    {10, 8}, {11, 9}, {8, 10}, {9, 11},     /* L1, R1, L2, R2 */
    {1, -1}, {2, -1},                       /* L3, R3 */
    {0, -1}, {3, -1},                       /* Select, Start */
};
/* The sticks: the answer's byte and its - / + controls (right x, right y,
 * left x, left y). */
struct StickOut {
    int byte, minus, plus;
};
const StickOut kStickOut[4] = {
    {4, PAD_RSTICK_LEFT, PAD_RSTICK_RIGHT}, {5, PAD_RSTICK_UP, PAD_RSTICK_DOWN},
    {6, PAD_LSTICK_LEFT, PAD_LSTICK_RIGHT}, {7, PAD_LSTICK_UP, PAD_LSTICK_DOWN},
};
/* A trigger counts as pressed from this pressure on (gamepad.cpp's). */
constexpr float kTriggerPress = 16.0f / 255.0f;

/* pad_pc::hold_reads, and the taps of this read (pad_pc::take_taps): bit c
 * = control c */
bool g_hold;
uint32_t g_taps[2];
/* the mouse wheel's notches seen (window::mouse_wheel) */
uint32_t g_wheelUp, g_wheelDown;

int16_t to_axis(float v)
{
    const float a = v * 32767.0f;
    return (int16_t)(a > 32767.0f ? 32767 : a < -32768.0f ? -32768 : lroundf(a));
}

/* The controller's dead zones (input_map::deadzone, percent): each stick's
 * radially, the triggers' at the start of their travel. */
void apply_deadzones(int port, PadSource *raw)
{
    const int prof = kController1 + port;
    for (int s = 0; s < 2; s++) {
        const float d = deadzone(prof, s) / 100.0f;
        if (d <= 0.0f)
            continue;
        int16_t *x = &raw->axes[s * 2], *y = &raw->axes[s * 2 + 1];
        const float fx = *x / 32767.0f, fy = *y / 32767.0f;
        float m = sqrtf(fx * fx + fy * fy);
        if (m <= d) {
            *x = *y = 0;
            continue;
        }
        const float k = apply_deadzone(m > 1.0f ? 1.0f : m, d) / m;
        *x = to_axis(fx * k);
        *y = to_axis(fy * k);
    }
    const float d = deadzone(prof, 2) / 100.0f;
    if (d > 0.0f)
        for (int a = kPadAxisLeftTrigger; a <= kPadAxisRightTrigger; a++)
            raw->axes[a] = to_axis(apply_deadzone(raw->axes[a] / 32767.0f, d));
}

/* A control's value: the larger of its slots'; analog: from an axis. */
float control_value(const Profile &p, int c, const Sources &s, const Binding **from)
{
    float v = 0.0f;
    *from = nullptr;
    for (int slot = 0; slot < kSlots; slot++) {
        const float x = value(p.map[c][slot], s);
        if (x > v || *from == nullptr) {
            v = x > v ? x : v;
            *from = &p.map[c][slot];
        }
    }
    return v;
}

void fill_answer(int port, unsigned char *d)
{
    memset(d, 0, 32);
    d[0] = 0;
    d[1] = MODE_DUALSHOCK2;
    d[2] = 0xFF;
    d[3] = 0xFF;
    d[4] = d[5] = d[6] = d[7] = 0x80;
    if (settings_menu::shown() || system_menu::shown() || debug_camera::holds_input())
        return;                     /* the settings / Esc menu (or the debug camera, unlocked) has the keyboard and
                                       the controllers: nothing pressed */
    const Profile &p = profile(kController1 + port);
    PadSource raw;
    const bool pad = gamepad::read_raw(port, &raw);
    if (pad)
        apply_deadzones(port, &raw);
    Sources s = {pad ? &raw : nullptr, nullptr, window::key_down, 0};
    for (int c = 0; c <= PAD_START; c++) {
        const Binding *from;
        const float v = control_value(p, c, s, &from);
        /* an analog source from halfway (a trigger from its slightest
         * press), its travel the pressure */
        const bool analog = v > 0.0f && is_analog(*from);
        const bool trigger = analog && from->source == Source::pad_axis &&
                             (from->index == kPadAxisLeftTrigger || from->index == kPadAxisRightTrigger);
        const bool tap = (g_taps[port] >> c) & 1;
        if (!(v >= (trigger ? kTriggerPress : 0.5f)) && !tap)
            continue;
        const ButtonOut &o = kButtonOut[c];
        d[2 + (o.bit >> 3)] &= (unsigned char)~(1 << (o.bit & 7));
        if (o.pressure >= 0) {
            const long pr = analog ? lroundf(v * 255.0f) : 0xFF;
            d[8 + o.pressure] = (unsigned char)(pr < 1 ? 0xFF : pr > 0xFF ? 0xFF : pr);   /* (a tap: full) */
        }
    }
    for (const StickOut &o : kStickOut) {
        const Binding *from;
        const float v = control_value(p, o.plus, s, &from) - control_value(p, o.minus, s, &from);
        const long b = lroundf(127.5f + v * 127.5f);
        d[o.byte] = (unsigned char)(b < 0 ? 0 : b > 0xFF ? 0xFF : b);
    }
}

} // namespace

namespace pad_pc {

void hold_reads(bool on)
{
    g_hold = on;
}

bool holding()
{
    return g_hold;
}

void take_taps()
{
    /* the keys each taken once (a key mapped twice taps both) */
    bool taken[256] = {}, pressed[256] = {};
    uint32_t up, down;
    window::mouse_wheel(&up, &down);
    const bool notchUp = up != g_wheelUp, notchDown = down != g_wheelDown;
    g_wheelUp = up;
    g_wheelDown = down;
    for (int port = 0; port < 2; port++) {
        const Profile &p = profile(kController1 + port);
        const uint32_t padPressed = gamepad::take_pressed_raw(port);
        g_taps[port] = 0;
        for (int c = 0; c <= PAD_START; c++)
            for (int slot = 0; slot < kSlots; slot++) {
                const Binding &b = p.map[c][slot];
                bool tap = false;
                if (b.source == Source::pad_button) {
                    tap = b.index < 32 && ((padPressed >> b.index) & 1);
                } else if (b.source == Source::key) {
                    if (!taken[b.index]) {
                        taken[b.index] = true;
                        pressed[b.index] = window::take_key_press(b.index);
                    }
                    tap = pressed[b.index];
                } else if (b.source == Source::mouse_wheel) {
                    tap = b.dir > 0 ? notchUp : notchDown;
                }
                if (tap)
                    g_taps[port] |= 1u << c;
            }
    }
    if (settings_menu::shown() || system_menu::shown() || debug_camera::holds_input())
        g_taps[0] = g_taps[1] = 0;  /* the settings / Esc menu's, the debug camera's (taken, dropped) */
}

void camera(int port, bool wheel, bool skipX, bool skipY, float *x, float *y)
{
    *x = *y = 0.0f;
    if (port < 0 || port > 1 || settings_menu::shown() || system_menu::shown())
        return;
    const int prof = (wheel ? kWheel1 : kController1) + port;
    const Profile &p = profile(prof);
    PadSource raw;
    gamepad::Joystick joy;
    JoySource js = {};
    Sources s = {nullptr, nullptr, window::key_down, 0};
    if (wheel) {
        if (gamepad::read_wheel(port, &joy)) {
            js = {joy.buttonCount, joy.axisCount, joy.hatCount, joy.buttons, joy.axes, joy.hats};
            s.joy = &js;
        }
    } else if (gamepad::read_raw(port, &raw)) {
        apply_deadzones(port, &raw);
        s.pad = &raw;
    }
    /* a control's value, the larger of its slots'; with skip, not from a
     * binding the right stick's controls on that axis have too */
    auto get = [&](int c, bool skip, int stickMinus, int stickPlus) {
        const int sticks[2] = {stickMinus, stickPlus};
        float v = 0.0f;
        for (int slot = 0; slot < kSlots; slot++) {
            const Binding &b = p.map[c][slot];
            bool taken = false;
            for (int i = 0; skip && !wheel && i < 2; i++)
                for (int ss = 0; ss < kSlots; ss++) {
                    const Binding &o = p.map[sticks[i]][ss];
                    taken = taken || (b.source != Source::none && b.source == o.source && b.index == o.index &&
                                      b.dir == o.dir);
                }
            if (!taken)
                v = std::max(v, value(b, s));
        }
        return v;
    };
    const int cam = camera_control(prof);
    *x = get(cam + 1, skipX, PAD_RSTICK_LEFT, PAD_RSTICK_RIGHT) - get(cam + 0, skipX, PAD_RSTICK_LEFT, PAD_RSTICK_RIGHT);
    *y = get(cam + 3, skipY, PAD_RSTICK_UP, PAD_RSTICK_DOWN) - get(cam + 2, skipY, PAD_RSTICK_UP, PAD_RSTICK_DOWN);
}

} // namespace pad_pc

extern "C" {

int scePadInit(int mode)
{
    (void)mode;
    return 1;
}

int scePadEnd(void)
{
    g_open[0] = g_open[1] = false;
    return 1;
}

int scePadPortOpen(int port, int slot, u128 *addr)
{
    (void)addr;
    if (port < 0 || port > 1 || slot != 0)
        return 0;
    g_open[port] = true;
    return 1;
}

int scePadPortClose(int port, int slot)
{
    if (port < 0 || port > 1 || slot != 0)
        return 0;
    g_open[port] = false;
    return 1;
}

/* The answer's length (32), 0 when nothing answers. */
int scePadRead(int port, int slot, unsigned char *rdata)
{
    if (!connected(port, slot))
        return 0;
    fill_answer(port, rdata);
    return 32;
}

int scePadGetState(int port, int slot)
{
    return connected(port, slot) ? scePadStateStable : scePadStateDiscon;
}

int scePadSetReqState(int port, int slot, int state)
{
    (void)port; (void)slot; (void)state;
    return 1;
}

int scePadGetReqState(int port, int slot)
{
    (void)port; (void)slot;
    return scePadReqStateComplete;
}

/* Two motors: the small one (on / off) and the large one (0..255). */
int scePadInfoAct(int port, int slot, int actno, int term)
{
    if (!connected(port, slot))
        return 0;
    if (actno < 0)
        return 2;
    switch (term) {
    case InfoActFunc: return 1;
    case InfoActSub:  return actno == 0 ? 0 : 1;
    case InfoActSize: return actno == 0 ? 0 : 1;
    case InfoActCurr: return actno == 0 ? 10 : 20;
    }
    return 0;
}

/* Modes: digital (4) and DualShock (7); the current one is DualShock. */
int scePadInfoMode(int port, int slot, int term, int offs)
{
    if (!connected(port, slot))
        return 0;
    switch (term) {
    case InfoModeCurID:
        return TYPE_DUALSHOCK;
    case InfoModeCurExID:
        return TYPE_DUALSHOCK;
    case InfoModeCurExOffs:
        return 1;
    case InfoModeIdTable:
        if (offs == -1)
            return 2;
        return offs == 0 ? TYPE_DIGITAL : offs == 1 ? TYPE_DUALSHOCK : 0;
    }
    return 0;
}

int scePadSetMainMode(int port, int slot, int offs, int lock)
{
    (void)offs; (void)lock;
    return connected(port, slot) ? 1 : 0;
}

/* Vibration: the motors of the port's game controller (none for the
 * keyboard).  Byte i of `data` drives the motor g_actAlign[port][i]. */
int scePadSetActDirect(int port, int slot, const unsigned char *data)
{
    if (!connected(port, slot))
        return 0;
    int small = 0, large = 0;
    for (int i = 0; i < 6; i++) {
        if (g_actAlign[port][i] == 0)
            small = data[i];
        else if (g_actAlign[port][i] == 1)
            large = data[i];
    }
    gamepad::set_motors(port, small, large);
    return 1;
}

int scePadSetActAlign(int port, int slot, const unsigned char *data)
{
    if (!connected(port, slot))
        return 0;
    memcpy(g_actAlign[port], data, 6);
    return 1;
}

int scePadSetButtonInfo(int port, int slot, int info)
{
    (void)info;
    return connected(port, slot) ? 1 : 0;
}

int scePadInfoPressMode(int port, int slot)
{
    return connected(port, slot) ? 1 : 0;
}

int scePadEnterPressMode(int port, int slot)
{
    return connected(port, slot) ? 1 : 0;
}

int scePadGetModVersion(void)
{
    return 0x0200;
}

/* padman command 0x14 (? its meaning; GT3 sends 0 at start-up). */
int pad_rpc_cmd_0x14(int arg)
{
    (void)arg;
    return 1;
}

} // extern "C"
