/* pc_still.h - PC: the arcade menus' course and class stills (docs/movies.md
 * "Stills").  No original: new PC code.
 *
 * Retail's are the pictures of stream/course.ipic (T250).  Here a still is, in
 * order: its PNG in memory (engine/media/native/still_cache.h: the first
 * picture of the .mp4 that plays for it, drawn in the movie's shape, else
 * stream/course/<name>.png of the data folder or a mod, any size, filling its
 * item as the disc's did), else that file read now (one that memory had no room
 * for, or that came after the start), else the archive's picture of that name,
 * an install the launcher has not converted (the callers' Ipic path).
 *
 * A still is a host image (engine/pgl/native/pgl_bridge.h): no GS VRAM, so its
 * size is the file's.  One a slot, for the two items that show them. */
#ifndef GAME_APP_PC_STILL_H
#define GAME_APP_PC_STILL_H

#include "game/game.h"

enum {
    PC_STILL_COURSE,
    PC_STILL_CLASS,
    PC_STILLS,
    PC_STILL_SIDE = 1024                    /* the GS texels of the picture, whatever its pixels */
};

/* Makes slot's still the one called `name': 1, or 0 when there is none.  On a
 * game thread. */
int pc_still_load(int slot, const char *name);
/* The TBP0 of slot's still (its TEX0 is PSMCT32, TBW 0, of a PC_STILL_SIDE
 * square). */
u32 pc_still_tbp(int slot);
/* The shape it is shown in, 0: fills its item. */
f32 pc_still_aspect(int slot);

#endif /* GAME_APP_PC_STILL_H */
