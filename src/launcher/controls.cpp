/* controls.cpp - the launcher window's Controls tab (controls.h).
 *
 * SDL runs on a thread of its own while the tab is shown (as in the game,
 * platform/gamepad.cpp: its device notifications belong to the thread that
 * starts it): it opens every joystick (and the gamepad of each one SDL maps
 * as such) and copies their state into a snapshot every few ms; the tab
 * reads the snapshot on a timer - the capture of a slot 0 binding (from any
 * device: one that is not the profile's own is named, `@<name>'), the live
 * view - and the window's own messages for slot 1's (filter). */
#include "controls.h"
#include "panel.h"
#include "platform/input_map.h"
#include "platform/ui_text.h"  /* PC (2026-11-16): the interface's languages */

#include <SDL3/SDL.h>
#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <stdio.h>
#include <string.h>
#include <string>
#include <thread>
#include <vector>

namespace controls {
namespace {

using namespace input_map;

/* Layout, in 96-DPI units (as ui.cpp's). */
constexpr int kPad = 12;
constexpr int kTopH = 34;                   /* the profile and the sub-tabs */
constexpr int kProfileW = 150, kSubTabW = 100, kSubTabH = 28;
constexpr int kHeadH = 20;                  /* the mapping's column heads */
constexpr int kRowH = 28, kLabelW = 150, kSlotH = 24, kGap = 6;
constexpr int kComboW = 240, kComboListH = 240;
constexpr int kSliderW = 200, kSliderH = 26, kValueW = 50, kValueGap = 6;
constexpr int kLiveH = 54, kNoteH = 36;
constexpr int kHotLabelW = 200, kHotNoteH = 72;

constexpr int kHotkeyIds = 32;              /* PC (2026-11-13): the ids kept for each of the hotkeys' three ranges */

constexpr UINT_PTR kTimer = 0x4C43;         /* the tab's WM_TIMER */
constexpr UINT kTimerMs = 15;
constexpr ULONGLONG kCaptureMs = 5000;

enum : int {
    IDC_PROFILE = kFirstId,
    IDC_SUB_MAPPING,
    IDC_SUB_CALIBRATION,
    IDC_RESET,
    IDC_HEAD0,                              /* .. + 2: the column heads */
    IDC_LIST = kFirstId + 10,               /* the inner panel */
    IDC_DEADZONE0 = kFirstId + 20,          /* .. + kDeadzones - 1: the sliders */
    IDC_DEADZONE_LABEL0 = kFirstId + 30,
    IDC_DEADZONE_VALUE0 = kFirstId + 80,
    IDC_DEVICE = kFirstId + 40,
    IDC_DEVICE_LABEL,
    IDC_LIVE,
    IDC_NOTE,
    IDC_INPUT,                              /* a wheel's Input: what the player plays with */
    IDC_INPUT_LABEL,
    IDC_SUB_HOTKEYS = kFirstId + 50,
    IDC_HOT_RESET,
    IDC_HOT_NOTE,
    /* PC (2026-11-13): 21 hotkeys (the texture replacement menu's) no longer
     * fit three ranges of 20: the ranges from here on packed again in the
     * tab's ids (kFirstId .. kLastId, controls.h: + 0 .. + 299), room for
     * 30 controls and kHotkeyIds hotkeys, each range checked below */
    IDC_CONTROL_LABEL0 = kFirstId + 100,    /* .. + kMaxControls - 1 (+ 100 .. + 129) */
    IDC_SLOT0 = kFirstId + 130,             /* control i, slot s: + i * 2 + s (+ 130 .. + 189) */
    IDC_HOT_LABEL0 = kFirstId + 190,        /* .. + kHotkeyCount - 1 (+ 190 .. + 221) */
    IDC_HOT_KEY0 = IDC_HOT_LABEL0 + kHotkeyIds,     /* (+ 222 .. + 253) */
    IDC_HOT_DEV0 = IDC_HOT_KEY0 + kHotkeyIds,       /* PC: the hotkeys' devices (+ 254 .. + 285) */
    IDC_END = IDC_HOT_DEV0 + kHotkeyIds,            /* (the first id after the tab's) */
};
static_assert(IDC_DEADZONE_VALUE0 + kDeadzones <= IDC_CONTROL_LABEL0, "the tab's ids");
static_assert(IDC_CONTROL_LABEL0 + kMaxControls <= IDC_SLOT0, "the tab's ids");
static_assert(IDC_SLOT0 + kMaxControls * kSlots <= IDC_HOT_LABEL0, "the tab's ids");
static_assert(kHotkeyCount <= kHotkeyIds && IDC_END - 1 <= kLastId, "the hotkeys' ids");

/* The sub-tabs. */
enum Sub { kMapping, kCalibration, kHotkeys, kSubCount };
const wchar_t *const kSubNames[kSubCount] = {N_(L"Mapping"), N_(L"Calibration"), N_(L"Hotkeys")};

/* ---- SDL ------------------------------------------------------------------ */

/* A gamepad (not a wheel) as SDL maps it. */
struct PadState {
    SDL_JoystickID id;
    std::string name;
    PadSource src;
};
/* Any joystick, raw. */
struct JoyState {
    SDL_JoystickID id;
    std::string name;
    bool gamepad, wheel;                    /* SDL maps it as a gamepad / types it a wheel */
    int buttonCount, axisCount, hatCount;
    uint8_t buttons[128];
    int16_t axes[16];
    uint8_t hats[4];
};
struct Snapshot {
    std::vector<PadState> pads;             /* in SDL's order: the game's ports take them so */
    std::vector<JoyState> joys;
    unsigned devices = 0;                   /* bumped when one comes or goes */
};

int g_sdl = 0;                              /* SDL3.dll: 0 not tried, 1 loaded, -1 missing or too old */
std::thread g_thread;
std::atomic<bool> g_quit{false};
std::mutex g_mutex;
Snapshot g_snap;

bool load_sdl()
{
    if (g_sdl == 0) {
        /* delay-loaded: from the launcher's folder (not the working folder),
         * no SDL function called without it */
        HMODULE dll = LoadLibraryExW(L"SDL3.dll", nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        const int v = dll != nullptr ? SDL_GetVersion() : 0;
        g_sdl = dll != nullptr && SDL_VERSIONNUM_MAJOR(v) == 3 && SDL_VERSIONNUM_MINOR(v) >= SDL_MINOR_VERSION ? 1 : -1;
    }
    return g_sdl > 0;
}

void sdl_main()
{
    /* no SDL window: the devices count whatever window has the focus */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        g_sdl = -1;
        return;
    }
    struct Open {
        SDL_JoystickID id;
        SDL_Joystick *joy;
        SDL_Gamepad *pad;
    };
    std::vector<Open> open;
    unsigned devices = 0;
    while (!g_quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_JOYSTICK_ADDED) {
                const SDL_JoystickID id = e.jdevice.which;
                if (std::none_of(open.begin(), open.end(), [&](const Open &o) { return o.id == id; })) {
                    Open o = {id, SDL_OpenJoystick(id), SDL_IsGamepad(id) ? SDL_OpenGamepad(id) : nullptr};
                    if (o.joy != nullptr) {
                        open.push_back(o);
                        devices++;
                    } else if (o.pad != nullptr) {
                        SDL_CloseGamepad(o.pad);
                    }
                }
            } else if (e.type == SDL_EVENT_JOYSTICK_REMOVED) {
                for (size_t i = 0; i < open.size(); i++)
                    if (open[i].id == e.jdevice.which) {
                        if (open[i].pad != nullptr)
                            SDL_CloseGamepad(open[i].pad);
                        SDL_CloseJoystick(open[i].joy);
                        open.erase(open.begin() + (ptrdiff_t)i);
                        devices++;
                        break;
                    }
            }
        }
        Snapshot s;
        s.devices = devices;
        for (const Open &o : open) {
            JoyState j = {};
            j.id = o.id;
            const char *name = SDL_GetJoystickName(o.joy);
            j.name = name != nullptr ? name : "";
            j.gamepad = o.pad != nullptr;
            j.wheel = SDL_GetJoystickType(o.joy) == SDL_JOYSTICK_TYPE_WHEEL;
            j.buttonCount = std::clamp(SDL_GetNumJoystickButtons(o.joy), 0, 128);
            j.axisCount = std::clamp(SDL_GetNumJoystickAxes(o.joy), 0, 16);
            j.hatCount = std::clamp(SDL_GetNumJoystickHats(o.joy), 0, 4);
            for (int i = 0; i < j.buttonCount; i++)
                j.buttons[i] = SDL_GetJoystickButton(o.joy, i) ? 1 : 0;
            for (int i = 0; i < j.axisCount; i++)
                j.axes[i] = SDL_GetJoystickAxis(o.joy, i);
            for (int i = 0; i < j.hatCount; i++)
                j.hats[i] = SDL_GetJoystickHat(o.joy, i);
            if (o.pad != nullptr && !j.wheel) {
                PadState p = {};
                p.id = o.id;
                p.name = j.name;
                for (int b = 0; b < kPadButtons; b++)
                    if (SDL_GetGamepadButton(o.pad, (SDL_GamepadButton)b))
                        p.src.buttons |= 1u << b;
                for (int a = 0; a < kPadAxes; a++)
                    p.src.axes[a] = SDL_GetGamepadAxis(o.pad, (SDL_GamepadAxis)a);
                s.pads.push_back(p);
            }
            s.joys.push_back(j);
        }
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_snap = std::move(s);
        }
        Sleep(5);
    }
    for (const Open &o : open) {
        if (o.pad != nullptr)
            SDL_CloseGamepad(o.pad);
        SDL_CloseJoystick(o.joy);
    }
    SDL_Quit();
}

