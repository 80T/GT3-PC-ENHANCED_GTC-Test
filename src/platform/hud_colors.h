/* hud_colors.h - the race HUD's colours ([Hud] in gt3pc.ini, docs/launcher.md
 * "Game > HUD"): the table settings, the F2 menu and the launcher share.  No
 * Windows headers.  game/app/pc_hud_color.h puts them on the HUD. */
#pragma once

#include <stdint.h>

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
    {"ColorText", L"ColorText", L"Text color", 0xD08018},
    {"ColorMessages", L"ColorMessages", L"Messages color", 0xC0C0C0},
    {"ColorWarnings", L"ColorWarnings", L"Warnings color", 0xF00000},
    {"ColorEvents", L"ColorEvents", L"Event board color", 0x49CC2B},
    {"ColorGapAhead", L"ColorGapAhead", L"Gap ahead color", 0x8080FF},
    {"ColorGapBehind", L"ColorGapBehind", L"Gap behind color", 0xF00000},
    {"ColorOdometer", L"ColorOdometer", L"Odometer color", 0x62C0C0},
    {"ColorMeterDial", L"ColorMeterDial", L"Meter dial color", 0x88A0A0},
    {"ColorMeterMarks", L"ColorMeterMarks", L"Meter marks color", 0x688080},
    {"ColorMeterRed", L"ColorMeterRed", L"Red zone color", 0xF00000},
    {"ColorNeedle", L"ColorNeedle", L"Needle color", 0xC00000},
    {"ColorRevBar", L"ColorRevBar", L"Rev bar color", 0xFFFFFF},
    {"ColorShiftLamp", L"ColorShiftLamp", L"Shift lamp color", 0xFF0000},
    {"ColorVscLamp", L"ColorVscLamp", L"VSC lamp color", 0x49CC2B},
    {"ColorTcsLamp", L"ColorTcsLamp", L"TCS lamp color", 0xFFB814},
    {"ColorBrakeLamps", L"ColorBrakeLamps", L"Brake lamps color", 0xF00000},
    {"ColorThrottleBar", L"ColorThrottleBar", L"Throttle bar color", 0x49CC2B},
    {"ColorBrakeBar", L"ColorBrakeBar", L"Brake bar color", 0xF00000},
    {"ColorMapShown", L"ColorMapShown", L"Map car color", 0x90FF00},
    {"ColorMapOthers", L"ColorMapOthers", L"Map others color", 0xFFF800},
    {"ColorPanels", L"ColorPanels", L"Back panels color", 0x000000},
};
