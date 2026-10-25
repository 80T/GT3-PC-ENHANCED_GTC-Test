/* display.cpp - the display settings the launcher sets (display.h). */
#include "display.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>

namespace display {
namespace {

const wchar_t kSection[] = L"Display";

int read_int(const std::wstring &ini, const wchar_t *key, int def)
{
    return (int)GetPrivateProfileIntW(kSection, key, def, ini.c_str());
}

void write_int(const std::wstring &ini, const wchar_t *key, int v)
{
    WritePrivateProfileStringW(kSection, key, std::to_wstring(v).c_str(), ini.c_str());
}

std::wstring read_string(const std::wstring &ini, const wchar_t *key, const wchar_t *def)
{
    wchar_t s[256] = L"";   /* the game's size for SpanMonitors */
    GetPrivateProfileStringW(kSection, key, def, s, 256, ini.c_str());
    return s;
}

std::wstring trim(const std::wstring &s)
{
    size_t a = s.find_first_not_of(L" \t"), b = s.find_last_not_of(L" \t");
    return a == std::wstring::npos ? std::wstring() : s.substr(a, b - a + 1);
}

/* The window frame of the game's windowed style (WS_OVERLAPPEDWINDOW). */
Size frame_size()
{
    RECT r = {0, 0, 100, 100};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    return Size{r.right - r.left - 100, r.bottom - r.top - 100};
}

BOOL CALLBACK add_monitor(HMONITOR mon, HDC, LPRECT, LPARAM lp)
{
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        const RECT &r = mi.rcMonitor;
        reinterpret_cast<std::vector<Monitor> *>(lp)->push_back(
            Monitor{mi.szDevice, std::wstring(), r.left, r.top, r.right - r.left, r.bottom - r.top});
    }
    return TRUE;
}

/* The monitors' friendly names (their EDID's, "DELL U2414H"): each active
 * path's target, matched to the monitor by its source's GDI device name.
 * Built-in panels often have none; a monitor without one keeps it empty. */
void add_friendly_names(std::vector<Monitor> &v)
{
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG r;
    do {
        UINT32 np = 0, nm = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS || np == 0)
            return;
        paths.resize(np);
        modes.resize(nm ? nm : 1);
        r = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr);
        paths.resize(np);
    } while (r == ERROR_INSUFFICIENT_BUFFER);   /* the configuration changed in between */
    if (r != ERROR_SUCCESS)
        return;
    for (const DISPLAYCONFIG_PATH_INFO &p : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = p.sourceInfo.adapterId;
        source.header.id = p.sourceInfo.id;
        DISPLAYCONFIG_TARGET_DEVICE_NAME target = {};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = p.targetInfo.adapterId;
        target.header.id = p.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
            DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS || !target.monitorFriendlyDeviceName[0])
            continue;
        /* (a cloned source has a path per target: the first one names it) */
        for (Monitor &m : v)
            if (m.name.empty() && _wcsicmp(m.device.c_str(), source.viewGdiDeviceName) == 0)
                m.name = target.monitorFriendlyDeviceName;
    }
}

/* Whether b is a's neighbour on the right (display.h, Span). */
bool right_neighbour(const Monitor &a, const Monitor &b)
{
    const int overlap = std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y);
    return std::abs(b.x - (a.x + a.w)) <= 2 && 2 * overlap >= std::min(a.h, b.h);
}

} // namespace

Settings load(const std::wstring &ini)
{
    Settings s;
    wchar_t mode[32] = L"";
    GetPrivateProfileStringW(kSection, L"Mode", L"windowed", mode, 32, ini.c_str());
    if (_wcsicmp(mode, L"borderless") == 0)
        s.mode = Mode::borderless;
    else if (_wcsicmp(mode, L"fullscreen") == 0)
        s.mode = Mode::fullscreen;
    s.width = read_int(ini, L"Width", 0);
    s.height = read_int(ini, L"Height", 0);
    if (s.width < 640 || s.height < 480)
        s.width = s.height = 0;
    s.refresh = read_int(ini, L"Refresh", 0);
    s.windowW = read_int(ini, L"WindowWidth", 1280);
    s.windowH = read_int(ini, L"WindowHeight", 960);
    if (s.windowW < 320 || s.windowH < 240) {
        s.windowW = 1280;
        s.windowH = 960;
    }
    s.spanMonitors = trim(read_string(ini, L"SpanMonitors", L""));
    s.splitMonitor = trim(read_string(ini, L"SplitMonitor", L"auto"));
    if (_wcsicmp(s.splitMonitor.c_str(), L"auto") == 0)
        s.splitMonitor.clear();
    return s;
}

