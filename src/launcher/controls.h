/* controls.h - the launcher window's Controls tab: the control mappings of
 * gt3pc.ini ([Controller1], [Controller2], [Wheel1], [Wheel2];
 * platform/input_map.h), with SDL3 for taking gamepad and wheel inputs.
 *
 * At the top the profile (Controller 1 / 2, Wheel 1 / 2) and three sub-tabs:
 *   Mapping      a row per virtual control with two slot buttons, the
 *                gamepad's (a wheel's: the joystick's) and the keyboard /
 *                mouse's.  A click (Enter, Space) waits for the input to
 *                take (5 s; Esc cancels): a gamepad button or an axis moved
 *                past half, a joystick button, hat or axis (an axis resting
 *                at an end - a pedal - as the whole axis), a key, a mouse
 *                button or wheel notch (the Debug camera profile's look:
 *                the mouse's movement too, 48 pixels one way).  The keys of the game's hotkeys
 *                without modifiers (F2, F3, F5, F6, F8, F9 by default) and the
 *                Guide button are not taken.  A right-click or Delete clears a
 *                slot; Reset to defaults the profile's.
 *   Calibration  the profile's dead zones, a wheel's device, and a live view
 *                of its analog controls after the dead zones.
 *   Hotkeys      the game window's own keys ([Hotkeys], input_map.h; not a
 *                profile's: the profile hidden), a key button each.  A click
 *                waits for a key that is not a modifier (5 s; a click cancels)
 *                and takes it with the modifiers held; another hotkey's
 *                combination, Alt+F4, and (without modifiers) a key a
 *                control's keyboard slot has are refused.  A right-click or
 *                Delete: none; Reset to defaults all of them.
 * Every change is written to gt3pc.ini at once.
 *
 * ui.cpp makes the tab's page (a panel: panel.h) and passes the tab's
 * messages on: its controls' WM_COMMAND (ids kFirstId .. kLastId), its
 * sliders' WM_HSCROLL, WM_DRAWITEM and WM_TIMER, and every message of the window's thread first
 * to filter (the input being captured: keys, mouse buttons and the wheel go
 * to the binding, not to the window). */
#pragma once

#include <windows.h>

#include <string>

namespace controls {

constexpr int kFirstId = 1400, kLastId = 1699;     /* the tab's controls */

/* Makes the tab's controls in `page' (a panel: panel.h) of the window;
 * `ini': gt3pc.ini. */
void create(HWND page, HINSTANCE inst, const std::wstring &ini);
/* The fonts: the controls', the sub-tabs'. */
void set_font(HFONT font, HFONT tabFont);
/* Lays the tab out in its page (already in its place) at `dpi'. */
void layout(UINT dpi);

/* The tab shown or hidden.  Shown: the profile read from gt3pc.ini, SDL
 * started on a thread of its own (the devices read); hidden: a capture
 * cancelled, SDL stopped (no device held: hide it before the game starts). */
void show(bool shown);

void command(int id, int code, HWND from);
/* A slider of the tab moved (WM_HSCROLL: its id, the notification code). */
void slider(int id, int code);
bool draw_item(const DRAWITEMSTRUCT *d);    /* true: one of the tab's */
bool timer(UINT_PTR id);                    /* true: the tab's */
/* A message of the window's thread, before anything else sees it: true when
 * the tab took it (an input captured, a slot button's key or right-click). */
bool filter(const MSG &m);
/* A label of the tab (its text drawn over the window's art). */
bool is_text(HWND c);

} // namespace controls
