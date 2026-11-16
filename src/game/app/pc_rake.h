/* pc_rake.h - PC: the body's pitch at rest (its rake) kept out of the
 * direction of the car's forces (docs/ROADMAP.md item 76).  No original: new
 * PC code, for the enhanced engine's ENH_PHYSICS_REAR_RIDE_HEIGHT
 * (game/enhanced/physics.h).
 *
 * User report (2026-11-14): "A physics bug with suspension ride height, if
 * the car's pitch isn't perfectly straight, the car behaves as if it is on a
 * hill even while on flat ground.  Maybe the fix is to use the wheels instead
 * of the body?"
 *
 * The cause.  The physics have no slope term: a hill pushes a car because
 * the suspension's force - the sum of the four corners' - is added to the
 * velocity along the BODY's up axis (car_body_motion, T298: vel += rot[2] x
 * dv) while gravity is added straight down.  On a slope the body leans with
 * the road, the lift leans with it, and what is left of the two is the pull
 * down the hill.  GT3 gives both axles one ride height (rideHeight[0]), so a
 * car at rest is level with the road and the body's lean is the road's
 * (plus what dive, squat and roll add while driving).  With the rear axle's
 * own height (Lupo 2's fix, ENH_PHYSICS_REAR_RIDE_HEIGHT) a car whose two
 * heights differ rests pitched by atan2(axleHeight[0] - axleHeight[1],
 * wheelbase) - DynCar_place (T330) starts it so and the springs hold it
 * there - and that pitch leans the lift as a slope of the same angle would:
 * 30 mm over a 2.6 m wheelbase is a 1.15 % grade, 0.11 m/s^2 for as long as
 * the race lasts, backward with the nose up.
 *
 * The fix.  The lift's direction is the up axis of the body turned back by
 * that rest pitch: the body's axes from (pitch - rake, roll, yaw).  At rest
 * on level ground it is straight up; on a hill it is the road's normal;
 * dive, squat and roll lean it exactly as they did, so a car with equal
 * heights - every car without the switch, and every GT3 set-up - is
 * untouched (pc_rake_axes returns 0 and the caller uses the body's own
 * axes: the same arithmetic as before).  Not done: the lift along the
 * ground's normal (the wheels' plane, groundRot) for every car - that would
 * also take the lean of dive, squat and roll out of every car's forces,
 * which is GT3's handling as it shipped.
 * A rolling start's velocity (DynCar_setInitialSpeed, T330: along the
 * body's forward axis) takes the same axes, or a raked car would start with
 * a vertical speed of v x sin(rake).
 *
 * GT3PC_RAKE_FIX=0 (environment) turns it off, for comparing. */
#ifndef GAME_APP_PC_RAKE_H
#define GAME_APP_PC_RAKE_H

#include "game/game.h"

class RaceCarState;

/* The car's rest pitch on level ground, radians (nose up positive): 0 with
 * equal axle heights, or without ENH_PHYSICS_REAR_RIDE_HEIGHT. */
f32 pc_rake_angle(RaceCarState *car);
/* The body's axes without its rest pitch - forward, lateral, up, as
 * eulerToAxes gives st->rot[0..2] - for a car whose axle heights differ: 1.
 * 0 (the arrays untouched) for every other car and with the fix off: the
 * caller uses st->rot.  Any thread (the cars' steps run on the physics
 * workers). */
int pc_rake_axes(RaceCarState *car, f32 *forward, f32 *lateral, f32 *up);

#endif /* GAME_APP_PC_RAKE_H */
