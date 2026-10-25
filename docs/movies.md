# Movies and IPU pictures

How GT3 plays its PSS movies and decodes its IPU still pictures on the PS2,
what the files contain, and the native decoder that replaces the IPU path in
the port (`src/engine/media/native/`, test tool `tools/pssdump/`); and the
MP4 (H.264 / AAC) movies a mod can put beside them (section 5).

## 1. The PS2 side

### Who plays what

All movies go through one object, `MenuBgMovie g_002CDC10` (T253), which
wraps an `mpegif` stream (T529). A `MovieParam` (T000: file, width, height,
`field`, `withSound`, `oneShot`) describes a movie.

| Caller | Movie | Size | field | sound | once |
|---|---|---|---|---|---|
| `main` 0x00100140 (`play_opening`) via `play_movie` 0x001287A8 | `stream/opening.pss` | 640x448 | 1 | yes | yes |
| `arcade_menu` 0x001165D8 (`play_ending`), `F_0013F508` (T061) | `stream/ending_short_full.pss`, `ending_long_full.pss` | 640x448 | 0 | yes | yes |
| `GtmodeControl::runRootPage` 0x00119B60 | `stream/title_full.pss` | 640x448 | 1 | no | loop |
| `ArcadeControl::runTopPage` 0x00113540 (`g_00292930[0]`) | `stream/arcade_menu_full.pss` | 640x448 | 1 | no | loop |
| `ArcadeControl::runLevelPage` 0x00113870 (`[1]`) | `stream/level_menu_full.pss` | 640x448 | 1 | no | loop |
| class select (T022, `[2]`), course select (T025, `[4]`) | `stream/laguna_full.pss`, then `stream/<course>_full.pss` after 6 frames on a course (`courseMovieName` 0x0010E130) | 640x448 | 1 | no | loop |
| car select / i.LINK wait (T021, T023, T014, `[3]`, preloaded by `ArcadeControl` T010) | `stream/menu_bg2.pss` | 320x112 | 0 | no | loop |
| `runPushStartPage` 0x001156F8 (`[5]`), `[6]`, `[7]` | `stream/menu_bg.pss`, `arcade_top.pss`, `arcade_level.pss` | | | | |

The last row's files are **not on the US disc** (see open questions).
`play_movie` (T051) runs a movie full screen until it ends or pad 1 presses
start, then fades out 30 frames and calls `MenuBgMovie::stopAndWait`. The
menu backgrounds are drawn by `gt2k_menu::MpegImage` items (T249) whenever
`g_002CDC10.lastFrame` is set.

### The pipeline

```
 .pss on disc ──IOP module pdimpeg──► 32 KB blocks of 4 KB video PES packets ──► mpegif::feed (EE feeder thread)
   (audio kept on the IOP, played          (pdimpeg_open/_feed/_close, T522)         │ mpeg_player_put_pes, 4 KB each
    through the SPU2)                                                                ▼
 MenuBgMovie::update ◄── 3-frame RGBA queue ◄── IPU CSC ◄── mpeg_player decoder thread (G002) + IPU BDEC/VDEC/FDEC
   (one frame per 2 fields)                                                           ▲ toIPU DMA ring of 16 KB elements
```

* **mpegif** (T529, 0x0025E548..0x0025F390). `start` / `play` / `switchTo`
  start the feeder thread (`threadMain` 0x0025EE38, priority `prioFeed`
  10) or pass a command to `control` 0x0025E970 (PLAY/SWITCH = switch file,
  STOP, GET_STATE). `feed` 0x0025EF00 creates the player
  (`mpeg_player_create`), opens the IOP stream (`pdimpeg_open` mode 2 with
  sound, 3 without; mode 1 for `preload`) and hands every 4 KB of each
  32 KB block to `mpeg_player_put_pes`; a full ring puts the feeder to sleep
  until the decoder wakes it. A packet without payload is the end of the
  movie: from a preloaded copy (`preload` 0x0025E710, 5 MB, used for
  `menu_bg2.pss`) the feeder rewinds (looping); from the IOP stream a
  one-shot movie asks the decoder to stop (`g_dec_state = DEC_STOP_REQUEST`).
  A non-one-shot thread loops `feed` while `control` asks for a switch.
  `nextFrame` 0x0025EC10 / `releaseFrame` 0x0025EC50 read the frame queue
  once it has been full once (`g_frameq_filled_once`).
* **pdimpeg** (T522, IOP client): RPC 'MPG1' open / feed / close; 'MPG2'
  fno 3 `F_0025CFC0(lo, hi)` (NOWAIT) receives the *presentation window*.
  The IOP module itself is not in the EE image: what it does is inferred
  from what the EE gets: video PES packets each at the start of a 4 KB unit
  (the files' packets are 4096 / 4082 bytes behind 14-byte pack headers, so
  the module re-aligns them), and nothing of the audio.
