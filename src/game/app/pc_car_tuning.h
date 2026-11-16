/* pc_car_tuning.h - PC (ROADMAP 30, no original): the car multipliers,
 * the cars' tyre wear, engine power and tyre grip in percent of the game's
 * own, the players' cars and the computer's ([Game] TireWearPlayer /
 * TireWearEnemies, PowerPlayer / PowerEnemies, GripPlayer / GripEnemies;
 * settings::car_tune).
 *
 * A car is a player's unless the computer drives it (DynCarState.driver
 * DRIVER_COMPUTER): DRIVER_PAD, and DRIVER_AUTO (a player's car the
 * computer drives in the trial modes; the time trials' ghost) are the
 * players'.
 *
 * Where they act:
 * - tyre wear: the wear each tick (car_tyre_wear, T296), so a race without
 *   tyre wear stays without; 0 %: none;
 * - power: the engine's torque (engine_output, T338: load x (torque x k +
 *   friction) - friction, the engine braking kept) and the electric motor's
 *   drive (gearbox_electric_torque, T344);
 * - grip: each wheel's grip (car_wheel_grip, T300: every tyre force), and
 *   the computer drivers' corner speeds (ai_corner_speed, T364) by the same
 *   factor, so they take the corners at the grip they have.
 *
 * The surface values (ROADMAP 34; [Game] SurfaceGrip<code> /
 * SurfaceResistance<code>; settings::surface_tune): the raw values in place
 * of the game's for each surface code (DynWheel.surface, 0 .. 15), every
 * car alike, the game's own where not set:
 * - grip: the tyre table's grip on the surface (ForceVol<code> / 100, x the
 *   tyre's grip), put into each tyre's surfaceGrip when a race car is built
 *   (TyreData_initLoadCurve inside DynCar_initialize's
 *   pc_surface_build_begin / _end; the menus' cars keep the table's), so
 *   every reader has it: the wheels' grip, latGripUse, the computer
 *   drivers' corner speeds;
 * - resistance: the surface's rolling resistance (car_rolling_resistance,
 *   T296; the game's: g_002D2CE0.rollingResistance, DynParams x 0.01 -
 *   tarmac 0.1, grass 0.2, sand 25.5, gravel 0.3, dirt 0.2: what slows a car
 *   off the road).
 *
 * A race takes the settings when its organization is made
 * (race_organization_create -> pc_car_tuning_bind) and keeps them.  A
 * replay runs the cars' physics again from their recorded inputs, so a
 * saved race records its values (its RaceInfoBlock's spare bytes: "TUNE",
 * six u16 percents; "SRAW", 16 x 2 s16 hundredths, -1 own) and its replay
 * runs them (none recorded: 100 %, the own).  The attract mode's demos run
 * at 100 % and the own.
 *
 * PC (2026-11-14): the challenge Car Decay (game/app/pc_challenge.h) puts the
 * players' power by the GT mode's day in place of PowerPlayer in the GT
 * mode's races, recorded as "TUNC" in place of "TUNE" (the same six u16, the
 * players' power in hundredths of a percent).
 *
 * The log: "PC: car multipliers: ...", "PC: surfaces: ...". */
#ifndef GAME_APP_PC_CAR_TUNING_H
#define GAME_APP_PC_CAR_TUNING_H

#include "types.h"

class RaceCarState;
class RaceInformation;
class RaceOrganization;
struct RaceInfoBlock;

enum PcCarTune { PC_TUNE_TIRE_WEAR, PC_TUNE_POWER, PC_TUNE_GRIP };

/* race_organization_create (before setupOrganization): org's multipliers
 * from the settings (a replay's or a demo's: none, 100 %). */
void pc_car_tuning_bind(RaceOrganization *org, RaceInformation *info);
/* RaceBase::virtual_7: org's multipliers into blk (when any is not 100). */
void pc_car_tuning_store(RaceInfoBlock *blk, const RaceOrganization *org);
/* RaceReplayInformation::setupOrganization: the multipliers blk recorded. */
void pc_car_tuning_load(RaceOrganization *org, const RaceInfoBlock *blk);

/* The factor of `what' for car (its race's, the player's or the enemies'
 * by its driver; 1 when its race has none). */
f32 pc_car_tuning(const RaceCarState *car, PcCarTune what);

/* DynCar_initialize, around CarDynamics_init: the cars built meanwhile take
 * org's surface grips (pc_surface_grip_raw). */
void pc_surface_build_begin(const RaceOrganization *org);
void pc_surface_build_end(void);
/* TyreData_initLoadCurve: surface code `code''s grip for the car being
 * built, hundredths (the tyre table's ForceVol in its place), or
 * settings::kSurfaceOwn (-1: the table's; also outside a build). */
int pc_surface_grip_raw(int code);
/* The rolling resistance on surface code `code' in car's race: its raw
 * value, else `own' (the game's). */
f32 pc_surface_resistance(const RaceCarState *car, int code, f32 own);

#endif /* GAME_APP_PC_CAR_TUNING_H */
