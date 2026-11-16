/* pc_customcam.h - PC: the user's own cameras on the view button (gt3pc.ini
 * [CustomCameras]; the F2 menu's CAMERAS > CUSTOM CAMERAS; docs/renderer.md
 * "Custom cameras").  No original: new PC code.
 *
 * User request (2026-11-14): "More camera modes: User makes custom cameras
 * per model code.  Also global cameras as offsets the same way the cockpit
 * cam works, but these need unrestricted camera position limits.  Options
 * for it: Camera Rigidity settings for each way the camera deviates from the
 * position slightly based on the car's movement.  The existing chase cam
 * angle setting should only apply to the base chase cam.  The user-made ones
 * get their own."
 *
 * A custom camera is a view of the onboard camera (OnBoardCamera, T378:
 * pcCustom, its number among the car's): an eye in the car's frame - an
 * offset, without limits, from the car's origin, the bumper camera's eye,
 * the chase camera's or the cockpit camera's - a pan and a tilt of the view,
 * an angle of its own (the swing over the car that [Graphics] ChaseAngle
 * gives the chase camera, which alone keeps that setting), a field of view,
 * and a rigidity for each way the game's cameras move with their car
 * (PC_RIGID_*: 100 % fixed to the car, 0 % the game's whole movement).  Its
 * type says which of the game's two fixed views it is to everything else:
 * chase - the camera controls swing it round the car (pc_freecam_apply),
 * the chase view's sounds, HUD layout and rear view setting (mount 1) - or
 * onboard - the controls look round from the eye (pc_freecam_look), the
 * bumper view's sounds, HUD and rear view (mount 0), the car drawn round it.
 *
 * A car's cameras are the global ones (scope "global": every car) followed
 * by its model's own (the scope the model file's name, "r_gt_00"), at most
 * settings::kCustomCameraMax of each; a live race's view button reaches them
 * after the cockpit view (OnBoardCamera::pcNextView), whatever the extra
 * cameras' and the cockpit camera's settings, and GameOptions::onboardMount
 * keeps the one in view for the next race.
 *
 * They are made and edited in a race (the F2 menu's items, through
 * platform/cockpit_osd.h, whose panel, keys and pad controls the editor
 * shares with the cockpit camera's calibration): a new camera for every car
 * or for the car viewed starts as a copy of the view shown; one row of the
 * panel at a time is adjusted; save writes the scope's cameras to gt3pc.ini,
 * cancel puts the saved camera back (a new one is gone). */
#ifndef GAME_APP_PC_CUSTOMCAM_H
#define GAME_APP_PC_CUSTOMCAM_H

#include "game/game.h"

class RaceEntryCar;
class Random;

/* The ways the game's cameras move with their car (settings::CustomRigid,
 * the same order): the chase view's lag behind the car's turn and behind its
 * pitch (car_lag_yaw / car_lag_pitch) and its level horizon; the in-car
 * mounts' random shake that grows with the speed (10 .. 80 km/h); the
 * cockpit camera's shake while the tyres slip; the in-car mounts' sideways
 * and vertical springs (the car's acceleration); the bumper view's sway
 * while the tyres slip, by the speed. */
enum {
    PC_RIGID_TURN, PC_RIGID_PITCH, PC_RIGID_ROLL, PC_RIGID_SPEED_SHAKE, PC_RIGID_SLIP_SHAKE, PC_RIGID_SIDE,
    PC_RIGID_BUMPS, PC_RIGID_SWAY, PC_RIGID_COUNT
};

/* A custom camera as OnBoardCamera shows it. */
struct PcCustomView {
    f32 eye[3];                             /* in the car's frame (metres: x the car's right, y up, z back) */
    f32 pan, tilt;                          /* degrees: the view turned right / up */
    f32 angle;                              /* degrees: the swing over the car at rest (pc_freecam_apply's rest) */
    f32 fov;                                /* degrees, vertical */
    int onboard;                            /* its type: 1 onboard (mount 0), 0 chase (mount 1) */
    f32 loose[PC_RIGID_COUNT];              /* 1 - the rigidity: the share of each movement it takes */
};

