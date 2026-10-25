/* debug_camera.h - the debug camera's switch and controls (docs/renderer.md
 * "Debug camera"; game/app/pc_debugcam.h flies it).  No Windows headers.
 *
 * In a race (a replay too) the DebugCamera hotkey (F7: input_map.h
 * [Hotkeys]) turns it on or off, "DEBUG CAMERA ENABLED" / "DISABLED" over the
 * picture for 2 s; outside a race the key does nothing, and a race's start
 * and end turn it off.  While it is on the race's HUD and mirror are not
 * drawn.  The Debug camera profile ([DebugCamera], the launcher's Controls
 * tab) flies it - from the first gamepad (with that profile's dead zones)
 * and the keyboard / mouse, the look on the mouse's movement (the cursor
 * hidden and confined to the window meanwhile: platform/window.cpp) - while
 * the players' controllers and wheels give the game nothing (at rest:
 * platform/pad_pc.cpp, wheel.cpp).  Its Lock control holds the camera where
 * it is and gives the players their input back ("DEBUG CAMERA LOCKED"), and
 * a press again flies it ("UNLOCKED"). */
#pragma once

namespace debug_camera {

/* The game: a race begins (on) / ends; the camera off either way. */
void race(bool on);
/* The window thread: the hotkey went down. */
void key();
/* On (any thread). */
bool active();
/* On and not locked: the players' input held at rest (any thread). */
bool holds_input();
/* That, and no menu shown: the mouse looks (the window's cursor hidden and
 * confined; any thread). */
bool wants_mouse();

/* The controls, each -1 .. 1 (a key or button 0 / 1); all 0 while locked. */
struct Controls {
    float forward, right, up;               /* move: forward / back, right / left, up / down */
    float lookRight, lookUp;                /* turn: a rate */
    int lookRightCounts, lookUpCounts;      /* turn by the mouse's movement since the last read (counts) */
    float faster;                           /* held: + faster, - slower */
    float fov;                              /* held: + wider, - narrower */
    int fasterSteps, fovSteps;              /* mouse wheel notches since the last read (a binding of one) */
};
/* The game thread, once a frame: the profile's controls now (all 0 while a
 * menu has the input); a press of Lock locks or unlocks. */
void read(Controls *out);
/* A short notice over the picture (the speed or the field of view as it
 * changes: settings_menu::notice). */
void notice(const char *text, double seconds);

} // namespace debug_camera
