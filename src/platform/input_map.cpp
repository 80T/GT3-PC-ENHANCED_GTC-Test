/* input_map.cpp - the control mappings of GT3 PC (input_map.h). */
#include "platform/input_map.h"

#include <windows.h>

#include <atomic>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

namespace input_map {
namespace {

const char *const kSections[kProfileCount] = {"Controller1", "Controller2", "Wheel1", "Wheel2", "DebugCamera"};
const char *const kNames[kProfileCount] = {"Controller 1", "Controller 2", "Wheel 1", "Wheel 2", "Debug camera"};

const ControlInfo kPadControls[kPadControlCount] = {
    {"Up", "D-pad up", ControlKind::button},
    {"Down", "D-pad down", ControlKind::button},
    {"Left", "D-pad left", ControlKind::button},
    {"Right", "D-pad right", ControlKind::button},
    {"Cross", "Cross", ControlKind::button},
    {"Circle", "Circle", ControlKind::button},
    {"Square", "Square", ControlKind::button},
    {"Triangle", "Triangle", ControlKind::button},
    {"L1", "L1", ControlKind::button},
    {"R1", "R1", ControlKind::button},
    {"L2", "L2", ControlKind::button},
    {"R2", "R2", ControlKind::button},
    {"L3", "L3 (left stick click)", ControlKind::button},
    {"R3", "R3 (right stick click)", ControlKind::button},
    {"Select", "Select", ControlKind::button},
    {"Start", "Start", ControlKind::button},
    {"LeftStickUp", "Left stick up", ControlKind::direction},
    {"LeftStickDown", "Left stick down", ControlKind::direction},
    {"LeftStickLeft", "Left stick left", ControlKind::direction},
    {"LeftStickRight", "Left stick right", ControlKind::direction},
    {"RightStickUp", "Right stick up", ControlKind::direction},
    {"RightStickDown", "Right stick down", ControlKind::direction},
    {"RightStickLeft", "Right stick left", ControlKind::direction},
    {"RightStickRight", "Right stick right", ControlKind::direction},
    {"CameraLeft", "Chase cam left", ControlKind::direction},
    {"CameraRight", "Chase cam right", ControlKind::direction},
    {"CameraUp", "Chase cam up", ControlKind::direction},
    {"CameraDown", "Chase cam down", ControlKind::direction},
};

const ControlInfo kWheelControls[kWheelControlCount] = {
    {"SteerLeft", "Steer left", ControlKind::direction},
    {"SteerRight", "Steer right", ControlKind::direction},
    {"Gas", "Gas pedal", ControlKind::pedal},
    {"Brake", "Brake pedal", ControlKind::pedal},
    {"Cross", "Cross", ControlKind::button},
    {"Square", "Square", ControlKind::button},
    {"Circle", "Circle", ControlKind::button},
    {"Triangle", "Triangle", ControlKind::button},
    {"R1", "R1", ControlKind::button},
    {"L1", "L1", ControlKind::button},
    {"R2", "R2", ControlKind::button},
    {"L2", "L2", ControlKind::button},
    {"Select", "Select", ControlKind::button},
    {"Start", "Start", ControlKind::button},
    {"R3", "R3", ControlKind::button},
    {"L3", "L3", ControlKind::button},
    {"Up", "D-pad up", ControlKind::button},
    {"Down", "D-pad down", ControlKind::button},
    {"Left", "D-pad left", ControlKind::button},
    {"Right", "D-pad right", ControlKind::button},
    {"CameraLeft", "Chase cam left", ControlKind::direction},
    {"CameraRight", "Chase cam right", ControlKind::direction},
    {"CameraUp", "Chase cam up", ControlKind::direction},
    {"CameraDown", "Chase cam down", ControlKind::direction},
};

const ControlInfo kDebugControls[kDebugControlCount] = {
    {"Forward", "Move forward", ControlKind::direction},
    {"Back", "Move back", ControlKind::direction},
    {"Left", "Move left", ControlKind::direction},
    {"Right", "Move right", ControlKind::direction},
    {"Up", "Move up", ControlKind::direction},
    {"Down", "Move down", ControlKind::direction},
    {"LookUp", "Look up", ControlKind::direction},
    {"LookDown", "Look down", ControlKind::direction},
    {"LookLeft", "Look left", ControlKind::direction},
    {"LookRight", "Look right", ControlKind::direction},
    {"Faster", "Move speed up", ControlKind::button},
    {"Slower", "Move speed down", ControlKind::button},
    {"FovUp", "Field of view up", ControlKind::button},
    {"FovDown", "Field of view down", ControlKind::button},
    {"Lock", "Lock camera (drive)", ControlKind::button},
};

const char *const kPadDeadzoneKeys[kDeadzones] = {"LeftStickDeadzone", "RightStickDeadzone", "TriggerDeadzone"};
const char *const kPadDeadzoneNames[kDeadzones] = {"Left stick dead zone", "Right stick dead zone",
                                                   "Trigger dead zone"};
const char *const kWheelDeadzoneKeys[kDeadzones] = {"SteeringDeadzone", "GasDeadzone", "BrakeDeadzone"};
const char *const kWheelDeadzoneNames[kDeadzones] = {"Steering dead zone", "Gas pedal dead zone",
                                                     "Brake pedal dead zone"};

/* SDL3's gamepad buttons and axes: the ini names (SDL_GetGamepadStringForButton's)
 * and the labels. */
const char *const kPadButtonNames[kPadButtons] = {
    "south", "east", "west", "north", "back", "guide", "start", "leftstick", "rightstick", "leftshoulder",
    "rightshoulder", "dpup", "dpdown", "dpleft", "dpright", "misc1", "paddle1", "paddle2", "paddle3", "paddle4",
    "touchpad", "misc2", "misc3", "misc4", "misc5", "misc6"};
const char *const kPadButtonLabels[kPadButtons] = {
    "South (A / Cross)", "East (B / Circle)", "West (X / Square)", "North (Y / Triangle)", "Back / Share",
    "Guide", "Start / Options", "Left stick click", "Right stick click", "Left shoulder (LB / L1)",
    "Right shoulder (RB / R1)", "D-pad up", "D-pad down", "D-pad left", "D-pad right", "Misc (Share / Mic)",
    "Right paddle 1", "Left paddle 1", "Right paddle 2", "Left paddle 2", "Touchpad", "Misc 2", "Misc 3",
    "Misc 4", "Misc 5", "Misc 6"};
const char *const kPadAxisNames[kPadAxes] = {"leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger"};
/* [axis][0 the - half, 1 the + half] */
const char *const kPadAxisLabels[kPadAxes][2] = {
    {"Left stick left", "Left stick right"},   {"Left stick up", "Left stick down"},
    {"Right stick left", "Right stick right"}, {"Right stick up", "Right stick down"},
    {"Left trigger -", "Left trigger (LT / L2)"}, {"Right trigger -", "Right trigger (RT / R2)"}};

const char *const kHatNames[4] = {"up", "right", "down", "left"};      /* SDL_HAT_* bits 0..3 */

struct KeyName {
    uint8_t vk;
    const char *name;
};
/* Besides A..Z and 0..9 (their own letters) */
const KeyName kKeyNames[] = {
    {VK_BACK, "Backspace"}, {VK_TAB, "Tab"}, {VK_RETURN, "Enter"}, {VK_SHIFT, "Shift"}, {VK_CONTROL, "Ctrl"},
    {VK_MENU, "Alt"}, {VK_PAUSE, "Pause"}, {VK_CAPITAL, "CapsLock"}, {VK_ESCAPE, "Escape"}, {VK_SPACE, "Space"},
    {VK_PRIOR, "PageUp"}, {VK_NEXT, "PageDown"}, {VK_END, "End"}, {VK_HOME, "Home"}, {VK_LEFT, "Left"},
    {VK_UP, "Up"}, {VK_RIGHT, "Right"}, {VK_DOWN, "Down"}, {VK_SNAPSHOT, "PrintScreen"}, {VK_INSERT, "Insert"},
    {VK_DELETE, "Delete"}, {VK_LWIN, "LWin"}, {VK_RWIN, "RWin"}, {VK_APPS, "Apps"},
    {VK_NUMPAD0, "Numpad0"}, {VK_NUMPAD1, "Numpad1"}, {VK_NUMPAD2, "Numpad2"}, {VK_NUMPAD3, "Numpad3"},
    {VK_NUMPAD4, "Numpad4"}, {VK_NUMPAD5, "Numpad5"}, {VK_NUMPAD6, "Numpad6"}, {VK_NUMPAD7, "Numpad7"},
    {VK_NUMPAD8, "Numpad8"}, {VK_NUMPAD9, "Numpad9"}, {VK_MULTIPLY, "NumpadMultiply"}, {VK_ADD, "NumpadAdd"},
    {VK_SEPARATOR, "NumpadSeparator"}, {VK_SUBTRACT, "NumpadSubtract"}, {VK_DECIMAL, "NumpadDecimal"},
    {VK_DIVIDE, "NumpadDivide"}, {VK_NUMLOCK, "NumLock"}, {VK_SCROLL, "ScrollLock"}, {VK_LSHIFT, "LShift"},
    {VK_RSHIFT, "RShift"}, {VK_LCONTROL, "LCtrl"}, {VK_RCONTROL, "RCtrl"}, {VK_LMENU, "LAlt"}, {VK_RMENU, "RAlt"},
    {VK_OEM_1, "Semicolon"}, {VK_OEM_PLUS, "Equals"}, {VK_OEM_COMMA, "Comma"}, {VK_OEM_MINUS, "Minus"},
    {VK_OEM_PERIOD, "Period"}, {VK_OEM_2, "Slash"}, {VK_OEM_3, "Grave"}, {VK_OEM_4, "LBracket"},
    {VK_OEM_5, "Backslash"}, {VK_OEM_6, "RBracket"}, {VK_OEM_7, "Apostrophe"}, {VK_OEM_102, "Oem102"},
};
/* The mouse's buttons (Source::key on their virtual keys) */
const KeyName kMouseNames[] = {
    {VK_LBUTTON, "left"}, {VK_RBUTTON, "right"}, {VK_MBUTTON, "middle"}, {VK_XBUTTON1, "x1"}, {VK_XBUTTON2, "x2"},
};
const char *const kMouseLabels[] = {"Mouse left", "Mouse right", "Mouse middle", "Mouse X1", "Mouse X2"};

int mouse_index(int vk)
{
    for (int i = 0; i < (int)(sizeof(kMouseNames) / sizeof(kMouseNames[0])); i++)
        if (kMouseNames[i].vk == vk)
            return i;
    return -1;
}

/* the key's ini name ("" if it has none of its own: 0xNN) */
void key_name(int vk, char *out, size_t n)
{
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        snprintf(out, n, "%c", (char)vk);
        return;
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        snprintf(out, n, "F%d", vk - VK_F1 + 1);
        return;
    }
    for (const KeyName &k : kKeyNames)
        if (k.vk == vk) {
            snprintf(out, n, "%s", k.name);
            return;
        }
    snprintf(out, n, "0x%02X", vk);
}

int parse_key(const char *s)
{
    if (s[0] != '\0' && s[1] == '\0') {
        const char c = (char)toupper((unsigned char)s[0]);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            return c;
    }
    if ((s[0] == 'F' || s[0] == 'f') && s[1] >= '1' && s[1] <= '9') {
        char *end;
        const long f = strtol(s + 1, &end, 10);
        if (*end == '\0' && f >= 1 && f <= 24)
            return VK_F1 + (int)f - 1;
    }
    for (const KeyName &k : kKeyNames)
        if (_stricmp(k.name, s) == 0)
            return k.vk;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        char *end;
        const long v = strtol(s + 2, &end, 16);
        if (*end == '\0' && v > 0 && v < 256)
            return (int)v;
    }
    return -1;
}

