/* sound_native.cpp - the IOP sound services of GT3 as native functions (see
 * sound_native.h).  Addresses in comments are pdispu2.irx / pdistr.irx
 * .text offsets (the modules load at 0; docs/sound.md lists them). */
#include "engine/sound/native/sound_native.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "engine/sound/native/ads_stream.h"
#include "engine/sound/native/spu2.h"

using namespace spu2;

static_assert(sizeof(SndNativeVoiceState) == 0x10, "SndNativeVoiceState");
static_assert(sizeof(SndNativeEffectAttr) == 0x14, "SndNativeEffectAttr");
static_assert(sizeof(SndNativeCoreState) == 0x24, "SndNativeCoreState");
static_assert(sizeof(SndNativeState) == 0x350, "SndNativeState");
static_assert(sizeof(SndNativeBgmStatus) == 0x40, "SndNativeBgmStatus");

namespace {

/* The BGM voices: pdispu2 export 52 (0x0002A8) sets the stream object to
 * core 1, voice 22 (left) and voice + 1 (right). */
constexpr int BGM_CORE = 1;
constexpr int BGM_VOICE_L = 22;
constexpr int BGM_VOICE_R = 23;
constexpr uint16_t BGM_ADSR1 = 0x000F;      /* pdispu2 0x000000: attack 0, decay 0, sustain level 15 */
constexpr uint16_t BGM_ADSR2 = 0x0000;      /*   sustain linear +0 (holds), release linear 0 */
constexpr int32_t VOL_UNIT = 0x8000;        /* the IOP's volume objects: 0x8000 = 1.0 */

/* pdispu2 0x0021C8 / 0x001F28: reverb work areas end at these bytes */
constexpr uint32_t EEA_CORE0 = 0x1DFFFF;
constexpr uint32_t EEA_CORE1 = 0x1FFFFF;
/* PC: they stay where the PS2 had them (the reverb's 20-bit word masks,
 * spu2_reverb.h); the larger memory is above */
static_assert(EEA_CORE0 < PS2_RAM_BYTES && EEA_CORE1 < PS2_RAM_BYTES, "the reverb areas in the first 2 MB");

/* ---- diagnostics (GT3PC_SND_TRACE, sound_native.h) ---- */

/* A stretch of sound memory and the upload that filled it. */
struct Region {
    uint32_t start, end;            /* bytes, end exclusive */
    char     name[48];
};

constexpr int TRACE_LINES_PER_PERIOD = 40;  /* suspicious key-ons / reads logged per summary period */

struct Trace {
    int level = 0;
    SndNativeTraceFn fn = nullptr;
    std::vector<Region> regions;    /* disjoint; an upload replaces what it covers */
    std::vector<Spu2::ReadReport> reports;
    /* the summary period */
    uint32_t frames = 0;
    uint32_t keyOns = 0, keyOnsCut = 0, badKeyOns = 0, badReads = 0, suppressed = 0;
    int lines = 0;
    int peakSounding = 0;
    /* the EE side's (level 2) */
    uint32_t frameNo = 0;           /* state blocks since start-up */
    uint32_t eeLines = 0, eeSuppressed = 0;
    struct Lost {
        const void *handle;
        int reason, voice, prio, byPrio;
        uint32_t frame;
    };
    Lost lost[1024] = {};
};

constexpr int TRACE_EE_LINES_PER_SECOND = 300;

/* ---- the scheduled engine (snd_native_set_scheduled; docs/sound.md 3.4) ----
 * On the PS2 the IOP applied each state block within a millisecond of the
 * vblank that made the EE send it, so in the SPU2's output the blocks are
 * 800 or 801 samples apart (48 kHz / 59.94 Hz).  Here a block is queued
 * with that vblank's time and the audio thread applies it at the sample
 * that time maps to (its render clock: frame = time x 48000 + an offset
 * fitted to the render calls), plus a delay that covers how late the blocks
 * arrive (the game tick's lateness behind its vblank, the music thread's)
 * and one render period: SCHED_DELAY_* below, adapted to the lateness of
 * the last blocks.  The blocks land 800 / 801 samples apart again, a few ms
 * after their vblank instead of the old paced FIFO's 25 ms. */
constexpr int SCHED_QUEUE_MAX = 64;         /* blocks queued at most (more: the oldest applied at once) */
constexpr int SCHED_LATE_WINDOW = 240;      /* blocks the delay looks back on (4 s) */
constexpr int SCHED_LATE_SKIP = 3;          /* of those, the latest not covered (a hitch's: they apply late) */
constexpr double SCHED_MARGIN = 48.0;       /* frames (1 ms) beyond the latest lateness and a period */
constexpr double SCHED_DELAY_MAX = 48000.0 * 0.060;    /* the most the delay grows (60 ms); later blocks apply late */
constexpr int SCHED_STATUS_FRAMES = 10;     /* the status after a block: ~200 us of rendering, as pdispu2's */
/* A block that keys off (or on again) a voice keyed on less than this
 * before waits until then: one tick, the least the IOP ever gave a note.
 * Late blocks applied back to back (a hitch's, the delay not covering
 * them) keyed a one-tick note on and off at the same sample: a note or a
 * sound effect missed (user report). */
constexpr double SCHED_MIN_NOTE = 800.0;    /* frames (48 kHz / 59.94 Hz) */

/* PC: the larger voice pool.  Extra voice e is core (e & 1), voice
 * VOICES_PER_CORE + (e >> 1) of the native SPU2 (sound_native.h). */
static_assert(SND_NATIVE_EXTRA_VOICES <= 2 * (VOICES_MAX - VOICES_PER_CORE), "the extra voices fit the cores");
inline int extra_core(int e) { return e & 1; }
inline int extra_voice(int e) { return VOICES_PER_CORE + (e >> 1); }

struct Pending {
    SndNativeState st;
    SndNativeVoiceState extra[SND_NATIVE_EXTRA_VOICES];
    bool hasExtra;
    double time;                    /* the block's vblank (seconds, the clock's) */
    bool held = false;              /* PC: it waited for a note's tick (SCHED_MIN_NOTE) */
};

struct State {
    std::mutex lock;
    Spu2 *spu = nullptr;
    AdsStream *bgm = nullptr;
    bool bgmThreaded = true;
    /* pdispu2's per-core masks (.bss 0x3870 + core * 0x2C); PC: 64-bit, the
     * voice pool's voices 24..63 too */
    uint64_t active[2] = {};        /* +0x20 keyed on, not yet ended */
    uint64_t keyOffPending[2] = {}; /* +0x24 keyed off, envelope not yet at 0 */
    uint64_t oneShot[2] = {};       /* +0x28 flag 0x100: ends at ENDX */
    uint64_t activeMask = 0;        /* +0x58 the answer */
    uint64_t activeExtra[2] = {};   /* PC: the extra voices' answer, bit e */
    /* pdistr / pdispu2 stream state */
    int32_t bgmVolume = VOL_UNIT;   /* the volume object's word 0 (fno 3) */
    int32_t bgmGate = 0;            /* word 4: 0x8000 while the voices play */
    bool bgmPlaying = false;
    bool bgmPaused = false;
    bool bgmPushPending = false;    /* a pushed stream waits for its first chunks */
    Trace trace;                    /* not the IOP's: diagnostics */
    /* the scheduled engine (not the IOP's; above) */
    bool scheduled = false;
    double (*clock)(void) = nullptr;
    std::vector<Pending> queue;     /* oldest first */
    double lastTime = -1e300;
    uint64_t frame = 0;             /* frames rendered */
    double off = 0.0;               /* frame at time t = t x 48000 + off */
    bool offValid = false;
    double delay = 0.0;             /* frames */
    float late[SCHED_LATE_WINDOW] = {};
    int lateCount = 0, latePos = 0;
    double period = 0.0;            /* the largest render call of late (frames; a bigger one at once, back slowly) */
    int outputLatency = 0;          /* frames between the render and the speaker (snd_native_set_output_latency) */
    int statusIn = -1;              /* frames to render before the status (-1: none due) */
    uint64_t keyOnFrame[2][VOICES_MAX] = {};    /* the frame of each voice's last key-on + 1 (0: none) */
    uint32_t blocks = 0, lateBlocks = 0, dropped = 0, heldBlocks = 0;
    std::vector<float> scratch;     /* snd_native_mix's float buffer */
    /* PC: an MP4 movie's PCM (snd_native_pcm_*): a ring of frames at its own
     * rate, read with a fractional position (4-point cubic to 48 kHz) */
    struct Pcm {
        bool open = false, started = false, paused = false, ended = false;
        int rate = 48000, channels = 2;
        std::vector<float> ring;    /* stereo frames (mono duplicated) */
        size_t cap = 0, head = 0, count = 0;
        double frac = 0.0;          /* the read position past ring[head] */
        float hist[2][2] = {};      /* the frame before head (the cubic's first point) */
        uint64_t consumed = 0;      /* frames played */
    } pcm;
};

State &st()
{
    static State s;
    return s;
}

Spu2 &spu()
{
    State &s = st();
    if (s.spu == nullptr)
        s.spu = new Spu2();
    return *s.spu;
}

AdsStream &bgm()
{
    State &s = st();
    if (s.bgm == nullptr)
        s.bgm = new AdsStream();
    return *s.bgm;
}

/* pdispu2 0x001C30: a * b * c * d, each product >> 15, clamped to 16 bits. */
int32_t vol_chain(int32_t a, int32_t b, int32_t c, int32_t d)
{
    int32_t x = (a * b) >> 15;
    x = (x * c) >> 15;
    x = (x * d) >> 15;
    return x < -0x8000 ? -0x8000 : (x > 0x7FFF ? 0x7FFF : x);
}

/* pdispu2 0x000424, the stream volume callback: left voice VOLL, right
 * voice VOLR = the chain >> 1 (15-bit fixed mode, at most 0x3FFF).  The
 * other volume of each voice stays 0. */
void bgm_apply_volume_locked()
{
    State &s = st();
    int32_t v = vol_chain(s.bgmVolume, VOL_UNIT, VOL_UNIT, s.bgmGate);
    uint16_t reg = (uint16_t)((v >> 1) & 0x3FFF);
    spu().set_voice_param(BGM_CORE, BGM_VOICE_L, VP_VOLL, reg);
    spu().set_voice_param(BGM_CORE, BGM_VOICE_R, VP_VOLR, reg);
}

/* pdispu2 0x000068: (re)starts the two stream voices: start address (here
 * the player), ADSR, pitch, the volume gate open, key-on. */
void bgm_voices_start_locked()
{
    State &s = st();
    Spu2 &p = spu();
    uint16_t pitch = bgm().voice_pitch();
    for (int v = BGM_VOICE_L; v <= BGM_VOICE_R; v++) {
        p.set_voice_source(BGM_CORE, v, &bgm());
        p.set_voice_param(BGM_CORE, v, VP_ADSR1, BGM_ADSR1);
        p.set_voice_param(BGM_CORE, v, VP_ADSR2, BGM_ADSR2);
        p.set_voice_param(BGM_CORE, v, VP_PITCH, pitch);
    }
    s.bgmGate = VOL_UNIT;
    bgm_apply_volume_locked();
    uint32_t mask = 1u << BGM_VOICE_L;
    if (bgm().header().channels > 1)
        mask |= 1u << BGM_VOICE_R;
    p.set_switch(BGM_CORE, SW_KON, mask);
}

/* pdispu2 0x000134: stops them: pitch 0, the gate closed (volumes 0),
 * key-off. */
void bgm_voices_stop_locked()
{
    State &s = st();
    Spu2 &p = spu();
    p.set_voice_param(BGM_CORE, BGM_VOICE_L, VP_PITCH, 0);
    p.set_voice_param(BGM_CORE, BGM_VOICE_R, VP_PITCH, 0);
    s.bgmGate = 0;
    bgm_apply_volume_locked();
    p.set_switch(BGM_CORE, SW_KOFF, (1u << BGM_VOICE_L) | (1u << BGM_VOICE_R));
}

/* pdispu2 0x000F6C: a voice keyed on is active until it ends: a one-shot
 * voice (flag 0x100) at its ENDX, any voice keyed off when its envelope
 * reaches 0.  The answer: core 0's mask | core 1's << 24. */
void update_status_locked()
{
    State &s = st();
    Spu2 &p = spu();
    for (int c = 0; c < 2; c++) {
        uint64_t ended = p.get_switch64(c, SW_ENDX) & s.oneShot[c];
        for (int v = 0; v < VOICES_MAX; v++) {
            uint64_t bit = 1ull << v;
            if ((s.keyOffPending[c] & bit) && p.get_voice_param(c, v, VP_ENVX) == 0)
                ended |= bit;
        }
        s.active[c] &= ~ended;
        s.keyOffPending[c] &= ~ended;
    }
    s.activeMask = (s.active[0] & ALL_VOICES) | ((s.active[1] & ALL_VOICES) << 24);
    s.activeExtra[0] = s.activeExtra[1] = 0;
    for (int e = 0; e < SND_NATIVE_EXTRA_VOICES; e++)
        if (s.active[extra_core(e)] & (1ull << extra_voice(e)))
            s.activeExtra[e >> 6] |= 1ull << (e & 63);
}

/* The key-ons of the blocks still queued (scheduled): those voices count as
 * active for the answer, as the IOP would have keyed them on by now. */
void pending_key_ons_locked(uint64_t *mask, uint64_t extra[2])
{
    State &s = st();
    for (const Pending &b : s.queue) {
        for (int i = 0; i < 48; i++)
            if (b.st.voice[i].flags & SND_NATIVE_V_KEY_ON)
                *mask |= 1ull << i;
        if (b.hasExtra)
            for (int e = 0; e < SND_NATIVE_EXTRA_VOICES; e++)
                if (b.extra[e].flags & SND_NATIVE_V_KEY_ON)
                    extra[e >> 6] |= 1ull << (e & 63);
    }
}

/* ------------------------------------------------------------------------ */
/* Diagnostics (GT3PC_SND_TRACE): not the IOP's.  Everything below runs with */
/* the sound lock held.                                                     */
/* ------------------------------------------------------------------------ */

/* One line: "snd: t=<mixer clock> ..." to the trace function. */
void trace_line(uint64_t tick, const char *fmt, ...)
{
    Trace &t = st().trace;
    if (t.fn == nullptr)
        return;
    char buf[1200];
    int n = std::snprintf(buf, sizeof(buf), "snd: t=%.3f ", (double)tick / OUTPUT_RATE);
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    va_end(ap);
    t.fn(buf);
}

/* A suspicious event may be logged (TRACE_LINES_PER_PERIOD per summary
 * period; the others are counted). */
bool trace_allow()
{
    Trace &t = st().trace;
    if (t.lines < TRACE_LINES_PER_PERIOD) {
        t.lines++;
        return true;
    }
    t.suppressed++;
    return false;
}

const Region *region_at(uint32_t addr)
{
    for (const Region &r : st().trace.regions)
        if (addr >= r.start && addr < r.end)
            return &r;
    return nullptr;
}

/* "<upload name> +0x<offset>", or "nothing uploaded". */
void describe(uint32_t addr, char *out, size_t size)
{
    const Region *r = region_at(addr);
    if (r != nullptr)
        std::snprintf(out, size, "%s +0x%X", r->name, addr - r->start);
    else
        std::snprintf(out, size, "no named upload");
}

/* An upload is about to fill [start, end): what it covers, the voices still
 * sounding there (they will play the new data from where they are), then
 * the region table. */
void trace_upload_locked(uint32_t start, uint32_t size, const char *name)
{
    Trace &t = st().trace;
    Spu2 &p = spu();
    uint32_t end = start + size;
    char covers[512] = "";
    char voices[512] = "";
    size_t cn = 0, vn = 0;
    int sounding = 0;

    for (const Region &r : t.regions) {
        if (r.start < end && start < r.end && cn < sizeof(covers) - 96)
            cn += std::snprintf(covers + cn, sizeof(covers) - cn, "%s%s 0x%07X..0x%07X", cn ? ", " : "",
                                r.name, r.start, r.end);
    }
    for (int c = 0; c < 2; c++) {
        for (int v = 0; v < VOICES_MAX; v++) {
            if (c == BGM_CORE && (v == BGM_VOICE_L || v == BGM_VOICE_R))
                continue;                   /* the stream voices do not read sound memory */
            if (!p.voice_sounding(c, v))
                continue;
            uint32_t nax = p.get_voice_addr(c, v, VA_NAX);
            if (nax < start || nax >= end)
                continue;
            sounding++;
            if (vn < sizeof(voices) - 48)
                vn += std::snprintf(voices + vn, sizeof(voices) - vn, " c%dv%d at 0x%07X (envelope 0x%04X)", c, v,
                                    nax, p.get_voice_param(c, v, VP_ENVX));
        }
    }
    trace_line(p.ticks(), "upload %s -> SPU 0x%07X..0x%07X (%u bytes)%s%s%s", name, start, end, size,
               cn ? "; covers " : "", covers, sounding ? "" : "; no voice sounds there");
    if (sounding)
        trace_line(p.ticks(), "  %d voice(s) still sounding in that range:%s", sounding, voices);

    std::vector<Region> out;
    for (const Region &r : t.regions) {
        if (r.end <= start || end <= r.start) {
            out.push_back(r);
            continue;
        }
        if (r.start < start) {
            Region a = r;
            a.end = start;
            out.push_back(a);
        }
        if (end < r.end) {
            Region b = r;
            b.start = end;
            out.push_back(b);
        }
    }
    Region nr;
    nr.start = start;
    nr.end = end;
    std::snprintf(nr.name, sizeof(nr.name), "%s", name ? name : "?");
    out.push_back(nr);
    t.regions.swap(out);
}

/* The voices about to be keyed on, before the KON: their start address
 * (SSA as set in this block) must be uploaded memory, ADPCM, and the start
 * of a sample (a bank's first block, or the block after a loop end: true
 * of all 8,768 sample starts of the disc's banks).  Level 2 logs them all. */
/* The EE's number of voice v of core c (the extra voices: 46.., sound_native.h). */
int ee_voice(int c, int v)
{
    return v < VOICES_PER_CORE ? c * VOICES_PER_CORE + v
                               : SND_NATIVE_EE_VOICES + (((v - VOICES_PER_CORE) << 1) | c);
}

void trace_key_ons_locked(const SndNativeState *in, const SndNativeVoiceState *extra, const uint64_t kon[2])
{
    State &s = st();
    Trace &t = s.trace;
    Spu2 &p = spu();
    const uint8_t *ram = p.ram_bytes();

    for (int c = 0; c < 2; c++) {
        for (int v = 0; v < VOICES_MAX; v++) {
            if (!(kon[c] & (1ull << v)))
                continue;
            const SndNativeVoiceState &vs = v < VOICES_PER_CORE ? in->voice[c * VOICES_PER_CORE + v]
                                                                : extra[((v - VOICES_PER_CORE) << 1) | c];
            uint32_t a = p.get_voice_addr(c, v, VA_SSA);
            bool cut = p.voice_sounding(c, v);
            const Region *r = region_at(a);
            const char *problem = nullptr;

            t.keyOns++;
            if (cut)
                t.keyOnsCut++;
            if (!p.written(a))
                problem = "nothing was uploaded there";
            else if (!adpcm_header_plausible(ram + a))
                problem = "not ADPCM";
            else if (!(r != nullptr && a == r->start) &&
                     !(a >= ADPCM_BLOCK_BYTES && (ram[a - ADPCM_BLOCK_BYTES + 1] & ADPCM_FLAG_LOOP_END)))
                problem = "not the start of a sample";
            if (problem != nullptr)
                t.badKeyOns++;
            if (problem != nullptr ? !trace_allow() : t.level < 2)
                continue;
            char where[96];
            describe(a, where, sizeof(where));
            trace_line(p.ticks(), "%skey-on c%dv%d (EE voice %d) at 0x%07X [%s]%s%s%s: pitch 0x%04X, "
                       "volume 0x%04X / 0x%04X, ADSR 0x%04X / 0x%04X, flags 0x%03X, header %02X %02X",
                       problem ? "SUSPICIOUS " : "", c, v, ee_voice(c, v), a, where,
                       cut ? " (cuts a sounding voice)" : "", problem ? ": " : "", problem ? problem : "",
                       (uint16_t)vs.pitch, vs.volL, vs.volR, vs.adsr1, vs.adsr2, (uint16_t)vs.flags & 0xFFF,
                       ram[a], ram[a + 1]);
        }
    }
}

/* The mixer's read reports since the last block, and the summary. */
void trace_frame_locked()
{
    State &s = st();
    Trace &t = s.trace;
    Spu2 &p = spu();

    p.take_reports(t.reports);
    for (const Spu2::ReadReport &r : t.reports) {
        t.badReads++;
        if (!trace_allow())
            continue;
        char from[96], where[96];
        describe(r.ssa, from, sizeof(from));
        describe(r.addr, where, sizeof(where));
        const char *what = r.problem == Spu2::READ_UNWRITTEN ? "reads a block nothing was uploaded to"
                         : r.problem == Spu2::READ_NOT_ADPCM ? "reads data that is not ADPCM"
                         : "loops back to a loop address it never passed (stale)";
        trace_line(r.tick, "SUSPICIOUS read: c%dv%d (EE voice %d), keyed on at 0x%07X [%s], %s at 0x%07X [%s]"
                   " (header %02X %02X)", r.core, r.voice, ee_voice(r.core, r.voice), r.ssa, from, what,
                   r.addr, where, r.head[0], r.head[1]);
    }
    t.reports.clear();

    /* the EE side's line budget, per 60 blocks */
    if (++t.frameNo % 60 == 0) {
        if (t.eeSuppressed > 0)
            trace_line(p.ticks(), "  (%u EE lines not logged in the last 60 frames)", t.eeSuppressed);
        t.eeLines = t.eeSuppressed = 0;
    }

    int sounding = 0;
    for (int c = 0; c < 2; c++)
        for (int v = 0; v < VOICES_MAX; v++)
            if (p.voice_sounding(c, v))
                sounding++;
    if (sounding > t.peakSounding)
        t.peakSounding = sounding;

    t.frames++;
    if (t.frames < (t.level >= 2 ? 60u : 600u))
        return;
    trace_line(p.ticks(), "summary of %u frames: %u key-ons (%u cut a sounding voice), %u suspicious key-ons, "
               "%u suspicious reads (%u lines not logged), voices sounding %d (peak %d); scheduled %s, delay "
               "%.1f ms, %u blocks late, %u held for a note's tick, %u applied early (queue full) so far", t.frames,
               t.keyOns, t.keyOnsCut, t.badKeyOns, t.badReads, t.suppressed, sounding, t.peakSounding,
               s.scheduled ? "on" : "off", s.delay * 1000.0 / OUTPUT_RATE, s.lateBlocks, s.heldBlocks, s.dropped);
    t.frames = t.keyOns = t.keyOnsCut = t.badKeyOns = t.badReads = t.suppressed = 0;
    t.lines = 0;
    t.peakSounding = 0;
}

} /* namespace */

