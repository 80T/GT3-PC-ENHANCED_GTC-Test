/* hud_colors.h - the race HUD's colours ([Hud] in gt3pc.ini, docs/launcher.md
 * "Game > HUD"): the table settings, the F2 menu and the launcher share.  No
 * Windows headers.  game/app/pc_hud_color.h puts them on the HUD. */
#pragma once

#include <stdint.h>

/* PC (2026-11-16): the labels marked for the interface's languages
 * (platform/ui_text.h's N_, the same definition: this header stays light
 * for the game's HUD code) */
#ifndef N_
#define N_(s) s
#endif

enum HudColor {
    kHudText,
    kHudMessages,
    kHudWarnings,
    kHudEvents,
    kHudGapAhead,
    kHudGapBehind,
    kHudOdometer,
    kHudMeterDial,
    kHudMeterMarks,
    kHudMeterRed,
    kHudNeedle,
    kHudRevBar,
    kHudShiftLamp,
    kHudVscLamp,
    kHudTcsLamp,
    kHudBrakeLamps,
    kHudThrottleBar,
    kHudBrakeBar,
    kHudMapShown,
    kHudMapOthers,
    kHudPanels,
    kHudColorCount
};

/* A colour: its gt3pc.ini key (narrow and wide), the launcher's label and the
 * game's own, 0xRRGGBB.  The HUD's code keeps the game's values (with their
 * alpha) where it draws; these stand for them in the settings' views. */
struct HudColorInfo {
    const char *key;
    const wchar_t *wideKey, *label;
    uint32_t rgb;
};

constexpr HudColorInfo kHudColors[kHudColorCount] = {
    {"ColorText", L"ColorText", N_(L"Text color"), 0xD08018},
    {"ColorMessages", L"ColorMessages", N_(L"Messages color"), 0xC0C0C0},
    {"ColorWarnings", L"ColorWarnings", N_(L"Warnings color"), 0xF00000},
    {"ColorEvents", L"ColorEvents", N_(L"Event board color"), 0x49CC2B},
    {"ColorGapAhead", L"ColorGapAhead", N_(L"Gap ahead color"), 0x8080FF},
    {"ColorGapBehind", L"ColorGapBehind", N_(L"Gap behind color"), 0xF00000},
    {"ColorOdometer", L"ColorOdometer", N_(L"Odometer color"), 0x62C0C0},
    {"ColorMeterDial", L"ColorMeterDial", N_(L"Meter dial color"), 0x88A0A0},
    {"ColorMeterMarks", L"ColorMeterMarks", N_(L"Meter marks color"), 0x688080},
    {"ColorMeterRed", L"ColorMeterRed", N_(L"Red zone color"), 0xF00000},
    {"ColorNeedle", L"ColorNeedle", N_(L"Needle color"), 0xC00000},
    {"ColorRevBar", L"ColorRevBar", N_(L"Rev bar color"), 0xFFFFFF},
    {"ColorShiftLamp", L"ColorShiftLamp", N_(L"Shift lamp color"), 0xFF0000},
    {"ColorVscLamp", L"ColorVscLamp", N_(L"VSC lamp color"), 0x49CC2B},
    {"ColorTcsLamp", L"ColorTcsLamp", N_(L"TCS lamp color"), 0xFFB814},
    {"ColorBrakeLamps", L"ColorBrakeLamps", N_(L"Brake lamps color"), 0xF00000},
    {"ColorThrottleBar", L"ColorThrottleBar", N_(L"Throttle bar color"), 0x49CC2B},
    {"ColorBrakeBar", L"ColorBrakeBar", N_(L"Brake bar color"), 0xF00000},
    {"ColorMapShown", L"ColorMapShown", N_(L"Map car color"), 0x90FF00},
    {"ColorMapOthers", L"ColorMapOthers", N_(L"Map others color"), 0xFFF800},
    {"ColorPanels", L"ColorPanels", N_(L"Back panels color"), 0x000000},
};