/* "<name><n>" -> n (0..max-1), the rest after it in *rest */
bool parse_numbered(const char *s, const char *name, int max, int *n, const char **rest)
{
    const size_t len = strlen(name);
    if (_strnicmp(s, name, len) != 0 || s[len] < '0' || s[len] > '9')
        return false;
    char *end;
    const long v = strtol(s + len, &end, 10);
    if (v < 0 || v >= max)
        return false;
    *n = (int)v;
    *rest = end;
    return true;
}

/* a half axis's sign: "+" / "-" (dir +1 / -1) */
bool parse_half(const char *s, int8_t *dir)
{
    if (strcmp(s, "+") == 0)
        *dir = 1;
    else if (strcmp(s, "-") == 0)
        *dir = -1;
    else
        return false;
    return true;
}

float half(int v, int dir)
{
    float f = dir > 0 ? (v > 0 ? v / 32767.0f : 0.0f) : (v < 0 ? -(float)v / 32768.0f : 0.0f);
    return f > 1.0f ? 1.0f : f;
}

void trim(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t')
        p++;
    size_t len = strlen(p);
    while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t'))
        len--;
    memmove(s, p, len);
    s[len] = '\0';
}

Binding pad_button(int b)
{
    Binding x;
    x.source = Source::pad_button;
    x.index = (uint8_t)b;
    return x;
}

