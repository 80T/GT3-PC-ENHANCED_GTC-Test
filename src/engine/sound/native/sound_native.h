/* sound_native.h - GT3's IOP sound services as native functions.
 *
 * On the PS2 the EE side of the sound (engine/sound: the voice manager and
 * sequencer; engine/pdi: the RPC clients) sent its commands to IOP modules:
 *   pdispu2.irx  'SPUP'  the SPU2 state block once a frame, status, output mode
 *                'SPUV'  EE <-> SPU RAM transfers (no callers in GT3)
 *   pdistr.irx   'PDIP'  file streaming; function 4 reads file data into SPU RAM
 *                        (the sound banks' samples, file_read_sound_bank)
 *                'PBGM'  the streamed BGM (.ads)
 * and those drove the SPU2 through libsd.  This module is their native
 * replacement: the same commands as C functions, executed against the
 * native SPU2 model (spu2.h) and BGM player (ads_stream.h), with the IOP
 * drivers' logic kept (the voice-active mask, key-on ordering, the BGM
 * voice setup, volume chain and clock).  docs/sound.md has the command ->
 * function table.
 *
 * Threads: every function may be called from any thread; the game thread's
 * commands and the audio thread's render are serialised by one lock (the
 * render holds it for one device period).  The BGM file is read by a feeder
 * thread of the BGM player, through the caller's read callback.
 *
 * PC (docs/sound.md 3.4, "The engine"): scheduled, the state blocks are
 * queued with their vblank's time and applied by the audio thread at the
 * sample that time maps to (snd_native_set_scheduled, snd_native_render);
 * the voice pool is 126 EE voices (the 46 of the PS2 and 80 more,
 * SND_NATIVE_EXTRA_VOICES); the mix has headroom (no 16-bit clamps but the
 * reverb's input: float out, a limiter after).
 *
 *
 * Plain C / C++ interface, no Windows or game dependency. */
#ifndef GT3PC_SOUND_NATIVE_H
#define GT3PC_SOUND_NATIVE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the SPU2 state block ('SPUP' function 4) ----
 * Binary identical to the EE's SpuState (engine/pdi/T520_pdispu2.h, 0x350
 * bytes): engine code passes its block as is. */
typedef struct SndNativeVoiceState {
    int16_t  flags;         /* 0x00 SND_NATIVE_V_* */
    int16_t  pitch;         /* 0x02 SPU2 pitch (0x1000 = 48 kHz) */
    uint16_t volL;          /* 0x04 fixed-mode volume, 15 bits */
    uint16_t volR;          /* 0x06 */
    int32_t  addr;          /* 0x08 start address in SPU RAM, bytes */
    uint16_t adsr1;         /* 0x0C */
    uint16_t adsr2;         /* 0x0E */
} SndNativeVoiceState;

typedef struct SndNativeEffectAttr {   /* sceSdEffectAttr */
    int32_t core;           /* 0x00 */
    int32_t mode;           /* 0x04 low byte: the effect mode; 0x100: clear the work area */
    int16_t depthL;         /* 0x08 */
    int16_t depthR;         /* 0x0A */
    int32_t delay;          /* 0x0C */
    int32_t feedback;       /* 0x10 */
} SndNativeEffectAttr;

typedef struct SndNativeCoreState {
    uint16_t unk_00;        /* 0x00 0x3FFF (not sent: the IOP sets MVOL itself) */
    uint16_t unk_02;        /* 0x02 */
    uint16_t evolL;         /* 0x04 effect (reverb) volume, EVOLL */
    uint16_t evolR;         /* 0x06 EVOLR */
    uint8_t  effectEnable;  /* 0x08 core attribute EFFECT_ENABLE */
    uint8_t  pad_09[3];     /* 0x09 */
    uint32_t mmix;          /* 0x0C the core's MMIX (0xFC0 / 0xFCC) */
    SndNativeEffectAttr effect;     /* 0x10 applied when the block is dirty */
} SndNativeCoreState;

typedef struct SndNativeState {
    SndNativeVoiceState voice[48];  /* 0x000 core 0 voices 0..23, core 1 voices 0..23 */
    SndNativeCoreState  core[2];    /* 0x300 */
    uint16_t unk_348;               /* 0x348 (not sent) */
    uint16_t unk_34A;               /* 0x34A */
    int32_t  dirty;                 /* 0x34C the effect attributes changed */
} SndNativeState;

