/* harness.h - pssgame, the movie path of GT3 PC driven as the game drives
 * it, without the renderer: the decompiled side (movie_game.cpp: what
 * MenuBgMovie / play_movie do with mpegif, compiled like the game) and the
 * native side (pssgame.cpp: the platform, the volume, frame dumps) talk
 * through these plain C functions. */
#ifndef PSSGAME_HARNESS_H
#define PSSGAME_HARNESS_H

#ifdef __cplusplus
extern "C" {
#endif

/* native side */
int    hn_lookup(const char *path, int *lsn, unsigned *size);      /* gt3.vol's table: 1 found */
int    hn_read(const char *path, void **data, unsigned *size);     /* a whole file (malloc'ed) */
void   hn_dump_frame(const void *blocks, int w, int h, int columnMajor, const char *name);
unsigned hn_frame_crc(const void *blocks, int bytes);
double hn_now(void);
const char *hn_out_dir(void);

/* decompiled side: runs one scenario on the game thread; 0 = passed */
int    hg_run(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif
