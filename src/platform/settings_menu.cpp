/* settings_menu.cpp - the in-game settings menu of GT3 PC (settings_menu.h). */
#include "platform/settings_menu.h"
#include "platform/audio.h"
#include "platform/frame_monitor.h"
#include "platform/gamepad.h"
#include "platform/gpu.h"
#include "platform/input_map.h"
#include "platform/language.h"
#include "platform/overlay.h"
#include "platform/settings.h"
#include "platform/window.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <stdio.h>
#include <string.h>

namespace settings_menu {
namespace {

using overlay::rgba;

std::atomic<bool> g_shown{false};
std::mutex g_lock;                              /* the selection and the changes (window / game threads) */
int g_sel;

/* ---- the settings ---------------------------------------------------- */

/* A setting: a list of values (count, get, set, value), or a value of its
 * own (count 0: text, step, edges); or a category (count kCategory), the
 * items after it one level deeper its own. */
constexpr int kCategory = -1;
struct Item {
    int depth;                                  /* 0 the top level */
    const char *label;
    const char *help;                           /* under the list while it is chosen */
    int count;                                  /* its values */
    int (*get)();
    void (*set)(int);
    const char *(*value)(int);
    void (*text)(char *out, size_t n);          /* the value shown */
    /* Left / Right (-1 / +1; heldMs: how long the key has been held, 0 a
     * fresh press), or 0 the next value (Enter, Cross) */
    void (*step)(int dir, int heldMs);
    int (*edges)();                             /* bit 0: a lower value exists, bit 1: a higher one */
    int hotkey = -1;                            /* the label followed by "(key)" (input_map::Hotkey) */
};

int aa_get() { return (int)settings::anti_aliasing(); }
void aa_set(int v) { settings::set_anti_aliasing((settings::AntiAliasing)v); }
const char *aa_value(int v)
{
    static const char *const names[settings::kAntiAliasingCount] = {"OFF", "FXAA", "MSAA 2X", "MSAA 4X",
                                                                    "MSAA 8X"};
    return v >= 0 && v < settings::kAntiAliasingCount ? names[v] : "?";
}

struct Ratio {
    int w, h;
    const char *name;
};
const Ratio kHudRatios[] = {{4, 3, "4:3"}, {16, 10, "16:10"}, {16, 9, "16:9"}, {21, 9, "21:9"}, {32, 9, "32:9"}};
constexpr int kHudRatioCount = (int)(sizeof(kHudRatios) / sizeof(kHudRatios[0]));

int hud_get()
{
    const float a = settings::display().hudMaxAspect;
    int best = 0;
    for (int i = 1; i < kHudRatioCount; i++) {
        const float d = std::fabs(std::log(a * kHudRatios[i].h / kHudRatios[i].w));
        const float db = std::fabs(std::log(a * kHudRatios[best].h / kHudRatios[best].w));
        if (d < db)
            best = i;
    }
    return best;
}
void hud_set(int v) { settings::set_hud_max_aspect(kHudRatios[v].w, kHudRatios[v].h); }
const char *hud_value(int v) { return v >= 0 && v < kHudRatioCount ? kHudRatios[v].name : "?"; }

int lod_get() { return settings::lod_enabled() ? 1 : 0; }
void lod_set(int v) { settings::set_lod_enabled(v != 0); }
const char *lod_value(int v) { return v ? "NORMAL" : "ALWAYS BEST"; }

/* The rear view by camera (settings::RearViewCamera) */
template <int C> int rear_get() { return settings::rear_view((settings::RearViewCamera)C) ? 1 : 0; }
template <int C> void rear_set(int v) { settings::set_rear_view((settings::RearViewCamera)C, v != 0); }
const char *on_off_value(int v) { return v ? "ON" : "OFF"; }

int split_get() { return (int)settings::split_screen(); }
void split_set(int v) { settings::set_split_screen((settings::SplitScreen)v); }
const char *split_value(int v) { return v == 2 ? "TWO MONITORS" : v == 1 ? "VERTICAL" : "HORIZONTAL"; }

int split_hud_get() { return settings::split_hud_analog() ? 1 : 0; }
void split_hud_set(int v) { settings::set_split_hud_analog(v != 0); }
int single_hud_get() { return settings::single_hud_digital() ? 1 : 0; }
void single_hud_set(int v) { settings::set_single_hud_digital(v != 0); }
const char *hud_digital_value(int v) { return v ? "ANALOG" : "DIGITAL"; }
const char *hud_analog_value(int v) { return v ? "DIGITAL" : "ANALOG"; }

const int kDistances[] = {100, 125, 150, 200, 300, 400};
const char *const kDistanceNames[] = {"100%", "125%", "150%", "200%", "300%", "400%"};
constexpr int kDistanceCount = (int)(sizeof(kDistances) / sizeof(kDistances[0]));

int distance_get()
{
    const int d = settings::render_distance();
    int best = 0;
    for (int i = 1; i < kDistanceCount; i++)
        if (std::abs(kDistances[i] - d) < std::abs(kDistances[best] - d))
            best = i;
    return best;
}
void distance_set(int v) { settings::set_render_distance(kDistances[v]); }
const char *distance_value(int v) { return v >= 0 && v < kDistanceCount ? kDistanceNames[v] : "?"; }

/* The fields of view (settings::fov_percent, replay_fov_percent): Left /
 * Right by 5 % (10 once held 1.5 s); Enter / Cross back to 100. */
void fov_step_of(int cur, void (*set)(int), int dir, int heldMs)
{
    if (dir == 0) {
        set(100);
        return;
    }
    const int step = heldMs < 1500 ? 5 : 10;
    /* to the step's grid first (a value set in the ini between them) */
    set(dir > 0 ? (cur / step + 1) * step : ((cur + step - 1) / step - 1) * step);
}

int fov_edges_of(int cur)
{
    return (cur > settings::kFovMin ? 1 : 0) | (cur < settings::kFovMax ? 2 : 0);
}

void fov_text(char *out, size_t n) { snprintf(out, n, "%d%%", settings::fov_percent()); }
void fov_step(int dir, int heldMs) { fov_step_of(settings::fov_percent(), settings::set_fov_percent, dir, heldMs); }
int fov_edges() { return fov_edges_of(settings::fov_percent()); }

/* The resolution scale (settings::render_scale): as the fields of view (5 %
 * a step, 10 once held; Enter / Cross: 100), shown with the window's size
 * at it. */
void scale_text(char *out, size_t n)
{
    const int s = settings::render_scale();
    int w = 0, h = 0;
    gpu::backbuffer_size(&w, &h);
    if (w > 0 && h > 0)
        snprintf(out, n, "%d%% (%dX%d)", s, (w * s + 50) / 100, (h * s + 50) / 100);
    else
        snprintf(out, n, "%d%%", s);
}
void scale_step(int dir, int heldMs) { fov_step_of(settings::render_scale(), settings::set_render_scale, dir, heldMs); }
int scale_edges()
{
    const int s = settings::render_scale();
    return (s > settings::kRenderScaleMin ? 1 : 0) | (s < settings::kRenderScaleMax ? 2 : 0);
}

/* The HUD safe zone (settings::hud_safe_zone): Left / Right by 1 % (5 once
 * held 1.5 s); Enter / Cross back to 100. */
void safe_text(char *out, size_t n) { snprintf(out, n, "%d%%", settings::hud_safe_zone()); }
void safe_step(int dir, int heldMs)
{
    const int cur = settings::hud_safe_zone();
    if (dir == 0) {
        settings::set_hud_safe_zone(100);
        return;
    }
    const int step = heldMs < 1500 ? 1 : 5;
    settings::set_hud_safe_zone(dir > 0 ? (cur / step + 1) * step : ((cur + step - 1) / step - 1) * step);
}
int safe_edges()
{
    const int s = settings::hud_safe_zone();
    return (s > settings::kHudSafeZoneMin ? 1 : 0) | (s < settings::kHudSafeZoneMax ? 2 : 0);
}

void replay_fov_text(char *out, size_t n) { snprintf(out, n, "%d%%", settings::replay_fov_percent()); }
void replay_fov_step(int dir, int heldMs)
{
    fov_step_of(settings::replay_fov_percent(), settings::set_replay_fov_percent, dir, heldMs);
}
int replay_fov_edges() { return fov_edges_of(settings::replay_fov_percent()); }

int monitor_get() { return (int)frame_monitor::mode(); }
void monitor_set(int v) { frame_monitor::set_mode((frame_monitor::Mode)v); }
const char *monitor_value(int v)
{
    static const char *const names[frame_monitor::kModes] = {"OFF", "SIMPLE", "DETAILED", "VERBOSE"};
    return v >= 0 && v < frame_monitor::kModes ? names[v] : "?";
}

/* The screen mode: the window's (Alt+Enter's too); set, the settings' as
 * well (the game starts in it) */
int screen_get() { return (int)window::mode(); }
void screen_set(int v)
{
    settings::set_display_mode((settings::DisplayMode)v);
    window::request_mode((settings::DisplayMode)v);
}
const char *screen_value(int v)
{
    static const char *const names[3] = {"WINDOWED", "BORDERLESS", "FULLSCREEN"};
    return v >= 0 && v < 3 ? names[v] : "?";
}

int menu_aspect_get() { return settings::menu_stretch() ? 1 : 0; }
void menu_aspect_set(int v) { settings::set_menu_stretch(v != 0); }
const char *menu_aspect_value(int v) { return v ? "STRETCH" : "ORIGINAL (4:3)"; }

int vsync_get() { return settings::vsync(); }
void vsync_set(int v) { settings::set_vsync(v); }
const char *vsync_value(int v)
{
    static const char *const names[4] = {"OFF", "1 FRAME", "2 FRAMES", "3 FRAMES"};
    return v >= 0 && v < 4 ? names[v] : "?";
}

/* The frame cap (settings::frame_cap_ms), as a frame rate: Left / Right to
 * the next of the usual rates below / above the cap's, OFF (no cap) at both
 * ends - a ring: right of the highest and left of the lowest, a held key
 * stopping there; as a frame time: Left / Right by 0.01 ms, 0.1 once held
 * 1.5 s, 1 once held 4 s (below the shortest, OFF; from OFF, the display's
 * refresh period).  A rate is the cap's within 0.2 %: the ms are kept to
 * 0.001, and 240 a second (4.167 ms) reads back as 239.98, which a tighter
 * test took for "below 240" - Right picked 240 again and stuck there, OFF out
 * of reach (user report). */
const double kRates[] = {30, 40, 45, 48, 50, 60, 72, 75, 90, 100, 120, 144, 165, 170,
                         175, 180, 200, 240, 280, 300, 360, 480, 500};
constexpr int kRateCount = (int)(sizeof(kRates) / sizeof(kRates[0]));
constexpr double kRateTolerance = 0.002;

double cap_rate()
{
    const double ms = settings::frame_cap_ms();
    return ms > 0.0 ? 1000.0 / ms : 0.0;
}

void rate_text(char *out, size_t n)
{
    const double r = cap_rate();
    if (r <= 0.0)
        snprintf(out, n, "OFF");
    else if (std::fabs(r - std::floor(r + 0.5)) < 0.005)
        snprintf(out, n, "%.0f FPS", r);
    else
        snprintf(out, n, "%.2f FPS", r);
}

void rate_step(int dir, int heldMs)
{
    const double r = cap_rate();
    if (dir == 0)
        dir = 1;                                /* (the next value: the ring onwards) */
    if (r <= 0.0) {
        /* from OFF: the lowest / the highest (a held key stays at OFF) */
        if (heldMs == 0)
            settings::set_frame_cap_ms(1000.0 / (dir > 0 ? kRates[0] : kRates[kRateCount - 1]));
        return;
    }
    const double tol = r * kRateTolerance;
    if (dir > 0) {
        for (int i = 0; i < kRateCount; i++)
            if (kRates[i] > r + tol) {
                settings::set_frame_cap_ms(1000.0 / kRates[i]);
                return;
            }
    } else {
        for (int i = kRateCount - 1; i >= 0; i--)
            if (kRates[i] < r - tol) {
                settings::set_frame_cap_ms(1000.0 / kRates[i]);
                return;
            }
    }
    settings::set_frame_cap_ms(0.0);            /* past the highest / the lowest: no cap */
}

int rate_edges()
{
    return 3;                                   /* (a ring) */
}

void time_text(char *out, size_t n)
{
    const double ms = settings::frame_cap_ms();
    if (ms <= 0.0)
        snprintf(out, n, "OFF");
    else
        snprintf(out, n, "%.3f MS", ms);
}

void time_step(int dir, int heldMs)
{
    const double ms = settings::frame_cap_ms();
    if (dir == 0)
        dir = 1;
    if (ms <= 0.0) {
        if (dir > 0 && heldMs == 0) {
            const double hz = gpu::refresh_hz();
            settings::set_frame_cap_ms(std::floor(100000.0 / (hz > 1.0 ? hz : 60.0)) / 100.0);
        }
        return;
    }
    const double step = heldMs < 1500 ? 0.01 : heldMs < 4000 ? 0.1 : 1.0;
    const double units = ms / step;
    const double v = (dir > 0 ? std::floor(units + 1e-6) + 1.0 : std::ceil(units - 1e-6) - 1.0) * step;
    settings::set_frame_cap_ms(v < settings::kFrameCapMin - 1e-9 ? 0.0 : std::min(v, settings::kFrameCapMax));
}

int time_edges()
{
    const double ms = settings::frame_cap_ms();
    if (ms <= 0.0)
        return 2;
    return 1 | (ms < settings::kFrameCapMax - 1e-9 ? 2 : 0);
}

int audio_out_get() { return (int)settings::audio_output(); }
void audio_out_set(int v) { settings::set_audio_output((settings::AudioOutput)v); }
const char *audio_out_value(int v)
{
    static const char *const names[settings::kAudioOutputCount] = {"AUTO", "LOW LATENCY KERNEL STREAMING", "WINDOWS WASAPI"};
    return v >= 0 && v < settings::kAudioOutputCount ? names[v] : "?";
}

const double kBuffers[] = {1.0, 1.5, 2.0, 2.5, 3.0, 4.0, 5.0, 6.0, 8.0, 10.0, 15.0, 20.0};
constexpr int kBufferCount = (int)(sizeof(kBuffers) / sizeof(kBuffers[0]));

/* The buffer as set (a crackle raises it by 0.5 ms: between the presets
 * too); Left / Right to the next preset below / above, the next value wraps
 * from the largest to the smallest. */
void buffer_text(char *out, size_t n) { snprintf(out, n, "%g MS", settings::audio_buffer_ms()); }

void buffer_step(int dir, int repeats)
{
    (void)repeats;
    const double ms = settings::audio_buffer_ms();
    if (dir < 0) {
        for (int i = kBufferCount - 1; i >= 0; i--)
            if (kBuffers[i] < ms - 0.01) {
                settings::set_audio_buffer_ms(kBuffers[i]);
                return;
            }
        return;
    }
    for (int i = 0; i < kBufferCount; i++)
        if (kBuffers[i] > ms + 0.01) {
            settings::set_audio_buffer_ms(kBuffers[i]);
            return;
        }
    if (dir == 0)
        settings::set_audio_buffer_ms(kBuffers[0]);
}

int buffer_edges()
{
    const double ms = settings::audio_buffer_ms();
    return (ms > kBuffers[0] + 0.01 ? 1 : 0) | (ms < kBuffers[kBufferCount - 1] - 0.01 ? 2 : 0);
}

/* The language (settings::language, platform/language.h): AUTO and the
 * languages installed, a ring; the game takes it at its next start (the
 * value says so while it differs from the running one's). */
void language_text(char *out, size_t n)
{
    const int s = settings::language();
    const bool later = language::chosen() != language::current();
    if (s == settings::kLanguageAuto)
        snprintf(out, n, "AUTO (%s)%s", language::name(language::disc_language()), later ? " - NEXT START" : "");
    else
        snprintf(out, n, "%s%s", language::name(s), later ? " - NEXT START" : "");
}

void language_step(int dir, int heldMs)
{
    if (heldMs != 0)
        return;                                 /* (one step a press) */
    int list[1 + language::kCount], count = 0, at = 0;
    list[count++] = settings::kLanguageAuto;
    for (int i = 0; i < language::kCount; i++)
        if (language::installed(i)) {
            if (settings::language() == i)
                at = count;
            list[count++] = i;
        }
    at = (at + (dir < 0 ? count - 1 : 1)) % count;
    settings::set_language(list[at]);
}

int language_edges()
{
    return 3;                                   /* (a ring) */
}

/* The units (settings::speed_unit, power_unit, torque_unit): AUTO (the
 * language's preset) then the units, in the game's own order; the game
 * takes a change at its next tick (game/app/pc_units.h). */
int speed_unit_get() { return settings::speed_unit() + 1; }
void speed_unit_set(int v) { settings::set_speed_unit(v - 1); }
const char *speed_unit_value(int v)
{
    static const char *const names[1 + settings::kSpeedUnits] = {"AUTO (THE LANGUAGE'S)", "KM/H, KM", "MPH, MILES"};
    return v >= 0 && v <= settings::kSpeedUnits ? names[v] : "?";
}
int power_unit_get() { return settings::power_unit() + 1; }
void power_unit_set(int v) { settings::set_power_unit(v - 1); }
const char *power_unit_value(int v)
{
    static const char *const names[1 + settings::kPowerUnits] = {"AUTO (THE LANGUAGE'S)", "PS", "HP", "BHP", "CV",
                                                                 "KW"};
    return v >= 0 && v <= settings::kPowerUnits ? names[v] : "?";
}
int torque_unit_get() { return settings::torque_unit() + 1; }
void torque_unit_set(int v) { settings::set_torque_unit(v - 1); }
const char *torque_unit_value(int v)
{
    static const char *const names[1 + settings::kTorqueUnits] = {"AUTO (THE LANGUAGE'S)", "KGF.M", "FT.LB", "NM"};
    return v >= 0 && v <= settings::kTorqueUnits ? names[v] : "?";
}

/* The starting credits (settings::starting_credits): the version's own,
 * 15,000, 18,000; x 100 (yen) shown in a Japanese run, as the game pays them
 * (game/app/pc_new_game.h). */
const int kCredits[] = {0, 15000, 18000};
int credits_get()
{
    const int c = settings::starting_credits();
    return c == 15000 ? 1 : c == 18000 ? 2 : 0;
}
void credits_set(int v) { settings::set_starting_credits(kCredits[v >= 0 && v < 3 ? v : 0]); }
const char *credits_value(int v)
{
    const bool yen = language::current() == language::kJapanese;
    switch (v) {
    case 1:
        return yen ? "1,500,000 YEN" : "15,000 CR.";
    case 2:
        return yen ? "1,800,000 YEN" : "18,000 CR.";
    default:
        return "VERSION'S OWN";
    }
}

/* the output running (audio::describe), shown only */
void audio_now_text(char *out, size_t n) { audio::describe(out, n); }
void no_step(int, int) {}
int no_edges() { return 0; }

/* The dead zones (input_map::deadzone of profile P, dead zone I): Left /
 * Right by 1 % (5 once held 1.5 s); Enter / Cross back to 0. */
template <int P, int I> void dz_text(char *out, size_t n) { snprintf(out, n, "%d%%", input_map::deadzone(P, I)); }
template <int P, int I> void dz_step(int dir, int heldMs)
{
    const int cur = input_map::deadzone(P, I);
    if (dir == 0) {
        input_map::set_deadzone(P, I, 0);
        return;
    }
    const int step = heldMs < 1500 ? 1 : 5;
    input_map::set_deadzone(P, I, dir > 0 ? (cur / step + 1) * step : ((cur + step - 1) / step - 1) * step);
}
template <int P, int I> int dz_edges()
{
    const int d = input_map::deadzone(P, I);
    return (d > 0 ? 1 : 0) | (d < input_map::kDeadzoneMax ? 2 : 0);
}
#define DEADZONE(P, I) 0, nullptr, nullptr, nullptr, dz_text<P, I>, dz_step<P, I>, dz_edges<P, I>

/* What player N plays with while both the controller and the wheel are
 * connected (input_map::plays_with_controller): 0 the controller, 1 the
 * wheel. */
template <int N> int device_get() { return input_map::plays_with_controller(N) ? 0 : 1; }
template <int N> void device_set(int v) { input_map::set_plays_with_controller(N, v == 0); }
const char *device_value(int v) { return v ? "WHEEL" : "CONTROLLER"; }
const char kDeviceHelp[] = "WHAT THE PLAYER PLAYS WITH WHILE BOTH ARE\nCONNECTED; WITH ONE CONNECTED, THAT ONE.";

const char kStickHelp[] = "HOW FAR THE STICK MOVES BEFORE IT COUNTS,\nOF ITS TRAVEL. ENTER: 0%.";
const char kTriggerHelp[] = "HOW FAR THE TRIGGERS GO BEFORE THEY COUNT,\nOF THEIR TRAVEL. ENTER: 0%.";
const char kSteerHelp[] = "HOW FAR THE WHEEL TURNS BEFORE IT STEERS,\nOF ITS TRAVEL EACH WAY. ENTER: 0%.";
const char kPedalHelp[] = "HOW FAR THE PEDAL GOES BEFORE IT COUNTS,\nOF ITS TRAVEL. ENTER: 0%.";

/* (help: any number of lines, '\n' between; the panel takes the most lines
 * and the longest line of them all) */
const Item kItems[] = {
    {0, "DISPLAY", "SCREEN MODE, MENU ASPECT, VERTICAL SYNC, FRAME RATE\nAND FRAME TIMES.", kCategory},
    {1, "SCREEN MODE", "WINDOWED: A RESIZABLE WINDOW.\nBORDERLESS: A WINDOW COVERING THE MONITOR.\nFULLSCREEN: EXCLUSIVE, THE MONITOR'S DISPLAY MODE.",
     3, screen_get, screen_set, screen_value},
    {1, "MENU ASPECT", "ORIGINAL: THE MENUS AT 4:3, BARS AT THE SIDES.\nSTRETCH: THE MENUS FILL THE WINDOW; THEIR CAR\nMODELS AND TEXTS KEEP THEIR SHAPE.",
     2, menu_aspect_get, menu_aspect_set, menu_aspect_value},
    {1, "VERTICAL SYNC", "OFF: NO VSYNC.\n1-3: VSYNC ON, NUMBER OF BUFFERED FRAMES.", 4,
     vsync_get, vsync_set, vsync_value},
    {1, "FRAME RATE CAP", "THE FRAMES AT MOST THIS OFTEN (A FRAME TIME CAP);\nOFF: AS FAST AS THEY ARE DRAWN.", 0, nullptr,
     nullptr, nullptr, rate_text, rate_step, rate_edges},
    {1, "  FRAME TIME CAP", "THE SAME CAP IN MS, 0.01 A STEP (HOLD: FASTER);\nBELOW 1 MS, OFF.", 0, nullptr, nullptr,
     nullptr, time_text, time_step, time_edges},
    {1, "FRAME TIME MONITOR", "THE FRAME TIMES OVER THE TOP LEFT CORNER.\nDETAILED: WITH THE FRAME TIME'S BREAKDOWN.\nVERBOSE: ALSO THE GPU'S TIME BY SECTION.",
     frame_monitor::kModes, monitor_get, monitor_set, monitor_value, nullptr, nullptr, nullptr,
     input_map::kHotFrameMonitor},
    {0, "GRAPHICS", "RESOLUTION, ANTI-ALIASING, DETAIL, FIELD OF VIEW AND REAR VIEW.", kCategory},
    {1, "RESOLUTION SCALE", "THE GAME'S RESOLUTION, OF THE WINDOW'S.\nABOVE 100%: SCALED DOWN TO THE WINDOW\n(SUPERSAMPLING AA); MORE GPU. ENTER: 100%.",
     0, nullptr, nullptr, nullptr, scale_text, scale_step, scale_edges},
    {1, "ANTI-ALIASING", "FXAA: THE RACE VIEWS' 3D, BEFORE THE HUD.\nMSAA: EVERY PICTURE, MENUS TOO; MORE GPU.",
     settings::kAntiAliasingCount, aa_get, aa_set, aa_value},
    {1, "LEVELS OF DETAIL", "BEST: CARS, COURSES, AND SPLIT-SCREEN ALWAYS AT FULL DETAIL.", 2,
     lod_get, lod_set, lod_value},
    {1, "RENDER DISTANCE", "HOW FAR THE COURSE AND THE CARS ARE DRAWN,\nOF THE ORIGINAL GAME'S DISTANCE.", kDistanceCount,
     distance_get, distance_set, distance_value},
    {1, "FIELD OF VIEW", "THE DRIVING VIEWS' ANGLE, OF EACH CAMERA'S OWN.\nNOT THE MIRROR. ENTER: 100%.", 0,
     nullptr, nullptr, nullptr, fov_text, fov_step, fov_edges},
    {1, "  REPLAY FIELD OF VIEW", "THE REPLAY CAMERAS' ANGLE, OF EACH CAMERA'S OWN\n(THE ATTRACT MODE'S DEMOS TOO). ENTER: 100%.",
     0, nullptr, nullptr, nullptr, replay_fov_text, replay_fov_step, replay_fov_edges},
    {1, "REAR VIEW", "THE REAR-VIEW MIRROR, BY CAMERA.\nHIGH IMPACT ON FRAME RATE.", kCategory},
    {2, "BUMPER CAM", "THE REAR-VIEW MIRROR IN THE BUMPER CAMERA.\nHIGH IMPACT ON FRAME RATE.", 2, rear_get<0>,
     rear_set<0>, on_off_value},
    {2, "CHASE CAM", "THE REAR-VIEW MIRROR IN THE CHASE CAMERA.\nHIGH IMPACT ON FRAME RATE.", 2, rear_get<1>,
     rear_set<1>, on_off_value},
    {0, "HUD", "THE RACE HUD'S WIDTH, SIZE AND METERS.", kCategory},
    {1, "RACE HUD MAX WIDTH", "ADJUST SIDE-ANCHORED HUD ELEMENTS.", kHudRatioCount, hud_get,
     hud_set, hud_value},
    {1, "HUD SAFE ZONE", "THE RACE HUD SCALED DOWN TO FIT THIS MUCH\nOF THE SCREEN. 100%: THE WHOLE SCREEN. ENTER: 100%.",
     0, nullptr, nullptr, nullptr, safe_text, safe_step, safe_edges},
    {1, "SINGLE PLAYER HUD", "A SINGLE PLAYER'S METERS.", 2, single_hud_get, single_hud_set, hud_analog_value},
    {0, "SPLIT SCREEN", "THE 2-PLAYER SPLIT SCREEN.", kCategory},
    {1, "SPLIT SCREEN", "HORIZONTAL: ONE VIEW ABOVE THE OTHER.\nVERTICAL: SIDE BY SIDE.\nTWO MONITORS: PLAYER 2 ON A SECOND MONITOR.\nFROM THE NEXT RACE OR REPLAY.",
     settings::kSplitScreenCount, split_get, split_set, split_value},
    {1, "SPLIT SCREEN HUD", "THE SPLIT SCREEN'S METERS.", 2, split_hud_get, split_hud_set, hud_digital_value},
    {0, "AUDIO", "THE SOUND OUTPUT.", kCategory},
    {1, "AUDIO OUTPUT", "KS: DIRECT-TO-DRIVER AUDIO, BEST LATENCY.\nOTHER PROGRAMS MAY NOT HAVE AUDIO\nIF YOUR AUDIO DEVICE IS NOT SHAREABLE.\nWASAPI: NORMAL WINDOWS WASAPI, WORSE AUDIO LATENCY\nAUTO: TRIES KS, ELSE WASAPI.",
     settings::kAudioOutputCount, audio_out_get, audio_out_set, audio_out_value},
    {1, "AUDIO BUFFER", "SHORTER: LESS LATENCY.\n AUTOMATICALLY ADJUSTS IF A CRACKLE IS DETECTED.",
     0, nullptr, nullptr, nullptr, buffer_text, buffer_step, buffer_edges},
    {1, "  RUNNING", "THE OUTPUT IN USE AND ITS LATENCY\n(OUTPUT + SCHEDULING)", 0, nullptr, nullptr, nullptr,
     audio_now_text, no_step, no_edges},
    {0, "GAME", "THE GAME'S LANGUAGE, UNITS AND A NEW GAME'S MONEY.", kCategory},
    {1, "LANGUAGE", "THE MENUS, TEXTS, CAR NAMES AND UNITS.\nAUTO: THE INSTALLED DISC'S OWN.\nTAKES EFFECT AT THE NEXT START.",
     0, nullptr, nullptr, nullptr, language_text, language_step, language_edges},
    {1, "SPEED UNIT", "SPEEDS AND DISTANCES.\nAUTO: THE LANGUAGE'S OWN (US ENGLISH MPH, THE OTHERS KM/H).\nA RACE'S SPEEDOMETER DIAL: FROM THE NEXT RACE.",
     1 + settings::kSpeedUnits, speed_unit_get, speed_unit_set, speed_unit_value},
    {1, "POWER UNIT", "ENGINE POWER.\nAUTO: THE LANGUAGE'S OWN (US ENGLISH HP, JAPANESE AND GERMAN PS,\nUK ENGLISH, ITALIAN AND SPANISH BHP, FRENCH CV).",
     1 + settings::kPowerUnits, power_unit_get, power_unit_set, power_unit_value},
    {1, "TORQUE UNIT", "ENGINE TORQUE.\nAUTO: THE LANGUAGE'S OWN (US ENGLISH FT.LB, THE OTHERS KGF.M).",
     1 + settings::kTorqueUnits, torque_unit_get, torque_unit_set, torque_unit_value},
    {1, "STARTING CREDITS", "THE MONEY A NEW GAME STARTS WITH (JAPANESE: x100, YEN).\nVERSION'S OWN: USA / EUROPE 18,000, JAPAN 1,500,000 YEN.\nFROM THE NEXT NEW GAME.",
     3, credits_get, credits_set, credits_value},
    {0, "CONTROLS", "EACH PLAYER'S DEVICE AND DEAD ZONES.\nTHE MAPPINGS: THE LAUNCHER'S CONTROLS TAB.", kCategory},
    {1, "PLAYER 1", "PLAYER 1'S DEVICE, CONTROLLER AND WHEEL.", kCategory},
    {2, "DEVICE", kDeviceHelp, 2, device_get<0>, device_set<0>, device_value},
    {2, "CONTROLLER", "PLAYER 1'S CONTROLLER (PORT 1).", kCategory},
    {3, "LEFT STICK DEAD ZONE", kStickHelp, DEADZONE(input_map::kController1, 0)},
    {3, "RIGHT STICK DEAD ZONE", kStickHelp, DEADZONE(input_map::kController1, 1)},
    {3, "TRIGGER DEAD ZONE", kTriggerHelp, DEADZONE(input_map::kController1, 2)},
    {2, "WHEEL", "PLAYER 1'S WHEEL.", kCategory},
    {3, "STEERING DEAD ZONE", kSteerHelp, DEADZONE(input_map::kWheel1, 0)},
    {3, "GAS PEDAL DEAD ZONE", kPedalHelp, DEADZONE(input_map::kWheel1, 1)},
    {3, "BRAKE PEDAL DEAD ZONE", kPedalHelp, DEADZONE(input_map::kWheel1, 2)},
    {1, "PLAYER 2", "PLAYER 2'S DEVICE, CONTROLLER AND WHEEL.", kCategory},
    {2, "DEVICE", kDeviceHelp, 2, device_get<1>, device_set<1>, device_value},
    {2, "CONTROLLER", "PLAYER 2'S CONTROLLER (PORT 2).", kCategory},
    {3, "LEFT STICK DEAD ZONE", kStickHelp, DEADZONE(input_map::kController2, 0)},
    {3, "RIGHT STICK DEAD ZONE", kStickHelp, DEADZONE(input_map::kController2, 1)},
    {3, "TRIGGER DEAD ZONE", kTriggerHelp, DEADZONE(input_map::kController2, 2)},
    {2, "WHEEL", "PLAYER 2'S WHEEL.", kCategory},
    {3, "STEERING DEAD ZONE", kSteerHelp, DEADZONE(input_map::kWheel2, 0)},
    {3, "GAS PEDAL DEAD ZONE", kPedalHelp, DEADZONE(input_map::kWheel2, 1)},
    {3, "BRAKE PEDAL DEAD ZONE", kPedalHelp, DEADZONE(input_map::kWheel2, 2)},
};
constexpr int kItemCount = (int)(sizeof(kItems) / sizeof(kItems[0]));
#undef DEADZONE

/* ---- the tree -------------------------------------------------------- */

/* The categories open (all closed at first; kept while the game runs). */
bool g_open[kItemCount];

/* The category item i is in (-1 at the top level). */
int parent(int i)
{
    for (int j = i - 1; j >= 0; j--)
        if (kItems[j].depth < kItems[i].depth)
            return j;
    return -1;
}

/* Every category above item i is open. */
bool visible(int i)
{
    for (int p = parent(i); p >= 0; p = parent(p))
        if (!g_open[p])
            return false;
    return true;
}

/* The visible items in their order (n: how many); the index of item `sel'
 * among them in *at. */
int visible_items(int *out, int sel, int *at)
{
    int n = 0;
    *at = 0;
    for (int i = 0; i < kItemCount; i++)
        if (visible(i)) {
            if (i == sel)
                *at = n;
            out[n++] = i;
        }
    return n;
}

/* The chosen setting's value moved by d (held at its ends), or to the next
 * one (wrapping) for d = 0; heldMs: how long the key has been held (0: a
 * fresh press).  On a category: Right (or the next value) opens it, Left
 * closes it (one already closed: its own category is chosen); a held key
 * does neither. */
void change(int d, int heldMs)
{
    const Item &it = kItems[g_sel];
    if (it.count == kCategory) {
        if (heldMs != 0)
            return;
        if (d == 0)
            g_open[g_sel] = !g_open[g_sel];
        else if (d > 0)
            g_open[g_sel] = true;
        else if (g_open[g_sel])
            g_open[g_sel] = false;
        else if (parent(g_sel) >= 0)
            g_sel = parent(g_sel);
        return;
    }
    if (it.count == 0) {
        it.step(d, heldMs);
        return;
    }
    const int cur = it.get();
    int v = d == 0 ? (cur + 1) % it.count : cur + d;
    v = std::max(0, std::min(it.count - 1, v));
    if (v != cur)
        it.set(v);
}

/* The value shown and whether lower / higher ones exist (bits 0 / 1). */
int item_value(const Item &it, char *out, size_t n)
{
    if (it.count == kCategory) {
        if (n > 0)
            out[0] = '\0';
        return 0;
    }
    if (it.count == 0) {
        it.text(out, n);
        return it.edges();
    }
    const int cur = it.get();
    snprintf(out, n, "%s", it.value(cur));
    return (cur > 0 ? 1 : 0) | (cur < it.count - 1 ? 2 : 0);
}

/* To the next / the previous visible item (wrapping). */
void move(int d)
{
    int list[kItemCount], at;
    const int n = visible_items(list, g_sel, &at);
    g_sel = list[(at + d + n) % n];
}

/* ---- the game controllers (read at each present) --------------------- */

enum : uint16_t {
    PAD_UP = 1u << 4, PAD_RIGHT = 1u << 5, PAD_DOWN = 1u << 6, PAD_LEFT = 1u << 7,
    PAD_CIRCLE = 1u << 13, PAD_CROSS = 1u << 14
};
uint16_t g_padPrev[2];
bool g_padSync = true;                          /* the next read only takes the buttons down (just opened) */
uint16_t g_repeatButton;                        /* the D-pad direction held */
ULONGLONG g_repeatAt;
ULONGLONG g_padDownAt;                          /* when it went down */
/* the keyboard's held key (key): when it went down, its last step */
ULONGLONG g_keyDownAt, g_keyStepAt;
/* A held key or direction repeats at most this often (ms): the keyboard's
 * autorepeat (~30 a second) made the frame time's steps run away */
constexpr ULONGLONG kRepeatMs = 66;

void poll_pads()
{
    uint16_t now = 0;
    for (int port = 0; port < 2; port++) {
        gamepad::State st;
        const uint16_t b = gamepad::read(port, &st) ? st.buttons : 0;
        if (g_padSync) {
            g_padPrev[port] = b;
            continue;
        }
        const uint16_t pressed = (uint16_t)(b & ~g_padPrev[port]);
        g_padPrev[port] = b;
        now |= b;
        if (pressed & PAD_CIRCLE) {
            g_shown = false;
            return;
        }
        if (pressed & PAD_UP) move(-1);
        if (pressed & PAD_DOWN) move(1);
        if (pressed & PAD_LEFT) change(-1, 0);
        if (pressed & PAD_RIGHT) change(1, 0);
        if (pressed & PAD_CROSS) change(0, 0);
        const uint16_t dir = pressed & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT);
        if (dir != 0) {
            g_repeatButton = dir;
            g_padDownAt = GetTickCount64();
            g_repeatAt = g_padDownAt + 400;
        }
    }
    g_padSync = false;
    /* a held direction repeats after 0.4 s, every kRepeatMs */
    if (g_repeatButton != 0 && (now & g_repeatButton) == g_repeatButton) {
        const ULONGLONG t = GetTickCount64();
        if (t >= g_repeatAt) {
            const int held = (int)(t - g_padDownAt);
            if (g_repeatButton & PAD_UP) move(-1);
            if (g_repeatButton & PAD_DOWN) move(1);
            if (g_repeatButton & PAD_LEFT) change(-1, held);
            if (g_repeatButton & PAD_RIGHT) change(1, held);
            g_repeatAt = t + kRepeatMs;
        }
    } else {
        g_repeatButton = 0;
    }
}

/* ---- drawing --------------------------------------------------------- */

const uint32_t kPanel = rgba(10, 12, 16, 215);
const uint32_t kFrame = rgba(120, 190, 255, 160);
const uint32_t kTitle = rgba(255, 255, 255, 255);
const uint32_t kLabel = rgba(170, 170, 170, 255);
const uint32_t kLabelSel = rgba(255, 255, 255, 255);
const uint32_t kValue = rgba(120, 190, 255, 255);
const uint32_t kHighlight = rgba(255, 255, 255, 30);
const uint32_t kHelp = rgba(200, 200, 150, 255);
const uint32_t kHint = rgba(130, 130, 130, 255);
const uint32_t kNotice = rgba(255, 200, 80, 255);

/* The help texts' most lines and longest line (characters). */
void help_extent(int *rows, int *cols)
{
    *rows = 1;
    *cols = 0;
    for (const Item &it : kItems) {
        int n = 1, len = 0;
        for (const char *p = it.help; *p != '\0'; p++) {
            if (*p == '\n') {
                n++;
                len = 0;
                continue;
            }
            *cols = std::max(*cols, ++len);
        }
        *rows = std::max(*rows, n);
    }
}

/* The list's rows (more visible items scroll); the first one shown. */
constexpr int kListRows = 18;
int g_scroll;

/* (%s: the settings menu's hotkey, F2) */
const char kHint1[] = "UP/DOWN CHOOSE   LEFT/RIGHT CHANGE   %s%sESC CLOSE";
const char kHint2[] = "A GROUP: RIGHT OPENS IT, LEFT CLOSES IT";
const char kHint3[] = "PAD: D-PAD, CROSS NEXT VALUE, CIRCLE CLOSE, GUIDE %s";

void build(int bw, int bh)
{
    char key[32], hint1[96], hint3[96];
    input_map::hotkey_text(input_map::kHotSettingsMenu, key, sizeof(key));
    snprintf(hint1, sizeof(hint1), kHint1, key, key[0] != '\0' ? "/" : "");
    snprintf(hint3, sizeof(hint3), kHint3, key[0] != '\0' ? key : "CLOSE");
    int helpRows, helpCols;
    help_extent(&helpRows, &helpCols);
    const int cols = std::max({52, helpCols, (int)strlen(hint1), (int)strlen(hint3)});
    const int rows = kListRows + helpRows + 8;
    /* 1080p: 3 screen pixels a font pixel; a small window fits it */
    const int s = std::max(1, std::min({bh / 360, (bw - 16) / (cols * 6 + 20), (bh - 16) / (rows * 12 + 20)}));
    const float cw = 6.0f * s, lineH = 12.0f * s, pad = 10.0f * s;
    const float pw = cols * cw + 2.0f * pad;
    const float ph = pad + lineH * rows + pad;
    const float px = std::floor(((float)bw - pw) * 0.5f), py = std::floor(((float)bh - ph) * 0.5f);
    const float b = (float)std::max(1, s / 2);
    overlay::rect(px, py, pw, ph, kPanel);
    overlay::rect(px, py, pw, b, kFrame);
    overlay::rect(px, py + ph - b, pw, b, kFrame);
    overlay::rect(px, py, b, ph, kFrame);
    overlay::rect(px + pw - b, py, b, ph, kFrame);

    const float x0 = px + pad, x1 = px + pw - pad;
    float y = py + pad;
    overlay::text(x0, y, s, "GT3 PC SETTINGS", kTitle);
    overlay::text(x1 - overlay::text_width(key, s), y, s, key, kHint);
    y += 2.0f * lineH;

    /* the visible items, the chosen one in view; a category "+" closed, "-"
     * open, each level two characters in */
    int list[kItemCount], at;
    const int n = visible_items(list, g_sel, &at);
    g_scroll = std::max(0, std::min({g_scroll, at, n - kListRows}));
    if (at >= g_scroll + kListRows)
        g_scroll = at - kListRows + 1;
    const float listY = y;
    for (int r = 0; r < kListRows && g_scroll + r < n; r++) {
        const int i = list[g_scroll + r];
        const Item &it = kItems[i];
        const bool sel = i == g_sel;
        if (sel)
            overlay::rect(x0 - 4.0f * s, y - 3.0f * s, x1 - x0 + 8.0f * s, lineH, kHighlight);
        char label[96], hot[32] = "";
        if (it.hotkey >= 0)
            input_map::hotkey_text(it.hotkey, hot, sizeof(hot));
        if (it.count == kCategory)
            snprintf(label, sizeof(label), "%s %s", g_open[i] ? "-" : "+", it.label);
        else if (hot[0] != '\0')
            snprintf(label, sizeof(label), "%s (%s)", it.label, hot);
        else
            snprintf(label, sizeof(label), "%s", it.label);
        overlay::text(x0 + 2.0f * cw * it.depth, y, s, label, sel ? kLabelSel : it.count == kCategory ? kTitle : kLabel);
        char value[32], v[48];
        const int edges = item_value(it, value, sizeof(value));
        if (sel && it.count != kCategory)
            snprintf(v, sizeof(v), "%s %s %s", (edges & 1) ? "<" : " ", value, (edges & 2) ? ">" : " ");
        else
            snprintf(v, sizeof(v), "%s", value);
        overlay::text(x1 - overlay::text_width(v, s), y, s, v, kValue);
        y += lineH;
    }
    if (n > kListRows) {
        /* a scroll bar by the right edge */
        const float trackH = kListRows * lineH - 3.0f * s, bx = px + pw - pad * 0.5f - b;
        overlay::rect(bx, listY - 3.0f * s, b, trackH, kHighlight);
        overlay::rect(bx, listY - 3.0f * s + trackH * g_scroll / n, b, trackH * kListRows / n, kFrame);
    }
    y = listY + kListRows * lineH + lineH;
    {
        /* every line of the chosen setting's help */
        char help[512];
        snprintf(help, sizeof(help), "%s", kItems[g_sel].help);
        float hy = y;
        for (char *line = help; line != nullptr; hy += lineH) {
            char *next = strchr(line, '\n');
            if (next != nullptr)
                *next++ = '\0';
            overlay::text(x0, hy, s, line, kHelp);
            line = next;
        }
    }
    y += (float)(helpRows + 1) * lineH;
    overlay::text(x0, y, s, "CHANGES APPLY AT ONCE, SAVED IN GT3PC.INI.", kHint);
    y += 2.0f * lineH;
    overlay::text(x0, y, s, hint1, kHint);
    y += lineH;
    overlay::text(x0, y, s, kHint2, kHint);
    y += lineH;
    overlay::text(x0, y, s, hint3, kHint);
}

/* ---- the notice ------------------------------------------------------ */

std::mutex g_noticeLock;
std::atomic<bool> g_noticeOn{false};
char g_noticeText[320];
ULONGLONG g_noticeMs, g_noticeEnd;

void draw_notice(ID3D11RenderTargetView *bb)
{
    std::lock_guard<std::mutex> l(g_noticeLock);
    const ULONGLONG now = GetTickCount64();
    if (g_noticeEnd == 0)
        g_noticeEnd = now + g_noticeMs;
    if (now >= g_noticeEnd) {
        g_noticeOn = false;
        return;
    }
    char text[sizeof(g_noticeText)];
    snprintf(text, sizeof(text), "%s", g_noticeText);
    const char *lines[4];
    int n = 0, cols = 0;
    for (char *p = text; p != nullptr && n < 4;) {
        lines[n++] = p;
        char *nl = strchr(p, '\n');
        if (nl != nullptr)
            *nl++ = '\0';
        cols = std::max(cols, (int)strlen(p));
        p = nl;
    }
    if (!overlay::begin())
        return;
    int bw, bh;
    overlay::size(&bw, &bh);
    const int s = std::max(1, std::min(bh / 480, (bw - 16) / (cols * 6 + 20)));
    const float lineH = 12.0f * s, pad = 8.0f * s;
    const float pw = cols * 6.0f * s + 2.0f * pad, ph = n * lineH + 2.0f * pad - 3.0f * s;
    const float px = std::floor(((float)bw - pw) * 0.5f), py = (float)(16 * s);
    const float b = (float)std::max(1, s / 2);
    overlay::rect(px, py, pw, ph, kPanel);
    overlay::rect(px, py, pw, b, kNotice);
    overlay::rect(px, py + ph - b, pw, b, kNotice);
    overlay::rect(px, py, b, ph, kNotice);
    overlay::rect(px + pw - b, py, b, ph, kNotice);
    for (int i = 0; i < n; i++)
        overlay::text(px + pad, py + pad + i * lineH, s, lines[i], i == 0 ? kNotice : kTitle);
    overlay::end(bb);
}

} // namespace

