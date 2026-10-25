/* display.h - the display settings the launcher sets (gt3pc.ini).
 *
 * The game reads them at start (src/platform/settings.h, the same keys):
 *
 *   [Display]
 *   Mode=windowed | borderless | fullscreen
 *   Width, Height           fullscreen resolution (0: the desktop's)
 *   Refresh                 fullscreen refresh rate in Hz (0: the nearest to 60)
 *   WindowWidth, WindowHeight   the window's client size
 *   WindowX, WindowY        the window's place (written by the game)
 *   SpanMonitors            the monitors one picture spans in borderless /
 *                           fullscreen mode: device names (\\.\DISPLAYn) with
 *                           commas, any order; empty: one monitor
 *   SplitMonitor            the 2-player split screen's second monitor: a
 *                           device name, or auto
 *
 * The fullscreen resolutions and rates are the ones Windows offers for the
 * primary monitor (EnumDisplaySettings). */
#pragma once

#include <string>
#include <vector>

namespace display {

enum class Mode { windowed, borderless, fullscreen };

struct Settings {
    Mode mode = Mode::windowed;
    int width = 0, height = 0;          /* fullscreen; 0: the desktop's */
    int refresh = 0;                    /* fullscreen; 0: the nearest to 60 Hz */
    int windowW = 1280, windowH = 960;  /* windowed client size */
    std::wstring spanMonitors;          /* SpanMonitors as written (empty: off) */
    std::wstring splitMonitor;          /* SplitMonitor's device name (empty: auto) */
};

Settings load(const std::wstring &ini);
/* Writes the keys above but WindowX / WindowY (the game's). */
void save(const std::wstring &ini, const Settings &s);

struct Size {
    int w, h;
};
/* The primary monitor's resolutions (32-bit colour, 640 x 480 or more),
 * largest first. */
std::vector<Size> fullscreen_sizes();
/* Its refresh rates at w x h, highest first. */
std::vector<int> refresh_rates(int w, int h);
/* 4:3 window client sizes that fit the primary monitor's work area,
 * smallest first. */
std::vector<Size> window_sizes();
/* The primary monitor's current resolution. */
Size desktop_size();

/* A monitor of the desktop. */
struct Monitor {
    std::wstring device;    /* its device name, \\.\DISPLAYn (MONITORINFOEX::szDevice) */
    std::wstring name;      /* its friendly name (QueryDisplayConfig); empty: unknown */
    int x, y, w, h;         /* its place on the desktop, in pixels */
};
/* The monitors, left to right (then top to bottom). */
std::vector<Monitor> monitors();

/* A run of monitors side by side that one picture can span: each one's left
 * edge on the one before's right edge (2 pixels either way), the two
 * overlapping vertically by half the smaller height at least. */
struct Span {
    std::vector<int> monitors;   /* indexes into monitors(), left to right */
    int w, h;                    /* the total width, the smallest height */
    std::wstring value;          /* its SpanMonitors (the devices left to right) */
};
/* Every run of 2 monitors or more (for A B C in a row: A+B, B+C, A+B+C),
 * the shorter runs first. */
std::vector<Span> spans(const std::vector<Monitor> &m);

/* The device names of a SpanMonitors value (split at the commas). */
std::vector<std::wstring> device_list(const std::wstring &s);
/* Whether two SpanMonitors values name the same monitors (in any order). */
bool same_devices(const std::wstring &a, const std::wstring &b);

} // namespace display
