/* pc_detail.h - the detail and distance settings of the races (docs/ROADMAP.md
 * items 11 and 12, docs/renderer.md "Detail and distance").  No original:
 * new PC code.
 *
 * Levels of detail (gt3pc.ini [Graphics] Lod, the F2 menu): on, the game
 * as it was; off, the full detail everywhere - every model's level 0 (T451
 * command 0x07: car bodies, wheels, course parts, menu cars), the tyres'
 * level 0 (T449), the course's full texture set (T451), the cars' real tyres
 * in the mirror and in the road reflection (T131), and the split screen
 * draws the full course: the main vision list and its sprites, the full far
 * clip, the environment map, the gadgets, the lights (occlusion tested),
 * the smoke and the tyre blur as a single view; the mirror the main vision
 * list too.
 *
 * Render distance ([Graphics] RenderDistance, 100 .. 400 %): the far clips
 * (the views, the mirror) and the lights' cut-off distances scaled by it;
 * the course drawn from the parts of the vision points within the extra
 * distance as well (each point lists what is seen from it within the
 * course's own distance), each part once. */
#ifndef GAME_APP_PC_DETAIL_H
#define GAME_APP_PC_DETAIL_H

#include "game/game.h"

struct VisionList;

/* Field of view ([Graphics] Fov, a replay's ReplayFov, 50 .. 200 %, the F2
 * menu): a race view's projection, just set (the projection matrix current),
 * widened or narrowed so that its vertical angle is that percent of the
 * camera's - the half-angle read from the matrix (its y scale is 1 / tan of
 * it, the window's aspect already in), times the percent, held to 5 .. 85
 * degrees - both axes alike.  Nothing at 100. */
void pc_fov_apply(int replay);

/* The levels of detail are used (else the full detail everywhere). */
int pc_lod(void);
/* The rear-view mirror ([Graphics] RearViewBumper, RearViewChase, the F2
 * menu's GRAPHICS > REAR VIEW) shown with a live race's bumper camera (on
 * by default: the original's) / its chase camera (off by default: the
 * original has none there) - OnBoardCamera's drawMirror (T378), looking
 * back neither. */
int pc_rear_view_bumper(void);
int pc_rear_view_chase(void);
/* The render distance as a factor of the original (1 .. 4). */
f32 pc_render_scale(void);
/* The parts {n, part[n]} of vision list v to draw from (x, y, z): the
 * nearest point's, and beyond 0 also those of the points within `extra' of
 * it (the farther ones first), each part once.  Cached per point. */
const u16 *pc_vision_parts(VisionList *v, f32 x, f32 y, f32 z, f32 extra);

#endif /* GAME_APP_PC_DETAIL_H */
