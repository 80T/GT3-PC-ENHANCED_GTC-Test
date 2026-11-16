/* pc_prize_cars.h - PC (ROADMAP 23, no original): the prize car randomizer
 * (settings::prize_car_randomizer, [Game] PrizeCarRandomizer).
 *
 * With it on, the cars of a prize screen (car_get_prizes, T061) are drawn
 * from the car database's cars (paramDB table CAR), not from the prize
 * sets: each set keeps its number of cars (the roulette's 1 .. 4), each car
 * gets a random colour (as the game gives a regional replacement), an even
 * chance in the roulette.
 *
 * The cars: the CAR rows available in the run's region (db_car_is_locked)
 * whose day and menu model files are there, by their labels (ShortName,
 * which hashes to the row's code).  Rows that are the same car with other
 * colours (the "_ext" prize versions: other body models, the same names and
 * specs) are one car: same names and every part's contents but the label,
 * the car they belong to and the body model.  Of each car its shortest
 * label is the unique entry, the others its duplicates.
 *
 * A draw takes, in order:
 *   1. the unique entry of a car not in the garage (any colour) and not drawn
 *      on this screen;
 *   2. (fallback) any entry of a car not drawn on this screen: a car the
 *      garage has, or a duplicate entry;
 *   3. any entry not drawn on this screen, then any entry.
 * The log: "PC: prize car randomizer: ..." (the cars once, then each draw). */
#ifndef GAME_APP_PC_PRIZE_CARS_H
#define GAME_APP_PC_PRIZE_CARS_H

#include "types.h"

/* A prize screen's start: 1 when the randomizer is on and has cars to give
 * (pc_prize_draw_set then draws each set's), else 0 (the sets' own cars;
 * pc_prize_car gives NULL). */
int pc_prize_draw_begin(void);

/* Draws the `count' cars (at most 4) of the screen's prize set `i' (`set':
 * its name, for the log). */
void pc_prize_draw_set(int i, const char *set, int count);

/* Car j of the screen's set i drawn: its label; NULL when it was not drawn
 * (the set's own car is used). */
const char *pc_prize_car(int i, int j);

#endif /* GAME_APP_PC_PRIZE_CARS_H */
