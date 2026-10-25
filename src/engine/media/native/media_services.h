/* media_services.h - the native movie / IPU services of GT3 PC, as plain C
 * functions for the decompiled side (docs/movies.md).
 *
 * The decompiled engine keeps the game-facing movie code: mpegif (T529,
 * the feeder thread and the commands) and the G002 player's interface
 * (engine/media/mpeg_player_pc.cpp: the decoder thread, the input budget,
 * the 3-frame queue, on the game scheduler as on the EE).  What was
 * hardware or IOP is here, native:
 *   - gt3m_video_*: the MPEG-2 decoding the G002 player did with the IPU
 *     (BDEC / VDEC / FDEC, its MMI motion compensation, the CSC to RGB32 in
 *     16 x 16 blocks of the column-major frame stores);
 *   - gt3m_ipu_decode_image: IDEC of an IPU still (ipu_decode_image);
 *   - gt3m_mpg_*: the pdimpeg IOP module ('MPG1' open / feed / close, 'MPG2'
 *     fno 3): the PSS read from gt3.vol, its video handed out as 4 KB PES
 *     units, its sound played through the native BGM player
 *     (sound_native.h's push functions) and held to the presentation
 *     window.
 * No game kernel calls inside: the callers are game threads (one runs at a
 * time); the sound functions take their own short lock.  Plain C types
 * only (the decompiled side includes this under pc_prelude.h). */
#ifndef GT3PC_MEDIA_SERVICES_H
#define GT3PC_MEDIA_SERVICES_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the G002 player's decoder --------------------------------------- */

typedef struct Gt3mVideo Gt3mVideo;

Gt3mVideo *gt3m_video_create(void);
void gt3m_video_destroy(Gt3mVideo *v);
/* PC: the stream's width x height (mpeg_player_create's): an MP4 movie's
 * pictures are scaled to it (docs/movies.md "MP4 movies"). */
void gt3m_video_set_size(Gt3mVideo *v, int width, int height);
/* Elementary-stream bytes of one video PES packet (pts 90 kHz, -1 none), or
 * an MP4 movie's frame token (engine/media/native/mp4_token.h). */
void gt3m_video_push(Gt3mVideo *v, const void *data, int size, long long pts);
/* Bytes pushed and not decoded yet (an MP4's: 16 KB a frame, so the feeder
 * keeps about a second ahead). */
int  gt3m_video_buffered(const Gt3mVideo *v);
/* No more data: the last reference picture comes out. */
void gt3m_video_end_of_stream(Gt3mVideo *v);
/* Decodes up to the next picture in display order: 1 and its size in
 * *width / *height, or 0 when more data is needed; -1 (an MP4 movie) when
 * the frame its worker thread decodes is not ready yet: ask again in a
 * moment.  An MP4's frame is width x 2 height: two pictures, 1 / 59.94 s
 * apart, stacked (MenuBgMovie shows the halves on alternate ticks). */
int  gt3m_video_next(Gt3mVideo *v, int *width, int *height);
/* PC: the last frame out was an MP4's (its halves full pictures, not a
 * PSS's two fields: MenuBgMovie::upload, fieldOffset). */
int  gt3m_movie_progressive(void);
/* PC: the frames a second of the stream the last frame out came from (its
 * sequence header's frame rate: 29.97 / 30 the US and Japanese movies, 25
 * the PAL ones; an MP4's 29.97: its frames are two pictures 1 / 59.94 s
 * apart); 0 before the first.  MenuBgMovie::update paces by it. */
double gt3m_movie_frame_rate(void);
/* The picture next() returned, colour-converted as the IPU's CSC did
 * (RGB32, alpha 0x80) into dst in the frame queue's layout: 16 x 16
 * blocks of 0x400 bytes, columns of blocks (mpeg_player_on_picture).
 * Nothing is written past dstSize bytes. */
void gt3m_video_output_blocks(Gt3mVideo *v, void *dst, int dstSize);
/* sequence_end_codes decoded so far. */
int  gt3m_video_sequence_ends(const Gt3mVideo *v);
/* Errors (VLC / unsupported) so far, for the log. */
int  gt3m_video_errors(const Gt3mVideo *v);

/* ---- IPU stills -------------------------------------------------------- */

/* ipu_decode_image's result for the "ipum" picture at img: width x height
 * RGBA32 pixels in 16 x 16 blocks of 0x400 bytes, rows of blocks, alpha
 * 0x80, at dst.  Returns 0, or -1 (logged) on a damaged picture. */
int  gt3m_ipu_decode_image(void *dst, const void *img);
/* The same into dst of dstSize bytes: nothing is written past it (a
 * bigger picture is cut: -1, logged). */
int  gt3m_ipu_decode_image_n(void *dst, unsigned dstSize, const void *img);

/* ---- the 'MPG1' / 'MPG2' service (pdimpeg) ----------------------------- */

/* Opens sectors x 2048 bytes from sector lsn of gt3.vol (or from byte lsn
 * of the host file hostName when it is not empty).  mode 1: preload (the
 * video only, zeros after the end); 2: play with sound (the sound to the
 * BGM player, zeros after the end); 3: play without sound (after the end
 * one empty unit, then the stream again: the menu backgrounds loop). */
void gt3m_mpg_open(int mode, unsigned lsn, unsigned sectors, const char *hostName);
/* Fills buf with size / 4096 units, each one video PES packet at its start
 * (what mpeg_pes_parse reads), or zeros (no packet: the end).  Returns the
 * number of video packets handed out (0: nothing left). */
int  gt3m_mpg_feed(void *buf, int size);
void gt3m_mpg_close(void);
/* The presentation window [lo, hi] in microseconds of the movie
 * (MenuBgMovie::update, once per frame); (0, INT64_MAX) lets the sound run
 * free.  The sound starts with the first window and pauses while it is
 * ahead of hi. */
void gt3m_mpg_window(long long lo, long long hi);

#ifdef __cplusplus
}
#endif

#endif /* GT3PC_MEDIA_SERVICES_H */