Binding pad_axis(int a, int dir)
{
    Binding x;
    x.source = Source::pad_axis;
    x.index = (uint8_t)a;
    x.dir = (int8_t)dir;
    return x;
}

Binding joy(Source s, int i, int dir)
{
    Binding x;
    x.source = s;
    x.index = (uint8_t)i;
    x.dir = (int8_t)dir;
    return x;
}

Binding key(int vk)
{
    Binding x;
    x.source = Source::key;
    x.index = (uint8_t)vk;
    return x;
}

Binding mouse_motion(int axis, int dir)
{
    Binding x;
    x.source = Source::mouse_motion;
    x.index = (uint8_t)axis;
    x.dir = (int8_t)dir;
    return x;
}

/* ---- the game's ---- */

Profile g_profiles[kProfileCount];
std::atomic<int> g_deadzones[kProfileCount][kDeadzones];
std::atomic<bool> g_controller[2];          /* the wheels' Input=controller */
std::string g_ini;

int clamp_deadzone(int v)
{
    return v < 0 ? 0 : v > kDeadzoneMax ? kDeadzoneMax : v;
}

/* ---- the hotkeys ---- */

const char kHotkeySection[] = "Hotkeys";
struct HotkeyInfo {
    const char *key, *name;
    KeyCombo def;
};
const HotkeyInfo kHotkeys[kHotkeyCount] = {
    {"SettingsMenu", "Settings menu", {VK_F2}},
    {"SystemMenu", "Esc menu", {VK_ESCAPE}},
    {"FrameMonitor", "Frame time monitor", {VK_F3}},
    {"Fullscreen", "Windowed / full screen", {VK_RETURN, false, true, false}},
    {"Pause", "Pause / next tick", {VK_F5}},
    {"Resume", "Resume", {VK_F6}},
    {"FlickerCapture", "Flicker capture (debug)", {VK_F8}},
    {"Trace", "Draw trace (debug)", {VK_F9}},
    {"DebugCamera", "Debug camera (in a race)", {VK_F7}},
};
KeyCombo g_hotkeys[kHotkeyCount];

} // namespace

const char *profile_section(int profile)
{
    return profile >= 0 && profile < kProfileCount ? kSections[profile] : "";
}

