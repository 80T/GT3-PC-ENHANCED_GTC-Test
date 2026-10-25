/* spu2.h - a native model of the PS2 sound processor (SPU2) as GT3 drives it.
 *
 * Not an emulator of the chip's interface: no DMA, no register file at
 * 0x1F900000, no IRQ, no IOP.  It is the sound generator the IOP modules
 * (pdispu2.irx through libsd.irx) programmed, with a libsd-shaped
 * interface (voice parameters, switches, addresses, core parameters,
 * effect attributes) so that the replacement of pdispu2's command handlers
 * (sound_native.cpp) reads like the original.
 *
 *   - sound memory (addresses in bytes in this API, as libsd takes them);
 *     samples are uploaded into it by address.  PC: 64 MB, RAM_BYTES (the
 *     SPU2 had 2 MB, 1 M 16-bit words): the game keeps the PS2's map in
 *     the first 2 MB, the two reverb work areas there too (spu2_reverb.h
 *     addresses them with the hardware's 20-bit word masks), and puts a
 *     bank larger than its PS2 place above (game/sound T468, the SPU
 *     windows).
 *   - 2 cores x 24 voices (PC: up to 64 per core, VOICES_MAX; headroom,
 *     set_headroom).  A voice decodes SPU2 ADPCM from its start
 *     address (SSA), follows the loop flags (ENDX, loop start / end,
 *     repeat), steps through the samples at its pitch (0x1000 = one sample
 *     per 48 kHz tick, at most 0x3FFF) with the 4-point gaussian
 *     interpolation, applies its ADSR envelope (the SPU's rate / shift /
 *     step model) and its left / right volumes (fixed mode: 15-bit signed
 *     register, x2).  Pitch modulation (PMON) is supported; noise (NON) and
 *     volume sweeps are not (GT3 never sets them).
 *   - Per core: dry / wet voice gates (VMIXL / VMIXR / VMIXEL / VMIXER),
 *     the MMIX input switches, the reverb (spu2_reverb.h), the effect
 *     volume EVOL, master volume MVOL; core 0's output enters core 1 as its
 *     external input (AVOL), core 1's output is the result.
 *   - A voice can take its ADPCM blocks from a VoiceSource instead of sound
 *     memory: the BGM stream player (ads_stream.h) feeds core 1 voices 22
 *     and 23 that way, where the IOP copied the stream into ring buffers.
 *
 * Output: 48 kHz stereo, the SPU2's own rate, so there is no resampling.
 * The class is not thread-safe; sound_native.cpp serialises the game's
 * commands and the audio thread's mixing with its own lock.
 *
 * Hardware behaviour from nocash's psx-spx (PS1 SPU, which the SPU2 core
 * extends) and PCSX2 2.6.3's SPU2 (reference only); the code is this
 * port's.  Plain C++, no Windows or game dependency. */
#ifndef GT3PC_SPU2_H
#define GT3PC_SPU2_H

#include <cstdint>
#include <vector>

#include "engine/sound/native/spu2_adpcm.h"
#include "engine/sound/native/spu2_reverb.h"

namespace spu2 {

constexpr uint32_t RAM_BYTES       = 64 * 1024 * 1024;     /* PC: a power of two (the masks); the SPU2: 2 MB */
constexpr uint32_t RAM_WORDS       = RAM_BYTES / 2;
constexpr uint32_t PS2_RAM_BYTES   = 2 * 1024 * 1024;      /* the SPU2's (the reverb areas stay below it) */
constexpr int      NUM_CORES       = 2;
constexpr int      VOICES_PER_CORE = 24;
constexpr int      NUM_VOICES      = NUM_CORES * VOICES_PER_CORE;
constexpr int      OUTPUT_RATE     = 48000;
constexpr uint32_t ALL_VOICES      = 0xFFFFFF;
/* PC: the larger voice pool (docs/sound.md "The engine"): each core has
 * voices 24..63 beyond the hardware's 24, mixed into the same core sums
 * (its gates, reverb and volumes).  The libsd-shaped calls with 24-bit
 * masks address voices 0..23 and leave the others; set_switch64 addresses
 * all 64. */
constexpr int      VOICES_MAX      = 64;

/* Voice parameters (libsd SD_VPARAM_*). */
enum VoiceParam {
    VP_VOLL = 0, VP_VOLR, VP_PITCH, VP_ADSR1, VP_ADSR2,
    VP_ENVX,                        /* read only: the envelope level 0..0x7FFF */
    VP_VOLXL, VP_VOLXR              /* read only: the current volumes */
};
/* Core parameters (libsd SD_PARAM_*). */
enum CoreParam {
    CP_MMIX = 0, CP_MVOLL, CP_MVOLR, CP_EVOLL, CP_EVOLR, CP_AVOLL, CP_AVOLR, CP_BVOLL, CP_BVOLR
};
/* Voice bit masks of a core (libsd SD_SWITCH_*). */
enum Switch {
    SW_PMON = 0, SW_NON, SW_KON, SW_KOFF, SW_ENDX, SW_VMIXL, SW_VMIXEL, SW_VMIXR, SW_VMIXER
};
/* Addresses, in bytes (libsd SD_ADDR_* / SD_VADDR_*). */
enum VoiceAddr { VA_SSA = 0, VA_LSAX, VA_NAX };
enum CoreAddr { CA_ESA = 0, CA_EEA };

/* MMIX bits (which inputs reach a core's dry / wet sums). */
enum : uint16_t {
    MMIX_WET_EXT_R = 0x001, MMIX_WET_EXT_L = 0x002,     /* core 0 output (core 1 only) */
    MMIX_DRY_EXT_R = 0x004, MMIX_DRY_EXT_L = 0x008,
    MMIX_WET_IN_R  = 0x010, MMIX_WET_IN_L  = 0x020,     /* sound data input (ADMA) */
    MMIX_DRY_IN_R  = 0x040, MMIX_DRY_IN_L  = 0x080,
    MMIX_WET_SND_R = 0x100, MMIX_WET_SND_L = 0x200,     /* the voices */
    MMIX_DRY_SND_R = 0x400, MMIX_DRY_SND_L = 0x800,
};

/* Where a stream voice gets its blocks.  next_block is called from mix()
 * (the audio thread) whenever the voice needs its next 16-byte block; the
 * block's flag byte is ignored (a source voice never loops or stops by
 * itself).  Returning false ends the voice (as a one-shot end). */
class VoiceSource {
public:
    virtual ~VoiceSource() {}
    virtual bool next_block(int core, int voice, uint8_t block[ADPCM_BLOCK_BYTES]) = 0;
};

class Spu2 {
public:
    Spu2();
    ~Spu2();
    Spu2(const Spu2 &) = delete;
    Spu2 &operator=(const Spu2 &) = delete;

