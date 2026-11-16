/* pc_replay_sync.h - PC (no original): a replay checked against its race.
 *
 * A replay runs the race again: the computer cars drive again, the players'
 * cars from their recorded inputs (AutomobileControlRecord, T318), so it
 * shows the race only while the simulation repeats it bit for bit.  A computer
 * car steers back to its line and hides a small difference; a car on recorded
 * inputs cannot, so any difference ends up in the player's car, whoever
 * started it.
 *
 * [Debug] ReplaySync=on (gt3pc.ini only) or GT3PC_REPLAY_SYNC=1: a
 * live race keeps every car's state of every physics frame (positions,
 * speeds, controls, engine, wheels; about 3 KB a frame in a race of 32 cars,
 * at most 256 MB: 23 minutes) and the player's input, and the replay played
 * after it in the same session compares its own against them, exactly.  Off:
 * nothing is kept.
 *
 * The log: "PC: replay sync: ...": for each car the first frame it differs
 * and how (the race's value / the replay's), in the order they happen, so the
 * first line names the frame and the car it began with; the player's input
 * when that differs (a recorder fault); a summary at the next race start. */
#ifndef GAME_RACE_DYNAMICS_PC_REPLAY_SYNC_H
#define GAME_RACE_DYNAMICS_PC_REPLAY_SYNC_H

class DynamicsConductor;

/* DynamicsConductor::initialize, at its end: a run starts, its frame 0 the
 * cars as placed. */
void pc_replay_sync_begin(DynamicsConductor *dc);
/* DynamicsConductor::update: a physics frame was stepped. */
void pc_replay_sync_frame(DynamicsConductor *dc);

#endif /* GAME_RACE_DYNAMICS_PC_REPLAY_SYNC_H */
