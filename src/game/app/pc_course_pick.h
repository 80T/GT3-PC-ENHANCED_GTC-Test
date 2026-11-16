/* pc_course_pick.h - PC (ROADMAP 24 / 25, no original): the course
 * randomizer ([Game] CourseRandomizer) and the course forcer ([Debug]
 * CourseForcer / ForcedCourse).
 *
 * The courses: every file of the run's course folder, courses/<region>/ and
 * the mods' (the course table, T179), each with its start places (its
 * runway's grid records, read from the file's head; -1 not known), by label;
 * the settings menu offers them (settings::set_course_choices), built at the
 * start (pc_course_pick_init, game_init).  A file needs nothing else.
 *
 * The forcer: race_organization_create (T123) puts every race entered (not
 * a RaceReplayInformation or a RaceArcadeDemoInformation) on the forced
 * course.  A course of the database (paramDB COURSE, table 0x22) becomes the
 * race's.  A file it does not have is only read in place of the race's own
 * course's (course_read_file_of): the race keeps its own name, kind and tyres,
 * and a saved race records the file ("FILE" in its RaceInfoBlock's spare
 * bytes).  More cars than start places start along the racing line
 * (pc_race_cars.h); a course with no start places is not forced (logged).
 *
 * The randomizer: the GT mode's career races (run_gtmode_race,
 * run_championship, T053: the events G.., E.., D..) on a random course of
 * their discipline - one the database's events of it race on (the leagues'
 * and the endurance events' for G / E, the rally events' for D) whose start
 * places hold the race's cars.  PC (2026-11-07): extended, the draw is from
 * every course of the database with a file and a start grid but the licence
 * sections ("L_"), whatever its surface - tarmac, dirt, wet - and a grid
 * short of the cars does (the cars past it start along the racing line,
 * pc_race_cars.h, as the forcer's).  A draw for each event entered and each
 * session of a championship (its free run and retries keep it).  The forcer
 * comes first.  The event pages' course pictures (T041
 * EventStatusButtonObject, T036 event_course_logo) become the page's "?"
 * picture (the texture hoge_T_localize_izumi4_r_texture_<language>_q_min,
 * found by its gzip name), the course's name (event_course_doc) none.
 *
 * The laps (ROADMAP 24 / 26): a race organization may run another lap count
 * than its race record's (pc_race_laps, bound by race_organization_create):
 * the lap forcer's ([Debug] LapForcer / ForcedLaps: every race entered but
 * a replay, a demo or a machine test, whose record's lap count is the test),
 * else, a career race under the randomizer, its record's laps scaled by its
 * course's lap length over the course run's (about the event's distance; 1
 * .. 255 laps).  It replaces the lap count where the race reads one (the
 * rules, DynamicsConductor::attach; the 2-player battles' option; the race
 * menu's "N Laps"; the HUD's endurance look); a race without laps (a time
 * limit) keeps none.  The race time (ROADMAP 27): the race time forcer
 * ([Debug] RaceTimeForcer / ForcedRaceTime) gives a race with a time limit
 * (the endurance races of two hours: no laps, the race over at the first
 * lap end past the time) that many minutes (DynamicsConductor::attach's
 * rules); a race without one keeps none.  A saved race records both (its
 * RaceInfoBlock's spare bytes: "LAPS" / "TIME", u16 each), its replay runs
 * them.
 *
 * The course mode (PC 2026-11-07, user: "what makes the track's finish line
 * position or logic to complete the race"): the course mode forcer ([Debug]
 * CourseModeForcer) gives every race entered but a replay, a demo, a
 * machine test or a licence test another end than its lap count, through
 * the base conductor (T317 / T308: the slots the machine test's and the
 * licence's conductors override): finish - the course's own finish line,
 * the runway's separate line (+0x1C) where a licence test of type 2 ends
 * (DynamicsConductorLicense::virtual_18); a normal race crosses it without
 * notice, its virtual_18 answering 0 - a course without one keeps its laps;
 * 400 / 1000 / max - the machine tests' lines (T311 g_002D75F0: 400, 1000,
 * 6845 m along the lap): the last is the finish line (virtual_30), the
 * ones before it the checkpoints (getGateCount / getGatePosition), the lap
 * count none (virtual_29), a line past the lap put 10 m before its end
 * (the lap distance wraps there).  At the line the car's race ends as a
 * lap end would at the lap count (virtual_18: its time posted, the goal
 * event, car 0's finish timer, its lap raised and its lap start time the
 * crossing - the running order ranks a finished car on the same lap behind
 * the running ones, so without the lap the first to finish came last).  A
 * saved race records the mode ("MODE", u16) and its replay runs it.
 *
 * The log: "PC: courses: ...", "PC: course randomizer: ...", "PC: course
 * forcer: ...", "PC: laps: ...", "PC: course mode: ...". */
