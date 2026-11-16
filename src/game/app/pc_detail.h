/* pc_detail.h - the detail and distance settings of the races (docs/ROADMAP.md
 * items 11 and 12, docs/renderer.md "Detail and distance").  No original:
 * new PC code.
 *
 * Levels of detail (gt3pc.ini [Graphics] Lod, the F2 menu): on, the game
 * as it was; off, the full detail everywhere - every model's level 0 (T451
 * command 0x07: car bodies, wheels, course parts, menu cars), the tyres'
 * level 0 (T449), the course's full texture set (T451), the cars' real tyres
 * in the mirror (T131; PC: the road reflection has a setting of its own,
 * [Graphics] ReflectionDetail: pc_reflection_full), and the split screen
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
 * degrees - both axes alike.  Nothing at 100.  PC (2026-11-09): nothing in
 * the cockpit camera's view either (cockpit: CameraManager::pcCockpit with
 * the cockpit camera on, game/app/pc_headcam.h) - its own field of view,
 * [Graphics] CockpitFov, is the one it shows (user: "Cockpit FOV should not
 * be affected by the modifiers").  PC (2026-11-14): nor in a custom
 * camera's view (game/app/pc_customcam.h: its field of view is its own row;
 * `cockpit' is then set too). */
void pc_fov_apply(int replay, int cockpit);
/* PC (2026-11-14): [Graphics] Fov, the driving views' percent (a custom
 * camera made from a fixed view starts with the field of view shown:
 * OnBoardCamera::pcDescribeView). */
f32 pc_fov_percent(void);

/* The levels of detail are used (else the full detail everywhere).  PC:
 * in the road reflection its own setting instead (pc_reflection_full; T451
 * modelset_pc_set_detail around T131 drawCarReflections). */
int pc_lod(void);
/* The road reflection's cars ([Graphics] ReflectionDetail, the F2 menu's
 * GRAPHICS > ROAD REFLECTION): level 0 and the real tyres (1), or the
 * original's lowest level and tyre cylinders (0), whatever Lod. */
int pc_reflection_full(void);
/* PC (2026-11-14): the road reflection's resolution ([Graphics]
 * ReflectionResolution, the F2 menu's GRAPHICS > ROAD REFLECTION
 * RESOLUTION): the factors by axis (at least 1) its reflW x reflH buffer is
 * drawn finer by, for a screen of screenW x screenH it is stretched over - 1,
 * 1 (normal: the PS2's), half the screen's resolution or the screen's
 * (T131 drawReflections: gfx_pc_target_detail, and the blur's steps that
 * much shorter). */
void pc_reflection_scale(int screenW, int screenH, int reflW, int reflH, f32 *dx, f32 *dy);
/* PC (2026-11-07): the replays' depth of field ([Graphics]
 * ReplayDepthOfField, the F2 menu's GRAPHICS > REPLAY DEPTH OF FIELD; T182
 * ReplayBlur, drawn by T131 drawView): the levels per doubling of the blur
 * behind the focus - 2 (the original's farSteps: each level at half alpha,
 * then opaque), 1 (reduced: each level once), or 0 for no depth of field at
 * all (the live race's plain copy of the view). */
int pc_replay_blur_steps(void);
/* The rear-view mirror ([Graphics] RearViewBumper, RearViewChase, the F2
 * menu's GRAPHICS > REAR VIEW) shown with a live race's bumper camera (on
 * by default: the original's) / its chase camera (off by default: the
 * original has none there) - OnBoardCamera's drawMirror (T378), looking
 * back neither. */
int pc_rear_view_bumper(void);
int pc_rear_view_chase(void);
/* The view button's extra cameras ([Graphics] ExtraCameras, the F2 menu's
 * GRAPHICS > EXTRA CAMERAS; OnBoardCamera::pcNextView, T378). */
int pc_extra_cameras(void);
/* The camera the view button chose, named over the picture a moment. */
void pc_camera_notice(const char *name);
/* The render distance as a factor of the original (1 .. 4). */
f32 pc_render_scale(void);
/* The parts {n, part[n]} of vision list v to draw from (x, y, z): the
 * nearest point's, and beyond 0 also those of the points within `extra' of
 * it (the farther ones first), each part once.  Cached per point. */
const u16 *pc_vision_parts(VisionList *v, f32 x, f32 y, f32 z, f32 extra);
/* Its cache dropped: a course loaded (another one, or one scaled otherwise,
 * may be at the same address with the same distance: game/app/
 * pc_course_scale.h). */
void pc_vision_parts_reset(void);

#endif /* GAME_APP_PC_DETAIL_H */