    /* Power-on: memory cleared, voices off, registers at their reset
     * values (MMIX, gates and volumes as PCSX2 documents them). */
    void reset();

    /* ---- sound memory ---- */
    void write_ram(uint32_t byteAddr, const void *src, uint32_t size);
    void read_ram(uint32_t byteAddr, void *dst, uint32_t size) const;
    const uint8_t *ram_bytes() const { return (const uint8_t *)ram_; }
    uint16_t *ram_words() { return ram_; }

    /* ---- libsd-shaped register access ---- */
    void     set_voice_param(int core, int voice, VoiceParam p, uint16_t value);
    uint16_t get_voice_param(int core, int voice, VoiceParam p) const;
    void     set_voice_addr(int core, int voice, VoiceAddr a, uint32_t byteAddr);
    uint32_t get_voice_addr(int core, int voice, VoiceAddr a) const;
    void     set_switch(int core, Switch s, uint32_t mask);     /* KON / KOFF act at once */
    uint32_t get_switch(int core, Switch s) const;
    /* PC: the switch bits `which' of all 64 voices of the core set to those
     * of mask (KON / KOFF: the voices of mask & which act). */
    void     set_switch64(int core, Switch s, uint64_t mask, uint64_t which);
    uint64_t get_switch64(int core, Switch s) const;
    /* PC: headroom (docs/sound.md "The engine"): the voices' and the cores'
     * sums are not clamped to 16 bits (only the reverb's input, which its
     * 16-bit work area holds); mix_float then gives the output unclamped,
     * full scale 1.0.  Off: the hardware's clamps (the tools' renders). */
    void     set_headroom(bool on) { headroom_ = on; }
    void     set_core_param(int core, CoreParam p, uint16_t value);
    uint16_t get_core_param(int core, CoreParam p) const;
    void     set_core_addr(int core, CoreAddr a, uint32_t byteAddr);
    void     set_effect_enable(int core, bool on);               /* core attribute EFFECT_ENABLE */
    bool     effect_enabled(int core) const { return cores_[core].fxEnable; }
    /* sceSdSetEffectAttr: the preset of `mode' (low byte; 0x100 = clear the
     * work area first), EVOL = depth.  delay / feedback are not used (only
     * ECHO / DELAY need them; GT3 does not). */
    void     set_effect_attr(int core, int mode, int16_t depthL, int16_t depthR, int delay, int feedback);
    int      effect_mode(int core) const { return cores_[core].reverb.mode(); }
    void     clear_effect_area(int core, int mode);

    /* A stream source for one voice (NULL: sound memory again). */
    void set_voice_source(int core, int voice, VoiceSource *src);