void sdl_start()
{
    if (g_thread.joinable() || !load_sdl())
        return;
    g_quit = false;
    g_thread = std::thread(sdl_main);
}

void sdl_stop()
{
    if (!g_thread.joinable())
        return;
    g_quit = true;
    g_thread.join();
    std::lock_guard<std::mutex> lock(g_mutex);
    g_snap = Snapshot();
}

Snapshot snapshot()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_snap;
}

/* ---- the tab --------------------------------------------------------------- */

struct Tab {
    HINSTANCE inst = nullptr;
    HWND page = nullptr, list = nullptr;
    std::string ini;                        /* gt3pc.ini (the A functions of input_map) */
    HWND profile = nullptr, reset = nullptr;
    HWND subTab[kSubCount] = {};            /* the sub-tabs' buttons */
    HWND head[3] = {};
    HWND label[kMaxControls] = {};
    HWND slot[kMaxControls][kSlots] = {};
    HWND deadzone[kDeadzones] = {}, deadzoneLabel[kDeadzones] = {}, deadzoneValue[kDeadzones] = {};
    HWND device = nullptr, deviceLabel = nullptr, live = nullptr, note = nullptr;
    HWND input = nullptr, inputLabel = nullptr;
    HWND hotLabel[kHotkeyCount] = {}, hotKey[kHotkeyCount] = {}, hotDev[kHotkeyCount] = {};
    HWND hotReset = nullptr, hotNote = nullptr;
    HFONT font = nullptr, tabFont = nullptr;
    UINT dpi = 96;
    bool shown = false;
    int prof = kController1;
    int sub = kMapping;
    Profile p;                              /* the profile shown */
    KeyCombo hot[kHotkeyCount];             /* the hotkeys ([Hotkeys]) */
    Binding hotDevice[kHotkeyCount];        /* PC: their devices' bindings */
    /* the hotkey capture: the hotkey (-1: none), its key (slot 0) or its
     * device (1: the devices' state at its start in rest, as the slots'), a
     * note instead of the prompt (until capUntil, as the slots') */
    int capHot = -1, capHotSlot = 0;
    std::wstring capHotNote;
    std::string otherDevice;                /* a wheel's: the other wheel's Device ("" auto) */
    Profile otherWheel;                     /* a wheel's: the other wheel's (the devices its bindings name) */
    std::string wheelDevice[2];             /* the wheels' Devices (not the controllers' pads) */
    /* the capture: control, slot (-1: none), until when, a note instead of
     * the prompt, the devices' state at its start (held inputs wait for
     * their release) */
    int capControl = -1, capSlot = 0;
    ULONGLONG capUntil = 0;
    std::wstring capNote;
    POINT capMouse = {};                    /* the cursor at the start (a movement taken: takes_motion) */
    Snapshot rest;
    /* the Device list: the snapshot's devices it shows, its entries' names
     * (0: auto, "") */
    unsigned listed = ~0u;
    std::vector<std::string> deviceNames;
    std::wstring liveText;
} t;

int px(int dip) { return MulDiv(dip, (int)t.dpi, 96); }

std::wstring widen(const char *s)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? (size_t)n : 1, L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    w.resize(wcslen(w.c_str()));
    return w;
}

HWND make(HWND parent, const wchar_t *cls, const wchar_t *text, DWORD style, int id)
{
    HWND w = CreateWindowExW(0, cls, text, WS_CHILD | style, 0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, t.inst, nullptr);
    if (t.font != nullptr)
        SendMessageW(w, WM_SETFONT, (WPARAM)t.font, FALSE);
    return w;
}

HWND make_label(HWND parent, const wchar_t *text, int id)
{
    return make(parent, WC_STATICW, text, SS_LEFT | SS_NOPREFIX, id);
}

/* PC (2026-11-16): a label's text's height wrapped at `width' in its own
 * font (the notes and heads grow with a translation: layout) */