/* ------------------------------------------------------------------------ */
/* Life cycle                                                               */
/* ------------------------------------------------------------------------ */

/* pdispu2 0x001F28 (module start: sceSdInit and the register setup), the
 * masks cleared (0x000F40), pdistr's BGM service idle. */
void snd_native_init(void)
{
    bgm().close();
    std::lock_guard<std::mutex> g(st().lock);
    State &s = st();
    Spu2 &p = spu();

    p.reset();
    for (int c = 0; c < 2; c++) {
        p.set_core_param(c, CP_MVOLL, 0);
        p.set_core_param(c, CP_MVOLR, 0);
        p.set_voice_source(c, BGM_VOICE_L, nullptr);
        p.set_voice_source(c, BGM_VOICE_R, nullptr);
    }
    /* ESA 0x1D8800 / 0x1F8800 are written too, but libsd's
     * sceSdSetEffectAttr places ESA below EEA by the mode's size */
    p.set_core_addr(0, CA_EEA, EEA_CORE0);
    p.set_core_addr(1, CA_EEA, EEA_CORE1);
    for (int c = 0; c < 2; c++) {
        p.clear_effect_area(c, EFFECT_STUDIO_3);
        p.set_effect_attr(c, EFFECT_OFF, 0, 0, 0, 0);
    }
    for (int c = 0; c < 2; c++) {
        for (int v = 0; v < VOICES_PER_CORE; v++) {
            p.set_voice_param(c, v, VP_VOLL, 0);
            p.set_voice_param(c, v, VP_VOLR, 0);
        }
        p.set_switch(c, SW_PMON, 0);
        p.set_switch(c, SW_NON, 0);
        p.set_switch(c, SW_VMIXL, ALL_VOICES);
        p.set_switch(c, SW_VMIXR, ALL_VOICES);
        p.set_switch(c, SW_VMIXEL, 0);
        p.set_switch(c, SW_VMIXER, 0);
        p.set_core_param(c, CP_MMIX, c ? 0xFCC : 0xFC0);
        p.set_core_param(c, CP_EVOLL, 0x7FFF);
        p.set_core_param(c, CP_EVOLR, 0x7FFF);
        p.set_core_param(c, CP_AVOLL, c ? 0x7FFF : 0);
        p.set_core_param(c, CP_AVOLR, c ? 0x7FFF : 0);
        p.set_core_param(c, CP_BVOLL, 0);
        p.set_core_param(c, CP_BVOLR, 0);
    }
    for (int c = 0; c < 2; c++) {
        p.set_core_param(c, CP_MVOLL, 0x3FFF);
        p.set_core_param(c, CP_MVOLR, 0x3FFF);
    }

    std::memset(s.active, 0, sizeof(s.active));
    std::memset(s.keyOffPending, 0, sizeof(s.keyOffPending));
    std::memset(s.oneShot, 0, sizeof(s.oneShot));
    s.activeMask = 0;
    s.activeExtra[0] = s.activeExtra[1] = 0;
    s.trace.regions.clear();        /* (the trace level and function stay) */
    s.trace.reports.clear();
    s.queue.clear();                /* (scheduled stays as set) */
    s.lastTime = -1e300;
    s.statusIn = -1;
    std::memset(s.keyOnFrame, 0, sizeof(s.keyOnFrame));
    s.bgmVolume = VOL_UNIT;         /* pdispu2 export 4 (0x001C8C): every word 0x8000 */
    s.bgmGate = 0;
    s.bgmPlaying = false;
    s.bgmPaused = false;
    s.bgmPushPending = false;
}

