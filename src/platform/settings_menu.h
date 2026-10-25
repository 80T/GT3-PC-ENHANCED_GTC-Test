/* settings_menu.h - the in-game settings menu of GT3 PC.
 *
 * F2 (the SettingsMenu hotkey, platform/input_map.h; or a tap of a
 * controller's Guide button, platform/gamepad.h) shows or hides it in the
 * middle of the picture (over the game, which goes on: pause
 * it first in a race).  While it is shown it takes the keyboard and the game
 * controllers - the game sees no input:
 *   Up / Down            choose a setting       (D-pad)
 *   Left / Right         change it              (D-pad; Cross: the next value)
 *   Enter / Space        the next value
 *   F2 / Esc / Backspace close                  (Circle)
 * The settings are in categories, a vertical list (closed at first): Right
 * on a category opens it, Left closes it (on a closed one: its own category
 * is chosen), Enter / Cross toggles it.  DISPLAY: the screen mode (the
 * window switched to it, the setting saved), vertical sync, the frame cap (as
 * a frame rate, the usual rates, and as a frame time, 0.01 ms steps that
 * speed up while held), the frame time monitor (F3); GRAPHICS: the
 * resolution scale, the anti-aliasing, the levels of detail, the render
 * distance, the fields of view; HUD: the race HUD's widest canvas, its safe
 * zone, a single player's meters; SPLIT SCREEN: its kind and meters; AUDIO:
 * the output, its buffer, what runs; CONTROLS: PLAYER 1 / 2, each its
 * DEVICE (the controller or the wheel while both are connected), its
 * CONTROLLER's and its WHEEL's dead zones (platform/input_map.h; the
 * mappings are the launcher's).  A change applies at once (the renderer picks it up at its
 * next frame) and is written to gt3pc.ini (platform/settings.h).
 * Drawn with platform/overlay.h.  No Windows headers. */
#pragma once

struct ID3D11RenderTargetView;

namespace settings_menu {

/* F2 (the window thread; the Guide button: the controller thread). */
void toggle();
/* Hidden (the Guide button's hold, which shows the Esc menu). */
void hide();
/* Shown: the game gets no input (platform/pad_pc.cpp, platform/window.cpp). */
bool shown();
/* A key went down while it is shown (the window thread; repeat: the key's
 * autorepeat, which speeds up the frame time's steps). */
void key(int vk, bool repeat);
/* The output (pgl_targets' present), on the back buffer just before
 * Present: the game controllers read, the menu drawn when shown, else a
 * notice while it lasts. */
void present(ID3D11RenderTargetView *backbuffer);
/* A notice at the top of the picture for `seconds' from its first frame
 * (lines split by '\n', up to 4; a newer one replaces it).  Any thread. */
void notice(const char *text, double seconds);

} // namespace settings_menu
