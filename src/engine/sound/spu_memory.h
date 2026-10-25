/* spu_memory.h - the SPU2's sound memory, as the game addresses it.
 *
 * Sound banks were uploaded by address into SPU2 memory (pdipipe fno 4,
 * file_read_sound_bank) and voices were started at SPU addresses.  The
 * port's mixer (engine/sound/native) owns that memory; these are the entry
 * points the rest of the engine uses to fill it.
 *
 * PC: the memory is larger than the SPU2's 2 MB (spu_memory_size: 64 MB).
 * The game keeps the PS2's map in the first SPU_MEMORY_PS2_BYTES, with the
 * two reverb work areas (0x1D9020..0x1DFFFF, 0x1F9020..0x1FFFFF), and puts
 * a bank larger than its PS2 place in a window above it (game/sound T468,
 * sound_spu_window_alloc). */
#pragma once

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Copies size bytes to SPU address spuAddr (bytes, below spu_memory_size()). */
void spu_memory_write(u32 spuAddr, const void *data, u32 size);

/* PC: the size of the sound memory in bytes (64 MB), and the SPU2's. */
u32  spu_memory_size(void);
#define SPU_MEMORY_PS2_BYTES    0x200000

/* Diagnostics, not the PS2's (GT3PC_SND_TRACE, docs/sound.md 3.6): the
 * trace level (0: off), and the name of the upload about to fill
 * [spuAddr, spuAddr + size) (the bank's file; a no-op unless tracing). */
int  snd_trace_level(void);
void spu_memory_name(u32 spuAddr, u32 size, const char *name);

/* Not the PS2's: the voices still sounding that read [spuAddr, spuAddr +
 * size) (their block or start address there); T523's upload waits on it. */
int  spu_memory_voices_reading(u32 spuAddr, u32 size);

/* The EE side's trace (GT3PC_SND_TRACE=2): why a voice handle lost its
 * voice (the voice manager and GtSoundHandle note it, the handle's next
 * key-on reads it back once) and a rate-limited "snd: t=..." line. */
enum { SND_LOST_NONE = 0, SND_LOST_STOLEN, SND_LOST_ENDED, SND_LOST_STOP, SND_LOST_STOP_FADE };
void snd_trace_lost(const void *handle, int reason, int voice, int prio, int byPrio);
int  snd_trace_lost_reason(const void *handle, int *voice, int *prio, int *byPrio, int *framesAgo);
void snd_trace_line(const char *fmt, ...);

#ifdef __cplusplus
}
#endif