const char *profile_name(int profile)
{
    return profile >= 0 && profile < kProfileCount ? kNames[profile] : "";
}

int control_count(int profile)
{
    return profile == kDebugCamera ? kDebugControlCount : is_wheel(profile) ? kWheelControlCount : kPadControlCount;
}

const ControlInfo &control(int profile, int i)
{
    return profile == kDebugCamera ? kDebugControls[i] : is_wheel(profile) ? kWheelControls[i] : kPadControls[i];
}

int control_in_order(int profile, int row)
{
    if (profile == kDebugCamera)
        return row;
    /* (the chase camera's are the profile's last controls) */
    return row < kCameraControls ? camera_control(profile) + row : row - kCameraControls;
}

bool slot_takes(int slot, Source s)
{
    if (s == Source::none)
        return true;
    const bool input = s == Source::key || s == Source::mouse_wheel || s == Source::mouse_motion;
    return slot == 1 ? input : !input;
}

/* The mouse's movement (Source::mouse_motion): its ini name by axis (0 x, 1
 * y) and direction (- left / up, + right / down). */
static const char *motion_name(int axis, int dir)
{
    return axis == 0 ? (dir < 0 ? "moveleft" : "moveright") : (dir < 0 ? "moveup" : "movedown");
}

void format(const Binding &b, char *out, size_t n)
{
    char k[24];
    switch (b.source) {
    case Source::pad_button:
        snprintf(out, n, "button:%s", b.index < kPadButtons ? kPadButtonNames[b.index] : "?");
        return;
    case Source::pad_axis:
        snprintf(out, n, "axis:%s%c", b.index < kPadAxes ? kPadAxisNames[b.index] : "?", b.dir < 0 ? '-' : '+');
        return;
    case Source::joy_button:
        snprintf(out, n, "joy:button%d", b.index);
        return;
    case Source::joy_axis:
        snprintf(out, n, "joy:axis%d%c%s", b.index, b.dir < 0 ? '-' : '+', (b.dir == 2 || b.dir == -2) ? "full" : "");
        return;
    case Source::joy_hat:
        for (int i = 0; i < 4; i++)
            if (b.dir == (1 << i)) {
                snprintf(out, n, "joy:hat%d%s", b.index, kHatNames[i]);
                return;
            }
        break;
    case Source::key: {
        const int m = mouse_index(b.index);
        if (m >= 0) {
            snprintf(out, n, "mouse:%s", kMouseNames[m].name);
            return;
        }
        key_name(b.index, k, sizeof(k));
        snprintf(out, n, "key:%s", k);
        return;
    }
    case Source::mouse_wheel:
        snprintf(out, n, "mouse:%s", b.dir > 0 ? "wheelup" : "wheeldown");
        return;
    case Source::mouse_motion:
        snprintf(out, n, "mouse:%s", motion_name(b.index, b.dir));
        return;
    case Source::none:
        break;
    }
    if (n > 0)
        out[0] = '\0';
}

bool parse(const char *text, Binding *out)
{
    char s[64];
    snprintf(s, sizeof(s), "%s", text);
    trim(s);
    *out = Binding();
    if (s[0] == '\0' || _stricmp(s, "none") == 0)
        return true;
    const char *colon = strchr(s, ':');
    if (colon == nullptr)
        return false;
    const std::string kind(s, colon - s);
    const char *v = colon + 1;
    if (_stricmp(kind.c_str(), "button") == 0) {
        for (int i = 0; i < kPadButtons; i++)
            if (_stricmp(v, kPadButtonNames[i]) == 0) {
                *out = pad_button(i);
                return true;
            }
        return false;
    }
    if (_stricmp(kind.c_str(), "axis") == 0) {
        for (int i = 0; i < kPadAxes; i++) {
            const size_t len = strlen(kPadAxisNames[i]);
            int8_t dir;
            if (_strnicmp(v, kPadAxisNames[i], len) == 0 && parse_half(v + len, &dir)) {
                *out = pad_axis(i, dir);
                return true;
            }
        }
        return false;
    }
    if (_stricmp(kind.c_str(), "joy") == 0) {
        int i;
        const char *rest;
        if (parse_numbered(v, "button", 128, &i, &rest) && *rest == '\0') {
            *out = joy(Source::joy_button, i, 0);
            return true;
        }
        if (parse_numbered(v, "axis", 16, &i, &rest)) {
            int8_t dir;
            if (parse_half(rest, &dir)) {
                *out = joy(Source::joy_axis, i, dir);
                return true;
            }
            if (_stricmp(rest, "+full") == 0 || _stricmp(rest, "-full") == 0) {
                *out = joy(Source::joy_axis, i, rest[0] == '+' ? 2 : -2);
                return true;
            }
            return false;
        }
        if (parse_numbered(v, "hat", 4, &i, &rest)) {
            for (int h = 0; h < 4; h++)
                if (_stricmp(rest, kHatNames[h]) == 0) {
                    *out = joy(Source::joy_hat, i, 1 << h);
                    return true;
                }
        }
        return false;
    }
    if (_stricmp(kind.c_str(), "key") == 0) {
        const int vk = parse_key(v);
        if (vk <= 0)
            return false;
        *out = key(vk);
        return true;
    }
    if (_stricmp(kind.c_str(), "mouse") == 0) {
        for (const KeyName &m : kMouseNames)
            if (_stricmp(v, m.name) == 0) {
                *out = key(m.vk);
                return true;
            }
        if (_stricmp(v, "wheelup") == 0 || _stricmp(v, "wheeldown") == 0) {
            out->source = Source::mouse_wheel;
            out->dir = (int8_t)(_stricmp(v, "wheelup") == 0 ? 1 : -1);
            return true;
        }
        for (int axis = 0; axis < 2; axis++)
            for (int dir = -1; dir <= 1; dir += 2)
                if (_stricmp(v, motion_name(axis, dir)) == 0) {
                    *out = mouse_motion(axis, dir);
                    return true;
                }
        return false;
    }
    return false;
}

