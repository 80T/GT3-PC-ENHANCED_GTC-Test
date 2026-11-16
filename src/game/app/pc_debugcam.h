/* pc_debugcam.h - the debug camera (docs/renderer.md "Debug camera"; its
 * switch, the hotkey and the controls: platform/debug_camera.h).  No
 * original: new PC code.
 *
 * While it is on, a race's view 0 (the single screen; player 1's of a split
 * screen) is seen from it: CameraManager::applyView puts its view matrix in
 * place of the camera's, getPositionX / Y / Z give its eye (so the course's
 * vision list, the levels of detail and the sounds' listener follow it),
 * isDrawBackMirror says no, virtual_12 / F_00209370 show every car (the
 * bumper view's own car too, and a live race's player 1 is put in the chase
 * view meanwhile: pc_debugcam_mount); the race views' projection takes its
 * field of view (pc_debugcam_projection); the view is not blended between ticks (it
 * moves every frame by itself).  The HUD is not drawn (RacePS2Base::render,
 * RaceSplitBattle::virtual_45).  It starts where the camera was - its eye,
 * heading, pitch and field of view - and flies by the controls once a frame
 * by the real time (pc_debugcam_frame), while the tick step pauses too:
 *   move forward / back, left / right   in the horizontal plane of the
 *                                       heading (X and Z)
 *   move up / down                      vertically (Y)
 *   look                                heading 120 degrees a second, pitch
 *                                       90 (to +-89 degrees); the mouse's
 *                                       movement 0.05 degrees a count
 *   lock                                held where it is, the players
 *                                       driving (a press again: flying)
 *   move speed up / down                x4 a second held, x1.25 a mouse wheel
 *                                       notch (0.5 .. 500 m/s; 15 at first)
 *   field of view up / down             45 degrees a second held, 5 a notch
 *                                       (10 .. 150 degrees vertically)
 * (an analog input past a 10% dead zone, squared: fine near the middle). */
#ifndef GAME_APP_PC_DEBUGCAM_H
#define GAME_APP_PC_DEBUGCAM_H

#include "game/game.h"

/* The debug camera is on. */
int pc_debugcam_on(void);
/* It is on and replaces view `view' (a CameraManager's current). */
int pc_debugcam_owns(int view);
/* It has been on in this tick step pause (pc_frame_ticks): the race's frames
 * go on at the last tick until the pause ends, it off again too.  A race
 * behind its pause menu draws no frames between ticks, so the pause's hold
 * kept the camera's last picture, and the hotkey had no frames to act on: a
 * softlock until F6 (user report 2026-10-31). */
int pc_debugcam_step_frames(void);
/* CameraManager::applyView of view `view': 1 and the view matrix to use in
 * place of the camera's `cam' (16 floats: the rotation's columns the axes, as
 * pgl_get_matrix gives it); the first call after it is turned on starts it
 * from cam. */
int pc_debugcam_view(int view, const f32 *cam, f32 *out);
/* Its eye (1; 0 while it does not replace view `view' yet). */
int pc_debugcam_eye(int view, f32 *xyz);
/* After a race view's projection is set (pc_fov_apply; the projection
 * matrix current): its vertical angle the debug camera's (the first call
 * takes the camera's). */
void pc_debugcam_projection(int view);
/* Once a frame (pc_frame_render_begin): the controls applied. */
void pc_debugcam_frame(void);
/* A race view is about to be drawn (RacePS2Base::drawView, drawSubView: the
 * course then is sure to be there): with the ground collision on, the eye put
 * at its height above the road under it.  That height starts as the eye's and
 * the frame's vertical moves change it, 0.3 m at the least; off the road the
 * last road's height stands (before any road, the eye flies freely).  The
 * road is the one it is over: a bridge above is not its ground. */
void pc_debugcam_ground(const struct Runway *runway);
/* A race begins (1) / ends (0): off (RacePS2Base). */
void pc_debugcam_race(int on);
/* OnBoardCamera::update, once a tick: the mount camera `key' is to use.
 * While the debug camera is on and `live' (a live race's player 1's camera):
 * `chase' (the bumper view hides the player's car; the mount kept to give
 * back), the player's view button overridden; once it is off, the mount it
 * had (else `mount' as it is). */
int pc_debugcam_mount(const void *key, int live, int mount, int chase);

/* The race's sounds are heard at the debug camera (user, 2026-10-28; before,
 * at the camera the game chose): RacePS2Base::virtual_29's listener, each
 * car's (RaceEntryCar::updateSound: the view's matrix, CameraManager's
 * isOwnCar 0 and getCameraKind 2 - outside every car) and the ambient
 * sound's (RaceAmbientSound::update).  Their Doppler and the ambient's
 * motion take the debug camera's own: its eye at the last sound tick
 * (pc_debugcam_listener_prev; the game camera's last eye against the
 * debug eye swung the Doppler wildly), the step held to 0.9 of the speed
 * of sound (it flies up to 500 m/s: a negative pitch past it), none after
 * a gap (a pause); the ambient's side axis its right axis
 * (pc_debugcam_side).  1 while the debug camera owns view `view'. */
int pc_debugcam_listener_prev(int view, f32 *xyz);
int pc_debugcam_side(int view, f32 *xyz);
/* After a sound tick (RacePS2Base::virtual_29): the eye remembered. */
void pc_debugcam_listener_tick(void);

#endif /* GAME_APP_PC_DEBUGCAM_H */