void snd_native_shutdown(void)
{
    bgm().close();
    State &s = st();
    std::lock_guard<std::mutex> g(s.lock);
    delete s.bgm;
    s.bgm = nullptr;
    delete s.spu;
    s.spu = nullptr;
}

/* ------------------------------------------------------------------------ */
/* 'SPUP'                                                                   */
/* ------------------------------------------------------------------------ */

/* The state of voice v of core c in a block (with the extra voices' array
 * for voices 24.. of the pool; nullptr: none given). */
static const SndNativeVoiceState *block_voice(const SndNativeState *in, const SndNativeVoiceState *extra, int c,
                                              int v)
{
    if (v < VOICES_PER_CORE)
        return &in->voice[c * VOICES_PER_CORE + v];
    if (extra == nullptr)
        return nullptr;
    const int e = ((v - VOICES_PER_CORE) << 1) | c;
    return e < SND_NATIVE_EXTRA_VOICES ? &extra[e] : nullptr;
}

/* PC: a block's key-ons and key-offs per core (voices 0..23, 24.. with the
 * extra array). */
static void block_keys(const SndNativeState *in, const SndNativeVoiceState *extra, uint64_t kon[2], uint64_t koff[2])
{
    const int top = extra != nullptr ? VOICES_MAX : VOICES_PER_CORE;
    for (int c = 0; c < 2; c++) {
        kon[c] = koff[c] = 0;
        for (int v = 0; v < top; v++) {
            const SndNativeVoiceState *vs = block_voice(in, extra, c, v);
            if (vs == nullptr)
                continue;
            const uint32_t f = (uint16_t)vs->flags;
            if (f & SND_NATIVE_V_KEY_OFF)
                koff[c] |= 1ull << v;
            if (f & SND_NATIVE_V_KEY_ON)
                kon[c] |= 1ull << v;
        }
    }
}

