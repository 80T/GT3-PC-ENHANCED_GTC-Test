/* pc_sound.h - the sound flush per game tick (docs/sound.md 3.4, "The
 * engine").  No original: new PC code.
 *
 * On the PS2 the music thread (T463 music_thread: the sequencer, snd_flush -
 * the voices' state block to the IOP -, the BGM status) woke at every
 * VBLANK_E, as did the game's frame: one game tick per state block, each
 * applied by the IOP within a millisecond of its vblank.  Here the game
 * ticks run when the frame loop gets to them (between the frames, caught up
 * below 60 a second: game/app/pc_frame.h) while the timer's VBLANK_E comes
 * at real time, so a block held two game ticks' sound changes or none (the
 * kerb rumble's restarts merged: the loud bursts, docs/sound.md 5).
 *
 * Now each game tick wakes the music thread right after its update
 * (pc_sound_tick, from pc_frame_tick_end), and its block carries the tick's
 * vblank time, which the native engine turns into the sample it applies the
 * block at (sound_native.h, scheduled: 800 / 801 samples apart, as on the
 * PS2).  The timer's VBLANK_E still wakes it while the game ticks stall (a
 * load: more than PC_SOUND_STALL vblanks behind), so the music keeps time;
 * a game tick never wakes it ahead of the vblanks. */
#ifndef GAME_APP_PC_SOUND_H
#define GAME_APP_PC_SOUND_H

#ifdef __cplusplus
extern "C" {
#endif

/* After a game tick's update (the render thread), vblankTime: the tick's
 * vblank (sched::now_seconds' clock). */
void pc_sound_tick(double vblankTime);
/* T463's VBLANK_E handler: 1 when the timer wakes the music thread (the game
 * ticks stalled), 0 when the game ticks do. */
int pc_sound_vblank(void);
/* T463's music thread around one wake-up's work: the flush's block time is
 * the latest wake-up's (two before the thread ran are one flush). */
void pc_sound_flush_begin(void);
void pc_sound_flush_end(void);
/* T520 (sending the state block): the music thread's flush's block time;
 * -1 outside it (snd_shutdown's: now). */
double pc_sound_block_time(void);

#ifdef __cplusplus
}
#endif

#endif /* GAME_APP_PC_SOUND_H */