void toggle()
{
    std::lock_guard<std::mutex> l(g_lock);
    g_padSync = true;
    g_repeatButton = 0;
    g_shown = !g_shown.load();
}

void hide()
{
    std::lock_guard<std::mutex> l(g_lock);
    g_shown = false;
}

bool shown()
{
    return g_shown.load(std::memory_order_relaxed);
}

void key(int vk, bool repeat)
{
    std::lock_guard<std::mutex> l(g_lock);
    /* a held key: its autorepeat taken at most every kRepeatMs, and how long
     * it has been held (the frame time's steps grow with it) */
    const ULONGLONG t = GetTickCount64();
    if (!repeat) {
        g_keyDownAt = t;
    } else {
        if (t - g_keyStepAt < kRepeatMs)
            return;
    }
    g_keyStepAt = t;
    const int held = repeat ? (int)(t - g_keyDownAt) : 0;
    switch (vk) {
    case VK_UP: move(-1); break;
    case VK_DOWN: move(1); break;
    case VK_LEFT: change(-1, held); break;
    case VK_RIGHT: change(1, held); break;
    case VK_RETURN:
    case VK_SPACE: change(0, 0); break;
    case VK_ESCAPE:
    case VK_BACK: g_shown = false; break;
    }
}

void present(ID3D11RenderTargetView *bb)
{
    if (bb == nullptr)
        return;
    if (!g_shown.load(std::memory_order_relaxed)) {
        if (g_noticeOn.load(std::memory_order_relaxed))
            draw_notice(bb);
        return;
    }
    std::lock_guard<std::mutex> l(g_lock);
    poll_pads();
    if (!g_shown.load() || !overlay::begin())
        return;
    int bw, bh;
    overlay::size(&bw, &bh);
    build(bw, bh);
    overlay::end(bb);
}

void notice(const char *text, double seconds)
{
    std::lock_guard<std::mutex> l(g_noticeLock);
    snprintf(g_noticeText, sizeof(g_noticeText), "%s", text);
    g_noticeMs = (ULONGLONG)(seconds * 1000.0);
    g_noticeEnd = 0;                            /* from its first frame */
    g_noticeOn = true;
}

} // namespace settings_menu