/* SndNativeVoiceState.flags */
enum {
    SND_NATIVE_V_KEY_OFF  = 0x001,
    SND_NATIVE_V_KEY_ON   = 0x002,
    SND_NATIVE_V_PMON     = 0x004,  /* pitch modulation by the previous voice */
    SND_NATIVE_V_NOISE    = 0x008,  /* noise (not modelled; GT3 never sets it) */
    SND_NATIVE_V_DRY_L    = 0x010,  /* VMIXL */
    SND_NATIVE_V_WET_L    = 0x020,  /* VMIXEL: reverb send */
    SND_NATIVE_V_DRY_R    = 0x040,  /* VMIXR */
    SND_NATIVE_V_WET_R    = 0x080,  /* VMIXER */
    SND_NATIVE_V_ONE_SHOT = 0x100,  /* the voice ends at its sample's end (ENDX) */
    SND_NATIVE_V_PITCH    = 0x200,  /* set the pitch */
    SND_NATIVE_V_VOLUME   = 0x400,  /* set the volumes */
    SND_NATIVE_V_ADSR     = 0x800   /* set the start address and the ADSR */
};

/* ---- the BGM status ('PBGM' function 6), binary identical to the EE's
 * BgmStatus (engine/pdi/pdi_internal.h, 0x40 bytes) ---- */
typedef struct SndNativeBgmStatus {
    int64_t time;           /* 0x00 stream position, microseconds */
    int32_t pos;            /* 0x08 samples played (chunk granularity) */
    int32_t unk_0C;         /* 0x0C (the high word of the IOP's 64-bit pos) */
    int32_t id;             /* 0x10 loops completed */
    int32_t ended;          /* 0x14 the song reached its end */
    uint8_t unk_18[0x28];   /* 0x18 */
} SndNativeBgmStatus;

/* Reads up to size bytes at byte offset `offset' of the .ads file: the
 * number of bytes read (0 at the end, < 0 on error).  Called on the BGM
 * feeder thread. */
typedef int (*SndNativeReadFn)(void *user, uint32_t offset, void *dst, uint32_t size);

/* ---- life cycle ---- */
/* pdispu2's start-up (sceSdInit and its register setup: master volumes,
 * reverb work areas, mixer switches) and pdistr's BGM service.  Call once
 * before the game's sound_init; calling it again resets everything. */
void     snd_native_init(void);
void     snd_native_shutdown(void);

/* ---- 'SPUP' (pdispu2_send_state / pdispu2_status / F_0025C280) ---- */
/* The PS2's EE voices (core i / 24, voice i % 24; core 1's 22 / 23 are the
 * stream's) and the pool's extra ones: EE voice 46 + e is core e & 1, voice
 * 24 + e / 2 of the native SPU2. */
#define SND_NATIVE_EE_VOICES    46
#define SND_NATIVE_EXTRA_VOICES 80
/* Function 4: applies the state block as pdispu2 did (key-offs, effect,
 * core switches, pitch / volume / address+ADSR, mixer gates, key-ons) and
 * computes the voice-active mask.  Scheduled: queued at the time of now. */
void     snd_native_send_state(const SndNativeState *st);
/* PC: the same with the extra voices' states (extra: nExtra =
 * SND_NATIVE_EXTRA_VOICES of them, or NULL) and the time of the vblank the
 * block belongs to (seconds, the scheduled clock's; < 0: now). */
void     snd_native_send_state_ext(const SndNativeState *st, const SndNativeVoiceState *extra, int nExtra,
                                   double time);
/* Word 0 of the 'SPUP' answer: bit i = EE voice i (core i / 24, voice
 * i % 24) still sounding, as of the last send / status request (scheduled:
 * the voices keyed on in blocks not yet applied count as sounding). */
uint64_t snd_native_voice_active_mask(void);
/* PC: the whole pool's answer: bit i of the 128 = EE voice i (0..125). */
void     snd_native_voice_active_mask128(uint64_t out[2]);
/* Function 3: recomputes the mask now. */
void     snd_native_request_status(void);
/* Function 5 (sound_init sends 1, 1, 1): the S/PDIF output mode
 * (sceSdSetCoreAttr SPDIF_MODE).  Nothing to do natively. */
void     snd_native_set_output_mode(int a, int b, int c);