/* The view an onboard camera shows, as a custom camera would give it: what a
 * new camera starts as (OnBoardCamera::pcDescribeView).  from: 0 the car's
 * origin, 1 the bumper camera's eye, 2 the chase camera's, 3 the cockpit
 * camera's (pc_headcam_from); pos: the offset from it; rigid: percent. */
struct PcCustomSource {
    int from, onboard;
    f32 pos[3];
    f32 pan, tilt, angle, fov;
    int rigid[PC_RIGID_COUNT];
};

/* The car's custom cameras: the global ones, then its model's. */
int pc_customcam_count(const RaceEntryCar *car);
/* Camera `index' (1 .. the count) of the car: 1 and *out, or 0 when the car
 * has no such camera. */
int pc_customcam_view(const RaceEntryCar *car, int index, PcCustomView *out);
/* Its type (1 onboard, 0 chase; 0 without such a camera) and its name (the
 * view button's notice; "" without). */
int pc_customcam_onboard(const RaceEntryCar *car, int index);
const char *pc_customcam_name(const RaceEntryCar *car, int index);

/* The cockpit camera's shake of camera `cam' (an OnBoardCamera: the key) - a
 * pair of CameraShakes of its own, stepped with the wheels' slip gain as the
 * bumper view steps its own: one step, their values (-1 .. 1), all 0. */
void pc_customcam_shake(const void *cam, Random *rng, f32 gain);
void pc_customcam_shake_get(const void *cam, f32 *x, f32 *y);
void pc_customcam_shake_reset(const void *cam);

/* The view camera `cam' shows this tick (its view matrix, 16 floats as
 * pgl_get_matrix gives it) and the car's frame (the matrix from the world to
 * the car's frame, the same layout): the eye's place in the car is kept for
 * pc_customcam_hide. */
void pc_customcam_shown(const void *cam, const f32 *view, const f32 *carFrame);
/* That eye (the car's frame); 0 when the camera has shown no view. */
int pc_customcam_eye(const void *cam, f32 eye[3]);
/* on: the draws of `car' that follow leave out what the eye of `cam' sits
 * in - a driver's head, the cabin's glass shells facing inward
 * (pc_headcam_hide_at); 0: as before.  Round the viewed car's draws
 * (RacePS2Base::drawCars). */
void pc_customcam_hide(const void *cam, const RaceEntryCar *car, int on);

/* The editor's mode is on (the F2 menu's NEW / EDIT items; the panel). */
int pc_customcam_editing(void);
/* Once a tick of a live race's onboard camera `cam' on `car', the player of
 * controller port `port' (0, 1): the F2 menu's requests are done and, the
 * editor on, the camera in view is adjusted - only by the editing player's
 * camera (cockpit_osd::player), a two-player race having two.  *index: the
 * custom camera in view (0: none), changed to a new camera's, or to 0 when
 * the one in view was deleted or given up.  src: the view shown as a camera
 * (NULL in a custom camera's view: a new one copies that camera).  stickX /
 * stickY: the camera controls (pc_freecam_stick).  Returns PC_CUSTOM_*. */
enum {
    PC_CUSTOM_STICK = 1,                    /* the controls were the editor's this tick (no look round) */
    PC_CUSTOM_LOCK  = 2,                    /* a camera is being edited: the view button leaves it in view */
    PC_CUSTOM_TYPE  = 4                     /* the camera's type changed (its mount with it) */
};
int pc_customcam_edit(const void *cam, const RaceEntryCar *car, int port, int *index, const PcCustomSource *src,
                      f32 stickX, f32 stickY);

#endif /* GAME_APP_PC_CUSTOMCAM_H */