void describe(const Binding &b, char *out, size_t n)
{
    switch (b.source) {
    case Source::pad_button:
        snprintf(out, n, "%s", b.index < kPadButtons ? kPadButtonLabels[b.index] : "?");
        return;
    case Source::pad_axis:
        snprintf(out, n, "%s", b.index < kPadAxes ? kPadAxisLabels[b.index][b.dir > 0 ? 1 : 0] : "?");
        return;
    case Source::joy_button:
        snprintf(out, n, "Button %d", b.index);
        return;
    case Source::joy_axis:
        if (b.dir == 2 || b.dir == -2)
            snprintf(out, n, "Axis %d (whole, at rest %s)", b.index, b.dir > 0 ? "-" : "+");
        else
            snprintf(out, n, "Axis %d %c", b.index, b.dir < 0 ? '-' : '+');
        return;
    case Source::joy_hat:
        for (int i = 0; i < 4; i++)
            if (b.dir == (1 << i)) {
                snprintf(out, n, "Hat %d %s", b.index, kHatNames[i]);
                return;
            }
        break;
    case Source::key: {
        const int m = mouse_index(b.index);
        if (m >= 0) {
            snprintf(out, n, "%s", kMouseLabels[m]);
            return;
        }
        /* the key's name in the keyboard's layout (extended keys: bit 24) */
        const UINT vk = b.index;
        const UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        bool ext = false;
        switch (vk) {
        case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME: case VK_LEFT: case VK_UP: case VK_RIGHT:
        case VK_DOWN: case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK: case VK_RCONTROL:
        case VK_RMENU: case VK_LWIN: case VK_RWIN: case VK_APPS: case VK_SNAPSHOT:
            ext = true;
            break;
        }
        wchar_t w[64];
        if (sc != 0 && GetKeyNameTextW((LONG)((sc << 16) | (ext ? 1u << 24 : 0)), w, 64) > 0 &&
            WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)n, nullptr, nullptr) > 0)
            return;
        key_name(b.index, out, n);
        return;
    }
    case Source::mouse_wheel:
        snprintf(out, n, "%s", b.dir > 0 ? "Mouse wheel up" : "Mouse wheel down");
        return;
    case Source::mouse_motion:
        snprintf(out, n, "Mouse move %s", b.index == 0 ? (b.dir < 0 ? "left" : "right") : (b.dir < 0 ? "up" : "down"));
        return;
    case Source::none:
        break;
    }
    if (n > 0)
        out[0] = '\0';
}

bool capture_axis(bool joyAxis, int axis, int rest, int now, Binding *out)
{
    if (joyAxis && abs(rest) > 16384) {
        /* at rest at an end: the whole axis, pressed halfway at least */
        const int d = now - rest;
        if (abs(d) < 32768)
            return false;
        *out = joy(Source::joy_axis, axis, d > 0 ? 2 : -2);
        return true;
    }
    if (abs(now) < 16384)
        return false;
    *out = joyAxis ? joy(Source::joy_axis, axis, now > 0 ? 1 : -1) : pad_axis(axis, now > 0 ? 1 : -1);
    return true;
}

const char *deadzone_key(int profile, int i)
{
    if (i < 0 || i >= kDeadzones)
        return "";
    return is_wheel(profile) ? kWheelDeadzoneKeys[i] : kPadDeadzoneKeys[i];
}

const char *deadzone_name(int profile, int i)
{
    if (i < 0 || i >= kDeadzones)
        return "";
    return is_wheel(profile) ? kWheelDeadzoneNames[i] : kPadDeadzoneNames[i];
}

