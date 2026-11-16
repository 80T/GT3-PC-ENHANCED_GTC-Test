/* pc_headcam.h - PC: the cockpit camera in the driver's head ([Graphics]
 * CockpitCamera, CockpitFov; the F2 menu's CAMERAS > COCKPIT CAMERA;
 * docs/renderer.md "Cockpit camera").  No original: new PC code.
 *
 * User request (2026-11-07): the cockpit view placed where the driver
 * model's head is - the helmet nearly every car carries - at a field of view
 * of 75 degrees, the chase camera's controls looking round, the head not
 * drawn so that it does not sit in the picture; a second cockpit camera for
 * a car with two drivers (the rally cars).  OnBoardCamera's cockpit view
 * (T378 applyMountView, pcCockpit 1 or 2: the slot) takes this camera when
 * the setting is on: the eye at pc_headcam_eye, the in-car mounts' head
 * shake, the car's whole rotation, the view turned by pc_freecam_look; the
 * view button reaches it with or without the extra cameras (pc_detail.h).
 * RacePS2Base::drawCars (T131) draws the viewed car whole round the eye - no
 * cut, as the other cockpit view has - between pc_headcam_hide(car, slot, 1)
 * and (car, slot, 0), which give the renderer the head's box to leave out
 * (vifmodel_pc_hide: whatever lies in it is not drawn, a batch of its own or
 * one merged with the seats alike).  The heads are found at the car's load
 * (pc_headcam_find.h). */
#ifndef GAME_APP_PC_HEADCAM_H
#define GAME_APP_PC_HEADCAM_H

#include "game/game.h"

class RaceEntryCar;
struct CarModelFile;

/* A car file loaded and relocated (RaceCarModel::setData, T180): where its
 * driver's head is (and a second driver's), found from its body's batches
 * (pc_headcam_find.h) and kept by the file for the calls below (a line in the
 * log); the loader's thread (a game thread: one runs at a time). */
void pc_headcam_model(CarModelFile *file);
/* PC (2026-11-11): a menu model's file (RaceCarModel::pcLoadMenu): its glass
 * shells facing the eye found as pc_headcam_model does (pc_headcam_hide_file
 * leaves them out); no driver - what the finder takes for heads there are the
 * seats, not used or logged. */
void pc_headcam_menu_file(CarModelFile *file);
/* The setting ([Graphics] CockpitCamera). */
int pc_headcam_on(void);
/* Its field of view, degrees (vertical; [Graphics] CockpitFov). */
f32 pc_headcam_fov(void);
/* The car's cockpit cameras: 2 with two drivers, else 1 (a car without a
 * head found has the Driver mount's eye). */
int pc_headcam_count(const RaceEntryCar *car);
/* The eye of cockpit camera `slot' (1, 2) in the car's frame (metres, y up,
 * the frame the car file's camera mounts are in): the head's, else the
 * "Driver" mount's point (the bumper view's eye); PC (2026-11-08) plus the
 * model's calibrated shift (gt3pc.ini [CockpitOffsets], settings.h). */
void pc_headcam_eye(const RaceEntryCar *car, int slot, f32 eye[3]);
/* PC (2026-11-08): the model's calibrated tilt of that camera, degrees, up
 * positive (applyMountView turns the view by it). */
f32 pc_headcam_tilt(const RaceEntryCar *car, int slot);
/* PC (2026-11-08): the calibration mode (platform/cockpit_osd.h: the F2
 * menu's CALIBRATE COCKPIT CAMERA) is on.  Once a tick of the camera; off
 * after being on, the offsets not saved go back to the saved ones. */
int pc_headcam_calibrating(void);
/* Once a tick of the onboard camera while calibrating: the panel's chosen
 * axis (side, height, forward or the tilt: one at a time) moves by the
 * hotkeys' steps and the camera controls (as pc_freecam_stick gives them: x
 * right negative, y up positive) on its natural direction, and the hotkeys'
 * save / cancel / reset are done - the offsets saved for the car's model
 * and slot, the saved ones put back, or the chosen axis zeroed.  car NULL:
 * not the cockpit view (the panel says so).  port: the viewed car's
 * player's controller port (0, 1; -1 none, a replay: player 1's); only the
 * calibrating player's camera (cockpit_osd::player: player 1 at first, the
 * other player's pad takes over) adjusts anything - a two-player race has
 * two cameras.  Returns 1 for that camera (its stick was the calibration's:
 * no look round this tick), 0 for the other player's. */
int pc_headcam_calibrate(const RaceEntryCar *car, int slot, int port, f32 stickX, f32 stickY);
/* on: the car's draws that follow leave slot's head out (the eye sits in
 * it); 0: as before.  Round the viewed car's draws in drawCars. */
void pc_headcam_hide(const RaceEntryCar *car, int slot, int on);
/* PC (2026-11-14): for the custom cameras (game/app/pc_customcam.h).
 * pc_headcam_from: the point a custom camera's offset starts from, in the
 * car's frame - which 0 the car's origin (on the ground at its middle), 1
 * the bumper camera's eye (the "Driver" mount's), 2 the chase camera's (the
 * chase mount's; the built-in one's without a mount table), 3 the cockpit
 * camera's (the driver's head with the model's calibration).
 * pc_headcam_hide_at: as pc_headcam_hide for an eye anywhere (the car's
 * frame): on, the car's draws that follow leave out the head whose box holds
 * the eye and the glass shells facing inward whose box holds it - neither
 * for an eye outside them; 0: as before. */
void pc_headcam_from(const RaceEntryCar *car, int which, f32 eye[3]);
void pc_headcam_hide_at(const RaceEntryCar *car, const f32 eye[3], int on);

/* PC (2026-11-11, user: "A graphics option to use the menu model of a car
 * while the cockpit cam is active ... pre-loading the menu model in memory
 * to keep it fast"): [Graphics] CockpitMenuModel, with the cockpit camera
 * on.  The cars the views follow then keep their menu model loaded
 * (RaceCarModel::pcLoadMenu, from RacePS2Base::virtual_3 at the first frame
 * a car is followed), and drawCars draws the cockpit view's own car's body
 * with it (RaceCarModel::pcMenuDraw round drawBody) - the shadows, tyres,
 * lamps, the other views, mirrors and reflections keep the race model, so a
 * change of view swaps nothing.  The menu body is the race body's frame
 * (the same bounds on the cars checked) with more detail and no driver.
 * PC (2026-11-15, user: "Cockpit menu model needs to be per-car.  The option
 * needs to be part of the calibrator"): per car model - car's own setting
 * ([CockpitMenuModels], settings.h; was [Graphics] CockpitMenuModel for
 * every car), the calibration's MENU MODEL row (pc_headcam_calibrate: Right
 * on, Left off, reset off; saved with the offsets, cancel puts it back; the
 * value tried acts at once - turned on, the file is read at the next
 * frame).  1: car's body drawn with its menu model in the cockpit view (the
 * cockpit camera on; car NULL: 0). */
int pc_headcam_menu_model(const RaceEntryCar *car);
/* The draws that follow leave out the glass shells of `file' facing the eye
 * (pc_headcam_model found them), no head box: a menu model's body. */
void pc_headcam_hide_file(const CarModelFile *file);

#endif /* GAME_APP_PC_HEADCAM_H */