/* ---- SPU RAM ('PDIP' function 4, 'SPUV' functions 1 / 2) ---- */
void     snd_native_write_spu(uint32_t spuAddr, const void *src, uint32_t size);
void     snd_native_read_spu(uint32_t spuAddr, void *dst, uint32_t size);
/* The engine's name for the same (engine/sound/spu_memory.h, called by
 * engine/pdi T523 pdipipe_read2 with toSpu set): defined here. */
void     spu_memory_write(uint32_t spuAddr, const void *data, uint32_t size);
/* PC: the sound memory's size in bytes (spu2.h RAM_BYTES, 64 MB; the PS2's
 * SPU2 had 2 MB), and the engine's name for it (engine/sound/spu_memory.h). */
uint32_t snd_native_spu_bytes(void);
uint32_t spu_memory_size(void);

/* ---- 'PBGM' (pdibgm_*) ---- */
/* Function 1: plays the .ads read through `read' (loop != 0: from the
 * start again at its end).  0, or -1 when it is not an ADPCM .ads. */
int      snd_native_bgm_start(SndNativeReadFn read, void *user, int loop);
void     snd_native_bgm_stop(void);                 /* function 2 */
/* Function 3: the value pdibgm_set_volume sends (volume * 32768). */
void     snd_native_bgm_set_volume(int volume);
void     snd_native_bgm_pause(void);                /* function 4 */
void     snd_native_bgm_resume(void);               /* function 5 */
void     snd_native_bgm_get_status(SndNativeBgmStatus *out);   /* function 6 */

/* ---- 'MPG1' with sound (pdimpeg_open mode 2): a movie's audio ----
 * pdistr demuxed the PSS's audio (MPEG private stream 1, substream 0xFF
 * 0xA1: drop each packet payload's 4-byte prefix) and played it with the
 * BGM's stream player and voices: the payloads concatenated are an .ads
 * image (GT3's opening / ending: ADPCM 44.1 kHz stereo, interleave 0x400).
 * The native movie player pushes them here; stop / volume / pause / status
 * are the BGM functions above (mpegif::feed sets the BGM volume to 1.0). */
void     snd_native_bgm_open_push(void);
/* Returns the bytes taken (fewer when the FIFOs, 256 KB per channel or
 * about 5 s, are full: keep the rest and push it later), -1 when the
 * stream is not an ADPCM .ads image.
 * The voices start once the head and two chunks are in. */
int      snd_native_bgm_push(const void *data, uint32_t size);
void     snd_native_bgm_push_end(void);
/* How far the stream's voices have played, in microseconds, to within one
 * ADPCM block (0.6 ms); the IOP status (function 6) moves in 1792-sample
 * chunks.  For the movie player's audio / video sync; -1 before a stream
 * has started. */
int64_t  snd_native_bgm_played_us(void);

/* ---- PC: an MP4 movie's sound (no original; docs/movies.md "MP4 movies") ----
 * Decoded PCM (16-bit, interleaved, 1 or 2 channels at its own rate) mixed
 * into the output after the SPU2, resampled to 48 kHz, at the gain the BGM
 * voices would give it (the BGM volume, core 1's master volume): a .pss's
 * sound goes through the BGM voices as SPU2 ADPCM, an .mp4's is not
 * re-encoded.  open stops the BGM; the sound waits for start (the first
 * presentation window, as the pushed .ads waits for its first chunks). */
void     snd_native_pcm_open(int rate, int channels);
/* Frames taken (fewer when about 2 s are buffered: push the rest later). */
int      snd_native_pcm_push(const int16_t *pcm, int frames);
int      snd_native_pcm_buffered(void);             /* frames not played yet */
void     snd_native_pcm_end(void);                  /* no more data: plays out */
void     snd_native_pcm_start(void);
void     snd_native_pcm_pause(int on);              /* held by the presentation window */
void     snd_native_pcm_stop(void);
/* As snd_native_bgm_played_us; -1 before the stream has started. */
int64_t  snd_native_pcm_played_us(void);

/* ---- output ---- */
/* Renders `frames' frames of 48 kHz interleaved stereo (L, R), 16-bit (the
 * tools; scheduled, as snd_native_render, clamped). */
void     snd_native_mix(int16_t *out, int frames);
/* Not the IOP's: the scheduled engine (docs/sound.md 3.4).  On, each state
 * block is queued with its vblank's time and applied by snd_native_render
 * at the sample that time maps to plus a delay - the latest lateness of the
 * recent blocks and a render period - so consecutive blocks take effect
 * 800 / 801 samples apart, as the IOP applied them at each vblank, a few ms
 * after it; the mix gets headroom (Spu2::set_headroom).  `clock': seconds
 * of the system's clock, the same the block times and snd_native_render's
 * `now' use.  Off by default (the test tools apply each block at once and
 * mix after it). */
