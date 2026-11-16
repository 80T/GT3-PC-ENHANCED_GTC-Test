/* pc_race_cars.h - PC (ROADMAP 35, no original): races of more cars.
 *
 * [Game] RaceCars (settings::race_cars, 6 .. 32): the races with opponents
 * (the GT mode's races and championships, the arcade's single races: their
 * RaceGTmodeInformation / RaceSinglePlayerInformation, not the attract
 * mode's demos) the game runs with 6 cars run with that many; with [Game]
 * RaceCarsOverride on, also those it runs with fewer (the rally events and
 * the arcade's dirt races 2, the wet courses 4).  The others keep theirs
 * (licences, time trials, the 2-player and i.LINK battles, the demos; a
 * replay its saved race's; a championship the cars it drew).  The arrays of
 * a race are sized PC_RACE_CARS_MAX (game/race/race_cars.h).
 *
 * The start grid: a course holds 6 grid places (Trial Mountain 8, Smokey
 * Mountain North 5); a car whose place is past them gets one made along the
 * course's racing line (pc_grid_place): the grid's own pattern - its last
 * two places' offsets across the line, its rows' spacing along it -
 * continued behind the last row, the heading the line's there, on the road,
 * clear of its walls and on the line's side of them (the offset goes toward
 * the line until it is).  A course without a racing line (or a grid too far
 * back for the lap) gives the car an existing place (place % the grid's
 * count).  The player starts last, as the races put it (moveEntryOrder to the
 * back).
 *
 * The log: "PC: race cars: ...", "PC: grid: ...". */
#ifndef GAME_APP_PC_RACE_CARS_H
#define GAME_APP_PC_RACE_CARS_H

#include "types.h"
#include "game/race/race_cars.h"            /* PC_RACE_CARS_MAX */

/* The most bytes of a saved replay's data (retail 0x41940: REPLAY_DATA_LOAD_
 * SIZE, the buffers it is loaded into 0x40 more for the alignment), with the
 * slots of the cars past 8 (pc_race_info_size). */
enum { PC_REPLAY_DATA_SIZE = 0x41940 + (PC_RACE_CARS_MAX - 8) * 0x280 };

class DynamicsConductor;
class RaceCourse;
struct RaceInfoBlock;
struct RaceInfoSlot;

/* A race with opponents' car count `own' (its information's: the
 * constructor's, the race record's, the course kind's): the setting's for
 * the 6-car races (and with the override the 2..5), else own. */
int pc_race_cars(int own);

/* DynamicsConductor::initialize: the start of the car at grid place
 * `order' (0 the pole): the front's x, y (the dynamics' axes: the grid
 * record's x, -z), its height and heading, from the course's grid when it
 * has the place (rot0: record 0's heading, as retail), else made
 * (pc_grid_place above). */
void pc_grid_start(DynamicsConductor *dc, RaceCourse *course, int order, f32 pos[3], f32 *yaw);

/* A saved race (replay, ghost: RaceInfoBlock, written by RaceBase::
 * virtual_7) of more than 8 cars: the block keeps its 8 slots; the slots
 * of cars 9.. follow it (from 0x191C, 0x280 bytes each) and its size word
 * starts past them, so the control records (Manager::save appends at the
 * size word) come after them.  Where the records start is known from the
 * block's carCount (the size word counts the records too once they are
 * saved).  A block of 8 cars or fewer is retail's, byte for byte, and a
 * saved race from before still loads. */
/* The bytes of the block of a race of `cars' cars (0x191C up to 8). */
int pc_race_info_size(int cars);
/* Car i's slot of blk (i < 8: blk->slots[i]; else after the block). */
RaceInfoSlot *pc_race_info_slot(RaceInfoBlock *blk, int i);
/* The cars of a saved race's block (its carCount, held to 0 ..
 * PC_RACE_CARS_MAX). */
int pc_race_info_cars(const RaceInfoBlock *blk);
/* What follows the block and its slots: the control records. */
u8 *pc_race_info_end(const RaceInfoBlock *blk);

/* A car whose physics blew up (car_view_update, T302: `what' out of all
 * bounds, value its value): logged once a race, with the car's state (its
 * number, driver, place, speeds, wheels' surfaces, physics threads), so
 * the log tells which car and where.  Game thread. */
class RaceCarState;
void pc_car_runaway(const RaceCarState *car, const char *what, f32 value);

#endif /* GAME_APP_PC_RACE_CARS_H */
