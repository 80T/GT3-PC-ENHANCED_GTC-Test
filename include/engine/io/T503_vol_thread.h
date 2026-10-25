/* T503_vol_thread.h - the volume reader thread (translation unit T503, unit u111),
 * 0x00240FA0..0x00241BF0.
 *
 * One request at a time, served by a thread (priority 4, 0x8000-byte stack)
 * that reads the file through the IOP pipe (T523) in 0x40000-byte pieces, or
 * inflates a packed one (GZIP::InflateLoader at priority 9), then hands the
 * result to every caller waiting for it.  T503 also owns vol_device_object
 * (its static constructor / destructor).  C++ only. */
#ifndef GAME_T503_VOL_THREAD_H
#define GAME_T503_VOL_THREAD_H

#include "game/game.h"

#ifdef __cplusplus

#include "engine/io/T510_vol_device.h"               /* VolDevice, held by value */

/* The gt3.vol device (mounted by vol_init). */
extern VolDevice vol_device_object;             /* 0x002F33B0 */

void  vol_thread_main(void *arg);               /* 0x002411E8 the reader thread */
/* Reads path into buf (NULL: memalign(64, size) here), waiting first for a
 * request in progress, then for this one.  unpack: inflate a packed (.gz)
 * file.  The buffer, NULL if cancelled and allocated here.
 * size: the bytes read (0: the whole file, its size rounded up to 64; a
 * bigger file is cut at size, logged once: "PC: <path> is N bytes, read
 * into M").  With buf and size 0 the whole file goes into buf, whatever
 * its size: new callers with a buffer of their own use
 * file_read_blocking_cap / _fit below.
 * PC: buf NULL + size 0 = a block of the file's size rounded up to 64 (the
 * unpacked size for a packed file: its .gz's ISIZE), memalign(64) from the
 * game heap (pdistd T532 heap_object), released with free() (the same
 * heap); the reader thread frees it itself only when the read is cancelled
 * (the result is NULL then).  NULL too if the heap is full (logged). */
void *file_read_blocking(const char *path, void *buf, int size, int unpack,
                         VolDevice *dev);       /* 0x002417C0 */
/* PC: file_read_blocking with buf's capacity (bytes).  buf != NULL: the file
 * (unpacked when unpack and it is packed) is read into buf, at most
 * capacity bytes: its size rounded up to 64, cut to capacity (a file bigger
 * than capacity is cut there, logged once: "PC: <path> is N bytes, read
 * into M"; nothing is written past buf + capacity, the inflate included);
 * the result is buf (NULL if cancelled).  buf NULL: as file_read_blocking
 * (path, NULL, 0, ...): a block of the file's size from the game heap, the
 * caller frees it with free(); capacity is not used. */
void *file_read_blocking_cap(const char *path, void *buf, int capacity, int unpack,
                             VolDevice *dev);
/* PC: as file_read_blocking_cap, but a file bigger than capacity is not cut:
 * it is read into a new block of its size (memalign(64), the game heap;
 * logged once: "PC: <path> is N bytes, more than its M-byte buffer ..."),
 * and that block is returned: *allocated = 1 (when allocated is not NULL),
 * the caller frees it with free() and buf stays the caller's, unused.
 * Otherwise the result is buf (*allocated = 0).  buf NULL: allocated as
 * file_read_blocking does (*allocated = 1). */
void *file_read_blocking_fit(const char *path, void *buf, int capacity, int unpack,
                             VolDevice *dev, int *allocated);
/* PC: the bytes file_read_blocking(path, NULL, 0, unpack, dev) allocates and
 * reads: the file's size (unpacked when unpack and it is packed: the .gz's
 * ISIZE) rounded up to 64.  Waits for a missing file as the reads do.  For
 * callers that size their own buffers. */
int   file_read_size(const char *path, int unpack, VolDevice *dev);
/* A sound bank (.ins "INST" / .es "ENGN") on the caller's thread: the header
 * into buf (NULL: memalign(64, size, or the header size if size is 0)), the
 * samples straight to SPU address spuAddr.  buf.
 * PC: a header bigger than size (buf's capacity) goes into a block of its
 * own (memalign(64), free() it; buf is left as it is), logged once.  The
 * samples' size (the header's spuSize) is not checked here: the SPU side
 * (pdipipe_read2 -> spu_memory_write) bounds it. */
void *file_read_sound_bank(const char *path, int spuAddr, void *buf, int size); /* 0x002418D0 */
/* Once: the disc sector cache (0x3180 bytes at 0x004A0CB8), mounts volName on
 * the disc (type 0), mc_manager_init, the reader thread. */
void  vol_init(const char *volName);            /* 0x00241A68 */
void  vol_shutdown(void);                       /* 0x00241B20 undoes vol_init */

/* 0x00241BA8 (.dtors 0x0028F278) / 0x00241BD0 (.ctors 0x0028F1A4): egcs's
 * static destructor and initialiser of vol_device_object (~VolDevice(2) /
 * VolDevice()), compiler-generated (T503 defines the object). */

#endif /* __cplusplus */

#endif /* GAME_T503_VOL_THREAD_H */