    /* ---- diagnostics (sound_native.cpp, GT3PC_SND_TRACE) ----
     * Not hardware: with the checks on, a voice playing sound memory reports
     * the first suspicious block after its key-on: a block nothing uploaded
     * (write_ram), a block whose header is not ADPCM as GT3's banks have it
     * (adpcm_header_plausible), or a loop end that
     * jumps to a loop address the voice never passed (no loop-start block
     * since its key-on, no LSAX write: a stale address). */
    enum ReadProblem { READ_UNWRITTEN = 1, READ_NOT_ADPCM, READ_STALE_LOOP };
    struct ReadReport {
        int      core, voice, problem;
        uint32_t ssa;                   /* bytes: where the voice was keyed on */
        uint32_t addr;                  /* bytes: the block (the loop address for READ_STALE_LOOP) */
        uint8_t  head[2];               /* the block's shift / filter and flag bytes */
        uint64_t tick;                  /* ticks() when it was read */
    };
    void set_checks(bool on) { checks_ = on; }
    /* The 16-byte block at byteAddr was uploaded (write_ram) since reset. */
    bool written(uint32_t byteAddr) const;
    /* Moves the reports made since the last call into out (64 are kept at
     * most in between). */
    void take_reports(std::vector<ReadReport> &out);
    /* The voice is keyed on and not yet silent (envelope phase not off). */
    bool voice_sounding(int core, int voice) const;

    /* Renders `frames' stereo frames (interleaved L, R) at 48 kHz. */
    void mix(int16_t *out, int frames);
    /* The same as float, 1.0 = 16-bit full scale (above it with headroom). */
    void mix_float(float *out, int frames);
    /* The same into 32-bit accumulators before the final clamp (tests). */
    void mix_frame(int32_t &outL, int32_t &outR);

    /* Tick counter (output samples since reset). */
    uint64_t ticks() const { return ticks_; }

private:
    enum Phase { PH_OFF = 0, PH_ATTACK, PH_DECAY, PH_SUSTAIN, PH_RELEASE };

    struct Voice {
        /* registers */
        uint16_t regVolL = 0, regVolR = 0;
        int32_t  volL = 0, volR = 0;        /* effective: (int16)(reg << 1) */
        uint16_t pitch = 0;
        uint16_t adsr1 = 0, adsr2 = 0;
        uint32_t ssa = 0;                   /* words */
        uint32_t lsa = 0;                   /* words */
        bool     lsaWritten = false;        /* LSAX written by the program: blocks' loop-start flags ignored */
        bool     lsaSeen = false;           /* diagnostics: a loop-start block set lsa since the key-on */
        bool     reported = false;          /* diagnostics: a read report made since the key-on */
        /* playback */
        uint32_t nax = 0;                   /* words: the block being played */
        uint8_t  blockFlags = 0;
        bool     haveBlock = false;
        int16_t  dec[ADPCM_BLOCK_SAMPLES] = {};
        int      decPos = ADPCM_BLOCK_SAMPLES;
        AdpcmHistory hist;
        int16_t  pv[4] = {};                /* the last four samples, pv[3] newest */
        uint32_t counter = 0;               /* pitch counter, 12-bit fraction */
        /* envelope */
        int      phase = PH_OFF;
        int32_t  env = 0;
        uint32_t envCounter = 0;
        int32_t  outX = 0;                  /* last output after the envelope (PMON) */
        VoiceSource *source = nullptr;
    };

    struct Core {
        Voice    voice[VOICES_MAX];
        uint64_t pmon = 0, non = 0, endx = 0;
        uint64_t vmixl = 0, vmixr = 0, vmixel = 0, vmixer = 0;
        int      top = VOICES_PER_CORE;      /* the voices mixed: past the highest ever keyed on */
        uint16_t mmix = 0;
        uint16_t regMvolL = 0, regMvolR = 0;
        int32_t  mvolL = 0, mvolR = 0;      /* (int16)(reg << 1) */
        int32_t  evolL = 0, evolR = 0;      /* signed 16 */
        int32_t  avolL = 0, avolR = 0;
        int32_t  bvolL = 0, bvolR = 0;
        bool     fxEnable = false;
        Reverb   reverb;
    };

    void key_on(int core, int v);
    void key_off(int core, int v);
    int32_t voice_tick(int core, int v, int32_t prevOutX);
    bool next_sample(int core, int v, int16_t &s);
    void envelope_tick(Voice &vc);
    void report(int core, int v, int problem, uint32_t addr, const uint8_t *head);

    static constexpr uint32_t WRITTEN_WORDS = RAM_BYTES / ADPCM_BLOCK_BYTES / 32;

    uint16_t *ram_;                 /* RAM_WORDS words */
    Core     cores_[NUM_CORES];
    uint64_t ticks_ = 0;
    bool     headroom_ = false;
    /* diagnostics */
    bool     checks_ = false;
    uint32_t *written_;             /* WRITTEN_WORDS: a bit per 16-byte block (512 KB: on the heap) */
    std::vector<ReadReport> reports_;
};

/* The 4-point gaussian interpolation table of the SPU: 256 phases x 4
 * weights (oldest .. newest sample), row = (pitch counter >> 4) & 0xFF.
 * In nocash's 512-entry form these are gauss[0xFF - i], gauss[0x1FF - i],
 * gauss[0x100 + i], gauss[i]. */
extern const int16_t *gauss_table();

} /* namespace spu2 */

#endif /* GT3PC_SPU2_H */