void     snd_native_set_scheduled(int on, double (*clock)(void));
/* The audio thread, once per device period: `frames' frames of 48 kHz
 * stereo float (1.0 = 16-bit full scale; above it with headroom), the
 * period starting at `now' (the clock's seconds). */
void     snd_native_render(float *out, int frames, double now);
/* Not the IOP's (a diagnostic, GT3PC_AUDIO_TIMING): the longest a render
 * waited for the sound lock since the last call (ms). */
double   snd_native_take_render_lock_wait(void);
/* The frames between the render and the speaker (the device's buffer):
 * the BGM clock (snd_native_bgm_played_us) leaves them out. */
void     snd_native_set_output_latency(int frames);
typedef struct SndNativeScheduleInfo {
    double   delayMs;       /* the delay now */
    int      queued;        /* blocks waiting */
    uint32_t blocks;        /* applied so far */
    uint32_t lateBlocks;    /* applied after their sample (arrived later than the delay) */
    uint32_t droppedBlocks; /* applied at once because the queue was full (no render) */
} SndNativeScheduleInfo;
void     snd_native_schedule_info(SndNativeScheduleInfo *out);

/* ---- diagnostics (GT3PC_SND_TRACE, docs/sound.md 3.6) ----
 * Not part of the PS2's interface.  Level 0: off.  1: each upload into
 * sound memory (its name, range, the regions it covers and the voices
 * still sounding there), each suspicious key-on (an address nothing was
 * uploaded to, not ADPCM, or not the start of a sample: every sample start
 * of GT3's banks is a bank start or follows a loop-end block), each voice
 * that reads suspicious blocks (spu2.h ReadReport), and a summary every
 * 10 s.  2: also every key-on and a summary every second.  Lines start
 * with "snd:" and the mixer's clock (t = samples mixed / 48000); fn gets
 * each line without a newline, with the sound lock held. */
typedef void (*SndNativeTraceFn)(const char *line);
void     snd_native_set_trace(int level, SndNativeTraceFn fn);
int      snd_native_trace_level(void);
/* Names the sound memory an upload is about to fill (the bank's file), for
 * the trace.  A no-op unless tracing. */
void     snd_native_name_region(uint32_t spuAddr, uint32_t size, const char *name);
/* The engine's names for the two above (engine/sound/spu_memory.h). */
int      snd_trace_level(void);
void     spu_memory_name(uint32_t spuAddr, uint32_t size, const char *name);

/* ---- sound memory in use (not the IOP's: engine/sound/spu_memory.h) ----
 * The voices (not the stream voices) still sounding (envelope not off) that
 * read [spuAddr, spuAddr + size): their current block or their start
 * address is in it.  The port's upload waits on it (T523). */
int      snd_native_voices_reading(uint32_t spuAddr, uint32_t size);
int      spu_memory_voices_reading(uint32_t spuAddr, uint32_t size);

/* ---- the EE side's trace (level 2, engine/sound/spu_memory.h) ----
 * Why a voice handle (an s8 of its owner) lost its voice, noted by the
 * voice manager / GtSoundHandle and read back at the handle's next key-on;
 * and a trace line from the EE side, with the mixer's clock and a limit of
 * 300 lines a second.  The reasons are engine/sound/spu_memory.h's
 * SND_LOST_* (0: none noted). */
void     snd_trace_lost(const void *handle, int reason, int voice, int prio, int byPrio);
int      snd_trace_lost_reason(const void *handle, int *voice, int *prio, int *byPrio, int *framesAgo);
void     snd_trace_line(const char *fmt, ...);

#ifdef __cplusplus
}

namespace spu2 { class Spu2; class AdsStream; }
/* Test access (tools/sndtest): the model behind the functions above.  Take
 * snd_native_lock() around direct use while another thread mixes. */
spu2::Spu2      &snd_native_spu();
spu2::AdsStream &snd_native_bgm();
void             snd_native_lock();
void             snd_native_unlock();
/* Offline rendering: no BGM feeder thread (the caller pumps the BGM file
 * with snd_native_bgm().pump()).  Set before snd_native_bgm_start. */
void             snd_native_set_bgm_threaded(bool threaded);
#endif

#endif /* GT3PC_SOUND_NATIVE_H */
