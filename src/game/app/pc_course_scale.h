/* pc_course_scale.h - the course scale (gt3pc.ini [Game] CourseScaleX,
 * CourseScaleY, CourseScaleZ: the launcher's Game > Extras, docs/launcher.md;
 * platform/settings.h).  No original: new PC code.
 *
 * A course's geometry scaled along the world's axes (x and z across, y up),
 * 25 .. 400 % each, about the world's origin.  The course file is
 * transformed in place once, when RaceCourse::load has read and relocated it
 * (pc_course_scale_apply):
 *   the runway: the road's triangles, the walls, the cell maps' bounds, the
 *     lap lines (their distances along the lap remapped: each stretch of the
 *     lap by the scale along the road there, so a point keeps its place on
 *     the lap), the lap length, the markers, the start grid (its headings
 *     turned with the stretch), the gadgets' places, the lights along the
 *     road;
 *   the vision lists' points and grids, the flare sources, the course
 *     cameras (their points; their ranges on the lap remapped; the zoom of
 *     those that aim at a car kept on the car), the sound emitters;
 *   the environment: the far clip (never shorter), the fogs' start, the
 *     reach of the runway lights and of the flares;
 *   the enemy lines when lines_prepare converts them (pc_course_scale_lines:
 *     their arcs refitted where x and z differ).
 * The models are drawn with a world scale before the view (pc_course_draw_
 * begin / _end, T179 and the environment map T276): the lighting's inverses
 * are general ones then and billboards still face the camera (pgl_set_world_
 * scale), the LODs' projected sizes follow.  The background too (2026-11-11,
 * user report: a building of it came toward the camera into the course):
 * model58, scenery in world places drawn at the eye's scale 0.01 (F_001A9240),
 * in the world as the course; the backdrops drawn around the eye (model54,
 * model60, the environment map's sky) about the eye - the same picture at
 * the same scale on every axis, stretched or mirrored with the course
 * otherwise.  The cars, the gadgets' shapes and the mini map's image keep
 * their size: the map's dots are put back to its scale (T164).  Course
 * distances in the code (a licence's launch point, the corner start, the
 * Pod's stop area) go through the lap remap.
 *
 * A race organization's course takes the settings' scale as they are when
 * it loads (race_organization_create binds it; the settings menu's GAME >
 * EXTRAS changes them in the game), a saved replay's or ghost's own
 * (RaceReplayInformation::setupOrganization: the scale its RaceInfoBlock
 * recorded, RaceBase::virtual_7; none recorded - the discs' demos, earlier
 * saves - 100 %), so a replay plays on the course it was driven on.
 *
 * PC (2026-11-14): in a GT mode race with the challenge Course Shrinkage on
 * (game/app/pc_challenge.h) the course takes the day's size on every axis in
 * place of the settings'; a scale not in whole percents is recorded in
 * tenths ("CSCT" in place of "CSCL"). */
#ifndef GAME_APP_PC_COURSE_SCALE_H
#define GAME_APP_PC_COURSE_SCALE_H

#include "game/game.h"

class RaceCourse;
class CourseData;
struct RaceInfoBlock;
struct Runway;
struct RaceMiniMapData;
struct EnemyLines;

/* `course' (a new organization's) loads at the settings' scale, or with blk
 * at the scale blk recorded (none: 100 %). */
void pc_course_scale_bind(const RaceCourse *course, const RaceInfoBlock *blk);
/* RaceCourse::load, after course_data_relocate: cd transformed at course's
 * scale. */
void pc_course_scale_apply(const RaceCourse *course, CourseData *cd);
/* RaceBase::virtual_7: course's scale into blk (its spare bytes, unk_080). */
void pc_course_scale_store(RaceInfoBlock *blk, const RaceCourse *course);
/* RaceBase::virtual_19 (a ghost into a running race): logged when blk was
 * recorded at another scale than course's (the ghost leaves the road). */
void pc_course_scale_check_ghost(const RaceInfoBlock *blk, const RaceCourse *course);

/* The course's models are drawn scaled between these (nothing when cd is
 * not scaled). */
void pc_course_draw_begin(const CourseData *cd);
void pc_course_draw_end(const CourseData *cd);
/* The factor of a far clip on cd (the largest of the scales, at least 1). */
f32  pc_course_far_factor(const CourseData *cd);
/* The runway's query widening (T457: 8 m around the box) on r. */
f32  pc_course_runway_reach(const Runway *r);
/* World x, z on the course of mini map m back to the map's own scale. */
void pc_course_map_unscale(const RaceMiniMapData *m, f32 *x, f32 *z);
/* A distance along the lap of the original course on cd (as it is when cd
 * is not scaled). */
f32  pc_course_lap_distance(const CourseData *cd, f32 dist);
/* cd's lap length over the original's (1 when not scaled). */
f32  pc_course_lap_ratio(const CourseData *cd);
/* lines_prepare, before it converts the loaded lines: on a scaled course
 * they are converted (as lines_prepare would) and scaled here; 1 then (the
 * conversion is done), 0 when cd is not scaled. */
int  pc_course_scale_lines(const CourseData *cd, EnemyLines *file);

#endif /* GAME_APP_PC_COURSE_SCALE_H */