* **mpeg_player** (G002, `mpeg_player.c` 0x0025F390..0x00263368, with
  `ipu.c` 0x00263688..0x00264E58). `mpeg_player_create` 0x0025F390 sets up a
  toIPU DMA ring (`mpeg_vibuf_*`: 32 elements of 16 KB up to 480 lines), a
  frame queue of 3 RGBA frames of width x height x 4 (`mpeg_frameq_*`) and
  the decoder thread (`mpeg_player_thread` 0x0025F5F8, priority
  `prioDecode` 11). `mpeg_video_decode_main` 0x00260998 parses the headers
  with the IPU's FDEC (`ipu_getbits` / `ipu_showbits`), the macroblock modes
  and vectors with VDEC (`ipu_vdec_*`), decodes each macroblock's blocks
  with BDEC (`ipu_bdec` 0x00263688: VLC, inverse quantisation, IDCT into
  s16[384] in the scratchpad), does the motion compensation in EE MMI code
  (`mpeg_mc_luma` 0x00261ED0, `mpeg_mc_chroma` 0x00262610: frame
  prediction only, first vector per direction) and adds the residual
  (`mpeg_mb_put_block` 0x00260D48). Frame stores are column-major arrays of
  0x180-byte macroblocks (Y 16x16, Cb 8x8, Cr 8x8). Pictures leave in
  display order (`mpeg_video_output_picture` 0x00260BD8: B at once, I/P when
  the next reference starts, the last at the sequence end) through
  `mpeg_player_on_picture` 0x0025F5A8, which converts the whole store with
  the IPU's CSC (`ipu_csc_rgb32` 0x00263938: RGB32, no dither, alpha 0x80)
  into the next queue slot. The player assumes a progressive sequence
  (`mpeg_video_setup_sequence` 0x00260AB8 sizes an interlaced one wrongly).