void save(const std::wstring &ini, const Settings &s)
{
    const wchar_t *mode = s.mode == Mode::borderless ? L"borderless" : s.mode == Mode::fullscreen ? L"fullscreen"
                                                                                                    : L"windowed";
    WritePrivateProfileStringW(kSection, L"Mode", mode, ini.c_str());
    write_int(ini, L"Width", s.width);
    write_int(ini, L"Height", s.height);
    write_int(ini, L"Refresh", s.refresh);
    write_int(ini, L"WindowWidth", s.windowW);
    write_int(ini, L"WindowHeight", s.windowH);
    WritePrivateProfileStringW(kSection, L"SpanMonitors", s.spanMonitors.c_str(), ini.c_str());
    WritePrivateProfileStringW(kSection, L"SplitMonitor", s.splitMonitor.empty() ? L"auto" : s.splitMonitor.c_str(),
                               ini.c_str());
}

std::vector<Size> fullscreen_sizes()
{
    std::vector<Size> v;
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &dm); i++) {
        const int w = (int)dm.dmPelsWidth, h = (int)dm.dmPelsHeight;
        if (dm.dmBitsPerPel != 32 || w < 640 || h < 480)
            continue;
        if (std::none_of(v.begin(), v.end(), [&](const Size &s) { return s.w == w && s.h == h; }))
            v.push_back(Size{w, h});
    }
    std::sort(v.begin(), v.end(), [](const Size &a, const Size &b) { return a.w != b.w ? a.w > b.w : a.h > b.h; });
    return v;
}

std::vector<int> refresh_rates(int w, int h)
{
    std::vector<int> v;
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &dm); i++) {
        const int r = (int)dm.dmDisplayFrequency;
        if (dm.dmBitsPerPel != 32 || (int)dm.dmPelsWidth != w || (int)dm.dmPelsHeight != h || r <= 1)
            continue;
        if (std::find(v.begin(), v.end(), r) == v.end())
            v.push_back(r);
    }
    std::sort(v.begin(), v.end(), [](int a, int b) { return a > b; });
    return v;
}

std::vector<Size> window_sizes()
{
    static const Size kSizes[] = {{640, 480},   {800, 600},   {960, 720},   {1024, 768},  {1280, 960},
                                  {1440, 1080}, {1600, 1200}, {1920, 1440}, {2048, 1536}, {2560, 1920},
                                  {2880, 2160}};
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    const Size f = frame_size();
    const int maxW = mi.rcWork.right - mi.rcWork.left - f.w, maxH = mi.rcWork.bottom - mi.rcWork.top - f.h;
    std::vector<Size> v;
    for (const Size &s : kSizes)
        if (s.w <= maxW && s.h <= maxH)
            v.push_back(s);
    if (v.empty())
        v.push_back(kSizes[0]);
    return v;
}

Size desktop_size()
{
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm))
        return Size{(int)dm.dmPelsWidth, (int)dm.dmPelsHeight};
    return Size{GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
}

std::vector<Monitor> monitors()
{
    std::vector<Monitor> v;
    EnumDisplayMonitors(nullptr, nullptr, add_monitor, (LPARAM)&v);
    std::sort(v.begin(), v.end(), [](const Monitor &a, const Monitor &b) { return a.x != b.x ? a.x < b.x : a.y < b.y; });
    add_friendly_names(v);
    return v;
}

std::vector<Span> spans(const std::vector<Monitor> &m)
{
    /* Every path along the right neighbours from every monitor, breadth
     * first: by length, then left to right.  The x places only grow along a
     * path, so it ends.  (Neighbours rather than the next one in x order:
     * a monitor stacked above another does not break the row beside it.) */
    std::vector<std::vector<int>> runs;
    for (int i = 0; i < (int)m.size(); i++)
        runs.push_back({i});
    for (size_t k = 0; k < runs.size(); k++) {
        const std::vector<int> run = runs[k];
        for (int j = 0; j < (int)m.size(); j++) {
            if (right_neighbour(m[run.back()], m[j])) {
                runs.push_back(run);
                runs.back().push_back(j);
            }
        }
    }
    std::vector<Span> v;
    for (const std::vector<int> &run : runs) {
        if (run.size() < 2)
            continue;
        Span s{run, 0, m[run[0]].h, std::wstring()};
        for (int i : run) {
            s.w += m[i].w;
            s.h = std::min(s.h, m[i].h);
            s.value += (s.value.empty() ? L"" : L",") + m[i].device;
        }
        v.push_back(s);
    }
    return v;
}

std::vector<std::wstring> device_list(const std::wstring &s)
{
    std::vector<std::wstring> v;
    for (size_t i = 0; i <= s.size();) {
        size_t j = s.find(L',', i);
        if (j == std::wstring::npos)
            j = s.size();
        std::wstring d = trim(s.substr(i, j - i));
        if (!d.empty())
            v.push_back(d);
        i = j + 1;
    }
    return v;
}

bool same_devices(const std::wstring &a, const std::wstring &b)
{
    auto key = [](const std::wstring &s) {
        std::vector<std::wstring> v = device_list(s);
        for (std::wstring &d : v)
            CharUpperBuffW(&d[0], (DWORD)d.size());
        std::sort(v.begin(), v.end());
        return v;
    };
    return key(a) == key(b);
}

} // namespace display
