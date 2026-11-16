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
 * back.  The height it swings back to is [Graphics] ChaseAngle's: 0 the
 * camera's own.
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
 * groundDefault the height to keep above where there is none.  rest (PC
 * 2026-11-14): the height of the swing at rest, degrees, above 0 looking down
 * on the car - the chase camera's is pc_freecam_chase_rest ([Graphics]
 * ChaseAngle), a custom camera's its own angle (game/app/pc_customcam.h):
 * the setting is the chase camera's alone (user request).
 * ownAxes (PC 2026-11-14): 0, the swing's up is away from the side the eye
 * is on (the eye seen along the view's back axis from the centre: the chase
 * camera's, always behind); 1, along the view's own axes whatever the eye's
 * place - a custom camera's: an eye straight over the centre (a bumper eye
 * over the car's middle) had no side, and the swing flipped tick by tick
 * (user report: the ANGLE "can twitch back to 0 for a couple frames"). */
void pc_freecam_apply(const void *key, f32 *view, const f32 *pivot, f32 stickX, f32 stickY, PcGroundFn ground,
                      void *ctx, f32 groundDefault, f32 rest, int ownAxes = 0);
f32 pc_freecam_chase_rest(void);
/* The stick for pc_freecam_apply of player `player' (0, 1) from its chase
 * camera controls (gt3pc.ini CameraLeft / Right / Up / Down of the player's
 * [ControllerN], or of [WheelN] when `wheel': the wheel is its device; the
 * launcher's Controls > Mapping, first rows; by default the right stick and
 * J / L / I / K), each axis with its dead zone on an exponential curve (half
 * way about 0.18).  takenX / takenY: the game's controller settings use the
 * right stick's x / y: a camera binding the right stick's own controls have
 * is left out on that axis (platform/pad_pc.h camera). */
void pc_freecam_stick(int player, int wheel, int takenX, int takenY, f32 *stickX, f32 *stickY);

/* PC (2026-11-07): the cockpit camera's look round (game/app/pc_headcam.h),
 * once a tick: the stick (as pc_freecam_stick gives it) turns the aim - left
 * / right up to 180 degrees (looking straight back), up / down up to 60,
 * each the orbit's way round (stick right looks right, stick up looks down:
 * user request, the chase camera's controls) - against the same pull back to
 * the front, and the view matrix `view' is turned by it about its own eye
 * (the eye stays, its axes turn: the look of a head turning).  `key': the
 * camera, its look kept per camera. */
void pc_freecam_look(const void *key, f32 *view, f32 stickX, f32 stickY);

#endif /* GAME_APP_PC_FREECAM_H */
