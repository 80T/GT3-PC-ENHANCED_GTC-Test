/* input_map.h - the control mappings of GT3 PC (gt3pc.ini [Controller1],
 * [Controller2], [Wheel1], [Wheel2], [DebugCamera]).
 *
 * Shared by the game and the launcher (no SDL, no game headers): the
 * launcher's Controls tab edits the mappings, the game reads them at start
 * (platform/pad_pc.cpp: the controllers, platform/wheel.cpp: the wheels) and
 * the in-game settings menu changes the dead zones.
 *
 * Four profiles: Controller 1 / 2, the DualShock 2s of the game's pad ports
 * 1 / 2, and Wheel 1 / 2, the Logitech Driving Forces (USB 0x46D:0xC294) of
 * its USB wheel ports 1 / 2 (player 1 / 2).  Each profile maps its virtual
 * controls (the DualShock 2's buttons, D-pad and stick directions; the
 * Driving Force's steering, pedals, 12 buttons and D-pad; and either's
 * chase camera directions, CameraLeft / Right / Up / Down: the free orbit,
 * game/app/pc_freecam.h - by default a controller's right stick and
 * Controller 1's J / L / I / K, none on a wheel) from two slots:
 *   slot 0  the device: Controller N's SDL gamepad (the Nth connected; buttons
 *           and axes by their position, Xbox-style: south = A / Cross), or
 *           Wheel N's SDL joystick (raw buttons, axes and hats);
 *   slot 1  the keyboard and the mouse.
 * The control's value is the larger of the two (0..1).  A fifth profile,
 * Debug camera, maps the debug camera's controls (DebugControl) the same way
 * from the first gamepad (Controller 1's) and the keyboard / mouse, with a
 * controller's dead zones (platform/debug_camera.h).
 *
 * In gt3pc.ini each control is `Key=slot0;slot1', a binding or nothing in
 * each (a missing key: the default; `Key=;' none at all):
 *   button:<name>          a gamepad button (south, east, west, north, back,
 *                          start, leftstick, rightstick, leftshoulder,
 *                          rightshoulder, dpup, dpdown, dpleft, dpright, misc1,
 *                          paddle1..4, touchpad, misc2..6; SDL3's order)
 *   axis:<name>+ / -       one half of a gamepad axis (leftx, lefty, rightx,
 *                          righty, lefttrigger, righttrigger; y down is +)
 *   joy:button<n>          a joystick button (0..127)
 *   joy:axis<n>+ / -       one half of a joystick axis (centred at rest)
 *   joy:axis<n>+full / -full   the whole axis, from its - end (+full) or its
 *                          + end (-full) at rest: a pedal
 *   joy:hat<n>up / right / down / left   a joystick hat's direction
 *   key:<name>             a key (Windows virtual key: A..Z, 0..9, F1..F24,
 *                          Up, Down, Left, Right, Enter, Space, Backspace, Tab,
 *                          Shift, Ctrl, Alt, Numpad0..9, ...; or 0xNN)
 *   mouse:left / right / middle / x1 / x2 / wheelup / wheeldown
 *   mouse:moveleft / moveright / moveup / movedown   the mouse's movement
 *                          (the Debug camera's look only)
 * Besides, per profile:
 *   Controller:  LeftStickDeadzone=0, RightStickDeadzone=0, TriggerDeadzone=0
 *                (percent, 0..50: the sticks' radially, the triggers' at the
 *                start of their travel; the rest rescaled to the full range)
 *   Wheel:       Device=auto (the joystick's name; auto: the Nth joystick
 *                SDL knows as a wheel), SteeringDeadzone=0, GasDeadzone=0,
 *                BrakeDeadzone=0 (percent, 0..50, on the virtual steering and
 *                pedals), Input=wheel | controller (what the player - 1 for
 *                Wheel 1, 2 for Wheel 2 - plays with while both the wheel and
 *                the controller are connected; with one of them, that one:
 *                engine/input T478)
 *
 * [Hotkeys]: the game window's own keys (platform/window.cpp), each a key
 * with exactly the modifiers held with it:
 *   SettingsMenu=F2  SystemMenu=Escape  FrameMonitor=F3  Fullscreen=Alt+Enter
 *   Pause=F5  Resume=F6  FlickerCapture=F8  Trace=F9  DebugCamera=F7
 * `Key=Ctrl+Shift+F5': Ctrl+, Alt+, Shift+ before a key's name (key:<name>'s;
 * not a modifier alone); `Key=none' (or empty) none; a missing or unreadable
 * value the default.  A hotkey's key does not reach the game, and the
 * launcher keeps the controls' bindings off the hotkeys' keys. */
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace input_map {

/* ---- the profiles and their controls ---------------------------------- */

enum { kController1, kController2, kWheel1, kWheel2, kDebugCamera, kProfileCount };
inline bool is_wheel(int profile) { return profile == kWheel1 || profile == kWheel2; }
const char *profile_section(int profile);   /* "Controller1" .. "Wheel2", "DebugCamera" */
const char *profile_name(int profile);      /* "Controller 1" .. "Wheel 2", "Debug camera" */

/* The controller's controls (a DualShock 2's), then the chase camera's
 * (not the DualShock 2's: its free orbit, game/app/pc_freecam.h; the
 * launcher lists them first: control_in_order). */
enum PadControl {
    PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT,
    PAD_CROSS, PAD_CIRCLE, PAD_SQUARE, PAD_TRIANGLE,
    PAD_L1, PAD_R1, PAD_L2, PAD_R2, PAD_L3, PAD_R3,
    PAD_SELECT, PAD_START,
    PAD_LSTICK_UP, PAD_LSTICK_DOWN, PAD_LSTICK_LEFT, PAD_LSTICK_RIGHT,
    PAD_RSTICK_UP, PAD_RSTICK_DOWN, PAD_RSTICK_LEFT, PAD_RSTICK_RIGHT,
    PAD_CAM_LEFT, PAD_CAM_RIGHT, PAD_CAM_UP, PAD_CAM_DOWN,
    kPadControlCount
};
/* The wheel's controls (a Driving Force): buttons in its USB report's order
 * (WHEEL_CROSS + i = report button i); then the chase camera's, as the
 * controller's. */
enum WheelControl {
    WHEEL_STEER_LEFT, WHEEL_STEER_RIGHT, WHEEL_GAS, WHEEL_BRAKE,
    WHEEL_CROSS, WHEEL_SQUARE, WHEEL_CIRCLE, WHEEL_TRIANGLE,
    WHEEL_R1, WHEEL_L1, WHEEL_R2, WHEEL_L2,
    WHEEL_SELECT, WHEEL_START, WHEEL_R3, WHEEL_L3,
    WHEEL_UP, WHEEL_DOWN, WHEEL_LEFT, WHEEL_RIGHT,
    WHEEL_CAM_LEFT, WHEEL_CAM_RIGHT, WHEEL_CAM_UP, WHEEL_CAM_DOWN,
    kWheelControlCount
};
/* A profile's chase camera controls: left, right, up, down from this one. */
inline int camera_control(int profile) { return is_wheel(profile) ? WHEEL_CAM_LEFT : PAD_CAM_LEFT; }
constexpr int kCameraControls = 4;
/* The debug camera's controls ([DebugCamera], profile kDebugCamera: the
 * first gamepad and the keyboard / mouse; platform/debug_camera.h). */
enum DebugControl {
    DEBUG_FORWARD, DEBUG_BACK, DEBUG_LEFT, DEBUG_RIGHT, DEBUG_UP, DEBUG_DOWN,
    DEBUG_LOOK_UP, DEBUG_LOOK_DOWN, DEBUG_LOOK_LEFT, DEBUG_LOOK_RIGHT,
    DEBUG_FASTER, DEBUG_SLOWER, DEBUG_FOV_UP, DEBUG_FOV_DOWN,
    DEBUG_LOCK,                             /* the camera held, the car the player's again (a press each way) */
    kDebugControlCount
};
constexpr int kWheelButtons = 12;
constexpr int kMaxControls = kPadControlCount > kWheelControlCount ? kPadControlCount : kWheelControlCount;
static_assert(kDebugControlCount <= kMaxControls, "a profile's controls");

/* What a control takes: a button (pressed or not; an analog source counts
 * from halfway, its travel as the pressure), a direction of a stick or the
 * steering (0..1), a pedal (0..1). */
enum class ControlKind : uint8_t { button, direction, pedal };
struct ControlInfo {
    const char *key;                        /* the ini key */
    const char *name;                       /* the launcher's label */
    ControlKind kind;
};
int control_count(int profile);
const ControlInfo &control(int profile, int i);
/* The control in the launcher's row `row' (0 .. control_count - 1): the
 * chase camera's first, then the others in their order. */
int control_in_order(int profile, int row);

/* ---- bindings -------------------------------------------------------- */

enum class Source : uint8_t {
    none,
    pad_button,                             /* index: SDL_GamepadButton */
    pad_axis,                               /* index: SDL_GamepadAxis; dir +1 / -1 */
    joy_button,                             /* index: the button */
    joy_axis,                               /* index: the axis; dir +1 / -1 a half, +2 / -2 the whole (+full / -full) */
    joy_hat,                                /* index: the hat; dir: SDL_HAT_* bit (1 up, 2 right, 4 down, 8 left) */
    key,                                    /* index: the virtual key (VK_LBUTTON .. VK_XBUTTON2: the mouse's buttons) */
    mouse_wheel,                            /* dir +1 up, -1 down (a notch: a tap) */
    mouse_motion                            /* index 0 x / 1 y, dir -1 left / up, +1 right / down: the
                                               mouse's movement (value 0: the debug camera counts it) */
};
struct Binding {
    Source source = Source::none;
    uint8_t index = 0;
    int8_t dir = 0;
};
constexpr int kSlots = 2;                   /* 0 the device, 1 the keyboard / mouse */
bool slot_takes(int slot, Source s);        /* slot 0: pad_* / joy_*; slot 1: key / mouse_wheel / mouse_motion */

/* The ini text of a binding ("" for none) and back (false: not a binding;
 * "" and "none" are none). */
void format(const Binding &b, char *out, size_t n);
bool parse(const char *text, Binding *out);
/* A label for the launcher ("Left stick up", "Button 3", "Axis 2 (whole, +
 * pressed)", the key's name in the keyboard's layout; "" for none). */
void describe(const Binding &b, char *out, size_t n);

/* The gamepad buttons / axes SDL3 has (Source::pad_*'s index range). */
constexpr int kPadButtons = 26, kPadAxes = 6;
constexpr int kPadButtonGuide = 5;          /* SDL_GAMEPAD_BUTTON_GUIDE: the menus (gamepad.h), never mapped */
constexpr int kPadAxisLeftTrigger = 4, kPadAxisRightTrigger = 5;
/* An axis moved far enough from its rest value to be taken as a binding
 * (the launcher's capture): a half of a centred axis, the whole of one that
 * rests at an end (|rest| > 16384: a pedal), the direction it moved.  joy:
 * a joystick axis (else a gamepad axis: always a half). */
bool capture_axis(bool joy, int axis, int rest, int now, Binding *out);

/* ---- a profile ------------------------------------------------------- */

constexpr int kDeadzones = 3;               /* controller: left stick, right stick, triggers; wheel: steering, gas, brake */
constexpr int kDeadzoneMax = 50;
const char *deadzone_key(int profile, int i);
const char *deadzone_name(int profile, int i);      /* the launcher's / settings menu's label */

struct Profile {
    Binding map[kMaxControls][kSlots];
    int deadzone[kDeadzones] = {0, 0, 0};
    char device[128] = "";                  /* wheels: the joystick's name ("": auto) */
    bool controller = false;                /* wheels: Input=controller (else wheel) */
};
void defaults(int profile, Profile *out);
/* gt3pc.ini at `ini' (a full path): a missing value the default. */
void load(const char *ini, int profile, Profile *out);
/* All of it written (the launcher). */
void save(const char *ini, int profile, const Profile &p);

/* ---- the sources ----------------------------------------------------- */

/* What a binding reads: the devices' raw state, the keyboard and mouse.
 * pad / joy: nullptr when absent. */
struct PadSource {
    uint32_t buttons;                       /* bit = SDL_GamepadButton */
    int16_t axes[kPadAxes];                 /* SDL's: sticks -32768..32767, triggers 0..32767 */
};
struct JoySource {
    int buttonCount, axisCount, hatCount;
    const uint8_t *buttons;
    const int16_t *axes;
    const uint8_t *hats;                    /* SDL_HAT_* bits */
};
struct Sources {
    const PadSource *pad;
    const JoySource *joy;
    bool (*key)(int vk);                    /* the key (or mouse button) is down; nullptr: none */
    int wheel;                              /* mouse wheel notches since the last read: bit 0 up, bit 1 down */
};
/* The binding's value 0..1 (a button 0 / 1). */
float value(const Binding &b, const Sources &s);
/* A binding's source is analog (an axis): a button taken from it counts
 * from halfway (a trigger from the slightest press: gamepad.h). */
bool is_analog(const Binding &b);

/* ---- the game's (loaded once; the dead zones changed in the game) ------ */

/* Reads the four profiles (after paths::init; the game's gt3pc.ini). */
void init(const char *ini);
const Profile &profile(int p);
/* The dead zone in percent; set: held to 0..kDeadzoneMax, written to
 * gt3pc.ini at once.  Any thread. */
int deadzone(int p, int i);
void set_deadzone(int p, int i, int percent);
/* A dead zone d (0..1) applied to a value 0..1: 0 inside, the rest
 * stretched to 0..1. */
float apply_deadzone(float v, float d);
/* Player `player' (0, 1) plays with the controller while both it and the
 * wheel are connected ([WheelN] Input=controller; else the wheel); set:
 * written to gt3pc.ini at once.  Any thread. */
bool plays_with_controller(int player);
void set_plays_with_controller(int player, bool on);

/* ---- the hotkeys ([Hotkeys]) ------------------------------------------ */

enum Hotkey {
    kHotSettingsMenu, kHotSystemMenu, kHotFrameMonitor, kHotFullscreen,
    kHotPause, kHotResume, kHotFlickerCapture, kHotTrace, kHotDebugCamera,
    kHotkeyCount
};
struct KeyCombo {
    uint8_t vk = 0;                         /* the virtual key; 0: none */
    bool ctrl = false, alt = false, shift = false;
};
inline bool operator==(const KeyCombo &a, const KeyCombo &b)
{
    return a.vk == b.vk && (a.vk == 0 || (a.ctrl == b.ctrl && a.alt == b.alt && a.shift == b.shift));
}
const char *hotkey_key(int h);              /* the ini key: "SettingsMenu" .. */
const char *hotkey_name(int h);             /* the launcher's label */
KeyCombo hotkey_default(int h);
/* A modifier key (Shift, Ctrl, Alt, either side's): never a combo's key. */
bool is_modifier(int vk);
/* The ini text of a combo ("Alt+Enter"; "none") and back (false: not one;
 * "" and "none" are none). */
void format_combo(const KeyCombo &k, char *out, size_t n);
bool parse_combo(const char *text, KeyCombo *out);
/* A label for the launcher: the key's name in the keyboard's layout after
 * the modifiers ("Ctrl+Esc"; "" for none). */
void describe_combo(const KeyCombo &k, char *out, size_t n);
/* gt3pc.ini at `ini': all of them (a missing or unreadable one the
 * default); one written. */
void load_hotkeys(const char *ini, KeyCombo out[kHotkeyCount]);
void save_hotkey(const char *ini, int h, const KeyCombo &k);

/* The game's (read by init). */
const KeyCombo &hotkey(int h);
/* The key vk going down with those modifiers is hotkey h. */
bool is_hotkey(int h, int vk, bool ctrl, bool alt, bool shift);
/* Hotkey h for the overlays: its ini text, Escape as "Esc" ("F2",
 * "Alt+Enter"; "" for none). */
void hotkey_text(int h, char *out, size_t n);

} // namespace input_map
