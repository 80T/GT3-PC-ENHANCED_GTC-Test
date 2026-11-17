/* cockpit_osd.h - PC (2026-11-08, no original): the cockpit camera's
 * calibration mode and its panel at the top right of the picture
 * (docs/renderer.md "Cockpit camera", "Calibration").
 *
 * User request: "Allow the user to manually tweak the cockpit camera
 * position and vertical angle.  User will add offsets and the configuration
 * gets saved per model code.  In the F2 menu, the user will select an option
 * to start calibration, entering a calibration mode.  User can adjust
 * position and angle, then when done, user presses a button to save the
 * calibration offsets to the config.  Make the calibration mode show a
 * popup in the top right corner similar to the song changer.  It should show
 * the controls and the button to save the calibration."  Then: "Calibration
 * mode should have all controls on the keyboard too.  Arrow keys are good.
 * Each step should only control one axis at a time, not two.  The current
 * axis should be highlighted in the popup.  The reset function should only
 * reset the current setting, not all of them."
 *
 * The F2 menu's CAMERAS > COCKPIT CAMERA > CALIBRATE COCKPIT CAMERA starts
 * it (the menu closes) and stops it.  While it is on, in a race in the
 * cockpit view, one axis at a time is adjusted - SIDE, HEIGHT, FORWARD
 * (metres) or TILT (degrees), the panel's list, the chosen one highlighted:
 * the hotkeys (input_map.h [Hotkeys]; the keyboard's arrows by default, no
 * device) CalibrationUp / CalibrationDown choose the axis and
 * CalibrationLeft / CalibrationRight step it (held: the key's repeat), as
 * the F2 menu's own keys work; the player's own D-pad and Cross / Circle /
 * Triangle do the same (user: "Controller / wheel should have dpad
 * controls too, as well as controls for save, delete and reset") - the
 * controller's or the wheel's, as the game's mappings have them, read and
 * held from the game by the pad backends (pad_state: platform/pad_pc.cpp,
 * wheel.cpp) while the mode is on; the chase camera controls (by default
 * the right stick; the wheel's as mapped) move the axis too instead of
 * looking round, on its natural direction.  CalibrationSave (F4, Cross)
 * writes the offsets for the car's model and slot (gt3pc.ini
 * [CockpitOffsets], settings.h) and ends the mode, CalibrationCancel
 * (Delete, Circle) puts the saved ones back and ends it, CalibrationReset
 * (0, Triangle) zeroes the chosen axis (kept only by a save).  These
 * hotkeys act only while the mode is on, so their keys may be the game's
 * too (the arrows are Controller 1's D-pad keys:
 * input_map::hotkey_shares_keys).  The race's side is game/app/pc_headcam.h
 * (pc_headcam_calibrate: the offsets, the ini).  The panel (drawn with
 * platform/overlay.h as the song changer's: upper case) shows the car and
 * the cockpit, the four axes with their values, the controls and the
 * hotkeys; not the cockpit view: it says to switch to it.  Shown while the
 * race's camera reports frames (a quarter second), so not in the menus.  No
 * Windows headers: game code can include it.
 *
 * PC (2026-11-14): the same panel, keys and pad controls are the custom
 * cameras' editor (game/app/pc_customcam.h; the F2 menu's CAMERAS > CUSTOM
 * CAMERAS).  The mode says whose it is (kModeCockpit, kModeCustom: one at a
 * time); the race's side gives the panel its title, its head line and its
 * rows - a name and a value's text each, as many as it has (the cockpit
 * camera's four - PC 2026-11-15: five, its menu model the last -, a custom
 * camera's seventeen) - and the chosen row is the
 * "axis" the keys step.  The F2 menu's custom camera items leave a request
 * for the race's side (a new camera for every car / for the car viewed,
 * edit the one in view, delete it). */
#pragma once

struct ID3D11RenderTargetView;

