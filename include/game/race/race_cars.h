/* race_cars.h - PC (ROADMAP 35, no original): the most cars a race holds.
 *
 * GT3 runs its races with 6 cars at most; its structures hold 7 or 8
 * (DynamicsConductor::cars[8], RaceEntryPS2::slots[7], the CarIcon[7]
 * arrays, RaceInfoBlock::slots[8]...).  The PC port's races may hold up to
 * PC_RACE_CARS_MAX ([Game] RaceCars, game/app/pc_race_cars.h): every array
 * indexed by a car of the race is sized by it. */
#ifndef GAME_RACE_RACE_CARS_H
#define GAME_RACE_RACE_CARS_H

enum { PC_RACE_CARS_MAX = 32 };

#endif /* GAME_RACE_RACE_CARS_H */