* **MenuBgMovie** (T253, 0x001CCBD0..0x001CD6A0). `open(w, h)` creates the
  mpegif (priorities 1 / 4 / 10 / 11); `start(MovieParam)` plays.
  `update` 0x001CD1E0 runs once per field from the render code: on the odd
  field (`PGL_FIELD_HALF_ODD`) it releases the previous frame, takes the next
  one and sends the IOP the presentation window
  `[t(n) - 1/2 frame, t(n+1) + 1/2 frame]`, t(n) = n * 2e9 / 59933 us (frame
  n at 29.97 Hz); with no frame ready it sends `(0, MAX)`. `upload` copies
  the frame (16x16 blocks of 0x400 bytes, **columns** of blocks: the frame
  store's order) into a GS buffer: whole (`uploadFrame`, bilinear) or, for
  `field = 1`, only the half the display is showing (`uploadField` 0x001CCEC0:
  the upper half on the odd field, the lower half on the even one; nearest
  filtering). `stopAndWait` 0x001CD558 opens the window, stops the stream and
  waits for the feeder to end.

### IPU still pictures

`stream/course.ipic` is GT3's only IPU picture file: an `IpicArchive`
(T250) loaded by `ArcadeControl::load` 0x00113F90 with the 42 course and
class stills shown by `loadCourseImage` 0x0010E240 and `showClassPicture`
0x00108988. `IpicArchive::find` points an `Ipic` at a picture,
`MpegImage::setIpic` 0x001CBFC8 allocates width x height x 4 bytes and
`Ipic::decode` 0x001CC6B8 calls `ipu_decode_image` 0x00264E58: IPU reset,
the whole picture DMA'd to the IPU, 17 bytes skipped with FDEC, `IPU_CTRL =
flags << 16` (DTD cleared), one `IDEC` (QSC 1, DTD = flags bit 2, RGB32, no
dither), the output drained by fromIPU DMA in chunks of 0x8C00 quadwords.
The output is 16x16 RGBA blocks in **rows** of blocks; `MpegImage::drawIpic`
uploads it block by block.

## 2. The formats

### PSS (all 51 movies)

MPEG-2 program stream (ISO 13818-1): 16 KB packs, each a 14-byte pack header
and four PES packets (video 0xE0: 4096 + 4096 + 4096 + 4082 bytes, an audio
packet in place of a video one where needed); one system header (0xBB) in
the first pack, a padding packet or two (0xBE), the program end code
(0xB9). Video PES headers carry a PTS on some packets (the first PTS of every
movie is 3995..12312); the video and audio first PTS are equal.

**Video** (checked on all 51 files, `pssdump check` and the header survey):

* MPEG-2 MP@ML (profile_and_level 0x48), 4:2:0, `progressive_sequence 1`,
  one sequence header (default intra matrix, a loaded non-intra matrix), one
  sequence end code; TMPGEnc b12c (user data).
* 640x448 (50 files) and 320x112 (`menu_bg2.pss`); frame_rate_code 4
  (29.97: opening, endings, title, class_dirt/class_s, laguna, midfield,
  r_rome, smtnorth, smtsouth, super) or 5 (30.000: the rest); about 9 Mbit/s.
* GOPs of about 18 pictures (I B B P..., M = 3); the first GOP is closed
  (and a few more at cuts: 32 in the opening, 5 in ending_long), the others
  open; I / P / B frame pictures only.
* Picture coding extension, everywhere: `picture_structure 3` (frame),
  `frame_pred_frame_dct 1` (so frame motion and frame DCT only),
  `q_scale_type 1`, `intra_vlc_format 1`, `alternate_scan 0`,
  `concealment_motion_vectors 0`, `top_field_first 1`,
  `repeat_first_field 0`, `progressive_frame 1`; `intra_dc_precision` 0, 1
  or 2 (8 / 9 / 10 bit) depending on the file.
* No motion vector reaches outside the picture (all 51); every slice is
  one macroblock row.

**Stacked fields.** The `field = 1` movies (opening, title, the course and
class previews, the arcade menu backgrounds) are interlaced video stored as
two 640x224 fields stacked in one 640x448 progressive frame: the upper half
is the top field (the even display lines), the lower half the bottom field,
the upper one first in time (`MenuBgMovie::uploadField` shows the upper half
on the field it takes the frame, the lower on the next). So these movies
are 640x224 at 59.94 fields a second. The parity was measured on the data:
on still content each lower-half line matches the average of upper lines i
and i + 1 (186 / 186 sampled frames of `title_full`, 72 / 72 of
`arcade_menu_full`). The endings (`field = 0`) and `menu_bg2.pss` are plain
progressive pictures.

**Audio** (opening and the two endings only): private stream 1 (0xBD); each
payload starts with a 4-byte sub-header `FF A1 00 00` (constant in all 6799
packets), then a continuous stream with the `.ads` BGM header:

```
"SShd" u32 0x18 | u32 codec 0x10 (SPU2 ADPCM) | u32 44100 | u32 channels 2 |
u32 interleave 0x400 | u32 loop start -1 | u32 loop end -1 | "SSbd" u32 data bytes
```

then 0x400 bytes of the left channel, 0x400 of the right, and so on: SPU2
ADPCM, 16-byte frames of 28 samples (byte 0 filter << 4 | shift, byte 1
flags, then 4-bit samples low nibble first). The data sizes are exact
multiples of 0x800 (opening 11374592 bytes = 225.69 s; ending_short 90.58 s;
ending_long 232.68 s). The opening's sound ends 1.07 s before its last
frame; ending_long's runs 8.1 s past its last frame (last video PTS
224.57 s).

### IPU pictures (`ipum`)

An `IpicArchive` is `"ipua"`, u32 count, count x {u32 offset, char
name[0x1C]}, then the pictures (at 64-byte aligned offsets). A picture:

| Offset | Field |
|---|---|
| 0x00 | `"ipum"` |
| 0x04 | u32 size: bytes from 0x10 to the end of the bitstream |
| 0x08 | s16 width, s16 height (640 x 448 for all 42) |
| 0x0C | u32 frames (1) |
| 0x10 | u8 flags: bits 0-1 intra_dc_precision, 2 DTD, 4 alternate scan, 5 intra VLC format, 6 q_scale_type, 7 MPEG-1 (GT3: 0x62 = 10-bit DC, B.15, non-linear scale) |
| 0x11 | the IDEC bitstream |

IDEC bitstream: per macroblock `macroblock_type` of an I picture (`1`, or
`01` followed by a 5-bit quantiser_scale_code), a `dct_type` bit when DTD
is set, six intra blocks (DC size + differential, AC run/level codes,
4:2:0), then a `macroblock_address_increment` (`1`) before the next one; the
picture ends with `0x000001B0` (then `0x000001B1`). IDEC starts with
quantiser_scale_code 1, the default intra matrix and the DC predictors at
128 << intra_dc_precision. Every macroblock is colour-converted and written
out in raster order. In GT3's file every row of macroblocks starts with a
quantiser change (28 or more per picture) and every picture ends exactly at
its `0x1B0` start code (fewer than 8 zero bits after the last macroblock).
The stills are progressive pictures (no stacked fields; the bottom 50 or so
lines are black).

### The IPU colour conversion

CSC / IDEC output (as documented in PCSX2's `yuv2rgb.cpp`), chroma taken per
2x2 block without interpolation:

```
lum = (0x95 * max(Y - 16, 0)) >> 6
R = clamp((lum + ((0xCC  * (Cr - 128)) >> 6) + 1) >> 1)
G = clamp((lum + ((-0x68 * (Cr - 128)) >> 6) + ((-0x32 * (Cb - 128)) >> 6) + 1) >> 1)
B = clamp((lum + ((0x102 * (Cb - 128)) >> 6) + 1) >> 1)
A = 0x80   (thresholds 0 / 0, set by ipu_init)
```

## 3. The native decoder

`src/engine/media/native/` - plain C++17, no Windows or game dependency, no
threads, no third-party code. `sources.cmake` lists the files
(`GT3_MEDIA_NATIVE_SOURCES`; the port's glue: `GT3_MEDIA_SERVICE_SOURCES`).

| File | What |
|---|---|
| `pss_demux.h/.cpp` | Program-stream demultiplexer over a `ByteSource` (read + optional rewind callbacks; memory and `FILE*` sources provided). Returns PES packets with PTS / DTS; strips the Sony audio sub-header. |
| `mpeg2_video.h/.cpp` | MPEG-2 video decoder: push elementary-stream bytes (with PTS), get pictures in display order as YCbCr 4:2:0 planes. Three frame stores, display reordering, PTS per picture, sequence end / looping, error concealment, statistics. |
| `mpeg2_block.h` | Shared block decoding: DC, run/level (B.14 / B.15), scans, inverse quantisation, saturation, mismatch control (or MPEG-1 escapes / oddification for the IPU's MP1 flag). |
| `mpeg2_tables.h/.cpp` | The Annex B VLC tables written as code strings and built into lookup tables (with a self-test of their code space). |
| `mpeg2_idct.h/.cpp` | Separable float IDCT (even/odd decomposition), reference rounding floor(x + 0.5); SSE path bit-identical to the scalar one. |
| `ipu_csc.h/.cpp` | The IPU colour conversion to RGBA8 (SSE2 + scalar, both exact); conversion to / from the PS2's 16x16-block layouts. |
| `ipu_image.h/.cpp` | `ipum` IDEC decoder to RGBA8 (linear or the IPU's block layout); `ipua` archive reader. |
| `pss_audio.h/.cpp` | SShd / SSbd parser and SPU2 ADPCM (and 16-bit PCM) decoder to interleaved 16-bit PCM. |
| `movie.h/.cpp` | `gt3media::Movie`: the whole movie (demux + video + audio + CSC), looping. |
| `media_services.h/.cpp`, `mpg_service.cpp` | The port's glue, plain C functions for the decompiled side (section 4): the G002 player's decoding (`gt3m_video_*`), the IPU stills (`gt3m_ipu_decode_image`), the pdimpeg service (`gt3m_mpg_*`). Need the platform layer and the native sound (`GT3_MEDIA_SERVICE_SOURCES`). |

Decoder scope: MPEG-2 MP@ML frame pictures (I / P / B), frame and field
motion compensation in frame pictures, frame and field DCT, both VLC tables,
both scans, both quantiser scales, 8..11-bit DC, loaded matrices, quant
matrix extension, concealment vectors (parsed). Not supported (counted in
the stats, the picture skipped or the macroblocks concealed): field
pictures, dual prime, MPEG-1 video, 4:2:2. None of these occur in GT3, and
the PS2 player could not play them either. Where the PS2 player departs from
ISO 13818-2 the native decoder follows the standard (the stream was encoded
against it): chroma vectors are `vector / 2` truncated toward zero (the PS2
shifts), mismatch control is applied, lost macroblocks are concealed.

### API

```cpp
#include "engine/media/native/movie.h"
using namespace gt3media;

ByteSource src;                 // read(user, dst, size) -> bytes (0 = end); rewind(user) -> bool
src.user = myFile; src.read = myRead; src.rewind = myRewind;

MovieOptions opt;
opt.decodeAudio = withSound;    // mpegif's withSound
opt.loop = !oneShot;            // menu backgrounds: rewind at the end
opt.alpha = 0x80;               // the IPU's alpha (0xFF for a plain image)
opt.audioSink = pushToAds;      // optional: raw .ads bytes to the sound system instead of readAudio
opt.audioSinkUser = ctx;

Movie movie;
if (!movie.open(src, opt)) { /* not a movie */ }
const MovieInfo &in = movie.info();     // width, height, frameRate, hasAudio, audio.{sampleRate, channels}

MovieFrame f;
while (movie.nextFrame(f, rgbaDst, rgbaStride)) {   // or nextFrame(f): internal RGBA buffer
    // f.rgba (RGBA8), f.picture.{y, cb, cr, yStride, cStride} (YCbCr planes),
    // f.index (display order), f.time = index / frameRate, f.picture.pts, f.loop (pass)
}
size_t got = movie.readAudio(pcm, frames);   // interleaved s16, in.audio.channels per frame
```

IPU pictures:

```cpp
#include "engine/media/native/ipu_image.h"
IpuImageInfo info;
ipu_image_info(hdr, size, &info);                       // "ipum" header
ipu_image_decode(hdr, size, dst, 0, IpuLayout::Linear); // RGBA8 rows
ipu_image_decode(hdr, SIZE_MAX, dst, 0, IpuLayout::Macroblocks, kIpuAlpha); // exactly ipu_decode_image's output
```

Lower level: `PssDemuxer`, `Mpeg2VideoDecoder` (push / next / setEndOfStream),
`PssAudioDecoder`, `ipu_csc_rgba`, `rgba_to_ps2_blocks` /
`ps2_blocks_to_rgba` (columnMajor = the movie queue's layout, rows = IDEC's).

A `VideoPicture`, the RGBA buffer and `MovieFrame` stay valid until the next
call into the decoder. `Movie` is not thread-safe; run it on one thread.

### Verification (`tools/pssdump`)

* `pssdump selftest`: code-space check of every VLC table; IDCT against a
  double-precision reference on 60000 random blocks (peak error 1, worst
  per-position mean square error 0.0026, IEEE 1180 limits 1 / 0.06); the
  CSC (SIMD path against the formula, every pixel of a random picture);
  ADPCM.
* `pssdump check stream/*.pss`: all 51 movies decode completely: frames =
  pictures in the stream (opening 6796, ending_long 6730, ending_short
  2735, ...), no VLC error, no concealed macroblock, every slice consumed
  exactly (fewer than 8 zero bits before the next start code, ending at the
  end of a row), PTS increasing, no demux resync; audio 9952768 / 3994368 /
  10260992 sample frames, the exact SSbd sizes. The SSE2 and scalar builds
  give identical checksums over all decoded planes.
* `pssdump ipic course.ipic`: all 42 pictures, 1120 macroblocks each,
  ending exactly at `0x000001B0`.
* Pictures looked at (`pssdump decode --out DIR --field`): opening, endings,
  title, arcade menu, menu_bg2, the course stills.
* Drift test (`--yraw` and a script): mean luma steps into I frames show no
  consistent sign over six movies (+0.17 .. -0.07 levels), while a decoder
  with the half-pel rounding deliberately broken shifts it by +0.25: no
  systematic reconstruction bias.
* Sound: spectrum and L/R correlation (0.78) of music, no clipping to speak
  of; exact sample counts; the decoder's PCM is bit-identical to an
  independent (Python) decode of the raw `.ads` image the audio sink
  delivers (40 + SSbd bytes, e.g. 11374632 for the opening).
* `pssdump play`: a simulated 59.94 Hz loop (sound pulled per field, a frame
  every second field): no under-run, at most 0.1 s of decoded sound
  buffered, video and sound durations as above.
* Damaged input (truncated, 200..1600 random bytes overwritten, garbage):
  no crash, errors counted and concealed.

Speed (this machine, one core, 32-bit RelWithDebInfo): 640x448 at 750..880
frames a second with RGBA conversion and sound (about 4 % of one core at
29.97 fps; 28x real time over all 51 movies); 320x112 at 3700 fps; a course
still in 2..5 ms.

## 4. Integration in the port

The game code above the player stays as decompiled: `MenuBgMovie` (T253),
`play_movie` (T051), `MpegImage` / `Ipic` (T249 / T250), the menus, and
`mpegif` (T529, the game-facing class: feeder thread, commands, preload)
with one `/* PC: */` change. Below it, what was hardware or IOP is native.

| PS2 | Port | Kind |
|---|---|---|
| `mpegif` (T529) | `src/engine/media/T529_mpegif.cpp`, unchanged but for missing files (below) | decompiled |
| G002 player interface: `mpeg_player_*`, the 3-slot frame queue `mpeg_frameq_*`, `mpeg_pes_parse`, `mpeg_vibuf_set_geometry` / `_get_size`, `g_dec_state`, `mpeg_dma_alloc` / `_free`, `ipu_decode_image` | `src/engine/media/mpeg_player_pc.cpp` (replaces `mpeg_player.c`, `ipu.c`, `ipu_image.c`, `dma_util.c`) | decompiled side (prelude) |
| the IPU's BDEC / VDEC / FDEC / CSC / IDEC, the MMI motion compensation | `gt3m_video_*`, `gt3m_ipu_decode_image` (`native/media_services.cpp` over the decoder) | native (`gt3_media`) |
| pdimpeg client (T522) | `src/engine/pdi/T522_pdimpeg.cpp`: each function calls the native service | decompiled side |
| the pdimpeg IOP module ('MPG1' / 'MPG2') | `gt3m_mpg_*` (`native/mpg_service.cpp`) | native (`gt3_media`) |
| the movie's sound on the IOP (BGM stream voices) | `snd_native_bgm_open_push` / `_push` / `_push_end` / `_stop`, `_pause` / `_resume`, `_played_us` (sound_native.h) | native (sound area) |

`src/CMakeLists.txt`: `mpeg_player_pc.cpp` and `T522_pdimpeg.cpp` are in
`gt3_engine` (the decompiled code's options); the decoder and the services
(`sources.cmake`: `GT3_MEDIA_NATIVE_SOURCES`, `GT3_MEDIA_SERVICE_SOURCES`)
are the static library `gt3_media` (no prelude), linked into `gt3pc`.

### The player (`mpeg_player_pc.cpp`)

The same objects and semantics as G002, on the game scheduler:

* `mpeg_player_create` starts the decoder thread as a **game thread** at
  mpegif's `prioDecode` (11) through the kernel API (CreateThread /
  StartThread), as the original did; one player at a time (the PS2 kept it
  in the scratchpad). The native decoding runs on that thread between
  kernel calls (about 1.3 ms a 640x448 picture), so the more urgent game
  threads (render 7, main 9, feeder 10) take over at its next call.
* The input: `mpeg_player_put_pes` hands the payload of each 4 KB video PES
  unit straight to the decoder's buffer, within the ring's budget (ring
  size less two elements: 480 KB at 640x448); over it, `PUT_PES_NOT_READY`
  and the feeder sleeps (`g_mpeg_wake_thread`) until the decoder wakes it,
  as the IPU's input callback did. The decoder wakes the feeder after each
  picture and when it runs dry, and then waits on a semaphore that
  `put_pes` signals.
* Pictures in display order go through the IPU's CSC (RGB32, alpha 0x80)
  into the next free slot, in the frame stores' column-major 16 x 16 block
  layout, so `MenuBgMovie::uploadFrame` / `uploadField` work unchanged.
  The queue (`mpeg_frameq_*`) is the original's code, its memory from the
  game heap (`mpeg_dma_alloc`); `g_frameq_filled_once`, DEC_RUNNING after
  the first picture, the reset (zeros) when the decoder ends: as before.
* Stops: `control()` aborts the queue and sets DEC_STOP_REQUEST: the decoder
  stops after its current picture. The end of a one-shot movie (`feed`
  sets DEC_STOP_REQUEST at the stream's empty unit, the queue not
  aborted): the PS2 decoder stopped after its current picture too, so the
  last ~0.4 s in its input ring were never shown; here it first decodes
  what it was given, up to the sequence end, so the movie plays to its end
  (its last two queued frames are cleared by the queue reset, as before).

### The 'MPG1' / 'MPG2' service (`mpg_service.cpp`)

* `pdimpeg_open(mode, lsn, sectors, host)`: the PSS from sector lsn of
  gt3.vol (`platform/disc.h`), or from byte lsn of the host file, read on
  the calling game thread (as T519 / T523 read).
* `pdimpeg_feed(buf, 32 KB)`: eight 4 KB units, each the next video PES
  packet as it is in the file (a packet over 4 KB would be split with
  synthetic headers; GT3 has none). At the end: one empty unit (the end
  marker `feed` reacts to), then mode 1 (preload) and 2 (with sound) give
  empty units; mode 3 (the menu backgrounds) starts the stream again: on
  the PS2 the menu backgrounds read from the disc (title, course and class
  previews) loop too, not only the preloaded `menu_bg2.pss`.
* Timing: the RPC blocked the feeder while the IOP worked, and
  `mpegif::feed` relies on it: after a one-shot movie's end marker it keeps
  reading empty blocks until the decoder - a lower-priority thread (11
  against 10) - has finished. The native read returns at once, so a feed
  that has nothing left waits 4 ms through the kernel (`DelayThread`, in
  T522); without it the feeder spins and the decoder never finishes.
* The sound (mode 2): `snd_native_bgm_open_push` at open (the movie's sound
  takes the BGM voices, as on the IOP); the private stream 1 payloads after
  their 4-byte prefix are queued and pushed into the BGM player's FIFOs as
  they take them; `snd_native_bgm_push_end` after the last; `_stop` at
  close.
* `F_0025CFC0(lo, hi)` (MenuBgMovie::update, once a frame from the second
  frame on): the presentation gate. The sound starts with the first window;
  when it has played past `hi` it is paused until a window passes it again;
  `(0, MAX)` resumes it.

### Frames on screen

Not changed: `MenuBgMovie::upload` (T253) and `MpegImage::drawIpic` (T249)
upload the frames block by block with `gs_upload_image`, as the PS2 did;
that path and `gs_get_field_mode` belong to the renderer (docs/graphics.md
8.23: the registry's per-macroblock form). Once the renderer exists, one
whole-picture upload (a `/* PC: */` change in T253 / T249; the decoder can
write linear RGBA as well) is the faster path. Field movies: T253 already
uploads the half of the frame that belongs to the field being shown (the
upper half on the odd field, the lower on the even), so with fields
presented at 59.94 Hz they are bobbed as on the PS2. The halves are the top
and bottom fields (measured, section 2): for a steady picture the renderer
should place the bottom field one output line (half a field line) lower
than the top field, as the interlaced display did - a property of all
field-mode output, not only of the movies.  Done in the progressive pass
(roadmap item 3; renderer.md, "Fields"): T253 keeps its own field
(`s_moviePhase`) instead of the display's, and `MpegImage::movieUV` samples
the upper field a quarter field line lower and the lower one a quarter
higher (bilinear), each on its own lines.

**The PAL movies** (the Europe disc, docs/languages.md): every one is 25
frames a second (`frame_rate_code` 3; 640 x 512, `menu_bg2.pss` 320 x 112),
the field movies 50 fields. The PS2 took a frame every other field at the
console's own field rate - 50 Hz on a PAL console - and the updates here
come at 59.94 Hz, which played them 6/5 too fast (user report, 2026-10-24).
`MenuBgMovie::update` paces a stream of another rate than 29.97 / 30 by its
own (`gt3m_movie_frame_rate`: the sequence header's rate of the last frame
out; an MP4's frames count as 29.97): from its first frame, update k shows
the stream's field `k x 2 rate / 59.94` - the frame taken when it is past
the one shown, the half (`s_moviePhase`) by the field's parity, so a 25 fps
field movie shows each field 1.2 updates on average and its frames repeat
every fifth - and the presentation window follows the stream's time (`n /
rate`), so the sound stays with the pictures. The 29.97 / 30 movies (the US
and Japanese discs', the MP4s) keep retail's cadence.

### Missing files

`file_resolve_retry` (T006) waits for a missing file forever. mpegif's
three calls of it (`preload`, `startThread`, the switch in `threadMain`) now
look the file up once (`file_locate`); a missing movie ends the stream at
once (state `MPEGIF_ENDED`, no thread), a missing preload leaves no copy.
`stream/menu_bg.pss` (`runAttract` -> `runPushStartPage`; `runAttract` has
no callers), `arcade_top.pss` and `arcade_level.pss` (unused entries of
`g_00292930`) are the US disc's missing ones; `MenuBgMovie::start`'s
enhanced first-frame wait then costs 2 s.

### Tests (`tools/pssdump/harness`: `pssgame`)

The program does not link yet (the renderer and others), so `pssgame`
drives the same code: the port's T529, `mpeg_player_pc.cpp` and T522
compiled with the decompiled code's options, on the platform's scheduler,
kernel, volume and WASAPI sound output, field by field at 59.94 Hz with
MenuBgMovie's priorities and its `update` / `play` / `start` /
`stopAndWait` logic (a vblank interrupt wakes the "render" thread). It checks
the frame cadence (a new frame on every odd field), repeated frames, the
sound position against the picture, and dumps frames.

| Scenario | Result |
|---|---|
| `play stream/opening.pss 25 1 1 640 448` (with sound, skipped after 25 s) | 749 frames in 25.0 s, none missed; sound - picture -14 .. -41 ms (mean -25: the sound starts with the first window, on the second frame); stopAndWait 16 ms |
| `play stream/opening.pss 300 1 1 640 448` (to its end) | ends by itself at 226.69 s: 6796 pictures decoded, 6794 shown, none missed; all 11374632 sound bytes pushed; stopAndWait at once |
| `play stream/ending_short_full.pss 120 1 1 640 448` (to its end) | ends at 91.19 s: 2735 decoded, 2733 shown, none missed, all sound pushed (its last 0.7 s are silent: the track is shorter) |
| `play stream/arcade_full.pss 20 0 1 640 448` (one-shot, no sound) | ends by itself at 11.94 s, all 360 pictures decoded once, 358 shown |
| `play stream/title_full.pss 45 0 0 640 448` (menu background from the disc) | loops: 1349 frames in 45 s, none missed |
| `switch stream/laguna_full.pss stream/rome_full.pss 10` (course preview switch) | 299 frames in the 10 s after the switch, the new movie |
| `preload stream/menu_bg2.pss 30 320 112` (arcade menu, from memory) | 899 frames in 30 s across the loop points, none missed |
| `missing stream/menu_bg.pss` | `MPEGIF_ENDED` at once, stopAndWait returns |
| `ipic` | the 42 stills through `ipu_decode_image`, correct in the block layout |

Run from `pc_enhanced` (it lists gt3.vol with `tools/gt3vol.py`):
`build\tools\pssdump\pssgame.exe --out DIR SCENARIO ...`.

## 5. MP4 movies (modding)

User, 2026-10-24: standard H.264 video for mods, beside the PSS movies (not
in their place: the game's own stay PSS, decoded natively as above).

**Using one.** Put `<name>.mp4` beside `<name>.pss` in the data folder (for
example `data\stream\title_full.mp4`): it plays wherever the game plays
that movie (`mpg_service.cpp` looks for it when it opens the `.pss`). An
`.mp4` alone works too (T529's `mpegif_locate` tries it when the `.pss` is
missing). The file: an MP4 (`moov` / `mvhd` box) with an H.264 video track
and, for the movies with sound (the opening, the endings), an AAC audio
track, any size, frame rate and pixel aspect Windows' decoders take
(Media Foundation: Windows' own H.264 and AAC decoders, no third-party
code). One that is not an MP4 gives way to the `.pss` beside it (logged).
The log's lines: `movie: MP4 <path> in place of <name> (mode, seconds,
frames)`, the sound's rate and channels, `<path>: W x H pictures (pixel
aspect ...)`, `MP4 stream closed`.

**Pictures.** The game takes a frame every other tick (59.94 Hz:
`MenuBgMovie::update`, frame n at n x 2e9 / 59933 us) and shows a frame's
upper half on one tick, its lower half on the next - a PSS's two fields.
An MP4's frame is two whole pictures stacked: the movie at frame n's time
above and 1 / 59.94 s later below, so it plays at up to 59.94 pictures a
second at the slot's full height (`MenuBgMovie::upload` treats it so
whatever its `MovieParam.field`, and `fieldOffset` gives no field line
offset). Each picture is the source picture shown at its time (a 59.94 /
60 fps movie gives every one, a 30 fps one each twice, a 24 fps one some
three times), scaled into the movie's slot - the size the game asks for,
640 x 448 (or 320 x 112 for `menu_bg2`) - with its aspect kept: the slot
counts as the PS2's pixels (640 x 448 shown at 4:3), so a 16:9 movie gets
black bars above and below (an area average down, linear up). The slot
size is the GS model's (the texture at most 1024 wide, 4 MB of VRAM): a
higher-resolution playback would need the renderer to take the movie's
picture directly.

**How.** The MP4 keeps the PSS path - mpegif's feeder, end marker, loops,
preloaded copy, stops and switches, the player's thread, budget and frame
queue - with a token per frame in each video PES unit (`mp4_token.h`:
frame index, count, loop flag, path). The player's decoder takes the frame
a token names from an `Mp4FrameWorker` (`mp4_frames.*`): a native thread of
its own (not a game thread - a 1080p picture takes 5 .. 20 ms to decode and
scale, and must not hold the game's threads) decoding a few frames ahead
through `Mp4VideoReader` (`mp4_movie.*`: Media Foundation's source reader
with its video processor, 32-bit pixels, cropped to the display aperture).
While a frame is not ready `gt3m_video_next` answers -1 and the decoder
thread waits 2 ms through the kernel (`DelayThread`). A pending token counts
16 KB against the player's input budget, so the feeder keeps about a second
of frames ahead, as a PSS's ring held some 0.4 s: tiny tokens let it reach
a one-shot movie's end marker at once, after which `mpegif::control`
refuses a stop until the decoder has finished (`g_dec_state` is no longer
DEC_RUNNING) - a skipped movie played on to its end, sound and all (user
report 2026-10-24). A jump (a switch) or
the loop restarts the decoding there (the reader seeks); a movie that does
not decode gives black frames, so its stream still ends.

**Sound.** Not re-encoded to SPU2 ADPCM: `Mp4AudioReader` decodes the AAC
to 16-bit PCM at its own rate (more than two channels: the front pair and
the centre), which `snd_native_pcm_*` (sound_native.h) mixes into the
output after the SPU2, resampled to 48 kHz (4-point cubic), at the gain the
BGM voices would give a `.pss`'s sound (the BGM volume, core 1's master
volume). It is decoded about a second ahead (from the feed and the
presentation window calls), starts with the first window and is held to
the pictures as a `.pss`'s (paused while it has played past the window).

**Checked** (a scratch test, encoding a 2 s 1280 x 720 60 fps H.264 / AAC
clip with Media Foundation's sink writer): 120 frames read in order,
top-down, B G R; the duration from `mvhd`; seeking; the worker's 61 frames,
16:9 letterboxed into 640 x 448 (bars at alpha 0x80), each half the source
picture at its time; the sound 44.1 kHz stereo, its length and level.

## 6. Open questions

* **IPU IDCT.** The IPU's exact IDCT arithmetic is unknown (PCSX2 uses
  libmpeg2's integer IDCT). The native one is IEEE 1180-accurate; the
  pictures can differ from the PS2's by a level here and there.
* **The audio sub-header** `FF A1 00 00`: its meaning (a Sony stream id?) is
  not known; the demultiplexer strips 4 bytes when the first is 0xFF.
* **pdimpeg on the IOP** (not in the EE image): how it gates the sound with
  the presentation window, whether it loops a streamed (not preloaded)
  `oneShot = 0` movie by itself, and whether the 8.1 s of sound after
  ending_long's last frame were heard on the PS2 (`play_movie` fades out and
  stops once the video has ended).
* **IDEC details not exercised by GT3:** the initial QSC with the linear
  scale (taken as a code, scale 2; PCSX2 uses 1), and the quantiser matrix
  with the alternate scan (the standard's natural-order weighting is used;
  PCSX2 indexes the matrix by scan position).
* **Field parity** was measured from the content, not from the hardware;
  if the PS2's GS shows the halves the other way round, swap the bob
  offsets.