void defaults(int profile, Profile *out)
{
    *out = Profile();
    if (is_wheel(profile)) {
        /* the steering on the first axis, the 12 buttons in the Driving
         * Force's order (Logitech's later wheels keep it), the D-pad on the
         * first hat; no pedals (which axes, and which end they rest at, vary) */
        out->map[WHEEL_STEER_LEFT][0] = joy(Source::joy_axis, 0, -1);
        out->map[WHEEL_STEER_RIGHT][0] = joy(Source::joy_axis, 0, 1);
        for (int i = 0; i < kWheelButtons; i++)
            out->map[WHEEL_CROSS + i][0] = joy(Source::joy_button, i, 0);
        out->map[WHEEL_UP][0] = joy(Source::joy_hat, 0, 1);
        out->map[WHEEL_RIGHT][0] = joy(Source::joy_hat, 0, 2);
        out->map[WHEEL_DOWN][0] = joy(Source::joy_hat, 0, 4);
        out->map[WHEEL_LEFT][0] = joy(Source::joy_hat, 0, 8);
        return;                             /* (the chase camera: none, the wheel has nothing spare) */
    }
    /* the gamepad by position (a DualShock 4's own layout) */
    enum { SOUTH, EAST, WEST, NORTH, BACK, GUIDE, START, LSTICK, RSTICK, LSHOULDER, RSHOULDER, DUP, DDOWN, DLEFT,
           DRIGHT };
    enum { LX, LY, RX, RY, LT, RT };
    if (profile == kDebugCamera) {
        /* the left stick moves, the right stick looks, the triggers up / down,
         * the shoulders the speed, the D-pad the field of view, the right
         * stick's click the lock; the keyboard W A S D, E / Q, the mouse's
         * movement, its wheel, Page Up / Down, F */
        const struct {
            int control;
            Binding pad, key;
        } kDebugDefaults[kDebugControlCount] = {
            {DEBUG_FORWARD, pad_axis(LY, -1), key('W')},
            {DEBUG_BACK, pad_axis(LY, 1), key('S')},
            {DEBUG_LEFT, pad_axis(LX, -1), key('A')},
            {DEBUG_RIGHT, pad_axis(LX, 1), key('D')},
            {DEBUG_UP, pad_axis(RT, 1), key('E')},
            {DEBUG_DOWN, pad_axis(LT, 1), key('Q')},
            {DEBUG_LOOK_UP, pad_axis(RY, -1), mouse_motion(1, -1)},
            {DEBUG_LOOK_DOWN, pad_axis(RY, 1), mouse_motion(1, 1)},
            {DEBUG_LOOK_LEFT, pad_axis(RX, -1), mouse_motion(0, -1)},
            {DEBUG_LOOK_RIGHT, pad_axis(RX, 1), mouse_motion(0, 1)},
            {DEBUG_FASTER, pad_button(RSHOULDER), joy(Source::mouse_wheel, 0, 1)},
            {DEBUG_SLOWER, pad_button(LSHOULDER), joy(Source::mouse_wheel, 0, -1)},
            {DEBUG_FOV_UP, pad_button(DUP), key(VK_PRIOR)},
            {DEBUG_FOV_DOWN, pad_button(DDOWN), key(VK_NEXT)},
            {DEBUG_LOCK, pad_button(RSTICK), key('F')},
        };
        for (const auto &d : kDebugDefaults) {
            out->map[d.control][0] = d.pad;
            out->map[d.control][1] = d.key;
        }
        return;
    }
    const struct {
        int control;
        Binding pad;
        int vk;                             /* Controller 1's key (0 none) */
    } kDefaults[kPadControlCount] = {
        {PAD_UP, pad_button(DUP), VK_UP},
        {PAD_DOWN, pad_button(DDOWN), VK_DOWN},
        {PAD_LEFT, pad_button(DLEFT), VK_LEFT},
        {PAD_RIGHT, pad_button(DRIGHT), VK_RIGHT},
        {PAD_CROSS, pad_button(SOUTH), 'Z'},
        {PAD_CIRCLE, pad_button(EAST), 'X'},
        {PAD_SQUARE, pad_button(WEST), 'A'},
        {PAD_TRIANGLE, pad_button(NORTH), 'S'},
        {PAD_L1, pad_button(LSHOULDER), 'Q'},
        {PAD_R1, pad_button(RSHOULDER), 'W'},
        {PAD_L2, pad_axis(LT, 1), 'E'},
        {PAD_R2, pad_axis(RT, 1), 'R'},
        {PAD_L3, pad_button(LSTICK), VK_NUMPAD7},
        {PAD_R3, pad_button(RSTICK), VK_NUMPAD9},
        {PAD_SELECT, pad_button(BACK), VK_BACK},
        {PAD_START, pad_button(START), VK_RETURN},
        {PAD_LSTICK_UP, pad_axis(LY, -1), VK_NUMPAD8},
        {PAD_LSTICK_DOWN, pad_axis(LY, 1), VK_NUMPAD2},
        {PAD_LSTICK_LEFT, pad_axis(LX, -1), VK_NUMPAD4},
        {PAD_LSTICK_RIGHT, pad_axis(LX, 1), VK_NUMPAD6},
        {PAD_RSTICK_UP, pad_axis(RY, -1), 'I'},
        {PAD_RSTICK_DOWN, pad_axis(RY, 1), 'K'},
        {PAD_RSTICK_LEFT, pad_axis(RX, -1), 'J'},
        {PAD_RSTICK_RIGHT, pad_axis(RX, 1), 'L'},
        /* the chase camera: the right stick's own (game/app/pc_freecam.h) */
        {PAD_CAM_LEFT, pad_axis(RX, -1), 'J'},
        {PAD_CAM_RIGHT, pad_axis(RX, 1), 'L'},
        {PAD_CAM_UP, pad_axis(RY, -1), 'I'},
        {PAD_CAM_DOWN, pad_axis(RY, 1), 'K'},
    };
    for (const auto &d : kDefaults) {
        out->map[d.control][0] = d.pad;
        if (profile == kController1)
            out->map[d.control][1] = key(d.vk);
    }
}

