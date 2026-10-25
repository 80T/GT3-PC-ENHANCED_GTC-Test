/* pc_save_ext.h - PC (ROADMAP 18, no original): the save records past their
 * retail capacities, and the name-keyed lists of the GameStatus 8.0 stream.
 *
 * The save record classes keep their fixed arrays (their layouts are the
 * PS2's, checked by EGCS_SIZE; GameOptions is copied with memcpy): the
 * elements past them live here, on the heap, keyed by the owning object's
 * address and a tag.  Also here per (owner, tag): the entries of a keyed
 * list that a load could not place (names the current data lacks), written
 * back unchanged by the next save.
 *
 * A keyed list in the stream: u32 count, then count entries {u16 key length,
 * the key bytes (no NUL), the record (a fixed size per list)}; all unaligned,
 * little-endian. */
#ifndef GAME_SAVE_PC_SAVE_EXT_H
#define GAME_SAVE_PC_SAVE_EXT_H

#include "types.h"

/* The owners' tags. */
enum {
    PC_EXT_EVENTS = 1,                          /* EventResults: EventResult past e[382] */
    PC_EXT_BEST_TIMES,                          /* CourseBestTimes: BestTime past t[44] */
    PC_EXT_ARCADE,                              /* ArcadeRecords: ArcadeRecord past rec[144] */
    PC_EXT_FREE_RUN,                            /* FreeRunRecords: FreeRunTable past tables[44] */
    PC_EXT_PRIZE_SETS,                          /* PrizeSetFlags: s8 past won[200] */
    PC_EXT_MUSIC                                /* MusicPlayList: MusicEntry past entries[64] */
};

/* The heap bytes of (owner, tag), grown to at least `bytes' (the new bytes
 * zeroed); *oldBytes (if given) = the size before, so the caller can reset
 * the new elements.  The block moves when it grows.  NULL while empty. */
u8  *pc_ext_get(const void *owner, int tag, u32 bytes, u32 *oldBytes);
u32  pc_ext_size(const void *owner, int tag);
void pc_ext_resize(const void *owner, int tag, u32 bytes);    /* to exactly bytes (new ones zeroed) */
void pc_ext_copy(const void *dst, const void *src, int tag);  /* the bytes and the kept entries */
void pc_ext_release(const void *owner, int tag);              /* both, freed */

/* The kept entries of (owner, tag): their raw stream bytes and count. */
void      pc_kept_clear(const void *owner, int tag);
void      pc_kept_add(const void *owner, int tag, const u8 *entry, u32 bytes);
const u8 *pc_kept_get(const void *owner, int tag, u32 *count, u32 *bytes);

/* A stream being written; p NULL only counts the bytes (getDataSize). */
struct PcSaveWriter {
    u8  *p;
    u32  size;
};
void pc_put(PcSaveWriter *w, const void *src, u32 n);
void pc_put_u32(PcSaveWriter *w, u32 v);

/* A keyed list being written: pc_keyed_end appends the kept entries of
 * (owner, tag) and stores the count. */
struct PcKeyedList {
    PcSaveWriter *w;
    u8           *countAt;
    u32           count;
};
void pc_keyed_begin(PcKeyedList *l, PcSaveWriter *w);
void pc_keyed_put(PcKeyedList *l, const void *key, u32 keyLen, const void *rec, u32 recSize);
void pc_keyed_end(PcKeyedList *l, const void *owner, int tag);

/* Reads a keyed list of recSize-byte records at src (end: the data's end):
 * place(ctx, key, keyLen, rec) for each entry (key NUL-terminated; keys
 * over 0xFF bytes are not offered), 1 if it placed it; the others are kept
 * for (owner, tag), whose kept entries are cleared first (logged once per
 * list name `what').  The end of the list; NULL if it runs past end. */
typedef int (*PcKeyedPlace)(void *ctx, const char *key, u32 keyLen, const u8 *rec);
const u8 *pc_keyed_load(const u8 *src, const u8 *end, u32 recSize, const void *owner, int tag,
                        PcKeyedPlace place, void *ctx, const char *what);
/* The end of the keyed list at src, NULL if it runs past end (or src is
 * NULL / past end): a load checks the whole stream before it copies. */
const u8 *pc_keyed_skip(const u8 *src, const u8 *end, u32 recSize);

/* The 64-bit code of a music entry as a keyed list's key (8 bytes). */
enum { PC_KEY_CODE_BYTES = 8 };

/* The records of the keyed lists (bytes after the key). */
enum {
    PC_REC_EVENT     = 4,                       /* BestPlace */
    PC_REC_BEST_TIME = 4,                       /* BestTime */
    PC_REC_ARCADE    = 1 + 0x14,                /* the difficulty, ArcadeRecord */
    PC_REC_FREE_RUN  = 0x300,                   /* FreeRunTable */
    PC_REC_PRIZE_SET = 1,                       /* the flag */
    PC_REC_MUSIC     = 4                        /* played, enabled, order (s16) */
};

#endif /* GAME_SAVE_PC_SAVE_EXT_H */