int text_height(HWND c, int width)
{
    if (c == nullptr || width <= 0)
        return 0;
    wchar_t text[2048] = L"";
    GetWindowTextW(c, text, 2048);
    HDC dc = GetDC(c);
    HFONT f = (HFONT)SendMessageW(c, WM_GETFONT, 0, 0);
    HGDIOBJ old = SelectObject(dc, f != nullptr ? (HGDIOBJ)f : GetStockObject(DEFAULT_GUI_FONT));
    RECT r = {0, 0, width, 0};
    DrawTextW(dc, text, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(c, dc);
    return (int)r.bottom;
}

HWND make_combo(HWND parent, int id)
{
    return make(parent, WC_COMBOBOXW, L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, id);
}

/* (TBS_TRANSPARENTBKGND: the parent's background at each paint, as ui.cpp's
 * sliders - a scrolled one kept its old place's) */
HWND make_slider(HWND parent, int id)
{
    return make(parent, TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_NOTICKS | TBS_TRANSPARENTBKGND, id);
}

/* Dead zone i's slider at its value, the value shown. */
void deadzone_text(int i)
{
    SendMessageW(t.deadzone[i], TBM_SETPOS, TRUE, t.p.deadzone[i]);
    SetWindowTextW(t.deadzoneValue[i], (std::to_wstring(t.p.deadzone[i]) + L"%").c_str());
}

void save()
{
    input_map::save(t.ini.c_str(), t.prof, t.p);
}

/* The hotkey a key alone is (the game keeps it: platform/window.cpp), or
 * -1.  A hotkey with modifiers leaves the key alone to the controls; so
 * does one that acts only while its mode is on (PC 2026-11-08: the cockpit
 * camera's calibration, the arrows: hotkey_shares_keys). */
int reserved(int vk)
{
    for (int h = 0; h < kHotkeyCount; h++)
        if (t.hot[h].vk == vk && !t.hot[h].ctrl && !t.hot[h].alt && !t.hot[h].shift && !hotkey_shares_keys(h))
            return h;
    return -1;
}

/* A wheel's binding names the joystick: a device of that wheel (its
 * pedals...), never an automatic wheel (as the game's, gamepad.cpp). */
bool named_by_wheel(const std::string &name)
{
    return is_wheel(t.prof) && (names_device(t.p, name.c_str()) || names_device(t.otherWheel, name.c_str()));
}

/* The wheel joystick the game takes for the profile shown: the one its
 * Device names, else (auto) the first wheel SDL types as one that the other
 * wheel does not name (as its Device or a binding's device), the second for
 * Wheel 2 when Wheel 1 is auto too. */
const JoyState *wheel_device(const Snapshot &s)
{
    if (!is_wheel(t.prof))
        return nullptr;
    if (t.p.device[0] != '\0') {
        for (const JoyState &j : s.joys)
            if (_stricmp(j.name.c_str(), t.p.device) == 0)
                return &j;
        return nullptr;
    }
    int skip = t.prof == kWheel2 && t.otherDevice.empty() ? 1 : 0;
    for (const JoyState &j : s.joys) {
        if (!j.wheel || named_by_wheel(j.name) ||
            (!t.otherDevice.empty() && _stricmp(j.name.c_str(), t.otherDevice.c_str()) == 0))
            continue;
        if (skip-- == 0)
            return &j;
    }
    return nullptr;
}

/* The gamepad of the controller profile shown: the Nth (not a wheel the
 * wheels' Device names); the debug camera's: the first. */
const PadState *pad_device(const Snapshot &s)
{
    if (is_wheel(t.prof) || is_hshifter(t.prof))
        return nullptr;                     /* (PC 2026-11-09: an H-Shifter has no device of its own) */
    int n = t.prof == kDebugCamera ? 0 : t.prof - kController1;
    for (const PadState &p : s.pads) {
        bool wheel = false;
        for (const std::string &d : t.wheelDevice)
            wheel = wheel || (!d.empty() && _stricmp(p.name.c_str(), d.c_str()) == 0);
        if (wheel)
            continue;
        if (n-- == 0)
            return &p;
    }
    return nullptr;
}

JoySource joy_source(const JoyState &j)
{
    return JoySource{j.buttonCount, j.axisCount, j.hatCount, j.buttons, j.axes, j.hats};
}

/* The devices the bindings name (input_map::device_name), as the game reads
 * them: the first joystick of each name in the snapshot `s' (src points into
 * it and into joy). */
struct Named {
    std::vector<JoySource> joy;
    std::vector<DeviceSource> src;
};
void named_devices(const Snapshot &s, Named *out)
{
    const size_t n = (size_t)device_count();
    out->joy.assign(n, JoySource{});
    out->src.assign(n, DeviceSource{});
    for (size_t d = 0; d < n; d++)
        for (const JoyState &j : s.joys) {
            if (_stricmp(j.name.c_str(), device_name((int)d + 1)) != 0)
                continue;
            out->joy[d] = joy_source(j);
            out->src[d].joy = &out->joy[d];
            for (const PadState &p : s.pads)
                if (p.id == j.id)
                    out->src[d].pad = &p.src;
            break;
        }
}

/* ---- the texts --------------------------------------------------------------- */

/* Slot s of control c takes the mouse's movement too: the debug camera's
 * look on the keyboard / mouse slot. */
bool takes_motion(int c, int s)
{
    return t.prof == kDebugCamera && s == 1 && c >= DEBUG_LOOK_UP && c <= DEBUG_LOOK_RIGHT;
}

void slot_text(int c, int s)
{
    std::wstring text;
    if (c == t.capControl && s == t.capSlot) {
        if (!t.capNote.empty())
            text = t.capNote;
        else if (takes_motion(c, s))
            text = tr(L"Press a key, click or move the mouse... (Esc)");
        else if (is_hshifter(t.prof))       /* PC (2026-11-09): any device, key or mouse button */
            text = tr(L"Press or move on any device, press a key or click... (Esc)");
        else if (s == 1)
            text = tr(L"Press a key or click... (Esc)");
        else
            text = tr(L"Press or move, on any device... (Esc)");
    } else {
        char d[256];                            /* (PC 2026-11-16: translated UTF-8) */
        describe(t.p.map[c][s], d, sizeof(d));
        text = d[0] != '\0' ? widen(d) : L"-";
    }
    SetWindowTextW(t.slot[c][s], text.c_str());
}

/* Hotkey h's key button (slot 0) or, PC, its device's (1). */
void hot_text(int h, int slot)
{
    std::wstring text;
    if (h == t.capHot && slot == t.capHotSlot) {
        if (!t.capHotNote.empty())
            text = t.capHotNote;
        else
            text = slot == 0 ? tr(L"Press the key... (click: cancel)") : tr(L"Press, on any controller or device... (Esc)");
    } else {
        char d[256];                            /* (PC 2026-11-16: translated UTF-8) */
        if (slot == 0)
            describe_combo(t.hot[h], d, sizeof(d));
        else
            describe(t.hotDevice[h], d, sizeof(d));
        text = d[0] != '\0' ? widen(d) : L"-";
    }
    SetWindowTextW(slot == 0 ? t.hotKey[h] : t.hotDev[h], text.c_str());
}

void hot_text(int h)
{
    hot_text(h, 0);
    hot_text(h, 1);
}

void heads_text()
{
    if (t.sub == kHotkeys) {
        /* PC: the hotkeys' columns */
        SetWindowTextW(t.head[0], tr(L"Hotkey"));
        SetWindowTextW(t.head[1], tr(L"Keyboard"));
        SetWindowTextW(t.head[2], g_sdl < 0 ? tr(L"Controller / device (no SDL3.dll)") : tr(L"Controller / device"));
        return;
    }
    SetWindowTextW(t.head[0], tr(L"Control"));
    std::wstring dev = is_hshifter(t.prof) ? tr(L"Any device, key or mouse button") : is_wheel(t.prof) ? tr(L"Wheel") : tr(L"Gamepad");
    if (g_sdl < 0)
        dev = trf(L"%s (no SDL3.dll)", dev.c_str());
    SetWindowTextW(t.head[1], dev.c_str());
    SetWindowTextW(t.head[2], is_hshifter(t.prof) ? L"" : tr(L"Keyboard / mouse"));     /* (PC 2026-11-09: one binding; hidden) */
}

/* The Device list: auto, the connected joysticks that are not plain
 * gamepads (wheels SDL maps as gamepads too), the saved name when it is
 * not connected.  Kept while it is dropped down. */
void fill_devices(const Snapshot &s)
{
    if (!is_wheel(t.prof) || SendMessageW(t.device, CB_GETDROPPEDSTATE, 0, 0))
        return;
    t.listed = s.devices;
    SendMessageW(t.device, CB_RESETCONTENT, 0, 0);
    t.deviceNames.assign(1, std::string());
    SendMessageW(t.device, CB_ADDSTRING, 0,
                 (LPARAM)(t.prof == kWheel1 ? tr(L"Automatic (the first wheel)") : tr(L"Automatic (the next wheel)")));
    int sel = 0;
    for (const JoyState &j : s.joys) {
        if (j.gamepad && !j.wheel)
            continue;
        if (_stricmp(j.name.c_str(), t.p.device) == 0)
            sel = (int)t.deviceNames.size();
        t.deviceNames.push_back(j.name);
        SendMessageW(t.device, CB_ADDSTRING, 0,
                     (LPARAM)(j.wheel ? trf(L"%s (wheel)", widen(j.name.c_str()).c_str()) : widen(j.name.c_str())).c_str());
    }
    if (sel == 0 && t.p.device[0] != '\0') {
        sel = (int)t.deviceNames.size();
        t.deviceNames.push_back(t.p.device);
        SendMessageW(t.device, CB_ADDSTRING, 0, (LPARAM)trf(L"%s (not connected)", widen(t.p.device).c_str()).c_str());
    }
    SendMessageW(t.device, CB_SETCURSEL, sel, 0);
}

/* The live view: the profile's analog controls after the dead zones. */
void live_text(const Snapshot &s)
{
    std::wstring buf;
    if (g_sdl < 0) {
        buf = tr(L"SDL3.dll is missing next to the launcher: no gamepads or wheels.");
    } else if (is_wheel(t.prof)) {
        const JoyState *j = wheel_device(s);
        if (j == nullptr) {
            buf = trf(L"No wheel connected for %s.", widen(tr(profile_name(t.prof))).c_str());
        } else {
            const JoySource js = joy_source(*j);
            Named named;                    /* (pedals of their own...) */
            named_devices(s, &named);
            const Sources src = {nullptr, &js, nullptr, 0, named.src.data(), (int)named.src.size()};
            auto v = [&](int c) {
                return std::max(value(t.p.map[c][0], src), value(t.p.map[c][1], src));
            };
            float steer = v(WHEEL_STEER_RIGHT) - v(WHEEL_STEER_LEFT);
            const float m = apply_deadzone(std::fabs(steer), t.p.deadzone[0] / 100.0f);
            steer = steer < 0.0f ? -m : m;
            /* PC (2026-11-10, user: "wheel devices need the clutch diagnostic
             * value next to the gas and brake"): the clutch as the game reads
             * it (clutch_input::clutch: after its dead zone), "-" when not
             * mapped */
            const int cl = clutch_control(t.prof);
            const bool clutchMapped = t.p.map[cl][0].source != Source::none || t.p.map[cl][1].source != Source::none;
            wchar_t clutch[16] = L"-";
            if (clutchMapped)
                swprintf(clutch, 16, L"%.2f", apply_deadzone(v(cl), t.p.deadzone[kWheelClutchDeadzone] / 100.0f));
            buf = trf(L"%s\nSteering %+.2f    Gas %.2f    Brake %.2f    Clutch %s", widen(j->name.c_str()).c_str(),
                      steer, apply_deadzone(v(WHEEL_GAS), t.p.deadzone[1] / 100.0f),
                      apply_deadzone(v(WHEEL_BRAKE), t.p.deadzone[2] / 100.0f), clutch);
        }
    } else {
        const PadState *p = pad_device(s);
        if (p == nullptr) {
            buf = trf(L"No gamepad connected for %s.", widen(tr(profile_name(t.prof))).c_str());
        } else {
            float st[2][2];
            for (int k = 0; k < 2; k++) {
                const float x = std::clamp(p->src.axes[k * 2] / 32767.0f, -1.0f, 1.0f);
                const float y = std::clamp(p->src.axes[k * 2 + 1] / 32767.0f, -1.0f, 1.0f);
                const float m = std::sqrt(x * x + y * y), d = t.p.deadzone[k] / 100.0f;
                const float f = m > d ? apply_deadzone(std::min(m, 1.0f), d) / m : 0.0f;
                st[k][0] = x * f;
                st[k][1] = y * f;
            }
            const float d = t.p.deadzone[2] / 100.0f;
            buf = trf(L"%s\nLeft stick %+.2f %+.2f    Right stick %+.2f %+.2f    L2 %.2f    R2 %.2f",
                      widen(p->name.c_str()).c_str(), st[0][0], st[0][1], st[1][0], st[1][1],
                      apply_deadzone(p->src.axes[kPadAxisLeftTrigger] / 32767.0f, d),
                      apply_deadzone(p->src.axes[kPadAxisRightTrigger] / 32767.0f, d));
        }
    }
    if (t.liveText != buf) {
        t.liveText = buf;
        SetWindowTextW(t.live, buf.c_str());
    }
}

/* Everything of the profile shown, and which controls show. */
void refresh()
{
    const bool map = t.sub == kMapping, cal = t.sub == kCalibration, hot = t.sub == kHotkeys;
    const int n = control_count(t.prof);
    for (int c = 0; c < kMaxControls; c++) {
        const bool on = map && c < n;
        if (c < n) {
            SetWindowTextW(t.label[c], widen(tr(control(t.prof, c).name)).c_str());
            for (int s = 0; s < kSlots; s++)
                slot_text(c, s);
        }
        ShowWindow(t.label[c], on ? SW_SHOW : SW_HIDE);
        for (int s = 0; s < kSlots; s++)        /* (PC 2026-11-09: an H-Shifter's one binding) */
            ShowWindow(t.slot[c][s], on && (s == 0 || !is_hshifter(t.prof)) ? SW_SHOW : SW_HIDE);
    }
    heads_text();
    for (HWND h : t.head)                       /* (PC 2026-11-09: an H-Shifter's one head over both columns) */
        ShowWindow(h, (map || hot) && !(map && h == t.head[2] && is_hshifter(t.prof)) ? SW_SHOW : SW_HIDE);
    ShowWindow(t.reset, map ? SW_SHOW : SW_HIDE);
    /* (the hotkeys are not a profile's) */
    ShowWindow(t.profile, hot ? SW_HIDE : SW_SHOW);
    const bool zones = cal && has_deadzones(t.prof);    /* (PC 2026-11-09: not an H-Shifter's) */
    for (int i = 0; i < kDeadzones; i++) {
        SetWindowTextW(t.deadzoneLabel[i], widen(tr(deadzone_name(t.prof, i))).c_str());
        deadzone_text(i);
        const bool on = zones && i < deadzone_count(t.prof);    /* (PC 2026-11-10: a wheel's fourth, the clutch's) */
        ShowWindow(t.deadzoneLabel[i], on ? SW_SHOW : SW_HIDE);
        ShowWindow(t.deadzone[i], on ? SW_SHOW : SW_HIDE);
        ShowWindow(t.deadzoneValue[i], on ? SW_SHOW : SW_HIDE);
    }
    const bool dev = cal && is_wheel(t.prof);
    ShowWindow(t.device, dev ? SW_SHOW : SW_HIDE);
    ShowWindow(t.deviceLabel, dev ? SW_SHOW : SW_HIDE);
    SetWindowTextW(t.inputLabel, t.prof == kWheel2 ? tr(L"Player 2 plays with") : tr(L"Player 1 plays with"));
    SendMessageW(t.input, CB_SETCURSEL, t.p.controller ? 1 : 0, 0);
    ShowWindow(t.input, dev ? SW_SHOW : SW_HIDE);
    ShowWindow(t.inputLabel, dev ? SW_SHOW : SW_HIDE);
    ShowWindow(t.live, zones ? SW_SHOW : SW_HIDE);
    ShowWindow(t.note, cal ? SW_SHOW : SW_HIDE);
    char menu[256];                             /* (PC 2026-11-16: translated UTF-8) */
    describe_combo(t.hot[kHotSettingsMenu], menu, sizeof(menu));
    char cam[256];
    describe_combo(t.hot[kHotDebugCamera], cam, sizeof(cam));
    /* (the debug camera's are not in the game's menu: its key instead) */
    const std::wstring inMenu =
        t.prof == kDebugCamera
            ? L" " + trf(L"The debug camera: %s in a race (the Hotkeys sub-tab).",
                         cam[0] != '\0' ? widen(cam).c_str() : tr(L"its hotkey"))
            : L" " + (menu[0] != '\0' ? trf(L"Also in the game's settings menu (%s), CONTROLS.", widen(menu).c_str())
                                      : std::wstring(tr(L"Also in the game's settings menu, CONTROLS.")));
    SetWindowTextW(t.note, is_hshifter(t.prof)
                               /* PC (2026-11-09) */
                               ? tr(L"An H-Shifter has nothing to calibrate: its gates are buttons. Its gears are "
                                    L"read with Gear shift behavior Realistic + Clutch and H-shifter On (Game tab, "
                                    L"Extras).")
                               : ((is_wheel(t.prof)
                                       ? std::wstring(tr(L"Analog stick center deadzone and "
                                                         L"Pedal starting travel."))
                                       : std::wstring(tr(L"Analog stick center deadzone and "
                                                         L"Trigger starting travel."))) +
                                  inMenu)
                                     .c_str());
    for (int h = 0; h < kHotkeyCount; h++) {
        SetWindowTextW(t.hotLabel[h], widen(tr(hotkey_name(h))).c_str());
        hot_text(h);
        ShowWindow(t.hotLabel[h], hot ? SW_SHOW : SW_HIDE);
        ShowWindow(t.hotKey[h], hot ? SW_SHOW : SW_HIDE);
        ShowWindow(t.hotDev[h], hot ? SW_SHOW : SW_HIDE);
    }
    ShowWindow(t.hotReset, hot ? SW_SHOW : SW_HIDE);
    ShowWindow(t.hotNote, hot ? SW_SHOW : SW_HIDE);
    t.listed = ~0u;
    fill_devices(snapshot());
    t.liveText.clear();
    for (HWND h : t.subTab)
        InvalidateRect(h, nullptr, FALSE);
}

void load_profile()
{
    load_hotkeys(t.ini.c_str(), t.hot, t.hotDevice);
    input_map::load(t.ini.c_str(), t.prof, &t.p);
    t.otherWheel = Profile();
    for (int w = 0; w < 2; w++) {
        Profile o;
        input_map::load(t.ini.c_str(), kWheel1 + w, &o);
        t.wheelDevice[w] = o.device;
        if (is_wheel(t.prof) && t.prof != kWheel1 + w)
            t.otherWheel = o;
    }
    t.otherDevice = t.prof == kWheel1 ? t.wheelDevice[1] : t.prof == kWheel2 ? t.wheelDevice[0] : std::string();
}

/* ---- the capture --------------------------------------------------------------- */

void cancel_capture()
{
    const int c = t.capControl, s = t.capSlot;
    t.capControl = -1;
    t.capNote.clear();
    if (c >= 0)
        slot_text(c, s);
}

void start_capture(int c, int s)
{
    const bool same = c == t.capControl && s == t.capSlot;
    cancel_capture();
    if (same)
        return;                             /* (a second click: cancelled) */
    t.capControl = c;
    t.capSlot = s;
    t.capUntil = GetTickCount64() + kCaptureMs;
    t.capNote = s == 0 && g_sdl < 0 ? tr(L"No SDL3.dll (Esc)") : L"";
    t.rest = snapshot();
    GetCursorPos(&t.capMouse);
    slot_text(c, s);
}

/* The capture takes b, from the gamepad `pad' (by position) or the joystick
 * `joy' (raw; both nullptr: the keyboard / mouse).  The profile's own device
 * reads it, or the device it came from by name (b.device):
 *   a controller's (the debug camera's): a gamepad its own (the game's port
 *     takes whichever is plugged in), any other joystick named;
 *   a wheel's: the joystick the game takes as this wheel its own (an
 *     automatic wheel without one connected: the joystick named its Device,
 *     unless a wheel's binding names it already), any other joystick or
 *     gamepad named. */
void take_hot_device(Binding b, const PadState *pad, const JoyState *joy);

void take(Binding b, const PadState *pad, const JoyState *joy)
{
    if (t.capHot >= 0) {
        take_hot_device(b, pad, joy);       /* PC: a hotkey's device */
        return;
    }
    const int c = t.capControl, s = t.capSlot;
    if (c < 0)
        return;
    const char *named = nullptr;
    if (is_hshifter(t.prof)) {
        /* PC (2026-11-09): an H-Shifter has no device of its own: a device's
         * binding names it, a gamepad's too */
        named = pad != nullptr ? pad->name.c_str() : joy != nullptr ? joy->name.c_str() : nullptr;
    } else {
    if (pad != nullptr && is_wheel(t.prof))
        named = pad->name.c_str();
    if (joy != nullptr && !is_wheel(t.prof))
        named = joy->name.c_str();
    }
    if (joy != nullptr && is_wheel(t.prof) && _stricmp(joy->name.c_str(), t.p.device) != 0) {
        const Snapshot now = snapshot();
        const JoyState *own = wheel_device(now);
        if (own == nullptr && t.p.device[0] == '\0' && !named_by_wheel(joy->name)) {
            snprintf(t.p.device, sizeof(t.p.device), "%s", joy->name.c_str());
            t.listed = ~0u;
            fill_devices(now);
        } else if (own == nullptr || own->id != joy->id) {
            named = joy->name.c_str();
        }
    }
    b.device = named != nullptr ? (uint8_t)device_id(named) : 0;
    t.p.map[c][s] = b;
    t.capControl = -1;
    t.capNote.clear();
    save();
    slot_text(c, s);
}

/* ---- the hotkey capture ---------------------------------------------------------- */

void cancel_hot_capture()
{
    const int h = t.capHot, s = t.capHotSlot;
    t.capHot = -1;
    t.capHotNote.clear();
    if (h >= 0)
        hot_text(h, s);
}

/* Hotkey h's key (slot 0) or, PC, its device (1: the devices' state now the
 * rest, as a slot 0 capture's). */
void start_hot_capture(int h, int slot)
{
    const bool same = h == t.capHot && slot == t.capHotSlot;
    cancel_capture();
    cancel_hot_capture();
    if (same)
        return;                             /* (a second click: cancelled) */
    t.capHot = h;
    t.capHotSlot = slot;
    t.capUntil = GetTickCount64() + kCaptureMs;
    if (slot == 1) {
        t.capHotNote = g_sdl < 0 ? tr(L"No SDL3.dll (Esc)") : L"";
        t.rest = snapshot();
    }
    hot_text(h, slot);
}

/* PC: the device's binding the capture took for the hotkey: a gamepad's
 * by position, on any controller (no device named); any other joystick's
 * (a wheel, a button box) on it by its name.  Refused: another hotkey's. */
void take_hot_device(Binding b, const PadState *pad, const JoyState *joy)
{
    (void)pad;
    const int h = t.capHot;
    if (h < 0 || t.capHotSlot != 1)
        return;
    b.device = joy != nullptr ? (uint8_t)device_id(joy->name.c_str()) : 0;
    for (int o = 0; o < kHotkeyCount; o++)
        if (o != h && t.hotDevice[o].source == b.source && t.hotDevice[o].index == b.index &&
            t.hotDevice[o].dir == b.dir && t.hotDevice[o].device == b.device) {
            t.capHotNote = trf(L"Taken by %s (Esc)", widen(tr(hotkey_name(o))).c_str());
            hot_text(h, 1);
            t.capHotNote.clear();           /* (the prompt's text stays until the next input) */
            return;
        }
    t.hotDevice[h] = b;
    t.capHot = -1;
    save_hotkey(t.ini.c_str(), h, t.hot[h], t.hotDevice[h]);
    hot_text(h, 1);
}

/* Why hotkey h may not be k ("" if it may): another hotkey's, a control's
 * key (k without modifiers: the game would keep it from the control; not
 * for a hotkey that acts only while its mode is on, PC 2026-11-08:
 * hotkey_shares_keys). */
std::wstring hot_refusal(int h, const KeyCombo &k)
{
    if (k.vk == VK_F4 && k.alt && !k.ctrl && !k.shift)
        return tr(L"Alt+F4 closes the game");
    for (int o = 0; o < kHotkeyCount; o++)
        if (o != h && t.hot[o] == k)
            return trf(L"Taken by %s", widen(tr(hotkey_name(o))).c_str());
    if (!k.ctrl && !k.alt && !k.shift && !hotkey_shares_keys(h))
        for (int p = 0; p < kProfileCount; p++) {
            Profile pr;
            input_map::load(t.ini.c_str(), p, &pr);
            for (int c = 0; c < control_count(p); c++) {
                const Binding &b = pr.map[c][is_hshifter(p) ? 0 : 1];   /* (PC 2026-11-09: an H-Shifter's slot 0) */
                if (b.source == Source::key && b.index == k.vk)
                    return trf(L"Taken by %s, %s", widen(tr(profile_name(p))).c_str(), widen(tr(control(p, c).name)).c_str());
            }
        }
    return L"";
}

void take_hot(const KeyCombo &k)
{
    const int h = t.capHot;
    if (h < 0)
        return;
    const std::wstring why = hot_refusal(h, k);
    if (!why.empty()) {
        t.capHotNote = trf(L"%s (click: cancel)", why.c_str());
        hot_text(h);
        t.capHotNote.clear();               /* (the prompt's text stays until the next input) */
        return;
    }
    t.hot[h] = k;
    t.capHot = -1;
    save_hotkey(t.ini.c_str(), h, k, t.hotDevice[h]);
    hot_text(h, 0);
}

/* The hotkey whose key button (*slot 0) or, PC, device button (1) `w' is,
 * or -1. */
int hot_of(HWND w, int *slot)
{
    if (w == nullptr || GetParent(w) != t.list)
        return -1;
    const int id = GetDlgCtrlID(w);
    *slot = id >= IDC_HOT_DEV0 ? 1 : 0;
    const int h = id - (*slot != 0 ? IDC_HOT_DEV0 : IDC_HOT_KEY0);
    return h >= 0 && h < kHotkeyCount ? h : -1;
}

/* The sub-tab of a sub-tab button's id, or -1. */
int sub_of(int id)
{
    return id == IDC_SUB_MAPPING ? kMapping : id == IDC_SUB_CALIBRATION ? kCalibration
                                            : id == IDC_SUB_HOTKEYS ? kHotkeys : -1;
}

/* The capture's look at the devices (slot 0): a button newly down, a hat's
 * new direction, an axis moved past half from where it was - on the
 * gamepads by position, on every other joystick (a wheel, pedals, a
 * shifter, a button box...) raw.  Inputs held at the start wait for their
 * release. */
void capture_poll()
{
    const Snapshot now = snapshot();
    for (const PadState &p : now.pads) {
        auto r = std::find_if(t.rest.pads.begin(), t.rest.pads.end(), [&](const PadState &x) { return x.id == p.id; });
        if (r == t.rest.pads.end()) {
            t.rest.pads.push_back(p);       /* (connected since: its state now the rest) */
            continue;
        }
        const uint32_t down = p.src.buttons & ~r->src.buttons & ~(1u << kPadButtonGuide);
        r->src.buttons &= p.src.buttons;
        for (int b = 0; b < kPadButtons; b++)
            if ((down >> b) & 1) {
                Binding x;
                x.source = Source::pad_button;
                x.index = (uint8_t)b;
                take(x, &p, nullptr);
                return;
            }
        for (int a = 0; a < kPadAxes; a++) {
            Binding x;
            if (std::abs(p.src.axes[a] - r->src.axes[a]) >= 16384 &&
                capture_axis(false, a, r->src.axes[a], p.src.axes[a], &x)) {
                take(x, &p, nullptr);
                return;
            }
        }
    }
    for (const JoyState &j : now.joys) {
        if (j.gamepad && !j.wheel)
            continue;                       /* (a gamepad: by position, above) */
        auto r = std::find_if(t.rest.joys.begin(), t.rest.joys.end(), [&](const JoyState &x) { return x.id == j.id; });
        if (r == t.rest.joys.end()) {
            t.rest.joys.push_back(j);
            continue;
        }
        for (int b = 0; b < j.buttonCount; b++) {
            const bool down = j.buttons[b] && !r->buttons[b];
            r->buttons[b] = j.buttons[b] && r->buttons[b];
            if (down) {
                Binding x;
                x.source = Source::joy_button;
                x.index = (uint8_t)b;
                take(x, nullptr, &j);
                return;
            }
        }
        for (int h = 0; h < j.hatCount; h++) {
            const uint8_t added = j.hats[h] & ~r->hats[h];
            r->hats[h] &= j.hats[h];
            for (int bit = 0; bit < 4; bit++)
                if ((added >> bit) & 1) {
                    Binding x;
                    x.source = Source::joy_hat;
                    x.index = (uint8_t)h;
                    x.dir = (int8_t)(1 << bit);
                    take(x, nullptr, &j);
                    return;
                }
        }
        for (int a = 0; a < j.axisCount; a++) {
            Binding x;
            const bool centred = std::abs(r->axes[a]) <= 16384;
            if ((!centred || std::abs(j.axes[a] - r->axes[a]) >= 16384) &&
                capture_axis(true, a, r->axes[a], j.axes[a], &x)) {
                take(x, nullptr, &j);
                return;
            }
        }
    }
}

/* The slot button `w' is: control *c, slot *s. */
bool slot_of(HWND w, int *c, int *s)
{
    if (w == nullptr)
        return false;
    const int id = GetDlgCtrlID(w) - IDC_SLOT0;
    if (id < 0 || id >= kMaxControls * kSlots || GetParent(w) != t.list)
        return false;
    *c = id / kSlots;
    *s = id % kSlots;
    return *c < control_count(t.prof);
}

/* The root window's art under the child c into dc (c's coordinates). */
void paint_root(HWND c, HDC dc)
{
    HWND root = GetAncestor(c, GA_ROOT);
    POINT o = {0, 0};
    MapWindowPoints(c, root, &o, 1);
    POINT old;
    OffsetViewportOrgEx(dc, -o.x, -o.y, &old);
    SendMessageW(root, WM_PRINTCLIENT, (WPARAM)dc, PRF_CLIENT);
    SetViewportOrgEx(dc, old.x, old.y, nullptr);
}

} // namespace

