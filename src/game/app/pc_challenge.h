/* pc_challenge.h - PC (2026-11-14, no original): the challenge modes ([Challenges]
 * MoneyDecay, CarDecay, CourseShrinkage: the launcher's Game > Challenges, the
 * settings menu's CHALLENGE MODES; platform/settings.h settings::challenge).
 * User: "Challenge modes: Money Decay ... Car Decay ... Course shrinkage".
 *
 * Each goes by the GT mode's day count (GameStatus::day: 1 in a new game, one
 * more each time a GT mode race, licence test, machine test or free run is
 * entered - T037's action_go_race, _license, _machine_test, _free_run):
 *
 * - Money decay: each day passed takes credits from the money, at the moment
 *   the day passes (pc_challenge_day_passed): on days 2 .. 9 (day - 1) x 50,
 *   from day 10 on (day - 9) x 500, at most 200,000 a day (day 2: 50, day 9:
 *   400, day 10: 500, day 103: 47,000, day 409 on: 200,000; the user chose
 *   these of the request's formulas and examples, which disagree); prices are
 *   x 100 in Japanese, the decay too.  Never below 0 credits.  A licence
 *   earned (the "licence acquired" screen, T059 license_get_screen) pauses it
 *   for the next 10 days passed; earning another meanwhile starts the 10 days
 *   again (never more than 10).  The pause is the save's: a section of the
 *   GameStatus 8.0 stream (PC_EXT_CHALLENGE, game/save/pc_save_ext.h).
 * - Car decay: the players' cars' power 0.25 % less for each day passed
 *   (day - 1), at least 5 % of it (day 381 on), in place of the Extras'
 *   Power (player) (the user's choice); the enemies keep theirs.  A race
 *   takes the day's value when it is made (game/app/pc_car_tuning.h).
 * - Course shrinkage: the course 0.2 % smaller on each axis for each day
 *   passed, at least 25 % of its size (day 376 on), in place of the Extras'
 *   course scale (game/app/pc_course_scale.h).
 *
 * Car decay and course shrinkage act in the GT mode's own races alone: the
 * races, championships, licence tests, machine tests and free runs main()
 * enters from the GT mode (T000: pc_challenge_scene); not the arcade mode,
 * the menus' race previews, the demos or the replay theatre.  A saved race
 * records the values it ran with (their replays run them).
 * PC (2026-11-15, user: "Car decay challenge mode: Power modifier needs to be
 * disabled for license tests"): car decay not in the licence tests (their
 * own scene, PC_SCENE_GT_LICENSE: the test and its demonstration run as with
 * car decay off - Extras > Power (player)); course shrinkage still.
 *
 * While one is on, a panel over the bottom left corner of the picture says so
 * with its values (platform/challenge_osd.h) in the GT mode's menus and races.
 *
 * The log: "PC: challenge: ...". */
#ifndef GAME_APP_PC_CHALLENGE_H
#define GAME_APP_PC_CHALLENGE_H

#include "types.h"

namespace gt2k_game {
class GameStatus;
}
struct PcSaveWriter;

/* The values by day (any day; 1 and below: none passed). */
s64 pc_challenge_money_decay(int day);          /* the credits day `day' takes (x 1, not the yen's x 100) */
int pc_challenge_power(int day);                /* the power, hundredths of a percent (10000 .. 500) */
int pc_challenge_course(int day);               /* the course size, tenths of a percent (1000 .. 250) */
/* The decay's money unit: 100 in Japanese (yen, as every price), else 1. */
s64 pc_challenge_money_scale(void);

/* What main() runs (T000): the GT mode's menus (and the screens around
 * them), one of its races, or something else (the arcade mode, a movie, a
 * replay of the theatre, the demos).  PC (2026-11-15): one of its licence
 * tests (main_license_test: the test, its demonstration, its menus) a race
 * without car decay. */
enum { PC_SCENE_OTHER, PC_SCENE_GT_MENUS, PC_SCENE_GT_RACE, PC_SCENE_GT_LICENSE };
void pc_challenge_scene(int scene);
/* main()'s next mode (MenuStatus::nextMode) as a scene. */
int  pc_challenge_scene_of_mode(int mode);
/* A GT mode race's entry (main_championship .. main_free_run): its scene
 * while it runs, the one before after. */
struct PcChallengeRace {
    int before;
    PcChallengeRace();
    ~PcChallengeRace();
};

/* T037, right after a day passed (GameStatus::day + 1): the money decay. */
void pc_challenge_day_passed(void);
/* T059 license_get_screen: licence `licence' earned. */
void pc_challenge_licence_earned(int licence);

/* A race being made (race_organization_create's binds): the players' power,
 * hundredths of a percent, and the course's size, tenths of a percent, by
 * the day; 0 when the challenge does not act (off, not a GT mode race; PC
 * 2026-11-15: the power not in a licence test). */
int pc_challenge_race_power(void);
int pc_challenge_race_course(void);

/* The money decay's pause of gs: GameStatus::init (a new game: none) and
 * load (none until its section) forget it; GameStatus::pcWriteBody writes
 * its section (only while a pause is ahead), pc_load_sections reads it. */
void pc_challenge_forget(const gt2k_game::GameStatus *gs);
void pc_challenge_save_section(const gt2k_game::GameStatus *gs, PcSaveWriter *w);
void pc_challenge_load_section(const gt2k_game::GameStatus *gs, const u8 *src, u32 bytes);
/* The last day of gs's pause (0: none; the days up to it pass free). */
int  pc_challenge_paused_through(const gt2k_game::GameStatus *gs);

/* The panel's text again from the game's GameStatus (after a load, a new
 * game; the hooks above do it themselves).  gs: the one changed (nothing
 * unless it is the game's: the trade's guest garage is not). */
void pc_challenge_refresh(const gt2k_game::GameStatus *gs);

#endif /* GAME_APP_PC_CHALLENGE_H */
