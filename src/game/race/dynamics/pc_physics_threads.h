/* pc_physics_threads.h - PC (no original): the cars' own part of
 * DynamicsConductor::update on native worker threads.
 *
 * GT3PC_PHYSICS_THREADS=<N> (a test switch, read once; unset or 0: off, the
 * game as before): N worker threads (at most 16), started at the first race
 * frame and kept.  update's first twelve per-car steps (car_save_prev ..
 * car_crash_sound_clear, T317) read the other cars' and the race's data
 * but write only their own car; each car's twelve run in their order on
 * one thread, the cars spread over the workers and the calling game thread.
 * The game thread then waits for the workers natively (spinning, then
 * yielding its OS time slice): it never yields to the game's scheduler, so
 * no other game thread runs meanwhile, as before.
 *
 * Bit-identical with the serial loops:
 * - every job runs in the caller's FPU mode: the x87 control word and the
 *   MXCSR are copied (the game threads round toward zero and flush
 *   denormals, platform/scheduler.cpp set_game_fpu_mode);
 * - the steps' only shared writes are first-use caches, filled on the game
 *   thread before the workers start (pc_car_tuning's race lookup, the CVT
 *   rates: T317 dc_part_a_threaded);
 * - the cars' order does not matter: no step reads what another car's
 *   steps of the same frame write (the pair states, the gap tables, the
 *   collision boxes and the traffic notes they read are written after
 *   them);
 * - a race where two cars read live or recorded inputs (two players, a
 *   time trial's ghost) runs the serial loops: their RaceInputs and the
 *   ghost's lap record would be shared.
 * The log: "physics: GT3PC_PHYSICS_THREADS: ...". */
#ifndef GAME_RACE_DYNAMICS_PC_PHYSICS_THREADS_H
#define GAME_RACE_DYNAMICS_PC_PHYSICS_THREADS_H

/* One car's work: car `car' of ctx. */
typedef void (*PcCarJob)(void *ctx, int car);

/* The worker threads (0: off).  The first call reads
 * GT3PC_PHYSICS_THREADS and starts them.  Game thread only. */
int  pc_physics_threads(void);
/* job(ctx, i) for every i in first .. last - 1, each on one thread (the
 * caller's or a worker's) in the caller's FPU mode, in no set order;
 * returns when all are done and no worker is inside the job any more.
 * Game thread only (one job at a time). */
void pc_physics_run(PcCarJob job, void *ctx, int first, int last);

#endif /* GAME_RACE_DYNAMICS_PC_PHYSICS_THREADS_H */