void load(const char *ini, int profile, Profile *out)
{
    defaults(profile, out);
    const char *sec = profile_section(profile);
    for (int c = 0; c < control_count(profile); c++) {
        char buf[160];
        GetPrivateProfileStringA(sec, control(profile, c).key, "\x01", buf, sizeof(buf), ini);
        if (strcmp(buf, "\x01") == 0)
            continue;                       /* missing: the default */
        char *semi = strchr(buf, ';');
        if (semi != nullptr)
            *semi++ = '\0';
        const char *texts[kSlots] = {buf, semi != nullptr ? semi : ""};
        for (int s = 0; s < kSlots; s++) {
            Binding b;
            out->map[c][s] = parse(texts[s], &b) && slot_takes(s, b.source) ? b : Binding();
        }
    }
    for (int i = 0; i < kDeadzones; i++)
        out->deadzone[i] = clamp_deadzone((int)GetPrivateProfileIntA(sec, deadzone_key(profile, i), 0, ini));
    if (is_wheel(profile)) {
        GetPrivateProfileStringA(sec, "Device", "auto", out->device, sizeof(out->device), ini);
        trim(out->device);
        if (_stricmp(out->device, "auto") == 0)
            out->device[0] = '\0';
        char input[16];
        GetPrivateProfileStringA(sec, "Input", "wheel", input, sizeof(input), ini);
        trim(input);
        out->controller = _stricmp(input, "controller") == 0;
    }
}

void save(const char *ini, int profile, const Profile &p)
{
    const char *sec = profile_section(profile);
    for (int c = 0; c < control_count(profile); c++) {
        char a[64], b[64], line[140];
        format(p.map[c][0], a, sizeof(a));
        format(p.map[c][1], b, sizeof(b));
        snprintf(line, sizeof(line), "%s;%s", a, b);
        WritePrivateProfileStringA(sec, control(profile, c).key, line, ini);
    }
    for (int i = 0; i < kDeadzones; i++) {
        char v[16];
        snprintf(v, sizeof(v), "%d", clamp_deadzone(p.deadzone[i]));
        WritePrivateProfileStringA(sec, deadzone_key(profile, i), v, ini);
    }
    if (is_wheel(profile)) {
        WritePrivateProfileStringA(sec, "Device", p.device[0] != '\0' ? p.device : "auto", ini);
        WritePrivateProfileStringA(sec, "Input", p.controller ? "controller" : "wheel", ini);
    }
}

float value(const Binding &b, const Sources &s)
{
    switch (b.source) {
    case Source::pad_button:
        return s.pad != nullptr && b.index < 32 && ((s.pad->buttons >> b.index) & 1) ? 1.0f : 0.0f;
    case Source::pad_axis:
        return s.pad != nullptr && b.index < kPadAxes ? half(s.pad->axes[b.index], b.dir) : 0.0f;
    case Source::joy_button:
        return s.joy != nullptr && b.index < s.joy->buttonCount && s.joy->buttons[b.index] ? 1.0f : 0.0f;
    case Source::joy_axis: {
        if (s.joy == nullptr || b.index >= s.joy->axisCount)
            return 0.0f;
        const int v = s.joy->axes[b.index];
        if (b.dir == 2)
            return (float)(v + 32768) / 65535.0f;
        if (b.dir == -2)
            return (float)(32767 - v) / 65535.0f;
        return half(v, b.dir);
    }
    case Source::joy_hat:
        return s.joy != nullptr && b.index < s.joy->hatCount && (s.joy->hats[b.index] & b.dir) ? 1.0f : 0.0f;
    case Source::key:
        return s.key != nullptr && s.key(b.index) ? 1.0f : 0.0f;
    case Source::mouse_wheel:
        return (s.wheel & (b.dir > 0 ? 1 : 2)) ? 1.0f : 0.0f;
    case Source::mouse_motion:              /* (a movement, not a level: its reader counts it) */
    case Source::none:
        break;
    }
    return 0.0f;
}

bool is_analog(const Binding &b)
{
    return b.source == Source::pad_axis || b.source == Source::joy_axis;
}

void init(const char *ini)
{
    g_ini = ini;
    for (int p = 0; p < kProfileCount; p++) {
        load(ini, p, &g_profiles[p]);
        for (int i = 0; i < kDeadzones; i++)
            g_deadzones[p][i] = g_profiles[p].deadzone[i];
    }
    for (int w = 0; w < 2; w++)
        g_controller[w] = g_profiles[kWheel1 + w].controller;
    load_hotkeys(ini, g_hotkeys);
}

bool plays_with_controller(int player)
{
    return player >= 0 && player < 2 && g_controller[player].load(std::memory_order_relaxed);
}

void set_plays_with_controller(int player, bool on)
{
    if (player < 0 || player >= 2 || g_controller[player].exchange(on) == on || g_ini.empty())
        return;
    WritePrivateProfileStringA(profile_section(kWheel1 + player), "Input", on ? "controller" : "wheel", g_ini.c_str());
}

const Profile &profile(int p)
{
    return g_profiles[p < 0 || p >= kProfileCount ? 0 : p];
}

