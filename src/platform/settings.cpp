/* settings.cpp - the settings of GT3 PC (settings.h). */
#include "platform/settings.h"
#include "platform/input_map.h"
#include "platform/log.h"
#include "platform/paths.h"

#include <windows.h>

#include <atomic>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

namespace settings {
namespace {

const char kSection[] = "Display";
const char kGraphics[] = "Graphics";
Display g_display;
std::atomic<int> g_aa{(int)AntiAliasing::off};
std::atomic<int> g_renderScale{100};
std::atomic<bool> g_lod{true};
std::atomic<bool> g_menuStretch{false};
std::atomic<bool> g_rearView[kRearViewCameras] = {{true}, {false}};     /* bumper, chase */
const char *const kRearViewKeys[kRearViewCameras] = {"RearViewBumper", "RearViewChase"};
std::atomic<int> g_renderDistance{100};
std::atomic<double> g_frameCap{0.0};
std::atomic<int> g_vsync{0};
std::atomic<int> g_fov{100};
std::atomic<int> g_replayFov{100};
std::atomic<int> g_splitScreen{(int)SplitScreen::horizontal};
const char *const kSplitNames[kSplitScreenCount] = {"horizontal", "vertical", "monitors"};
std::atomic<bool> g_splitHudAnalog{false};
std::atomic<bool> g_singleHudDigital{false};
std::atomic<int> g_hudSafeZone{100};

int clamp_safe_zone(int p)
{
    return p < kHudSafeZoneMin ? kHudSafeZoneMin : p > kHudSafeZoneMax ? kHudSafeZoneMax : p;
}

int clamp_fov(int p)
{
    return p < kFovMin ? kFovMin : p > kFovMax ? kFovMax : p;
}

int clamp_render_scale(int p)
{
    return p < kRenderScaleMin ? kRenderScaleMin : p > kRenderScaleMax ? kRenderScaleMax : p;
}

double clamp_cap(double ms)
{
    if (!(ms > 0.0))
        return 0.0;
    if (ms < kFrameCapMin)
        ms = kFrameCapMin;
    if (ms > kFrameCapMax)
        ms = kFrameCapMax;
    return (double)(long long)(ms * 1000.0 + 0.5) / 1000.0;
}
const char *const kAaNames[kAntiAliasingCount] = {"off", "fxaa", "msaa2", "msaa4", "msaa8"};

const char kAudio[] = "Audio";
const char *const kAudioNames[kAudioOutputCount] = {"auto", "ks", "wasapi"};
std::atomic<int> g_audioOut{(int)AudioOutput::automatic};
std::atomic<double> g_audioBuffer{2.0};
std::atomic<int> g_audioGen{0};

double clamp_buffer(double ms)
{
    if (!(ms >= kAudioBufferMin))
        ms = kAudioBufferMin;
    if (ms > kAudioBufferMax)
        ms = kAudioBufferMax;
    return (double)(long long)(ms * 10.0 + 0.5) / 10.0;
}
const char kGame[] = "Game";
constexpr int kLanguages = 7;
const char *const kLanguageNames[kLanguages] = {"japanese", "english_us", "english_uk", "french",
                                                "german",   "italian",    "spanish"};
std::atomic<int> g_language{kLanguageAuto};

CarKind g_carKinds[32];
int g_carKindCount;
bool g_debugConsole;
bool g_debugFileLog;
bool g_debugSyncLog;
bool g_debugFillGarage, g_debugFillMoney, g_debugUnlockArcade, g_debugUnlockLicenses;
std::atomic<int> g_startingCredits{0};          /* 0: auto */
/* the units: [Game] SpeedUnit / PowerUnit / TorqueUnit (kUnitAuto or the value) */
const char *const kSpeedUnitNames[kSpeedUnits] = {"kmh", "mph"};
const char *const kPowerUnitNames[kPowerUnits] = {"ps", "hp", "bhp", "cv", "kw"};
const char *const kTorqueUnitNames[kTorqueUnits] = {"kgfm", "ftlb", "nm"};
std::atomic<int> g_speedUnit{kUnitAuto}, g_powerUnit{kUnitAuto}, g_torqueUnit{kUnitAuto};
std::atomic<int> g_unitsGen{0};

std::string ini_path();

int read_unit(const char *key, const char *const *names, int count)
{
    char v[16] = "";
    GetPrivateProfileStringA(kGame, key, "auto", v, sizeof(v), ini_path().c_str());
    for (int i = 0; i < count; i++)
        if (_stricmp(v, names[i]) == 0)
            return i;
    return kUnitAuto;
}

void write_unit(std::atomic<int> &cur, int u, const char *key, const char *const *names, int count)
{
    if (u < kUnitAuto || u >= count || u == cur.load())
        return;
    cur = u;
    g_unitsGen++;
    WritePrivateProfileStringA(kGame, key, u == kUnitAuto ? "auto" : names[u], ini_path().c_str());
    pclog::write("settings: %s %s", key, u == kUnitAuto ? "auto" : names[u]);
}

std::string ini_path()
{
    return paths::game_dir() + "gt3pc.ini";
}

/* s without the spaces and tabs at both ends, into dst (size n) */
void copy_trimmed(char *dst, size_t n, const char *s, size_t len)
{
    while (len > 0 && (*s == ' ' || *s == '\t')) {
        s++;
        len--;
    }
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t'))
        len--;
    if (len >= n)
        len = n - 1;
    memcpy(dst, s, len);
    dst[len] = '\0';
}

/* [CarKinds]: "code=kind" entries (comments and entries without '='
 * skipped) */
void load_car_kinds()
{
    char buf[4096];
    DWORD got = GetPrivateProfileSectionA("CarKinds", buf, sizeof(buf), ini_path().c_str());
    g_carKindCount = 0;
    for (const char *p = buf; p < buf + got && *p; p += strlen(p) + 1) {
        const char *eq = strchr(p, '=');
        if (*p == ';' || *p == '#' || eq == nullptr)
            continue;
        if (g_carKindCount == (int)(sizeof(g_carKinds) / sizeof(g_carKinds[0])))
            break;
        CarKind &ck = g_carKinds[g_carKindCount];
        copy_trimmed(ck.code, sizeof(ck.code), p, (size_t)(eq - p));
        copy_trimmed(ck.kind, sizeof(ck.kind), eq + 1, strlen(eq + 1));
        if (ck.code[0] == '\0' || ck.kind[0] == '\0')
            continue;
        pclog::write("settings: car kind %s = %s", ck.code, ck.kind);
        g_carKindCount++;
    }
}

int read_int(const char *key, int def)
{
    return (int)GetPrivateProfileIntA(kSection, key, def, ini_path().c_str());
}

void write_int(const char *key, int v)
{
    char s[16];
    snprintf(s, sizeof(s), "%d", v);
    WritePrivateProfileStringA(kSection, key, s, ini_path().c_str());
}

} // namespace

const char *mode_name(DisplayMode m)
{
    switch (m) {
    case DisplayMode::borderless: return "borderless";
    case DisplayMode::fullscreen: return "fullscreen";
    default: return "windowed";
    }
}

void load()
{
    Display d;
    char mode[32] = "";
    GetPrivateProfileStringA(kSection, "Mode", "windowed", mode, sizeof(mode), ini_path().c_str());
    if (_stricmp(mode, "borderless") == 0)
        d.mode = DisplayMode::borderless;
    else if (_stricmp(mode, "fullscreen") == 0)
        d.mode = DisplayMode::fullscreen;
    d.width = read_int("Width", 0);
    d.height = read_int("Height", 0);
    if (d.width < 640 || d.height < 480)
        d.width = d.height = 0;
    d.refresh = read_int("Refresh", 0);
    d.windowW = read_int("WindowWidth", 1280);
    d.windowH = read_int("WindowHeight", 960);
    if (d.windowW < 320 || d.windowH < 240) {
        d.windowW = 1280;
        d.windowH = 960;
    }
    d.windowX = read_int("WindowX", INT_MIN);
    d.windowY = read_int("WindowY", INT_MIN);
    d.windowPlaced = d.windowX != INT_MIN && d.windowY != INT_MIN;
    {
        /* "W:H" or a number */
        char hud[32] = "";
        GetPrivateProfileStringA(kSection, "HudMaxAspect", "", hud, sizeof(hud), ini_path().c_str());
        float w = 0.0f, h = 0.0f;
        if (sscanf(hud, "%f:%f", &w, &h) == 2 && w > 0.0f && h > 0.0f)
            d.hudMaxAspect = w / h;
        else if (sscanf(hud, "%f", &w) == 1 && w > 0.0f)
            d.hudMaxAspect = w;
        if (d.hudMaxAspect < 4.0f / 3.0f)
            d.hudMaxAspect = 4.0f / 3.0f;
        if (d.hudMaxAspect > 32.0f / 9.0f)
            d.hudMaxAspect = 32.0f / 9.0f;
    }
    GetPrivateProfileStringA(kSection, "SpanMonitors", "", d.spanMonitors, sizeof(d.spanMonitors),
                             ini_path().c_str());
    GetPrivateProfileStringA(kSection, "SplitMonitor", "auto", d.splitMonitor, sizeof(d.splitMonitor),
                             ini_path().c_str());
    if (_stricmp(d.splitMonitor, "auto") == 0)
        d.splitMonitor[0] = 0;
    {
        char menu[16] = "";
        GetPrivateProfileStringA(kSection, "MenuAspect", "original", menu, sizeof(menu), ini_path().c_str());
        g_menuStretch = _stricmp(menu, "stretch") == 0;
        pclog::write("settings: menus %s", g_menuStretch.load() ? "stretched over the window" : "at 4:3");
    }
    {
        char lang[32] = "";
        GetPrivateProfileStringA(kGame, "Language", "auto", lang, sizeof(lang), ini_path().c_str());
        int v = kLanguageAuto;
        for (int i = 0; i < kLanguages; i++)
            if (_stricmp(lang, kLanguageNames[i]) == 0)
                v = i;
        g_language = v;
        pclog::write("settings: language %s", language_name(v));
    }
    g_display = d;
    pclog::write("settings: display %s, fullscreen %dx%d %d Hz (0: the desktop's / the nearest to 60), window"
                 " %dx%d, race HUD up to %.3f:1", mode_name(d.mode), d.width, d.height, d.refresh, d.windowW,
                 d.windowH, d.hudMaxAspect);
    if (d.spanMonitors[0] != 0)
        pclog::write("settings: span monitors %s (borderless / fullscreen)", d.spanMonitors);
    if (d.splitMonitor[0] != 0)
        pclog::write("settings: split screen second monitor %s", d.splitMonitor);
    {
        char aa[32] = "";
        GetPrivateProfileStringA(kGraphics, "AntiAliasing", "off", aa, sizeof(aa), ini_path().c_str());
        int v = (int)AntiAliasing::off;
        for (int i = 0; i < kAntiAliasingCount; i++)
            if (_stricmp(aa, kAaNames[i]) == 0)
                v = i;
        g_aa = v;
        pclog::write("settings: anti-aliasing %s", kAaNames[v]);
        g_renderScale = clamp_render_scale((int)GetPrivateProfileIntA(kGraphics, "RenderScale", 100,
                                                                      ini_path().c_str()));
        if (g_renderScale.load() != 100)
            pclog::write("settings: resolution scale %d%%", g_renderScale.load());
    }
    {
        char lod[16] = "";
        GetPrivateProfileStringA(kGraphics, "Lod", "on", lod, sizeof(lod), ini_path().c_str());
        g_lod = !(_stricmp(lod, "off") == 0 || strcmp(lod, "0") == 0);
        int rd = (int)GetPrivateProfileIntA(kGraphics, "RenderDistance", 100, ini_path().c_str());
        g_renderDistance = rd < kRenderDistanceMin ? kRenderDistanceMin : rd > kRenderDistanceMax ? kRenderDistanceMax : rd;
        pclog::write("settings: levels of detail %s, render distance %d%%", g_lod.load() ? "on" : "off",
                     g_renderDistance.load());
        for (int c = 0; c < kRearViewCameras; c++) {
            char on[16] = "";
            GetPrivateProfileStringA(kGraphics, kRearViewKeys[c], c == 0 ? "on" : "off", on, sizeof(on),
                                     ini_path().c_str());
            g_rearView[c] = _stricmp(on, "on") == 0 || strcmp(on, "1") == 0;
        }
        pclog::write("settings: rear view: bumper camera %s, chase camera %s", g_rearView[0].load() ? "on" : "off",
                     g_rearView[1].load() ? "on" : "off");
    }
    {
        char cap[32] = "", vs[16] = "";
        GetPrivateProfileStringA(kGraphics, "FrameCap", "0", cap, sizeof(cap), ini_path().c_str());
        g_frameCap = clamp_cap(atof(cap));
        GetPrivateProfileStringA(kGraphics, "VSync", "off", vs, sizeof(vs), ini_path().c_str());
        const int n = atoi(vs);
        g_vsync = n >= 1 && n <= 3 ? n : 0;
        if (g_frameCap.load() > 0.0)
            pclog::write("settings: frame cap %.3f ms (%.2f frames a second), vsync %s", g_frameCap.load(),
                         1000.0 / g_frameCap.load(), g_vsync.load() ? vs : "off");
        else
            pclog::write("settings: no frame cap, vsync %s", g_vsync.load() ? vs : "off");
        g_fov = clamp_fov((int)GetPrivateProfileIntA(kGraphics, "Fov", 100, ini_path().c_str()));
        g_replayFov = clamp_fov((int)GetPrivateProfileIntA(kGraphics, "ReplayFov", 100, ini_path().c_str()));
        if (g_fov.load() != 100 || g_replayFov.load() != 100)
            pclog::write("settings: field of view %d%%, replays %d%%", g_fov.load(), g_replayFov.load());
        char split[16] = "";
        GetPrivateProfileStringA(kGraphics, "SplitScreen", "horizontal", split, sizeof(split), ini_path().c_str());
        int sv = (int)SplitScreen::horizontal;
        for (int i = 0; i < kSplitScreenCount; i++)
            if (_stricmp(split, kSplitNames[i]) == 0)
                sv = i;
        g_splitScreen = sv;
        if (sv != (int)SplitScreen::horizontal)
            pclog::write("settings: split screen %s", kSplitNames[sv]);
        char hud[16] = "";
        GetPrivateProfileStringA(kGraphics, "SplitHud", "digital", hud, sizeof(hud), ini_path().c_str());
        g_splitHudAnalog = _stricmp(hud, "analog") == 0;
        GetPrivateProfileStringA(kGraphics, "SingleHud", "analog", hud, sizeof(hud), ini_path().c_str());
        g_singleHudDigital = _stricmp(hud, "digital") == 0;
        if (g_splitHudAnalog.load() || g_singleHudDigital.load())
            pclog::write("settings: split screen HUD %s, single player HUD %s",
                         g_splitHudAnalog.load() ? "analog" : "digital", g_singleHudDigital.load() ? "digital" : "analog");
        g_hudSafeZone = clamp_safe_zone((int)GetPrivateProfileIntA(kGraphics, "HudSafeZone", 100, ini_path().c_str()));
        if (g_hudSafeZone.load() != 100)
            pclog::write("settings: HUD safe zone %d%%", g_hudSafeZone.load());
    }
    {
        char out[16] = "", buf[32] = "";
        GetPrivateProfileStringA(kAudio, "Output", "auto", out, sizeof(out), ini_path().c_str());
        int v = (int)AudioOutput::automatic;
        for (int i = 0; i < kAudioOutputCount; i++)
            if (_stricmp(out, kAudioNames[i]) == 0)
                v = i;
        g_audioOut = v;
        GetPrivateProfileStringA(kAudio, "Buffer", "2", buf, sizeof(buf), ini_path().c_str());
        g_audioBuffer = clamp_buffer(atof(buf));
        pclog::write("settings: audio output %s, buffer %.1f ms", kAudioNames[v], g_audioBuffer.load());
    }
    load_car_kinds();
    input_map::init(ini_path().c_str());
    {
        /* the hotkeys, as read ([Hotkeys]) */
        char line[512];
        size_t at = 0;
        for (int h = 0; h < input_map::kHotkeyCount && at < sizeof(line); h++) {
            char k[32];
            input_map::format_combo(input_map::hotkey(h), k, sizeof(k));
            at += (size_t)snprintf(line + at, sizeof(line) - at, "%s%s=%s", h ? ", " : "", input_map::hotkey_key(h), k);
        }
        pclog::write("settings: hotkeys %s", line);
    }
    {
        char con[16] = "";
        GetPrivateProfileStringA("Debug", "Console", "off", con, sizeof(con), ini_path().c_str());
        g_debugConsole = _stricmp(con, "on") == 0 || strcmp(con, "1") == 0;
        if (g_debugConsole)
            pclog::write("settings: debug console on");
        GetPrivateProfileStringA("Debug", "FileLog", "off", con, sizeof(con), ini_path().c_str());
        g_debugFileLog = _stricmp(con, "on") == 0 || strcmp(con, "1") == 0;
        if (g_debugFileLog)
            pclog::write("settings: the files read and written logged (file: ...)");
        GetPrivateProfileStringA("Debug", "SyncLog", "off", con, sizeof(con), ini_path().c_str());
        g_debugSyncLog = _stricmp(con, "on") == 0 || strcmp(con, "1") == 0;
        struct {
            const char *key;
            bool *flag;
        } const kStart[] = {{"FillGarage", &g_debugFillGarage},
                            {"FillMoney", &g_debugFillMoney},
                            {"UnlockArcade", &g_debugUnlockArcade},
                            {"UnlockLicenses", &g_debugUnlockLicenses}};
        for (const auto &k : kStart) {
            GetPrivateProfileStringA("Debug", k.key, "off", con, sizeof(con), ini_path().c_str());
            *k.flag = _stricmp(con, "on") == 0 || strcmp(con, "1") == 0;
            if (*k.flag)
                pclog::write("settings: debug %s on", k.key);
        }
    }
    {
        char credits[16] = "";
        GetPrivateProfileStringA(kGame, "StartingCredits", "auto", credits, sizeof(credits), ini_path().c_str());
        const int c = atoi(credits);
        g_startingCredits = c == 15000 || c == 18000 ? c : 0;
        if (g_startingCredits.load() != 0)
            pclog::write("settings: starting credits %d", g_startingCredits.load());
    }
    g_speedUnit = read_unit("SpeedUnit", kSpeedUnitNames, kSpeedUnits);
    g_powerUnit = read_unit("PowerUnit", kPowerUnitNames, kPowerUnits);
    g_torqueUnit = read_unit("TorqueUnit", kTorqueUnitNames, kTorqueUnits);
    pclog::write("settings: units speed %s, power %s, torque %s (auto: the language's)",
                 g_speedUnit.load() < 0 ? "auto" : kSpeedUnitNames[g_speedUnit.load()],
                 g_powerUnit.load() < 0 ? "auto" : kPowerUnitNames[g_powerUnit.load()],
                 g_torqueUnit.load() < 0 ? "auto" : kTorqueUnitNames[g_torqueUnit.load()]);
}

int speed_unit() { return g_speedUnit.load(std::memory_order_relaxed); }
int power_unit() { return g_powerUnit.load(std::memory_order_relaxed); }
int torque_unit() { return g_torqueUnit.load(std::memory_order_relaxed); }
void set_speed_unit(int u) { write_unit(g_speedUnit, u, "SpeedUnit", kSpeedUnitNames, kSpeedUnits); }
void set_power_unit(int u) { write_unit(g_powerUnit, u, "PowerUnit", kPowerUnitNames, kPowerUnits); }
void set_torque_unit(int u) { write_unit(g_torqueUnit, u, "TorqueUnit", kTorqueUnitNames, kTorqueUnits); }
int units_generation() { return g_unitsGen.load(std::memory_order_acquire); }

int starting_credits()
{
    return g_startingCredits.load(std::memory_order_relaxed);
}

void set_starting_credits(int credits)
{
    if (credits != 0 && credits != 15000 && credits != 18000)
        return;
    if (credits == g_startingCredits.load())
        return;
    g_startingCredits = credits;
    char s[16];
    snprintf(s, sizeof(s), "%d", credits);
    WritePrivateProfileStringA(kGame, "StartingCredits", credits != 0 ? s : "auto", ini_path().c_str());
    pclog::write("settings: starting credits %s (from the next new game)", credits != 0 ? s : "auto");
}

bool debug_fill_garage() { return g_debugFillGarage; }
bool debug_fill_money() { return g_debugFillMoney; }
bool debug_unlock_arcade() { return g_debugUnlockArcade; }
bool debug_unlock_licenses() { return g_debugUnlockLicenses; }

bool debug_console()
{
    return g_debugConsole;
}

bool debug_file_log()
{
    return g_debugFileLog;
}

bool debug_sync_log()
{
    return g_debugSyncLog;
}

double frame_cap_ms()
{
    return g_frameCap.load(std::memory_order_relaxed);
}

void set_frame_cap_ms(double ms)
{
    ms = clamp_cap(ms);
    if (ms == g_frameCap.load())
        return;
    g_frameCap = ms;
    char s[32];
    snprintf(s, sizeof(s), "%.3f", ms);
    WritePrivateProfileStringA(kGraphics, "FrameCap", ms > 0.0 ? s : "0", ini_path().c_str());
    if (ms > 0.0)
        pclog::write("settings: frame cap %.3f ms (%.2f frames a second)", ms, 1000.0 / ms);
    else
        pclog::write("settings: no frame cap");
}

int vsync()
{
    return g_vsync.load(std::memory_order_relaxed);
}

int fov_percent()
{
    return g_fov.load(std::memory_order_relaxed);
}

void set_fov_percent(int percent)
{
    percent = clamp_fov(percent);
    if (percent == g_fov.load())
        return;
    g_fov = percent;
    char s[16];
    snprintf(s, sizeof(s), "%d", percent);
    WritePrivateProfileStringA(kGraphics, "Fov", s, ini_path().c_str());
    pclog::write("settings: field of view %d%%", percent);
}

int replay_fov_percent()
{
    return g_replayFov.load(std::memory_order_relaxed);
}

void set_replay_fov_percent(int percent)
{
    percent = clamp_fov(percent);
    if (percent == g_replayFov.load())
        return;
    g_replayFov = percent;
    char s[16];
    snprintf(s, sizeof(s), "%d", percent);
    WritePrivateProfileStringA(kGraphics, "ReplayFov", s, ini_path().c_str());
    pclog::write("settings: replay field of view %d%%", percent);
}

void set_vsync(int frames)
{
    if (frames < 0 || frames > 3)
        frames = 0;
    if (frames == g_vsync.load())
        return;
    g_vsync = frames;
    char s[8];
    snprintf(s, sizeof(s), "%d", frames);
    WritePrivateProfileStringA(kGraphics, "VSync", frames ? s : "off", ini_path().c_str());
    pclog::write(frames ? "settings: vsync, %d frame(s) queued at most" : "settings: vsync off", frames);
}

AudioOutput audio_output()
{
    return (AudioOutput)g_audioOut.load(std::memory_order_relaxed);
}

void set_audio_output(AudioOutput out)
{
    const int v = (int)out;
    if (v < 0 || v >= kAudioOutputCount || v == g_audioOut.load())
        return;
    g_audioOut = v;
    g_audioGen++;
    WritePrivateProfileStringA(kAudio, "Output", kAudioNames[v], ini_path().c_str());
    pclog::write("settings: audio output %s", kAudioNames[v]);
}

const char *audio_output_name(AudioOutput out)
{
    const int v = (int)out;
    return v >= 0 && v < kAudioOutputCount ? kAudioNames[v] : "auto";
}

double audio_buffer_ms()
{
    return g_audioBuffer.load(std::memory_order_relaxed);
}

void set_audio_buffer_ms(double ms)
{
    ms = clamp_buffer(ms);
    if (ms == g_audioBuffer.load())
        return;
    g_audioBuffer = ms;
    g_audioGen++;
    char s[32];
    snprintf(s, sizeof(s), "%.1f", ms);
    WritePrivateProfileStringA(kAudio, "Buffer", s, ini_path().c_str());
    pclog::write("settings: audio buffer %.1f ms", ms);
}

int audio_generation()
{
    return g_audioGen.load(std::memory_order_relaxed);
}

SplitScreen split_screen()
{
    return (SplitScreen)g_splitScreen.load(std::memory_order_relaxed);
}

void set_split_screen(SplitScreen s)
{
    const int v = (int)s;
    if (v < 0 || v >= kSplitScreenCount || v == g_splitScreen.load())
        return;
    g_splitScreen = v;
    WritePrivateProfileStringA(kGraphics, "SplitScreen", kSplitNames[v], ini_path().c_str());
    pclog::write("settings: split screen %s", kSplitNames[v]);
}

bool split_hud_analog()
{
    return g_splitHudAnalog.load(std::memory_order_relaxed);
}

void set_split_hud_analog(bool on)
{
    if (on == g_splitHudAnalog.load())
        return;
    g_splitHudAnalog = on;
    WritePrivateProfileStringA(kGraphics, "SplitHud", on ? "analog" : "digital", ini_path().c_str());
    pclog::write("settings: split screen HUD %s", on ? "analog" : "digital");
}

bool single_hud_digital()
{
    return g_singleHudDigital.load(std::memory_order_relaxed);
}

void set_single_hud_digital(bool on)
{
    if (on == g_singleHudDigital.load())
        return;
    g_singleHudDigital = on;
    WritePrivateProfileStringA(kGraphics, "SingleHud", on ? "digital" : "analog", ini_path().c_str());
    pclog::write("settings: single player HUD %s", on ? "digital" : "analog");
}

int hud_safe_zone()
{
    return g_hudSafeZone.load(std::memory_order_relaxed);
}

void set_hud_safe_zone(int percent)
{
    percent = clamp_safe_zone(percent);
    if (percent == g_hudSafeZone.load())
        return;
    g_hudSafeZone = percent;
    char s[16];
    snprintf(s, sizeof(s), "%d", percent);
    WritePrivateProfileStringA(kGraphics, "HudSafeZone", s, ini_path().c_str());
    pclog::write("settings: HUD safe zone %d%%", percent);
}

bool lod_enabled()
{
    return g_lod.load(std::memory_order_relaxed);
}

void set_lod_enabled(bool on)
{
    if (on == g_lod.load())
        return;
    g_lod = on;
    WritePrivateProfileStringA(kGraphics, "Lod", on ? "on" : "off", ini_path().c_str());
    pclog::write("settings: levels of detail %s", on ? "on" : "off");
}

bool rear_view(RearViewCamera c)
{
    const int i = (int)c;
    return i >= 0 && i < kRearViewCameras && g_rearView[i].load(std::memory_order_relaxed);
}

void set_rear_view(RearViewCamera c, bool on)
{
    const int i = (int)c;
    if (i < 0 || i >= kRearViewCameras || on == g_rearView[i].load())
        return;
    g_rearView[i] = on;
    WritePrivateProfileStringA(kGraphics, kRearViewKeys[i], on ? "on" : "off", ini_path().c_str());
    pclog::write("settings: rear view, %s camera %s", i == 0 ? "bumper" : "chase", on ? "on" : "off");
}

int render_distance()
{
    return g_renderDistance.load(std::memory_order_relaxed);
}

void set_render_distance(int percent)
{
    if (percent < kRenderDistanceMin)
        percent = kRenderDistanceMin;
    if (percent > kRenderDistanceMax)
        percent = kRenderDistanceMax;
    if (percent == g_renderDistance.load())
        return;
    g_renderDistance = percent;
    char s[16];
    snprintf(s, sizeof(s), "%d", percent);
    WritePrivateProfileStringA(kGraphics, "RenderDistance", s, ini_path().c_str());
    pclog::write("settings: render distance %d%%", percent);
}

AntiAliasing anti_aliasing()
{
    return (AntiAliasing)g_aa.load(std::memory_order_relaxed);
}

void set_anti_aliasing(AntiAliasing aa)
{
    const int v = (int)aa;
    if (v < 0 || v >= kAntiAliasingCount || v == g_aa.load())
        return;
    g_aa = v;
    WritePrivateProfileStringA(kGraphics, "AntiAliasing", kAaNames[v], ini_path().c_str());
    pclog::write("settings: anti-aliasing %s", kAaNames[v]);
}

int render_scale()
{
    return g_renderScale.load(std::memory_order_relaxed);
}

void set_render_scale(int percent)
{
    percent = clamp_render_scale(percent);
    if (percent == g_renderScale.load())
        return;
    g_renderScale = percent;
    char s[16];
    snprintf(s, sizeof(s), "%d", percent);
    WritePrivateProfileStringA(kGraphics, "RenderScale", s, ini_path().c_str());
    pclog::write("settings: resolution scale %d%%", percent);
}

int msaa_samples(AntiAliasing aa)
{
    switch (aa) {
    case AntiAliasing::msaa2: return 2;
    case AntiAliasing::msaa4: return 4;
    case AntiAliasing::msaa8: return 8;
    default: return 1;
    }
}

const char *anti_aliasing_name(AntiAliasing aa)
{
    const int v = (int)aa;
    return v >= 0 && v < kAntiAliasingCount ? kAaNames[v] : "off";
}

void set_hud_max_aspect(int w, int h)
{
    if (w <= 0 || h <= 0)
        return;
    float aspect = (float)w / (float)h;
    if (aspect < 4.0f / 3.0f)
        aspect = 4.0f / 3.0f;
    if (aspect > 32.0f / 9.0f)
        aspect = 32.0f / 9.0f;
    g_display.hudMaxAspect = aspect;
    char s[32];
    snprintf(s, sizeof(s), "%d:%d", w, h);
    WritePrivateProfileStringA(kSection, "HudMaxAspect", s, ini_path().c_str());
    pclog::write("settings: race HUD up to %s", s);
}

int car_kind_count()
{
    return g_carKindCount;
}

const CarKind &car_kind(int i)
{
    return g_carKinds[i];
}

const Display &display()
{
    return g_display;
}

void save_window(int x, int y, int w, int h)
{
    if (w < 320 || h < 240)
        return;
    g_display.windowX = x;
    g_display.windowY = y;
    g_display.windowW = w;
    g_display.windowH = h;
    g_display.windowPlaced = true;
    write_int("WindowX", x);
    write_int("WindowY", y);
    write_int("WindowWidth", w);
    write_int("WindowHeight", h);
}

bool menu_stretch()
{
    return g_menuStretch.load(std::memory_order_relaxed);
}

void set_menu_stretch(bool on)
{
    if (on == g_menuStretch.load())
        return;
    g_menuStretch = on;
    WritePrivateProfileStringA(kSection, "MenuAspect", on ? "stretch" : "original", ini_path().c_str());
    pclog::write("settings: menus %s", on ? "stretched over the window" : "at 4:3");
}

int language()
{
    return g_language.load(std::memory_order_relaxed);
}

void set_language(int lang)
{
    if (lang < kLanguageAuto || lang >= kLanguages || lang == g_language.load())
        return;
    g_language = lang;
    WritePrivateProfileStringA(kGame, "Language", language_name(lang), ini_path().c_str());
    pclog::write("settings: language %s (from the next start)", language_name(lang));
}

const char *language_name(int lang)
{
    return lang >= 0 && lang < kLanguages ? kLanguageNames[lang] : "auto";
}

void set_display_mode(DisplayMode m)
{
    if (m != DisplayMode::windowed && m != DisplayMode::borderless && m != DisplayMode::fullscreen)
        return;
    if (m == g_display.mode)
        return;
    g_display.mode = m;
    WritePrivateProfileStringA(kSection, "Mode", mode_name(m), ini_path().c_str());
    pclog::write("settings: display mode %s", mode_name(m));
}

} // namespace settings