void create(HWND page, HINSTANCE inst, const std::wstring &ini)
{
    t.inst = inst;
    t.page = page;
    const int n = WideCharToMultiByte(CP_ACP, 0, ini.c_str(), -1, nullptr, 0, nullptr, nullptr);
    t.ini.assign(n > 0 ? (size_t)n : 1, '\0');
    if (n > 0)
        WideCharToMultiByte(CP_ACP, 0, ini.c_str(), -1, &t.ini[0], n, nullptr, nullptr);
    t.ini.resize(strlen(t.ini.c_str()));

    /* creation order = tab order */
    t.profile = make_combo(page, IDC_PROFILE);
    for (int p = 0; p < kProfileCount; p++)
        SendMessageW(t.profile, CB_ADDSTRING, 0, (LPARAM)widen(tr(profile_name(p))).c_str());
    SendMessageW(t.profile, CB_SETCURSEL, 0, 0);
    ShowWindow(t.profile, SW_SHOW);
    const int subIds[kSubCount] = {IDC_SUB_MAPPING, IDC_SUB_CALIBRATION, IDC_SUB_HOTKEYS};
    for (int i = 0; i < kSubCount; i++)
        t.subTab[i] = make(page, WC_BUTTONW, tr(kSubNames[i]), WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, subIds[i]);
    for (int i = 0; i < 3; i++)
        t.head[i] = make_label(page, L"", IDC_HEAD0 + i);
    t.list = panel::create(page, inst, IDC_LIST);
    ShowWindow(t.list, SW_SHOW);
    for (int c = 0; c < kMaxControls; c++) {
        t.label[c] = make_label(t.list, L"", IDC_CONTROL_LABEL0 + c);
        for (int s = 0; s < kSlots; s++)
            t.slot[c][s] = make(t.list, WC_BUTTONW, L"", WS_TABSTOP | BS_PUSHBUTTON, IDC_SLOT0 + c * kSlots + s);
    }
    t.reset = make(t.list, WC_BUTTONW, tr(L"Reset to defaults"), WS_TABSTOP | BS_PUSHBUTTON, IDC_RESET);
    for (int i = 0; i < kDeadzones; i++) {
        t.deadzoneLabel[i] = make_label(t.list, L"", IDC_DEADZONE_LABEL0 + i);
        t.deadzone[i] = make_slider(t.list, IDC_DEADZONE0 + i);
        SendMessageW(t.deadzone[i], TBM_SETRANGE, FALSE, MAKELPARAM(0, kDeadzoneMax));
        SendMessageW(t.deadzone[i], TBM_SETLINESIZE, 0, 1);
        SendMessageW(t.deadzone[i], TBM_SETPAGESIZE, 0, 5);
        t.deadzoneValue[i] = make_label(t.list, L"", IDC_DEADZONE_VALUE0 + i);
    }
    t.inputLabel = make_label(t.list, L"", IDC_INPUT_LABEL);
    t.input = make_combo(t.list, IDC_INPUT);
    SendMessageW(t.input, CB_ADDSTRING, 0, (LPARAM)tr(L"The wheel (while both are connected)"));
    SendMessageW(t.input, CB_ADDSTRING, 0, (LPARAM)tr(L"The controller (while both are connected)"));
    t.deviceLabel = make_label(t.list, tr(L"Device"), IDC_DEVICE_LABEL);
    t.device = make_combo(t.list, IDC_DEVICE);
    t.live = make_label(t.list, L"", IDC_LIVE);
    t.note = make_label(t.list, L"", IDC_NOTE);
    for (int h = 0; h < kHotkeyCount; h++) {
        t.hotLabel[h] = make_label(t.list, L"", IDC_HOT_LABEL0 + h);
        t.hotKey[h] = make(t.list, WC_BUTTONW, L"", WS_TABSTOP | BS_PUSHBUTTON, IDC_HOT_KEY0 + h);
        t.hotDev[h] = make(t.list, WC_BUTTONW, L"", WS_TABSTOP | BS_PUSHBUTTON, IDC_HOT_DEV0 + h);
    }
    t.hotReset = make(t.list, WC_BUTTONW, tr(L"Reset to defaults"), WS_TABSTOP | BS_PUSHBUTTON, IDC_HOT_RESET);
    t.hotNote = make_label(t.list,
                           tr(L"A click, then the key, with any of Ctrl, Alt and Shift held; a click again cancels. "
                              L"A right-click or Delete: no key. The game keeps a hotkey's key from the controls, and "
                              L"reads the hotkeys when it starts. "
                              L"Controller / device: a click, then a button, D-pad direction or axis on any "
                              L"controller, wheel or other device (Esc cancels). A controller's works on any "
                              L"controller; the press also reaches the game."),
                           IDC_HOT_NOTE);
    load_profile();
    refresh();
}