int deadzone(int p, int i)
{
    if (p < 0 || p >= kProfileCount || i < 0 || i >= kDeadzones)
        return 0;
    return g_deadzones[p][i].load(std::memory_order_relaxed);
}

void set_deadzone(int p, int i, int percent)
{
    if (p < 0 || p >= kProfileCount || i < 0 || i >= kDeadzones)
        return;
    percent = clamp_deadzone(percent);
    if (g_deadzones[p][i].exchange(percent) == percent || g_ini.empty())
        return;
    char v[16];
    snprintf(v, sizeof(v), "%d", percent);
    WritePrivateProfileStringA(profile_section(p), deadzone_key(p, i), v, g_ini.c_str());
}

float apply_deadzone(float v, float d)
{
    if (v <= d)
        return 0.0f;
    if (d <= 0.0f)
        return v > 1.0f ? 1.0f : v;
    v = (v - d) / (1.0f - d);
    return v > 1.0f ? 1.0f : v;
}

const char *hotkey_key(int h)
{
    return h >= 0 && h < kHotkeyCount ? kHotkeys[h].key : "";
}

const char *hotkey_name(int h)
{
    return h >= 0 && h < kHotkeyCount ? kHotkeys[h].name : "";
}

KeyCombo hotkey_default(int h)
{
    return h >= 0 && h < kHotkeyCount ? kHotkeys[h].def : KeyCombo();
}

bool is_modifier(int vk)
{
    return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LSHIFT || vk == VK_RSHIFT ||
           vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU;
}

void format_combo(const KeyCombo &k, char *out, size_t n)
{
    if (k.vk == 0) {
        snprintf(out, n, "none");
        return;
    }
    char key[32];
    key_name(k.vk, key, sizeof(key));
    snprintf(out, n, "%s%s%s%s", k.ctrl ? "Ctrl+" : "", k.alt ? "Alt+" : "", k.shift ? "Shift+" : "", key);
}

bool parse_combo(const char *text, KeyCombo *out)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", text);
    trim(buf);
    KeyCombo k;
    if (buf[0] == '\0' || _stricmp(buf, "none") == 0) {
        *out = k;
        return true;
    }
    /* the modifiers up to the last '+' (a key named "+" has none: NumpadAdd) */
    char *p = buf;
    for (char *plus; (plus = strchr(p, '+')) != nullptr && plus[1] != '\0'; p = plus + 1) {
        *plus = '\0';
        trim(p);
        if (_stricmp(p, "Ctrl") == 0 || _stricmp(p, "Control") == 0)
            k.ctrl = true;
        else if (_stricmp(p, "Alt") == 0)
            k.alt = true;
        else if (_stricmp(p, "Shift") == 0)
            k.shift = true;
        else
            return false;
    }
    trim(p);
    const int vk = parse_key(p);
    if (vk <= 0 || vk >= 256 || is_modifier(vk) || mouse_index(vk) >= 0)
        return false;
    k.vk = (uint8_t)vk;
    *out = k;
    return true;
}

void describe_combo(const KeyCombo &k, char *out, size_t n)
{
    if (k.vk == 0) {
        if (n > 0)
            out[0] = '\0';
        return;
    }
    char key[64];
    describe(input_map::key(k.vk), key, sizeof(key));
    snprintf(out, n, "%s%s%s%s", k.ctrl ? "Ctrl+" : "", k.alt ? "Alt+" : "", k.shift ? "Shift+" : "", key);
}

void load_hotkeys(const char *ini, KeyCombo out[kHotkeyCount])
{
    for (int h = 0; h < kHotkeyCount; h++) {
        char buf[64];
        GetPrivateProfileStringA(kHotkeySection, kHotkeys[h].key, "\x01", buf, sizeof(buf), ini);
        if (strcmp(buf, "\x01") == 0 || !parse_combo(buf, &out[h]))
            out[h] = kHotkeys[h].def;       /* missing or unreadable: the default */
    }
}

void save_hotkey(const char *ini, int h, const KeyCombo &k)
{
    if (h < 0 || h >= kHotkeyCount)
        return;
    char buf[64];
    format_combo(k, buf, sizeof(buf));
    WritePrivateProfileStringA(kHotkeySection, kHotkeys[h].key, buf, ini);
}

const KeyCombo &hotkey(int h)
{
    static const KeyCombo none;
    return h >= 0 && h < kHotkeyCount ? g_hotkeys[h] : none;
}

bool is_hotkey(int h, int vk, bool ctrl, bool alt, bool shift)
{
    const KeyCombo &k = hotkey(h);
    return k.vk != 0 && k.vk == vk && k.ctrl == ctrl && k.alt == alt && k.shift == shift;
}

void hotkey_text(int h, char *out, size_t n)
{
    const KeyCombo &k = hotkey(h);
    if (k.vk == VK_ESCAPE)
        snprintf(out, n, "%s%s%sEsc", k.ctrl ? "Ctrl+" : "", k.alt ? "Alt+" : "", k.shift ? "Shift+" : "");
    else if (k.vk != 0)
        format_combo(k, out, n);
    else if (n > 0)
        out[0] = '\0';
}

} // namespace input_map