/* PC (SCHED_MIN_NOTE): the frame a block keying these on / off may apply
 * at - a tick after the last key-on of each voice it keys (0: any). */
static double key_hold_locked(const uint64_t kon[2], const uint64_t koff[2])
{
    State &s = st();
    double hold = 0.0;
    for (int c = 0; c < 2; c++) {
        uint64_t m = kon[c] | koff[c];
        for (int v = 0; m != 0; v++, m >>= 1)
            if ((m & 1) && s.keyOnFrame[c][v] != 0) {
                const double h = (double)(s.keyOnFrame[c][v] - 1) + SCHED_MIN_NOTE;
                if (h > hold)
                    hold = h;
            }
    }
    return hold;
}

/* pdispu2 0x0010A4 ('SPUP' function 4): applies a block.  PC: the voice
 * pool's extra voices with it (extra: SND_NATIVE_EXTRA_VOICES, or nullptr:
 * those voices untouched). */
static void apply_block_locked(const SndNativeState *in, const SndNativeVoiceState *extra)
{
    State &s = st();
    Spu2 &p = spu();
    /* the voices the block describes: 0..23 of each core, and 24..63 with
     * the extra array */
    const uint64_t which = extra != nullptr ? ~0ull : (uint64_t)ALL_VOICES;
    const int top = extra != nullptr ? VOICES_MAX : VOICES_PER_CORE;

    /* the flag bits of each core's voices as masks */
    uint64_t koff[2] = {}, kon[2] = {}, pmon[2] = {}, non[2] = {};
    uint64_t dryL[2] = {}, wetL[2] = {}, dryR[2] = {}, wetR[2] = {}, one[2] = {};
    for (int c = 0; c < 2; c++) {
        for (int v = 0; v < top; v++) {
            const SndNativeVoiceState *vs = block_voice(in, extra, c, v);
            if (vs == nullptr)
                continue;
            uint32_t f = (uint16_t)vs->flags;
            uint64_t bit = 1ull << v;
            if (f & SND_NATIVE_V_KEY_OFF)  koff[c] |= bit;
            if (f & SND_NATIVE_V_KEY_ON)   kon[c] |= bit;
            if (f & SND_NATIVE_V_PMON)     pmon[c] |= bit;
            if (f & SND_NATIVE_V_NOISE)    non[c] |= bit;
            if (f & SND_NATIVE_V_DRY_L)    dryL[c] |= bit;
            if (f & SND_NATIVE_V_WET_L)    wetL[c] |= bit;
            if (f & SND_NATIVE_V_DRY_R)    dryR[c] |= bit;
            if (f & SND_NATIVE_V_WET_R)    wetR[c] |= bit;
            if (f & SND_NATIVE_V_ONE_SHOT) one[c] |= bit;
        }
        s.active[c] |= kon[c];
        s.keyOffPending[c] = (s.keyOffPending[c] | koff[c]) & ~kon[c];
        s.oneShot[c] = (s.oneShot[c] & ~which) | one[c];
    }
    /* PC: the key-ons in the answer at once.  Scheduled, the answer comes
     * SCHED_STATUS_FRAMES after the block, and a render call ending before
     * that left the block's voices neither queued nor active until the next
     * call: snd_flush_voices took one for ended and gave it to the next note
     * or sound, cutting this one after a tick (user report: missed notes) */
    s.activeMask |= (kon[0] & ALL_VOICES) | ((kon[1] & ALL_VOICES) << 24);
    for (int e = 0; e < SND_NATIVE_EXTRA_VOICES; e++)
        if (kon[extra_core(e)] & (1ull << extra_voice(e)))
            s.activeExtra[e >> 6] |= 1ull << (e & 63);

    /* 0x0011F4: voices to key on start silent (their volume is set after
     * the key-on below, not with the other volumes) */
    for (int c = 0; c < 2; c++)
        for (int v = 0; v < top; v++)
            if (kon[c] & (1ull << v)) {
                p.set_voice_param(c, v, VP_VOLL, 0);
                p.set_voice_param(c, v, VP_VOLR, 0);
            }

    /* 0x001260: key-offs (pdispu2's switch thread applies them, then the
     * caller waits 100 us) */
    for (int c = 0; c < 2; c++)
        if (koff[c])
            p.set_switch64(c, SW_KOFF, koff[c], which);

    /* 0x001280: the effect attributes when they changed */
    if (in->dirty) {
        for (int c = 0; c < 2; c++) {
            const SndNativeEffectAttr &e = in->core[c].effect;
            p.set_effect_attr(c, e.mode, e.depthL, e.depthR, e.delay, e.feedback);
        }
    }

    /* 0x0012D8: per core: effect enable, MMIX, EVOL */
    for (int c = 0; c < 2; c++) {
        const SndNativeCoreState &cs = in->core[c];
        p.set_effect_enable(c, cs.effectEnable != 0);
        p.set_core_param(c, CP_MMIX, (uint16_t)cs.mmix);
        p.set_core_param(c, CP_EVOLL, cs.evolL);
        p.set_core_param(c, CP_EVOLR, cs.evolR);
    }

    /* 0x001334: per voice: pitch, volumes, start address + ADSR */
    for (int c = 0; c < 2; c++) {
        for (int v = 0; v < top; v++) {
            const SndNativeVoiceState *vs = block_voice(in, extra, c, v);
            if (vs == nullptr)
                continue;
            uint32_t f = (uint16_t)vs->flags;
            if (kon[c] & (1ull << v))
                f &= ~SND_NATIVE_V_VOLUME;
            if (f & SND_NATIVE_V_PITCH)
                p.set_voice_param(c, v, VP_PITCH, (uint16_t)vs->pitch);
            if (f & SND_NATIVE_V_VOLUME) {
                p.set_voice_param(c, v, VP_VOLL, vs->volL);
                p.set_voice_param(c, v, VP_VOLR, vs->volR);
            }
            if (f & SND_NATIVE_V_ADSR) {
                p.set_voice_addr(c, v, VA_SSA, (uint32_t)vs->addr);
                p.set_voice_param(c, v, VP_ADSR1, vs->adsr1);
                p.set_voice_param(c, v, VP_ADSR2, vs->adsr2);
            }
        }
        /* 0x0013F0: the core's switches */
        p.set_switch64(c, SW_PMON, pmon[c], which);
        p.set_switch64(c, SW_NON, non[c], which);
        p.set_switch64(c, SW_VMIXL, dryL[c], which);
        p.set_switch64(c, SW_VMIXEL, wetL[c], which);
        p.set_switch64(c, SW_VMIXR, dryR[c], which);
        p.set_switch64(c, SW_VMIXER, wetR[c], which);
    }

    /* PC: not the IOP's: the key-ons checked for the trace (GT3PC_SND_TRACE) */
    if (s.trace.level > 0)
        trace_key_ons_locked(in, extra, kon);

    /* 0x001464: key-ons, then their volumes */
    for (int c = 0; c < 2; c++)
        if (kon[c])
            p.set_switch64(c, SW_KON, kon[c], which);
    for (int c = 0; c < 2; c++)
        for (int v = 0; v < top; v++)
            if (kon[c] & (1ull << v)) {
                const SndNativeVoiceState *vs = block_voice(in, extra, c, v);
                p.set_voice_param(c, v, VP_VOLL, vs->volL);
                p.set_voice_param(c, v, VP_VOLR, vs->volR);
            }
}

/* Immediately (the tools; or scheduled with no clock yet): the block, the
 * answer, the trace. */
static void apply_now_locked(const SndNativeState *in, const SndNativeVoiceState *extra)
{
    State &s = st();
    apply_block_locked(in, extra);
    /* 0x0014E8: the answer */
    update_status_locked();
    /* PC: the trace's read reports and summary */
    if (s.trace.level > 0)
        trace_frame_locked();
}

void snd_native_send_state(const SndNativeState *in)
{
    snd_native_send_state_ext(in, nullptr, 0, -1.0);
}

void snd_native_send_state_ext(const SndNativeState *in, const SndNativeVoiceState *extra, int nExtra, double time)
{
    State &s = st();
    std::lock_guard<std::mutex> g(s.lock);
    if (nExtra < SND_NATIVE_EXTRA_VOICES)
        extra = nullptr;                    /* (all or none) */
    if (!s.scheduled || s.clock == nullptr) {
        apply_now_locked(in, extra);
        return;
    }
    /* PC: scheduled (above): queued with its vblank's time, monotonic */
    const double now = s.clock();
    if (time < 0.0)
        time = now;
    if (time < s.lastTime)
        time = s.lastTime;
    s.lastTime = time;
    if ((int)s.queue.size() >= SCHED_QUEUE_MAX) {
        /* the audio thread does not render (no device, a stall): the oldest
         * applied at once, so the state goes on */
        apply_now_locked(&s.queue.front().st, s.queue.front().hasExtra ? s.queue.front().extra : nullptr);
        s.queue.erase(s.queue.begin());
        s.dropped++;
    }
    s.queue.emplace_back();
    Pending &b = s.queue.back();
    b.st = *in;
    b.hasExtra = extra != nullptr;
    if (extra != nullptr)
        std::memcpy(b.extra, extra, sizeof(b.extra));
    b.time = time;
    /* how late it came (frames behind its vblank): the delay covers the
     * latest of these but SCHED_LATE_SKIP (a hitch's blocks apply late, the
     * delay stays) and a render period; a stall's (later than the delay
     * goes) is not counted */
    const float lateFrames = (float)((now - time) * OUTPUT_RATE);
    if (lateFrames < (float)SCHED_DELAY_MAX) {
        s.late[s.latePos] = lateFrames > 0.0f ? lateFrames : 0.0f;
        s.latePos = (s.latePos + 1) % SCHED_LATE_WINDOW;
        if (s.lateCount < SCHED_LATE_WINDOW)
            s.lateCount++;
    }
    float top[SCHED_LATE_SKIP + 1] = {};    /* the largest, in descending order */
    for (int i = 0; i < s.lateCount; i++) {
        float v = s.late[i];
        for (int k = 0; k <= SCHED_LATE_SKIP; k++)
            if (v > top[k]) {
                const float t = top[k];
                top[k] = v;
                v = t;
            }
    }
    int keep = s.lateCount - 1 < SCHED_LATE_SKIP ? s.lateCount - 1 : SCHED_LATE_SKIP;
    const float worst = keep < 0 ? 0.0f : top[keep];
    double want = worst + s.period + SCHED_MARGIN;
    if (want > SCHED_DELAY_MAX)
        want = SCHED_DELAY_MAX;
    if (want > s.delay)
        s.delay = want;                     /* at once: a later block must not find its sample past */
    else
        s.delay -= (s.delay - want) * 0.02; /* slowly back (no jumps in the blocks' spacing) */
}

uint64_t snd_native_voice_active_mask(void)
{
    State &s = st();
    std::lock_guard<std::mutex> g(s.lock);
    uint64_t mask = s.activeMask;
    uint64_t extra[2] = {};
    pending_key_ons_locked(&mask, extra);
    return mask;
}

void snd_native_voice_active_mask128(uint64_t out[2])
{
    State &s = st();
    std::lock_guard<std::mutex> g(s.lock);
    uint64_t mask = s.activeMask;
    uint64_t extra[2] = {s.activeExtra[0], s.activeExtra[1]};
    pending_key_ons_locked(&mask, extra);
    /* EE voices 0..45: the original answer's bits (46 / 47 were the stream
     * voices); 46.. the extra voices */
    const uint64_t low = mask & ((1ull << SND_NATIVE_EE_VOICES) - 1);
    out[0] = low | (extra[0] << SND_NATIVE_EE_VOICES);
    out[1] = (extra[0] >> (64 - SND_NATIVE_EE_VOICES)) | (extra[1] << SND_NATIVE_EE_VOICES);
}