#ifndef GAME_APP_PC_COURSE_PICK_H
#define GAME_APP_PC_COURSE_PICK_H

#include "types.h"

class RaceInformation;
class RaceOrganization;
struct RaceInfoBlock;
namespace gt2k_menu {
class Item;
class PageLayout;
struct MenuLayoutItem;
}

/* The courses and the randomizer's pools (game_init, after the databases
 * and the course table). */
void pc_course_pick_init(void);

/* The forcer on info's race (its race selected): 1 when it is on the
 * forced course now. */
int pc_course_force(RaceInformation *info);

/* A GT mode career race (its race selected): the forcer's course, else the
 * randomizer's draw. */
void pc_course_career(RaceInformation *info);

/* 1 when the randomizer hides the course of event `event' (a career event,
 * the randomizer on). */
int pc_course_hidden(const char *event);

/* The page's "?" picture in place of course `course''s: centred on pos, or
 * (pos NULL) where the hidden picture `course' of the page would be; NULL
 * when the page has no "?" or no such place (the caller shows the course). */
gt2k_menu::Item *pc_course_question(gt2k_menu::PageLayout *layout, const char *course,
                                    gt2k_menu::MenuLayoutItem *pos);

/* race_organization_create (its course set, before setupOrganization):
 * org's lap count from the lap forcer or the randomizer's scale, its time
 * limit from the race time forcer (none: the record's). */
void pc_race_run_bind(RaceOrganization *org, RaceInformation *info);
/* The laps org's race runs where it would run `own' (own <= 0: no laps,
 * kept). */
int pc_race_laps(const RaceOrganization *org, int own);
/* The minutes org's race runs where its time limit would be `own' (own <=
 * 0: no time limit, kept). */
int pc_race_minutes(const RaceOrganization *org, int own);
/* PC (2026-11-07): the course mode forcer's mode of org's race
 * (settings::CourseMode as an int; 0 none), its lines - 0 for none or the
 * course's own finish line, else 1 .. 3 (400; 400, 1000; 400, 1000, 6845 m)
 * - and line i's distance along a lap of lapLength m (held 10 m short of
 * the lap; -1 past the lines). */
int pc_race_mode(const RaceOrganization *org);
int pc_race_mode_lines(const RaceOrganization *org);
f32 pc_race_mode_line(const RaceOrganization *org, int i, f32 lapLength);
/* DynamicsConductor::initialize (the course loaded): the mode's lines on
 * this course logged (ownLine: the runway's own finish line, 0 none). */
void pc_race_mode_course(const RaceOrganization *org, f32 ownLine, f32 lapLength);
/* RaceBase::virtual_7: org's laps, time and mode into blk (when not the
 * record's). */
void pc_race_run_store(RaceInfoBlock *blk, const RaceOrganization *org);
/* RaceReplayInformation::setupOrganization: the laps and time blk
 * recorded. */
void pc_race_run_load(RaceOrganization *org, const RaceInfoBlock *blk);

#endif /* GAME_APP_PC_COURSE_PICK_H */