void set_font(HFONT font, HFONT tabFont)
{
    t.font = font;
    t.tabFont = tabFont;
    std::vector<HWND> all = {t.profile, t.reset, t.device, t.deviceLabel, t.input, t.inputLabel, t.live, t.note,
                             t.hotReset, t.hotNote};
    for (HWND h : t.head)
        all.push_back(h);
    for (int h = 0; h < kHotkeyCount; h++) {
        all.push_back(t.hotLabel[h]);
        all.push_back(t.hotKey[h]);
        all.push_back(t.hotDev[h]);
    }
    for (int c = 0; c < kMaxControls; c++) {
        all.push_back(t.label[c]);
        for (HWND s : t.slot[c])
            all.push_back(s);
    }
    for (int i = 0; i < kDeadzones; i++) {
        all.push_back(t.deadzone[i]);
        all.push_back(t.deadzoneLabel[i]);
        all.push_back(t.deadzoneValue[i]);
    }
    for (HWND h : all)
        if (h != nullptr)
            SendMessageW(h, WM_SETFONT, (WPARAM)font, FALSE);
    for (HWND h : t.subTab)
        InvalidateRect(h, nullptr, FALSE);
}

void layout(UINT dpi)
{
    if (t.page == nullptr)
        return;
    t.dpi = dpi;
    RECT r;
    GetClientRect(t.page, &r);
    const int w = r.right - r.left, h = r.bottom - r.top;
    /* the page does not scroll: the top row, the heads, the list below */
    panel::begin(t.page, h, px(kRowH));
    const int x = px(kPad);
    panel::place(t.page, t.profile, x, px(4), px(kProfileW), px(kComboListH));
    for (int i = 0; i < kSubCount; i++)
        panel::place(t.page, t.subTab[i], x + px(kProfileW) + px(16) + i * px(kSubTabW), px(2), px(kSubTabW),
                     px(kSubTabH));
    int y = px(kTopH);
    /* the slots: the list's width less the label, two alike (PC: the
     * hotkeys' key and device too, after their wider label) */
    const int scroll = GetSystemMetricsForDpi(SM_CXVSCROLL, dpi);
    const int labelW = px(t.sub == kHotkeys ? kHotLabelW : kLabelW);
    const int slotW = (w - scroll - 2 * x - labelW - px(kGap)) / 2;
    if (t.sub == kMapping || t.sub == kHotkeys) {
        /* (PC 2026-11-09: an H-Shifter's head over both columns, as its slot)
         * (PC 2026-11-16: two lines when a head's translation needs them) */
        const bool both = t.sub == kMapping && is_hshifter(t.prof);
        const int hw[3] = {labelW, both ? 2 * slotW + px(kGap) : slotW, slotW};
        int headH = px(kHeadH);
        for (int i = 0; i < 3; i++)
            headH = std::max(headH, std::min(2 * px(kHeadH), text_height(t.head[i], hw[i]) + px(2)));
        panel::place(t.page, t.head[0], x, y, hw[0], headH);
        panel::place(t.page, t.head[1], x + labelW, y, hw[1], headH);
        panel::place(t.page, t.head[2], x + labelW + slotW + px(kGap), y, hw[2], headH);
        y += headH;
    }
    /* PC (2026-11-16): the notes as tall as their text at their width (a
     * translation runs longer; the hotkeys' note was a line short at 150 %
     * dpi in English too), their old heights at least */
    const int noteW = w - scroll - 2 * x;
    const int hotNoteH = std::max(px(kHotNoteH), text_height(t.hotNote, noteW) + px(2));
    const int noteH = std::max(px(kNoteH), text_height(t.note, noteW) + px(2));
    panel::place(t.page, t.list, 0, y, w, h - y);

    const int n = control_count(t.prof);
    if (t.sub == kHotkeys) {
        const int content = kHotkeyCount * px(kRowH) + px(kRowH) + hotNoteH + px(kPad);
        const int cw = panel::begin(t.list, content, px(kRowH));
        HWND after = HWND_TOP;
        for (int h = 0; h < kHotkeyCount; h++) {
            const int ry = h * px(kRowH) + px(2);
            panel::place(t.list, t.hotLabel[h], x, ry + px(4), labelW, px(20));
            panel::place(t.list, t.hotKey[h], x + labelW, ry, slotW, px(kSlotH));
            panel::place(t.list, t.hotDev[h], x + labelW + slotW + px(kGap), ry, slotW, px(kSlotH));
            for (HWND b : {t.hotKey[h], t.hotDev[h]}) {     /* (the Tab key's order: a row's two, row by row) */
                SetWindowPos(b, after, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                after = b;
            }
        }
        SetWindowPos(t.hotReset, after, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        const int ry = kHotkeyCount * px(kRowH) + px(6);
        panel::place(t.list, t.hotReset, x, ry, px(150), px(kSlotH));
        panel::place(t.list, t.hotNote, x, ry + px(kRowH) + px(4), cw - 2 * x, hotNoteH);
    } else if (t.sub == kMapping) {
        const int content = n * px(kRowH) + px(kRowH) + px(kPad);
        panel::begin(t.list, content, px(kRowH));
        /* the rows in the launcher's order (the chase camera's first), the
         * Tab key's too (the slots' z-order) */
        HWND after = HWND_TOP;
        /* (PC 2026-11-09: an H-Shifter's one binding over both columns) */
        const int slot0W = is_hshifter(t.prof) ? 2 * slotW + px(kGap) : slotW;
        for (int r = 0; r < n; r++) {
            const int c = control_in_order(t.prof, r);
            const int ry = r * px(kRowH) + px(2);
            panel::place(t.list, t.label[c], x, ry + px(4), px(kLabelW), px(20));
            panel::place(t.list, t.slot[c][0], x + px(kLabelW), ry, slot0W, px(kSlotH));
            panel::place(t.list, t.slot[c][1], x + px(kLabelW) + slotW + px(kGap), ry, slotW, px(kSlotH));
            for (HWND s : t.slot[c]) {
                SetWindowPos(s, after, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                after = s;
            }
        }
        SetWindowPos(t.reset, after, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        panel::place(t.list, t.reset, x, n * px(kRowH) + px(6), px(150), px(kSlotH));
    } else if (is_hshifter(t.prof)) {
        /* PC (2026-11-09): an H-Shifter's: its note alone */
        const int cw = panel::begin(t.list, noteH + px(kPad), px(kRowH));
        panel::place(t.list, t.note, x, px(6), cw - 2 * x, noteH);
    } else {
        const int rows = deadzone_count(t.prof) + (is_wheel(t.prof) ? 2 : 0);
        const int content = rows * px(kRowH) + px(kLiveH) + noteH + px(kPad);
        const int cw = panel::begin(t.list, content, px(kRowH));
        int ry = px(2);
        if (is_wheel(t.prof)) {
            const int comboW = std::min(px(kComboW + 60), cw - x - px(kLabelW) - x);
            panel::place(t.list, t.inputLabel, x, ry + px(4), px(kLabelW), px(20));
            panel::place(t.list, t.input, x + px(kLabelW), ry, comboW, px(kComboListH));
            ry += px(kRowH);
            panel::place(t.list, t.deviceLabel, x, ry + px(4), px(kLabelW), px(20));
            panel::place(t.list, t.device, x + px(kLabelW), ry, comboW, px(kComboListH));
            ry += px(kRowH);
        }
        const int sliderW = std::max(px(60), std::min(px(kSliderW), cw - 2 * x - px(kLabelW) - px(kValueGap) - px(kValueW)));
        for (int i = 0; i < deadzone_count(t.prof); i++, ry += px(kRowH)) {
            panel::place(t.list, t.deadzoneLabel[i], x, ry + px(4), px(kLabelW), px(20));
            panel::place(t.list, t.deadzone[i], x + px(kLabelW), ry, sliderW, px(kSliderH));
            panel::place(t.list, t.deadzoneValue[i], x + px(kLabelW) + sliderW + px(kValueGap), ry + px(4), px(kValueW),
                         px(20));
        }
        panel::place(t.list, t.live, x, ry + px(4), cw - 2 * x, px(kLiveH));
        panel::place(t.list, t.note, x, ry + px(4) + px(kLiveH), cw - 2 * x, noteH);
    }
    InvalidateRect(t.page, nullptr, TRUE);
}

void show(bool shown)
{
    if (shown == t.shown)
        return;
    HWND root = GetAncestor(t.page, GA_ROOT);
    if (!shown) {
        cancel_capture();
        cancel_hot_capture();
        KillTimer(root, kTimer);
        sdl_stop();
        t.shown = false;
        return;
    }
    t.shown = true;
    sdl_start();
    load_profile();
    refresh();
    layout(t.dpi);
    SetTimer(root, kTimer, kTimerMs, nullptr);
}

void command(int id, int code, HWND from)
{
    (void)from;
    if (id == IDC_PROFILE) {
        if (code != CBN_SELCHANGE)
            return;
        const int sel = (int)SendMessageW(t.profile, CB_GETCURSEL, 0, 0);
        if (sel < 0 || sel >= kProfileCount || sel == t.prof)
            return;
        cancel_capture();
        t.prof = sel;
        load_profile();
        refresh();
        layout(t.dpi);
        return;
    }
    if (sub_of(id) >= 0) {
        if (code != BN_CLICKED || sub_of(id) == t.sub)
            return;
        cancel_capture();
        cancel_hot_capture();
        t.sub = sub_of(id);
        refresh();
        layout(t.dpi);
        return;
    }
    if (id == IDC_HOT_RESET) {
        if (code != BN_CLICKED)
            return;
        cancel_hot_capture();
        for (int h = 0; h < kHotkeyCount; h++) {
            t.hot[h] = hotkey_default(h);
            t.hotDevice[h] = hotkey_device_default(h);
            save_hotkey(t.ini.c_str(), h, t.hot[h], t.hotDevice[h]);
            hot_text(h);
        }
        return;
    }
    if (id >= IDC_HOT_KEY0 && id < IDC_HOT_KEY0 + kHotkeyCount) {
        if (code == BN_CLICKED)
            start_hot_capture(id - IDC_HOT_KEY0, 0);
        return;
    }
    if (id >= IDC_HOT_DEV0 && id < IDC_HOT_DEV0 + kHotkeyCount) {
        if (code == BN_CLICKED)
            start_hot_capture(id - IDC_HOT_DEV0, 1);   /* PC: the hotkey's device */
        return;
    }
    if (id == IDC_RESET) {
        if (code != BN_CLICKED)
            return;
        cancel_capture();
        /* the mapping's defaults; the dead zones and the device kept */
        Profile d;
        defaults(t.prof, &d);
        memcpy(d.deadzone, t.p.deadzone, sizeof(d.deadzone));
        memcpy(d.device, t.p.device, sizeof(d.device));
        t.p = d;
        save();
        refresh();
        return;
    }
    if (id == IDC_INPUT) {
        if (code != CBN_SELCHANGE)
            return;
        const int sel = (int)SendMessageW(t.input, CB_GETCURSEL, 0, 0);
        if (sel == 0 || sel == 1) {
            t.p.controller = sel == 1;
            save();
        }
        return;
    }
    if (id == IDC_DEVICE) {
        if (code != CBN_SELCHANGE)
            return;
        const int sel = (int)SendMessageW(t.device, CB_GETCURSEL, 0, 0);
        if (sel >= 0 && sel < (int)t.deviceNames.size()) {
            snprintf(t.p.device, sizeof(t.p.device), "%s", t.deviceNames[sel].c_str());
            save();
        }
        return;
    }
    const int slot = id - IDC_SLOT0;
    if (slot >= 0 && slot < kMaxControls * kSlots && code == BN_CLICKED && slot / kSlots < control_count(t.prof))
        start_capture(slot / kSlots, slot % kSlots);
}

void slider(int id, int code)
{
    const int i = id - IDC_DEADZONE0;
    if (i < 0 || i >= deadzone_count(t.prof))
        return;
    /* the live view follows the thumb; written once it is let go */
    t.p.deadzone[i] = std::clamp((int)SendMessageW(t.deadzone[i], TBM_GETPOS, 0, 0), 0, kDeadzoneMax);
    SetWindowTextW(t.deadzoneValue[i], (std::to_wstring(t.p.deadzone[i]) + L"%").c_str());
    if (code != TB_THUMBTRACK)
        save();
}

bool draw_item(const DRAWITEMSTRUCT *d)
{
    const int sb = sub_of((int)d->CtlID);
    if (sb < 0)
        return false;
    /* as the window's tabs: the art, the chosen one bright and underlined */
    const RECT &r = d->rcItem;
    paint_root(d->hwndItem, d->hDC);
    const bool chosen = sb == t.sub;
    if (chosen) {
        RECT bar = {r.left + px(10), r.bottom - px(4), r.right - px(10), r.bottom - px(1)};
        HBRUSH red = CreateSolidBrush(RGB(214, 22, 22));
        FillRect(d->hDC, &bar, red);
        DeleteObject(red);
    }
    SetBkMode(d->hDC, TRANSPARENT);
    SetTextColor(d->hDC, chosen ? RGB(255, 255, 255) : RGB(168, 168, 168));
    HGDIOBJ old = SelectObject(d->hDC, t.tabFont != nullptr ? t.tabFont : t.font);
    RECT box = r;
    box.bottom -= px(3);
    DrawTextW(d->hDC, tr(kSubNames[sb]), -1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(d->hDC, old);
    if ((d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT)) {
        RECT f = r;
        InflateRect(&f, -px(3), -px(3));
        DrawFocusRect(d->hDC, &f);
    }
    return true;
}

bool timer(UINT_PTR id)
{
    if (id != kTimer)
        return false;
    if (!t.shown)
        return true;
    if (t.capControl >= 0) {
        if (GetTickCount64() >= t.capUntil)
            cancel_capture();
        else if (t.capSlot == 0)
            capture_poll();
    }
    if (t.capHot >= 0 && GetTickCount64() >= t.capUntil)
        cancel_hot_capture();
    else if (t.capHot >= 0 && t.capHotSlot == 1)
        capture_poll();                     /* PC: a hotkey's device (take -> take_hot_device) */
    if (t.sub == kCalibration) {
        const Snapshot s = snapshot();
        if (s.devices != t.listed)
            fill_devices(s);
        live_text(s);
    }
    return true;
}

bool filter(const MSG &m)
{
    if (!t.shown)
        return false;
    if (t.capHot >= 0 && t.capHotSlot == 1) {
        /* PC: a hotkey's device: the keys not taken (Esc cancels), a click
         * elsewhere cancels (on its button too: start_hot_capture) */
        switch (m.message) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (m.wParam == VK_ESCAPE && !(m.lParam & (1 << 30)))
                cancel_hot_capture();
            return true;
        case WM_KEYUP:
        case WM_SYSKEYUP:
        case WM_CHAR:
        case WM_SYSCHAR:
        case WM_DEADCHAR:
        case WM_SYSDEADCHAR:
            return true;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN:
            if (m.hwnd != t.hotDev[t.capHot])
                cancel_hot_capture();
            return false;
        }
        return false;
    }
    if (t.capHot >= 0) {
        /* a hotkey: the next key that is not a modifier, with the modifiers
         * held (Esc too: the Esc menu's own); a click elsewhere cancels, one
         * on its button too (start_hot_capture) */
        switch (m.message) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            const int vk = (int)m.wParam;
            if ((m.lParam & (1 << 30)) || vk <= 0 || vk >= 256 || is_modifier(vk) || vk == VK_PROCESSKEY)
                return true;
            KeyCombo k;
            k.vk = (uint8_t)vk;
            k.ctrl = GetKeyState(VK_CONTROL) < 0;
            k.alt = GetKeyState(VK_MENU) < 0;
            k.shift = GetKeyState(VK_SHIFT) < 0;
            take_hot(k);
            return true;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP:
        case WM_CHAR:
        case WM_SYSCHAR:
        case WM_DEADCHAR:
        case WM_SYSDEADCHAR:
            return true;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN:
            if (m.hwnd != t.hotKey[t.capHot])
                cancel_hot_capture();
            return false;
        }
        return false;
    }
    if (t.capControl >= 0) {
        /* slot 1: the keyboard and the mouse; PC (2026-11-09) an H-Shifter's
         * slot 0 too (its one binding, any device) */
        const bool input = t.capSlot == 1 || is_hshifter(t.prof);
        Binding b;
        switch (m.message) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            if (m.lParam & (1 << 30))
                return true;                /* a held key's repeat */
            const int vk = (int)m.wParam;
            if (vk == VK_ESCAPE) {
                cancel_capture();
                return true;
            }
            if (!input)
                return true;
            if (reserved(vk) >= 0) {
                t.capNote = trf(L"The hotkey %s's (Esc)", widen(tr(hotkey_name(reserved(vk)))).c_str());
                slot_text(t.capControl, t.capSlot);
                t.capNote.clear();          /* (the prompt's text stays until the next input) */
                return true;
            }
            if (vk <= 0 || vk >= 256)
                return true;
            b.source = Source::key;
            b.index = (uint8_t)vk;
            take(b, nullptr, nullptr);
            return true;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP:
        case WM_CHAR:
        case WM_SYSCHAR:
        case WM_DEADCHAR:
        case WM_SYSDEADCHAR:
            return true;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONDBLCLK: {
            if (!input) {
                cancel_capture();           /* (a click elsewhere: slot 0 waits for a device) */
                return true;
            }
            int vk = VK_LBUTTON;
            if (m.message == WM_RBUTTONDOWN || m.message == WM_RBUTTONDBLCLK)
                vk = VK_RBUTTON;
            else if (m.message == WM_MBUTTONDOWN || m.message == WM_MBUTTONDBLCLK)
                vk = VK_MBUTTON;
            else if (m.message == WM_XBUTTONDOWN || m.message == WM_XBUTTONDBLCLK)
                vk = GET_XBUTTON_WPARAM(m.wParam) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
            b.source = Source::key;
            b.index = (uint8_t)vk;
            take(b, nullptr, nullptr);
            return true;
        }
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
        case WM_XBUTTONUP:
            return true;
        case WM_MOUSEWHEEL:
            /* (not an H-Shifter's gate: a notch is no held position) */
            if (input && !is_hshifter(t.prof) && GET_WHEEL_DELTA_WPARAM(m.wParam) != 0) {
                b.source = Source::mouse_wheel;
                b.dir = (int8_t)(GET_WHEEL_DELTA_WPARAM(m.wParam) > 0 ? 1 : -1);
                take(b, nullptr, nullptr);
            }
            return true;
        case WM_MOUSEMOVE:
        case WM_NCMOUSEMOVE:
            /* the debug camera's look: the cursor moved far enough one way */
            if (takes_motion(t.capControl, t.capSlot)) {
                POINT p;
                GetCursorPos(&p);
                const int dx = p.x - t.capMouse.x, dy = p.y - t.capMouse.y;
                if (std::max(std::abs(dx), std::abs(dy)) >= px(48)) {
                    b.source = Source::mouse_motion;
                    b.index = (uint8_t)(std::abs(dx) >= std::abs(dy) ? 0 : 1);
                    b.dir = (int8_t)((b.index == 0 ? dx : dy) < 0 ? -1 : 1);
                    take(b, nullptr, nullptr);
                }
            }
            return false;
        }
        return false;
    }
    /* a hotkey's button's right-click or Delete: no key (PC: no device) */
    int hs = 0;
    const int h = hot_of(m.hwnd, &hs);
    if (h >= 0) {
        if (m.message == WM_RBUTTONDOWN)
            return true;
        if (m.message == WM_RBUTTONUP || (m.message == WM_KEYDOWN && m.wParam == VK_DELETE)) {
            if (hs == 0)
                t.hot[h] = KeyCombo();
            else
                t.hotDevice[h] = Binding();
            save_hotkey(t.ini.c_str(), h, t.hot[h], t.hotDevice[h]);
            hot_text(h, hs);
            return true;
        }
    }
    /* a slot button's right-click or Delete: cleared */
    int c, s;
    if (slot_of(m.hwnd, &c, &s)) {
        if (m.message == WM_RBUTTONDOWN)
            return true;
        if (m.message == WM_RBUTTONUP || (m.message == WM_KEYDOWN && m.wParam == VK_DELETE)) {
            t.p.map[c][s] = Binding();
            save();
            slot_text(c, s);
            return true;
        }
    }
    return false;
}

bool is_text(HWND c)
{
    if (c == nullptr)
        return false;
    if (c == t.live || c == t.note || c == t.deviceLabel || c == t.inputLabel || c == t.hotNote)
        return true;
    for (HWND h : t.hotLabel)
        if (h == c)
            return true;
    for (HWND h : t.head)
        if (h == c)
            return true;
    for (HWND h : t.label)
        if (h == c)
            return true;
    for (HWND h : t.deadzoneLabel)
        if (h == c)
            return true;
    /* (the sliders take the same brush for their background) */
    for (int i = 0; i < kDeadzones; i++)
        if (t.deadzoneValue[i] == c || t.deadzone[i] == c)
            return true;
    return false;
}

} // namespace controls