/* 'SPUP' function 3 (0x001B34 -> 0x000F6C). */
void snd_native_request_status(void)
{
    std::lock_guard<std::mutex> g(st().lock);
    update_status_locked();
}

/* 'SPUP' function 5 (0x001B54 -> 0x001E54): sceSdSetCoreAttr(SPDIF_MODE,
 * ...) from the three words.  There is no S/PDIF output here. */
void snd_native_set_output_mode(int a, int b, int c)
{
    (void)a;
    (void)b;
    (void)c;
}

/* ------------------------------------------------------------------------ */
/* SPU RAM                                                                  */
/* ------------------------------------------------------------------------ */

/* PC: a bank's samples (music.ins: 1.2 MB) in pieces of this, the lock let
 * go between them: the audio thread's render waits for it, and it has
 * about 1 ms at the shortest buffers (user report: a crackle at the menu's
 * start, as its banks came in). */
constexpr uint32_t SPU_WRITE_PIECE = 64u << 10;

void snd_native_write_spu(uint32_t spuAddr, const void *src, uint32_t size)
{
    const uint8_t *p = static_cast<const uint8_t *>(src);
    while (size > 0) {
        const uint32_t n = size < SPU_WRITE_PIECE ? size : SPU_WRITE_PIECE;
        {
            std::lock_guard<std::mutex> g(st().lock);
            spu().write_ram(spuAddr, p, n);
        }
        spuAddr += n;
        p += n;
        size -= n;
    }
}

void snd_native_read_spu(uint32_t spuAddr, void *dst, uint32_t size)
{
    std::lock_guard<std::mutex> g(st().lock);
    spu().read_ram(spuAddr, dst, size);
}

/* engine/sound/spu_memory.h: the sample upload of file_read_sound_bank
 * ('PDIP' function 4) as the engine port calls it. */
void spu_memory_write(uint32_t spuAddr, const void *data, uint32_t size)
{
    snd_native_write_spu(spuAddr, data, size);
}

/* Not the IOP's: the sound memory's size (spu2.h RAM_BYTES; the SPU2's 2 MB
 * larger), for the game's placement of the banks. */
uint32_t snd_native_spu_bytes(void)
{
    return RAM_BYTES;
}

uint32_t spu_memory_size(void)
{
    return RAM_BYTES;
}

/* ------------------------------------------------------------------------ */
/* Diagnostics (GT3PC_SND_TRACE)                                            */
/* ------------------------------------------------------------------------ */

void snd_native_set_trace(int level, SndNativeTraceFn fn)
{
    State &s = st();
    std::lock_guard<std::mutex> g(s.lock);
    s.trace.level = fn != nullptr && level > 0 ? level : 0;
    s.trace.fn = fn;
    spu().set_checks(s.trace.level > 0);
}

int snd_native_trace_level(void)
{
    return st().trace.level;
}

void snd_native_name_region(uint32_t spuAddr, uint32_t size, const char *name)
{
    State &s = st();
    if (s.trace.level <= 0 || size == 0)
        return;
    std::lock_guard<std::mutex> g(s.lock);
    trace_upload_locked(spuAddr & (RAM_BYTES - 1), size, name);
}

/* engine/sound/spu_memory.h's names. */
int snd_trace_level(void)
{
    return snd_native_trace_level();
}

void spu_memory_name(uint32_t spuAddr, uint32_t size, const char *name)
{
    snd_native_name_region(spuAddr, size, name);
}

/* Not the IOP's: what an upload waits on (T523, sound_native.h). */
int snd_native_voices_reading(uint32_t spuAddr, uint32_t size)
{
    State &s = st();
    std::lock_guard<std::mutex> g(s.lock);
    Spu2 &p = spu();
    uint32_t start = spuAddr & (RAM_BYTES - 1), end = start + size;
    int n = 0;
    for (int c = 0; c < 2; c++) {
        for (int v = 0; v < VOICES_MAX; v++) {
            if (c == BGM_CORE && (v == BGM_VOICE_L || v == BGM_VOICE_R))
                continue;
            if (!p.voice_sounding(c, v))
                continue;
            uint32_t nax = p.get_voice_addr(c, v, VA_NAX);
            uint32_t ssa = p.get_voice_addr(c, v, VA_SSA);
            if ((nax >= start && nax < end) || (ssa >= start && ssa < end))
                n++;
        }
    }
    return n;
}

int spu_memory_voices_reading(uint32_t spuAddr, uint32_t size)
{
    return snd_native_voices_reading(spuAddr, size);
}

/* ---- the EE side's trace (level 2) ---- */

static Trace::Lost *lost_slot(Trace &t, const void *handle, bool create)
{
    uint32_t h = (uint32_t)(((uintptr_t)handle * 2654435761u) >> 22) & 1023;
    Trace::Lost *oldest = nullptr;
    for (int i = 0; i < 8; i++) {
        Trace::Lost &e = t.lost[(h + i) & 1023];
        if (e.handle == handle)
            return &e;
        if (!create)
            continue;
        if (e.handle == nullptr)
            return &e;
        if (oldest == nullptr || e.frame < oldest->frame)
            oldest = &e;
    }
    return oldest;
}

void snd_trace_lost(const void *handle, int reason, int voice, int prio, int byPrio)
{
    State &s = st();
    if (s.trace.level < 2 || handle == nullptr)
        return;
    std::lock_guard<std::mutex> g(s.lock);
    Trace::Lost *e = lost_slot(s.trace, handle, true);
    e->handle = handle;
    e->reason = reason;
    e->voice = voice;
    e->prio = prio;
    e->byPrio = byPrio;
    e->frame = s.trace.frameNo;
}

int snd_trace_lost_reason(const void *handle, int *voice, int *prio, int *byPrio, int *framesAgo)
{
    State &s = st();
    if (s.trace.level < 2)
        return 0;                           /* SND_LOST_NONE */
    std::lock_guard<std::mutex> g(s.lock);
    Trace::Lost *e = lost_slot(s.trace, handle, false);
    if (e == nullptr)
        return 0;                           /* SND_LOST_NONE */
    *voice = e->voice;
    *prio = e->prio;
    *byPrio = e->byPrio;
    *framesAgo = (int)(s.trace.frameNo - e->frame);
    int r = e->reason;
    e->handle = nullptr;                    /* read once */
    return r;
}

void snd_trace_line(const char *fmt, ...)
{
    State &s = st();
    if (s.trace.level < 2 || s.trace.fn == nullptr)
        return;
    std::lock_guard<std::mutex> g(s.lock);
    if (s.trace.eeLines >= (uint32_t)TRACE_EE_LINES_PER_SECOND) {
        s.trace.eeSuppressed++;
        return;
    }
    s.trace.eeLines++;
    char buf[1200];
    int n = std::snprintf(buf, sizeof(buf), "snd: t=%.3f ", (double)spu().ticks() / OUTPUT_RATE);
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    va_end(ap);
    s.trace.fn(buf);
}

/* ------------------------------------------------------------------------ */
/* 'PBGM'                                                                   */
/* ------------------------------------------------------------------------ */

/* pdistr 0x00049C: stops a stream that plays (voices, then the file). */
static void bgm_stop_all()
{
    State &s = st();
    {
        std::lock_guard<std::mutex> g(s.lock);
        if (s.bgmPlaying || s.bgmPaused)
            bgm_voices_stop_locked();
        spu().set_voice_source(BGM_CORE, BGM_VOICE_L, nullptr);
        spu().set_voice_source(BGM_CORE, BGM_VOICE_R, nullptr);
        s.bgmPlaying = false;
        s.bgmPaused = false;
        s.bgmPushPending = false;
    }
    bgm().close();
}

/* 'PBGM' function 1 (pdistr 0x000A70 -> 0x0004E8 -> the reader 0x000100
 * and pdispu2's stream thread 0x000498). */
int snd_native_bgm_start(SndNativeReadFn read, void *user, int loop)
{
    State &s = st();
    bgm_stop_all();
    if (!bgm().open(read, user, loop != 0, s.bgmThreaded))
        return -1;
    bgm().bind_voice(BGM_CORE, BGM_VOICE_L, 0);
    bgm().bind_voice(BGM_CORE, BGM_VOICE_R, 1);
    bgm().pump();                   /* the first chunks before the voices start */
    std::lock_guard<std::mutex> g(s.lock);
    bgm_voices_start_locked();
    s.bgmPlaying = true;
    s.bgmPaused = false;
    return 0;
}

