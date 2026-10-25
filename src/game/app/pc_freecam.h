/* pc_freecam.h - the chase camera's free orbit (docs/renderer.md "Free
 * camera").  No original: new PC code.
 *
 * The chase camera (OnBoardCamera's mount 1, T378) sits behind the car
 * looking past it; its controls (pc_freecam_stick: the right analog stick,
 * keyboard I / J / K / L by default; a wheel's as mapped) swing it
 * round the car while held, the stick's deflection the speed (user request:
 * additive), the view turned with it so that the car keeps its place in the
 * picture: left / right up to 180 degrees (stick right: round the car's left
 * side; at the stops in front of the car, looking back at it), up to 45
 * degrees (looking down on the car), down to 45 degrees as far as the road
 * lets it - the eye stays a margin above the runway (the course's road
 * surface, the cars' collision mesh).  A pull back behind fights the stick,
 * growing with the angle (a spring: weak near the middle, strong far out): a
 * held deflection settles where the two meet, let go the camera swings
 * back.
 * The orbit is done on the camera's own view matrix, in its own axes (right,
 * up, back from the pivot): no assumption about the world's handedness. */
#ifndef GAME_APP_PC_FREECAM_H
#define GAME_APP_PC_FREECAM_H

#include "game/game.h"

/* The ground under (x, z) (y: the eye's height, for a road above another):
 * 1 and *groundY, or 0 when there is no road there. */
typedef int (*PcGroundFn)(void *ctx, f32 x, f32 y, f32 z, f32 *groundY);

/* Once a tick: turns the camera's aim by the stick (x right +, y up +, -1
 * .. 1, after its dead zone and curve: the turning speed) against the pull
 * back behind, and swings the view matrix `view' (16 floats as
 * pgl_get_matrix gives it: the rotation's rows at 0, 4, 8, the translation
 * at 12) round `pivot' (world) by it.  `key': the camera (its swing is kept
 * per camera, eased toward the aim).  ground: the road under the eye;
 * groundDefault the height to keep above where there is none. */
void pc_freecam_apply(const void *key, f32 *view, const f32 *pivot, f32 stickX, f32 stickY, PcGroundFn ground,
                      void *ctx, f32 groundDefault);
/* The stick for pc_freecam_apply of player `player' (0, 1) from its chase
 * camera controls (gt3pc.ini CameraLeft / Right / Up / Down of the player's
 * [ControllerN], or of [WheelN] when `wheel': the wheel is its device; the
 * launcher's Controls > Mapping, first rows; by default the right stick and
 * J / L / I / K), each axis with its dead zone on an exponential curve (half
 * way about 0.18).  takenX / takenY: the game's controller settings use the
 * right stick's x / y: a camera binding the right stick's own controls have
 * is left out on that axis (platform/pad_pc.h camera). */
void pc_freecam_stick(int player, int wheel, int takenX, int takenY, f32 *stickX, f32 *stickY);

#endif /* GAME_APP_PC_FREECAM_H */