namespace cockpit_osd {

/* The cockpit camera's axes, its rows in order (axis_key steps through the
 * rows the race's side gave last).  PC (2026-11-15): kAxisMenuModel, the
 * car's menu model on / off (a step either way switches it; reset: off). */
enum Axis { kAxisSide, kAxisHeight, kAxisForward, kAxisTilt, kAxisMenuModel, kAxisCount };
/* What a hotkey asked for (race_frame). */
enum Action { kNone, kSave, kCancel, kReset };
/* PC (2026-11-14): whose the mode is, and what the F2 menu's custom camera
 * items ask of the race's side (take_request). */
enum Mode { kModeCockpit, kModeCustom };
enum Request { kRequestNone, kRequestNewGlobal, kRequestNewCar, kRequestEdit, kRequestDelete };
/* A row of the panel: its name and its value as shown; `caption', when not
 * empty, is a line of its own above it (a heading for the rows from here).
 * PC (2026-11-16): the texts in the interface's language (platform/
 * ui_text.h, the race's side translates them), UTF-8 - 48, 24 and 24 bytes
 * before. */
constexpr int kMaxRows = 24;
struct Row {
    char caption[160];
    char name[80];
    char value[80];
};

/* The F2 menu's item: the cockpit camera's calibration started / stopped
 * (any thread).  Returns whether it is on after.  (The custom cameras'
 * editor on: it ends, the calibration starts.) */
bool toggle();
/* PC (2026-11-14): the custom cameras' editor started with `request' (a
 * Request: what the race's side is to open it on), or - the editor on -
 * stopped.  Returns whether it is on after. */
bool toggle_custom(int request);
/* A request without the mode (kRequestDelete). */
void request(int r);
/* The mode (Mode), meaningful while active(). */
int mode();
/* The race's thread: the request left since (kRequestNone: none). */
int take_request();
/* Ended (saved or cancelled: the race's side). */
void stop();
bool active();
/* The axis chosen (Axis). */
int axis();

/* The hotkeys (the window thread), while active: the axis before / after
 * (dir -1 / +1), a step of it down / up (dir -1 / +1), save, cancel, the
 * axis to 0. */
void axis_key(int dir);
void adjust_key(int dir);
void save_key();
void cancel_key();
void reset_key();

/* The player's own controls (the pad backends, each read, while active):
 * the D-pad and Cross / Circle / Triangle held, as the game maps them, of
 * the controller (which 0) or the wheel (1) of port `port' (0, 1).  For the
 * player calibrating (player()) a new press is the key's: Up / Down the
 * axis, Left / Right a step (repeating held, as the settings menu's D-pad),
 * Cross save, Circle cancel, Triangle the axis to 0; a press on the other
 * player's pad takes the calibration over to that player (a two-player
 * race: the first press switches, the next ones act).  Returns whether the
 * controls are the calibration's now: the backend then holds them from the
 * game. */
enum { kPadUp = 1, kPadDown = 2, kPadLeft = 4, kPadRight = 8, kPadCross = 16, kPadCircle = 32, kPadTriangle = 64 };
bool pad_state(int which, int port, unsigned held);
/* The player calibrating: a controller port (0, 1) - player 1 when the mode
 * starts, the other player's pad takes it over (pad_state); the car viewed
 * by that player's camera is the one adjusted (user, 2026-11-08: "2-Player
 * mode needs special attention too, currently the calibrator only edits
 * player 2's car and the UI is buggy": both players' cameras fed one
 * calibration). */
int player();
/* The race's thread: player `port''s onboard camera ticked (0, 1; a
 * replay's, without a player, counts as 0).  Two ports within a quarter
 * second: a two-player race - the panel names the player and sits over that
 * player's half of the split screen. */
void player_seen(int port);

/* The race's thread, each tick of the calibrating player's camera while
 * active.  take_steps: the keys' steps of the chosen axis since (+ up, -
 * down; a few at most).  race_frame (PC 2026-11-14: the panel's texts are
 * the race's side's): the panel's title, its head line (the car and the
 * camera; or, with no rows, why there is nothing to adjust: "SWITCH TO THE
 * COCKPIT VIEW") and its rows, the first `count' of `rows' (0: none; the
 * chosen axis is one of them); returns the action asked for since (one a
 * call). */
int take_steps();
int race_frame(const char *title, const char *head, const Row *rows, int count);

/* The panel over the picture (the present, as the other overlays). */
void present(ID3D11RenderTargetView *backbuffer);

} // namespace cockpit_osd