/* 'PBGM' function 2. */
void snd_native_bgm_stop(void)
{
    bgm_stop_all();
}

/* 'PBGM' function 3 (pdistr 0x0006F8 -> pdispu2 export 10, 0x001D84): the
 * value is stored as the low halfword of the volume object's first word
 * (so 32768, volume 1.0, is 0x8000 = unity), then the voices' volumes are
 * recomputed. */
void snd_native_bgm_set_volume(int volume)
{
    State &s = st();
    std::lock_guard<std::mutex> g(s.lock);
    s.bgmVolume = (int32_t)(uint16_t)volume;
    bgm_apply_volume_locked();
}

/* 'PBGM' function 4 (pdistr 0x00074C -> pdispu2 export 47): the stream
 * thread sleeps instead of refilling; the IRQ handler then finds the next
 * half empty and stops the voices (0x000134).  Natively: at once. */
void snd_native_bgm_pause(void)
{
    State &s = st();
    std::lock_guard<std::mutex> g(s.lock);
    if (!s.bgmPlaying || s.bgmPaused)
        return;
    bgm().set_paused(true);
    bgm_voices_stop_locked();
    s.bgmPaused = true;
}

/* 'PBGM' function 5 (export 48): the thread wakes, refills and restarts
 * the voices (0x000068) where the stream stopped. */
void snd_native_bgm_resume(void)
{
    State &s = st();
    std::lock_guard<std::mutex> g(s.lock);
    if (!s.bgmPaused)
        return;
    bgm().set_paused(false);
    bgm_voices_start_locked();
    s.bgmPaused = false;
}

/* The movie audio (pdistr's 'MPG1' handler 0x00127C, open mode 2: the
 * demuxed audio into the same stream player, 0x000D0C). */
void snd_native_bgm_open_push(void)
{
    State &s = st();
    bgm_stop_all();
    bgm().open_push();
    bgm().bind_voice(BGM_CORE, BGM_VOICE_L, 0);
    bgm().bind_voice(BGM_CORE, BGM_VOICE_R, 1);
    std::lock_guard<std::mutex> g(s.lock);
    s.bgmPushPending = true;
}

/* Starts the voices of a pushed stream once enough is buffered. */
static void bgm_push_start_if_ready(bool atEnd)
{
    State &s = st();
    if (!s.bgmPushPending || !bgm().header_ready())
        return;
    if (!atEnd && bgm().buffered() < 2 * bgm().header().interleave)
        return;
    std::lock_guard<std::mutex> g(s.lock);
    if (!s.bgmPushPending)
        return;
    s.bgmPushPending = false;
    bgm_voices_start_locked();
    s.bgmPlaying = true;
    s.bgmPaused = false;
}

int snd_native_bgm_push(const void *data, uint32_t size)
{
    int taken = bgm().push(data, size);
    if (taken >= 0)
        bgm_push_start_if_ready(false);
    return taken;
}

void snd_native_bgm_push_end(void)
{
    bgm().push_end();
    bgm_push_start_if_ready(true);
}

int64_t snd_native_bgm_played_us(void)
{
    AdsStream &b = bgm();
    if (!b.header_ready() || b.play_rate() == 0)
        return -1;
    int64_t us = (int64_t)(b.played_samples() * 1000000u / b.play_rate());
    /* PC: what the device still holds of the rendered audio is not played
     * yet (snd_native_set_output_latency) */
    State &s = st();
    us -= (int64_t)s.outputLatency * 1000000 / OUTPUT_RATE;
    return us < 0 ? 0 : us;
}

/* ---- PC: an MP4 movie's PCM (sound_native.h) ---- */

/* A 15-bit fixed volume register as a gain (sweep mode: 1). */
static float fixed_gain(uint16_t reg)
{
    return (reg & 0x8000) ? 1.0f : (float)(int16_t)(uint16_t)(reg << 1) / 32768.0f;
}

void snd_native_pcm_open(int rate, int channels)
{
    bgm_stop_all();                         /* (the movie's sound takes the BGM's place) */
    State &s = st();
    std::lock_guard<std::mutex> g(s.lock);
    State::Pcm &m = s.pcm;
    m = State::Pcm();
    m.rate = rate > 0 ? rate : 48000;
    m.channels = channels >= 2 ? 2 : 1;
    m.cap = (size_t)m.rate * 2 + 4096;      /* about 2 s */
    m.ring.assign(m.cap * 2, 0.0f);
    m.open = true;
}

int snd_native_pcm_push(const int16_t *pcm, int frames)
{
    State &s = st();
    std::lock_guard<std::mutex> g(s.lock);
    State::Pcm &m = s.pcm;
    if (!m.open || frames <= 0)
        return 0;
    const size_t room = m.cap - m.count;
    const int n = (int)std::min<size_t>(room, (size_t)frames);
    size_t at = (m.head + m.count) % m.cap;
    for (int i = 0; i < n; i++) {
        const float l = pcm[i * m.channels] * (1.0f / 32768.0f);
        const float r = m.channels == 2 ? pcm[i * 2 + 1] * (1.0f / 32768.0f) : l;
        m.ring[at * 2] = l;
        m.ring[at * 2 + 1] = r;
        at = at + 1 == m.cap ? 0 : at + 1;
    }
    m.count += (size_t)n;
    return n;
}

int snd_native_pcm_buffered(void)
{
    State &s = st();
    std::lock_guard<std::mutex> g(s.lock);
    return s.pcm.open ? (int)s.pcm.count : 0;
}

void snd_native_pcm_end(void)
{
    State &s = st();
    std::lock_guard<std::mutex> g(s.lock);
    s.pcm.ended = true;
}

void snd_native_pcm_start(void)
{
    State &s = st();
    std::lock_guard<std::mutex> g(s.lock);
    if (s.pcm.open)
        s.pcm.started = true;
}

void snd_native_pcm_pause(int on)
{
    State &s = st();
    std::lock_guard<std::mutex> g(s.lock);
    s.pcm.paused = on != 0;
}

void snd_native_pcm_stop(void)
{
    State &s = st();
    std::lock_guard<std::mutex> g(s.lock);
    s.pcm = State::Pcm();
}

int64_t snd_native_pcm_played_us(void)
{
    State &s = st();
    std::lock_guard<std::mutex> g(s.lock);
    if (!s.pcm.open || !s.pcm.started)
        return -1;
    int64_t us = (int64_t)(s.pcm.consumed * 1000000u / (uint64_t)s.pcm.rate);
    us -= (int64_t)s.outputLatency * 1000000 / OUTPUT_RATE;    /* (as the BGM's) */
    return us < 0 ? 0 : us;
}

/* The PCM into out (added), resampled to 48 kHz (4-point Catmull-Rom), at
 * the BGM voices' gain: the BGM volume's chain (the voices' VOLL / VOLR)
 * and core 1's master volume (BGM_CORE).  Starved (fewer than 3 frames
 * before its end): silence until more comes. */
static void pcm_mix_locked(float *out, int frames)
{
    State &s = st();
    State::Pcm &m = s.pcm;
    if (!m.open || !m.started || m.paused || m.count == 0)
        return;
    const float vol = (float)vol_chain(s.bgmVolume, VOL_UNIT, VOL_UNIT, VOL_UNIT) / 32768.0f;
    const float gain[2] = {vol * fixed_gain(spu().get_core_param(BGM_CORE, CP_MVOLL)),
                           vol * fixed_gain(spu().get_core_param(BGM_CORE, CP_MVOLR))};
    const double step = (double)m.rate / OUTPUT_RATE;
    auto at = [&m](size_t k, int c) {       /* frame head + k (the last one past the end) */
        if (k >= m.count)
            k = m.count - 1;
        return m.ring[((m.head + k) % m.cap) * 2 + c];
    };
    for (int i = 0; i < frames; i++) {
        if (m.count == 0 || (m.count < 3 && !m.ended))
            break;
        const float t = (float)m.frac;
        for (int c = 0; c < 2; c++) {
            const float p0 = m.hist[0][c], p1 = at(0, c), p2 = at(1, c), p3 = at(2, c);
            const float v = p1 + 0.5f * t * (p2 - p0 + t * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 +
                                                             t * (3.0f * (p1 - p2) + p3 - p0)));
            out[2 * i + c] += v * gain[c];
        }
        m.frac += step;
        while (m.frac >= 1.0 && m.count > 0) {
            m.hist[0][0] = at(0, 0);
            m.hist[0][1] = at(0, 1);
            m.head = m.head + 1 == m.cap ? 0 : m.head + 1;
            m.count--;
            m.consumed++;
            m.frac -= 1.0;
        }
    }
}

