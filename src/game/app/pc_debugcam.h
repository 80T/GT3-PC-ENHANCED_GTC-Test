/* pc_debugcam.h - the debug camera (docs/renderer.md "Debug camera"; its
 * switch, the hotkey and the controls: platform/debug_camera.h).  No
 * original: new PC code.
 *
 * While it is on, a race's view 0 (the single screen; player 1's of a split
 * screen) is seen from it: CameraManager::applyView puts its view matrix in
 * place of the camera's, getPositionX / Y / Z give its eye (so the course's
 * vision list, the levels of detail and the sound's listener follow it),
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
/* A race begins (1) / ends (0): off (RacePS2Base). */
void pc_debugcam_race(int on);
/* OnBoardCamera::update, once a tick: the mount camera `key' is to use.
 * While the debug camera is on and `live' (a live race's player 1's camera):
 * `chase' (the bumper view hides the player's car; the mount kept to give
 * back), the player's view button overridden; once it is off, the mount it
 * had (else `mount' as it is). */
int pc_debugcam_mount(const void *key, int live, int mount, int chase);

/* While one lives the camera manager answers with the game's camera, not
 * the debug camera: the race's sounds (RacePS2Base::virtual_29: each car's
 * and the ambient sound's listener, panning and Doppler at the camera the
 * game chose - the driver's head in the bumper view - not where the debug
 * camera flies; with its eye against the camera's last one the Doppler
 * swung wildly). */
struct PcDebugcamBypass {
    PcDebugcamBypass();
    ~PcDebugcamBypass();
};

#endif /* GAME_APP_PC_DEBUGCAM_H */