/* Not the IOP's: the scheduled engine (SCHED_* above).  Off by default (the
 * test tools mix right after each block themselves); the game's audio
 * output turns it on, with its clock. */
void snd_native_set_scheduled(int on, double (*clock)(void))
{
    State &s = st();
    std::lock_guard<std::mutex> g(s.lock);
    if (!on || clock == nullptr) {
        while (!s.queue.empty()) {
            apply_now_locked(&s.queue.front().st, s.queue.front().hasExtra ? s.queue.front().extra : nullptr);
            s.queue.erase(s.queue.begin());
        }
    }
    s.scheduled = on != 0 && clock != nullptr;
    s.clock = clock;
    s.offValid = false;
    s.delay = 0.0;
    s.lateCount = s.latePos = 0;
    spu().set_headroom(s.scheduled);
}

void snd_native_set_output_latency(int frames)
{
    State &s = st();
    std::lock_guard<std::mutex> g(s.lock);
    s.outputLatency = frames > 0 ? frames : 0;
}

void snd_native_schedule_info(SndNativeScheduleInfo *out)
{
    State &s = st();
    std::lock_guard<std::mutex> g(s.lock);
    out->delayMs = s.delay * 1000.0 / OUTPUT_RATE;
    out->queued = (int)s.queue.size();
    out->blocks = s.blocks;
    out->lateBlocks = s.lateBlocks;
    out->droppedBlocks = s.dropped;
}

/* 'PBGM' function 6 (pdistr 0x000B18): the clock of the stream started
 * last (zeros before the first start). */
void snd_native_bgm_get_status(SndNativeBgmStatus *out)
{
    std::memset(out, 0, sizeof(*out));
    AdsClock c = bgm().clock();
    out->time = c.timeUs;
    out->pos = (int32_t)c.pos;
    out->unk_0C = 0;
    out->id = (int32_t)c.loops;
    out->ended = c.ended ? 1 : 0;
}

/* ------------------------------------------------------------------------ */
/* Output                                                                   */
/* ------------------------------------------------------------------------ */

void snd_native_mix(int16_t *out, int frames)
{
    State &s = st();
    if (s.scheduled) {
        /* (the game's output renders with snd_native_render; a caller of
         * this one gets the same, converted and clamped) */
        while (frames > 0) {
            const int n = frames < 256 ? frames : 256;
            float f[2 * 256];
            snd_native_render(f, n, s.clock != nullptr ? s.clock() : 0.0);
            for (int i = 0; i < 2 * n; i++) {
                const float v = f[i] * 32768.0f;
                out[i] = (int16_t)(v < -32768.0f ? -32768 : (v > 32767.0f ? 32767 : (int)v));
            }
            out += 2 * n;
            frames -= n;
        }
        return;
    }
    while (frames > 0) {
        int n = frames < 64 ? frames : 64;
        {
            std::lock_guard<std::mutex> g(s.lock);
            spu().mix(out, n);
        }
        out += 2 * n;
        frames -= n;
    }
}

/* PC: the scheduled engine's render (SCHED_* above).  The render clock: the
 * frame this call starts at against `now', fitted (the calls jitter by a
 * fraction of their period; the device's clock drifts against the
 * system's).  Each queued block is applied at the frame its time maps to
 * plus the delay - exactly there, the mixing split around it - or at once
 * when that frame has passed (late).  The answer is computed
 * SCHED_STATUS_FRAMES after a block, as pdispu2's came ~200 us after it. */
/* PC (a diagnostic): the longest wait for the sound lock in a render since
 * the last snd_native_take_render_lock_wait (ns; the audio thread's alone
 * writes it). */
static std::atomic<int64_t> g_renderLockWait{0};

double snd_native_take_render_lock_wait(void)
{
    return (double)g_renderLockWait.exchange(0) / 1e6;
}

void snd_native_render(float *out, int frames, double now)
{
    State &s = st();
    const auto t0 = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> g(s.lock);
    const int64_t waited =
        (int64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    if (waited > g_renderLockWait.load(std::memory_order_relaxed))
        g_renderLockWait.store(waited, std::memory_order_relaxed);
    Spu2 &p = spu();
    if (frames <= 0)
        return;
    if (frames > s.period)
        s.period = frames;
    else
        s.period += (frames - s.period) * 0.002;    /* (an output change: the new period's in a few s) */

    /* the fit: frame = now x rate + off */
    const double measured = (double)s.frame - now * OUTPUT_RATE;
    if (!s.offValid || measured - s.off > 2400.0 || s.off - measured > 2400.0) {
        s.off = measured;                   /* (a start, a device change, a stall: from here) */
        s.offValid = true;
    } else {
        s.off += (measured - s.off) * 0.01;
    }

    int done = 0;
    while (done < frames) {
        const double at = (double)s.frame + done;
        /* the blocks due by this frame (PC: and not held: SCHED_MIN_NOTE) */
        while (!s.queue.empty()) {
            Pending &b = s.queue.front();
            const SndNativeVoiceState *extra = b.hasExtra ? b.extra : nullptr;
            const double target = b.time * OUTPUT_RATE + s.off + s.delay;
            if (target > at + 0.5)
                break;
            uint64_t kon[2], koff[2];
            block_keys(&b.st, extra, kon, koff);
            if (key_hold_locked(kon, koff) > at + 0.5) {
                b.held = true;
                break;
            }
            if (b.held)
                s.heldBlocks++;
            if (target < at - 1.0)
                s.lateBlocks++;
            apply_block_locked(&b.st, extra);
            for (int c = 0; c < 2; c++)
                for (int v = 0; v < VOICES_MAX; v++)
                    if (kon[c] & (1ull << v))
                        s.keyOnFrame[c][v] = (uint64_t)at + 1;
            s.queue.erase(s.queue.begin());
            s.blocks++;
            s.statusIn = SCHED_STATUS_FRAMES;
            if (s.trace.level > 0)
                trace_frame_locked();
        }
        /* render up to the next block (PC: or the end of its hold), the
         * status point or the end */
        int n = frames - done;
        if (!s.queue.empty()) {
            const Pending &b = s.queue.front();
            uint64_t kon[2], koff[2];
            block_keys(&b.st, b.hasExtra ? b.extra : nullptr, kon, koff);
            const double target = std::max(b.time * OUTPUT_RATE + s.off + s.delay, key_hold_locked(kon, koff));
            const double until = target - at;
            if (until < (double)n)
                n = until < 1.0 ? 1 : (int)(until + 0.5);
        }
        if (s.statusIn > 0 && s.statusIn < n)
            n = s.statusIn;
        p.mix_float(out + 2 * done, n);
        done += n;
        if (s.statusIn > 0) {
            s.statusIn -= n;
            if (s.statusIn <= 0) {
                update_status_locked();
                s.statusIn = -1;
            }
        }
    }
    pcm_mix_locked(out, frames);            /* PC: an MP4 movie's sound */
    s.frame += (uint64_t)frames;
}

/* ------------------------------------------------------------------------ */
/* Test access                                                              */
/* ------------------------------------------------------------------------ */

spu2::Spu2 &snd_native_spu()
{
    return spu();
}

spu2::AdsStream &snd_native_bgm()
{
    return bgm();
}

void snd_native_lock()
{
    st().lock.lock();
}

void snd_native_unlock()
{
    st().lock.unlock();
}

void snd_native_set_bgm_threaded(bool threaded)
{
    st().bgmThreaded = threaded;
}
