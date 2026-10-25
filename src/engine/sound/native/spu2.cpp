/* spu2.cpp - the native SPU2 sound generator (see spu2.h). */
#include "engine/sound/native/spu2.h"

#include <cmath>
#include <cstring>

namespace spu2 {

/* ------------------------------------------------------------------------ */
/* Gaussian interpolation table                                             */
/* ------------------------------------------------------------------------ */

/* The SPU's interpolation table is a windowed sinc with 512 entries; the
 * construction below (published by nocash in psx-spx and by Near, from a
 * dump of the PS1 SPU's table) gives the hardware values exactly: each
 * phase's four weights are spread so they sum to 0x7F80.  Stored as 256
 * rows of the four weights of a phase, oldest sample first. */
static int16_t g_gauss[256][4];

static bool build_gauss()
{
    const double pi = 3.14159265358979323846;
    double t[512];
    double sum = 0.0;
    for (int n = 0; n < 512; n++) {
        double k = 0.5 + n;
        double s = std::sin(pi * k * 2.048 / 1024.0);
        double c1 = (std::cos(pi * k * 2.000 / 1023.0) - 1.0) * 0.50;
        double c2 = (std::cos(pi * k * 4.000 / 1023.0) - 1.0) * 0.08;
        t[511 - n] = s * (c1 + c2 + 1.0) / k;
    }
    for (double v : t)
        sum += v;
    double scale = 0x7F80 * 128 / sum;
    for (double &v : t)
        v *= scale;
    for (int ph = 0; ph < 256; ph++) {
        double w[4] = { t[ph], t[ph + 256], t[511 - ph], t[255 - ph] };
        double d = (w[0] + w[1] + w[2] + w[3] - 0x7F80) / 4.0;
        for (int i = 0; i < 4; i++)
            g_gauss[255 - ph][i] = (int16_t)std::round(w[i] - d);    /* half away from zero */
    }
    return true;
}

const int16_t *gauss_table()
{
    static const bool built = build_gauss();
    (void)built;
    return &g_gauss[0][0];
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

static inline int32_t clamp16(int32_t v)
{
    return v < -32768 ? -32768 : (v > 32767 ? 32767 : v);
}

static inline int32_t vol15(int32_t x, int32_t vol)
{
    return (int32_t)(((int64_t)x * vol) >> 15);    /* (64-bit: unclamped sums with headroom) */
}

/* A fixed-mode volume register (bit 15 clear): 15-bit signed, x 2. */
static inline int32_t fixed_volume(uint16_t reg)
{
    return (int16_t)(uint16_t)(reg << 1);
}

/* ------------------------------------------------------------------------ */
/* Spu2                                                                     */
/* ------------------------------------------------------------------------ */

Spu2::Spu2()
{
    ram_ = new uint16_t[RAM_WORDS];
    written_ = new uint32_t[WRITTEN_WORDS];
    gauss_table();
    reset();
}

Spu2::~Spu2()
{
    delete[] written_;
    delete[] ram_;
}

void Spu2::reset()
{
    std::memset(ram_, 0, RAM_WORDS * sizeof(uint16_t));
    std::memset(written_, 0, WRITTEN_WORDS * sizeof(uint32_t));
    reports_.clear();
    for (int c = 0; c < NUM_CORES; c++) {
        Core &core = cores_[c];
        for (Voice &v : core.voice)
            v = Voice();
        core.pmon = core.non = core.endx = 0;
        /* reset values (PCSX2 documents them as measured after sceSdInit;
         * PC: the pool's voices 24..63 alike) */
        core.vmixl = core.vmixr = ~0ull;
        core.vmixel = core.vmixer = ~0ull;
        core.top = VOICES_PER_CORE;
        core.mmix = c ? 0xFFC : 0xFF0;
        core.regMvolL = core.regMvolR = 0;
        core.mvolL = core.mvolR = 0;
        core.evolL = core.evolR = 0;
        core.avolL = core.avolR = 0x7FFF;
        core.bvolL = core.bvolR = 0x7FFF;
        core.fxEnable = false;
        core.reverb.reset(c ? 0xFFFFF : 0x7FFFF);
    }
    ticks_ = 0;
}

void Spu2::write_ram(uint32_t byteAddr, const void *src, uint32_t size)
{
    const uint8_t *s = (const uint8_t *)src;
    uint8_t *ram = (uint8_t *)ram_;
    byteAddr &= RAM_BYTES - 1;
    if (size <= RAM_BYTES - byteAddr) {
        std::memcpy(ram + byteAddr, s, size);       /* (banks of megabytes: no byte loop) */
    } else {
        for (uint32_t i = 0; i < size; i++)         /* past the end: wraps, as the hardware's addresses */
            ram[(byteAddr + i) & (RAM_BYTES - 1)] = s[i];
    }
    /* diagnostics: the blocks touched count as uploaded */
    if (size > 0) {
        uint32_t first = byteAddr / ADPCM_BLOCK_BYTES;
        uint32_t last = (byteAddr + size - 1) / ADPCM_BLOCK_BYTES;
        for (uint32_t b = first; b <= last; b++) {
            uint32_t k = b & (RAM_BYTES / ADPCM_BLOCK_BYTES - 1);
            written_[k >> 5] |= 1u << (k & 31);
        }
    }
}

bool Spu2::written(uint32_t byteAddr) const
{
    uint32_t k = (byteAddr & (RAM_BYTES - 1)) / ADPCM_BLOCK_BYTES;
    return (written_[k >> 5] >> (k & 31)) & 1;
}

void Spu2::take_reports(std::vector<ReadReport> &out)
{
    out.insert(out.end(), reports_.begin(), reports_.end());
    reports_.clear();
}

bool Spu2::voice_sounding(int core, int voice) const
{
    return cores_[core & 1].voice[voice & (VOICES_MAX - 1)].phase != PH_OFF;
}

/* Diagnostics: one report per key-on, kept until sound_native takes them. */
void Spu2::report(int core, int v, int problem, uint32_t addr, const uint8_t *head)
{
    Voice &vc = cores_[core].voice[v];
    vc.reported = true;
    if (reports_.size() >= 64)
        return;
    ReadReport r;
    r.core = core;
    r.voice = v;
    r.problem = problem;
    r.ssa = vc.ssa << 1;
    r.addr = addr;
    r.head[0] = head ? head[0] : 0;
    r.head[1] = head ? head[1] : 0;
    r.tick = ticks_;
    reports_.push_back(r);
}

void Spu2::read_ram(uint32_t byteAddr, void *dst, uint32_t size) const
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *ram = (const uint8_t *)ram_;
    byteAddr &= RAM_BYTES - 1;
    if (size <= RAM_BYTES - byteAddr) {
        std::memcpy(d, ram + byteAddr, size);
        return;
    }
    for (uint32_t i = 0; i < size; i++)
        d[i] = ram[(byteAddr + i) & (RAM_BYTES - 1)];
}

void Spu2::set_voice_param(int core, int voice, VoiceParam p, uint16_t value)
{
    Voice &v = cores_[core & 1].voice[voice & (VOICES_MAX - 1)];
    switch (p) {
    case VP_VOLL:
        v.regVolL = value;
        if (!(value & 0x8000))          /* sweep mode (bit 15) is not modelled */
            v.volL = fixed_volume(value);
        break;
    case VP_VOLR:
        v.regVolR = value;
        if (!(value & 0x8000))
            v.volR = fixed_volume(value);
        break;
    case VP_PITCH:
        v.pitch = value;
        break;
    case VP_ADSR1:
        v.adsr1 = value;
        break;
    case VP_ADSR2:
        v.adsr2 = value;
        break;
    default:
        break;
    }
}

uint16_t Spu2::get_voice_param(int core, int voice, VoiceParam p) const
{
    const Voice &v = cores_[core & 1].voice[voice & (VOICES_MAX - 1)];
    switch (p) {
    case VP_VOLL:  return v.regVolL;
    case VP_VOLR:  return v.regVolR;
    case VP_PITCH: return v.pitch;
    case VP_ADSR1: return v.adsr1;
    case VP_ADSR2: return v.adsr2;
    case VP_ENVX:  return (uint16_t)v.env;
    case VP_VOLXL: return (uint16_t)v.volL;
    case VP_VOLXR: return (uint16_t)v.volR;
    }
    return 0;
}

void Spu2::set_voice_addr(int core, int voice, VoiceAddr a, uint32_t byteAddr)
{
    Voice &v = cores_[core & 1].voice[voice & (VOICES_MAX - 1)];
    uint32_t w = (byteAddr >> 1) & (RAM_WORDS - 1) & ~7u;
    switch (a) {
    case VA_SSA:
        v.ssa = w;
        break;
    case VA_LSAX:
        v.lsa = w;
        v.lsaWritten = true;
        break;
    case VA_NAX:
        break;                          /* read only */
    }
}

uint32_t Spu2::get_voice_addr(int core, int voice, VoiceAddr a) const
{
    const Voice &v = cores_[core & 1].voice[voice & (VOICES_MAX - 1)];
    switch (a) {
    case VA_SSA:  return v.ssa << 1;
    case VA_LSAX: return v.lsa << 1;
    case VA_NAX:  return v.nax << 1;
    }
    return 0;
}

void Spu2::set_switch(int core, Switch s, uint32_t mask)
{
    set_switch64(core, s, mask, ALL_VOICES);
}

uint32_t Spu2::get_switch(int core, Switch s) const
{
    return (uint32_t)(get_switch64(core, s) & ALL_VOICES);
}

void Spu2::set_switch64(int core, Switch s, uint64_t mask, uint64_t which)
{
    Core &c = cores_[core & 1];
    mask &= which;
    auto put = [&](uint64_t &reg) { reg = (reg & ~which) | mask; };
    switch (s) {
    case SW_PMON:   put(c.pmon); c.pmon &= ~1ull; break;    /* voice 0 has no previous voice */
    case SW_NON:    put(c.non); break;
    case SW_VMIXL:  put(c.vmixl); break;
    case SW_VMIXEL: put(c.vmixel); break;
    case SW_VMIXR:  put(c.vmixr); break;
    case SW_VMIXER: put(c.vmixer); break;
    case SW_KOFF:
        for (int v = 0; v < VOICES_MAX; v++)
            if (mask & (1ull << v))
                key_off(core & 1, v);
        break;
    case SW_KON:
        for (int v = 0; v < VOICES_MAX; v++)
            if (mask & (1ull << v)) {
                key_on(core & 1, v);
                if (v + 1 > c.top)
                    c.top = v + 1;
            }
        break;
    case SW_ENDX:
        break;                          /* read only (cleared by key-on) */
    }
}

uint64_t Spu2::get_switch64(int core, Switch s) const
{
    const Core &c = cores_[core & 1];
    switch (s) {
    case SW_PMON:   return c.pmon;
    case SW_NON:    return c.non;
    case SW_ENDX:   return c.endx;
    case SW_VMIXL:  return c.vmixl;
    case SW_VMIXEL: return c.vmixel;
    case SW_VMIXR:  return c.vmixr;
    case SW_VMIXER: return c.vmixer;
    default:        return 0;
    }
}

void Spu2::set_core_param(int core, CoreParam p, uint16_t value)
{
    Core &c = cores_[core & 1];
    switch (p) {
    case CP_MMIX:  c.mmix = value & 0xFFF; break;
    case CP_MVOLL: c.regMvolL = value; if (!(value & 0x8000)) c.mvolL = fixed_volume(value); break;
    case CP_MVOLR: c.regMvolR = value; if (!(value & 0x8000)) c.mvolR = fixed_volume(value); break;
    case CP_EVOLL: c.evolL = (int16_t)value; break;
    case CP_EVOLR: c.evolR = (int16_t)value; break;
    case CP_AVOLL: c.avolL = (int16_t)value; break;
    case CP_AVOLR: c.avolR = (int16_t)value; break;
    case CP_BVOLL: c.bvolL = (int16_t)value; break;
    case CP_BVOLR: c.bvolR = (int16_t)value; break;
    }
}

uint16_t Spu2::get_core_param(int core, CoreParam p) const
{
    const Core &c = cores_[core & 1];
    switch (p) {
    case CP_MMIX:  return c.mmix;
    case CP_MVOLL: return c.regMvolL;
    case CP_MVOLR: return c.regMvolR;
    case CP_EVOLL: return (uint16_t)c.evolL;
    case CP_EVOLR: return (uint16_t)c.evolR;
    case CP_AVOLL: return (uint16_t)c.avolL;
    case CP_AVOLR: return (uint16_t)c.avolR;
    case CP_BVOLL: return (uint16_t)c.bvolL;
    case CP_BVOLR: return (uint16_t)c.bvolR;
    }
    return 0;
}

void Spu2::set_core_addr(int core, CoreAddr a, uint32_t byteAddr)
{
    Core &c = cores_[core & 1];
    if (a == CA_EEA) {
        /* only the top bits are a register: the area ends at a 128 KB
         * boundary minus one */
        c.reverb.set_end(((byteAddr >> 17) << 16) | 0xFFFF);
        c.reverb.set_mode(c.reverb.mode());
    }
    /* ESA is placed by set_effect_attr below EEA, as libsd does */
}

void Spu2::set_effect_enable(int core, bool on)
{
    cores_[core & 1].fxEnable = on;
}

void Spu2::set_effect_attr(int core, int mode, int16_t depthL, int16_t depthR, int delay, int feedback)
{
    (void)delay;
    (void)feedback;
    Core &c = cores_[core & 1];
    int m = mode & 0xFF;
    if (m >= EFFECT_MODE_COUNT)
        return;
    if (mode & 0x100)
        c.reverb.clear_area(ram_, m);
    c.reverb.set_mode(m);
    c.evolL = depthL;
    c.evolR = depthR;
}

void Spu2::clear_effect_area(int core, int mode)
{
    cores_[core & 1].reverb.clear_area(ram_, mode & 0xFF);
}

void Spu2::set_voice_source(int core, int voice, VoiceSource *src)
{
    cores_[core & 1].voice[voice & (VOICES_MAX - 1)].source = src;
}

/* ------------------------------------------------------------------------ */
/* Voices                                                                   */
/* ------------------------------------------------------------------------ */

void Spu2::key_on(int core, int v)
{
    Voice &vc = cores_[core].voice[v];
    cores_[core].endx &= ~(1ull << v);
    vc.nax = vc.ssa;
    vc.haveBlock = false;
    vc.blockFlags = 0;
    vc.decPos = ADPCM_BLOCK_SAMPLES;
    vc.hist = AdpcmHistory();
    std::memset(vc.pv, 0, sizeof(vc.pv));
    vc.counter = 0;
    vc.lsaWritten = false;
    vc.lsaSeen = false;
    vc.reported = false;
    vc.phase = PH_ATTACK;
    vc.env = 0;
    vc.envCounter = 0;
    vc.outX = 0;
}

void Spu2::key_off(int core, int v)
{
    Voice &vc = cores_[core].voice[v];
    if (vc.phase != PH_OFF) {
        vc.phase = PH_RELEASE;
        vc.envCounter = 0;
    }
}

/* The next sample of the voice's stream; false when the voice stopped at a
 * loop end without the repeat flag (or its source ended). */
bool Spu2::next_sample(int core, int v, int16_t &s)
{
    Voice &vc = cores_[core].voice[v];
    if (vc.decPos >= ADPCM_BLOCK_SAMPLES) {
        uint8_t block[ADPCM_BLOCK_BYTES];
        if (vc.source != nullptr) {
            if (!vc.source->next_block(core, v, block)) {
                cores_[core].endx |= 1ull << v;
                vc.phase = PH_OFF;
                vc.env = 0;
                return false;
            }
            vc.blockFlags = ADPCM_FLAG_LOOP;
        } else {
            if (vc.haveBlock) {
                if (vc.blockFlags & ADPCM_FLAG_LOOP_END) {
                    cores_[core].endx |= 1ull << v;
                    if (checks_ && !vc.reported && (vc.blockFlags & ADPCM_FLAG_LOOP) && !vc.lsaSeen &&
                        !vc.lsaWritten)
                        report(core, v, READ_STALE_LOOP, vc.lsa << 1, nullptr);
                    vc.nax = vc.lsa;
                    if (!(vc.blockFlags & ADPCM_FLAG_LOOP)) {
                        vc.phase = PH_OFF;
                        vc.env = 0;
                        return false;
                    }
                } else {
                    vc.nax = (vc.nax + 8) & (RAM_WORDS - 1);
                }
            }
            const uint8_t *p = (const uint8_t *)&ram_[vc.nax & ~7u];
            std::memcpy(block, p, ADPCM_BLOCK_BYTES);
            vc.blockFlags = block[1];
            if ((vc.blockFlags & ADPCM_FLAG_LOOP_START) && !vc.lsaWritten) {
                vc.lsa = vc.nax & ~7u;
                vc.lsaSeen = true;
            }
            if (checks_ && !vc.reported) {
                uint32_t at = (vc.nax & ~7u) << 1;
                if (!written(at))
                    report(core, v, READ_UNWRITTEN, at, block);
                else if (!adpcm_header_plausible(block))
                    report(core, v, READ_NOT_ADPCM, at, block);
            }
        }
        vc.haveBlock = true;
        adpcm_decode_block(block, vc.dec, vc.hist);
        vc.decPos = 0;
    }
    s = vc.dec[vc.decPos++];
    return true;
}

void Spu2::envelope_tick(Voice &vc)
{
    int shift, step;
    bool expo, decr;
    int32_t target;

    switch (vc.phase) {
    case PH_ATTACK:
        expo = (vc.adsr1 & 0x8000) != 0;
        decr = false;
        shift = (vc.adsr1 >> 10) & 0x1F;
        step = 7 - ((vc.adsr1 >> 8) & 3);
        target = 0x7FFF;
        break;
    case PH_DECAY:
        expo = true;
        decr = true;
        shift = (vc.adsr1 >> 4) & 0x0F;
        step = -8;
        target = ((vc.adsr1 & 0x0F) + 1) << 11;
        break;
    case PH_SUSTAIN:
        expo = (vc.adsr2 & 0x8000) != 0;
        decr = (vc.adsr2 & 0x4000) != 0;
        shift = (vc.adsr2 >> 8) & 0x1F;
        step = 7 - ((vc.adsr2 >> 6) & 3);
        if (decr)
            step = ~step;                   /* -8 .. -5 */
        target = 0;
        break;
    case PH_RELEASE:
        expo = (vc.adsr2 & 0x0020) != 0;
        decr = true;
        shift = vc.adsr2 & 0x1F;
        step = -8;
        target = 0;
        break;
    default:
        vc.env = 0;
        return;
    }

    uint32_t counterInc = 0x8000u >> (shift > 11 ? shift - 11 : 0);
    int32_t levelInc = step * (1 << (shift < 11 ? 11 - shift : 0));
    if (expo) {
        if (!decr && vc.env > 0x6000)
            counterInc >>= 2;
        if (decr)
            levelInc = (int16_t)((levelInc * vc.env) >> 15);
    }
    if (counterInc == 0)
        counterInc = 1;
    vc.envCounter += counterInc;
    if (vc.envCounter >= 0x8000) {
        vc.envCounter = 0;
        int32_t e = vc.env + levelInc;
        vc.env = e < 0 ? 0 : (e > 0x7FFF ? 0x7FFF : e);
    }

    switch (vc.phase) {
    case PH_ATTACK:
        if (vc.env >= target)
            vc.phase = PH_DECAY;
        break;
    case PH_DECAY:
        if (vc.env <= target)
            vc.phase = PH_SUSTAIN;
        break;
    case PH_SUSTAIN:
        if (vc.env == 0)                    /* a decreasing sustain that reached 0 */
            vc.phase = PH_OFF;
        break;
    case PH_RELEASE:
        if (vc.env <= 0)
            vc.phase = PH_OFF;
        break;
    }
}

/* One 48 kHz tick of a voice: its sample after the envelope (OUTX). */
int32_t Spu2::voice_tick(int core, int v, int32_t prevOutX)
{
    Voice &vc = cores_[core].voice[v];
    if (vc.phase == PH_OFF) {
        vc.outX = 0;
        return 0;
    }

    int32_t pitch = vc.pitch > 0x3FFF ? 0x3FFF : vc.pitch;
    if (cores_[core].pmon & (1ull << v)) {
        pitch = (pitch * (32768 + prevOutX)) >> 15;
        pitch = pitch < 0 ? 0 : (pitch > 0x3FFF ? 0x3FFF : pitch);
    }
    vc.counter += (uint32_t)pitch;
    while (vc.counter >= 0x1000) {
        int16_t s;
        vc.counter -= 0x1000;
        if (!next_sample(core, v, s)) {
            vc.outX = 0;
            return 0;                       /* stopped at its end: silent from this tick */
        }
        vc.pv[0] = vc.pv[1];
        vc.pv[1] = vc.pv[2];
        vc.pv[2] = vc.pv[3];
        vc.pv[3] = s;
    }

    const int16_t *g = &g_gauss[(vc.counter >> 4) & 0xFF][0];
    int32_t val = ((g[0] * vc.pv[0]) >> 15) + ((g[1] * vc.pv[1]) >> 15)
                + ((g[2] * vc.pv[2]) >> 15) + ((g[3] * vc.pv[3]) >> 15);

    envelope_tick(vc);
    val = vol15(val, vc.env);
    vc.outX = val;
    return val;
}

void Spu2::mix_frame(int32_t &outL, int32_t &outR)
{
    int32_t extL = 0, extR = 0;             /* core 0's output, core 1's external input */
    int32_t resL = 0, resR = 0;

    /* PC: headroom (set_headroom): the sums kept whole, only the reverb's
     * input clamped (its work area holds 16-bit samples) */
    auto sat = [this](int32_t v) { return headroom_ ? v : clamp16(v); };
    for (int c = 0; c < NUM_CORES; c++) {
        Core &core = cores_[c];
        int32_t dryL = 0, dryR = 0, wetL = 0, wetR = 0;
        int32_t prev = 0;
        for (int v = 0; v < core.top; v++) {
            int32_t x = voice_tick(c, v, prev);
            prev = x;
            if (x == 0)
                continue;
            Voice &vc = core.voice[v];
            int32_t l = vol15(x, vc.volL);
            int32_t r = vol15(x, vc.volR);
            uint64_t bit = 1ull << v;
            if (core.vmixl & bit)  dryL += l;
            if (core.vmixr & bit)  dryR += r;
            if (core.vmixel & bit) wetL += l;
            if (core.vmixer & bit) wetR += r;
        }
        dryL = sat(dryL);
        dryR = sat(dryR);
        wetL = sat(wetL);
        wetR = sat(wetR);

        /* the sound data input (ADMA) is not used by GT3: silent */
        int32_t tdL = 0, tdR = 0, twL = 0, twR = 0;
        if (core.mmix & MMIX_DRY_SND_L) tdL += dryL;
        if (core.mmix & MMIX_DRY_SND_R) tdR += dryR;
        if (core.mmix & MMIX_WET_SND_L) twL += wetL;
        if (core.mmix & MMIX_WET_SND_R) twR += wetR;
        if (c == 1) {
            if (core.mmix & MMIX_DRY_EXT_L) tdL += extL;
            if (core.mmix & MMIX_DRY_EXT_R) tdR += extR;
            if (core.mmix & MMIX_WET_EXT_L) twL += extL;
            if (core.mmix & MMIX_WET_EXT_R) twR += extR;
        }

        int32_t rvL, rvR;
        core.reverb.tick(clamp16(twL), clamp16(twR), rvL, rvR, ram_, core.fxEnable);
        int32_t oL = sat(tdL + vol15(rvL, core.evolL));
        int32_t oR = sat(tdR + vol15(rvR, core.evolR));
        oL = vol15(oL, core.mvolL);
        oR = vol15(oR, core.mvolR);

        if (c == 0) {
            extL = vol15(oL, cores_[1].avolL);
            extR = vol15(oR, cores_[1].avolR);
        } else {
            resL = oL;
            resR = oR;
        }
    }
    ticks_++;
    outL = resL;
    outR = resR;
}

void Spu2::mix(int16_t *out, int frames)
{
    for (int i = 0; i < frames; i++) {
        int32_t l, r;
        mix_frame(l, r);
        out[2 * i] = (int16_t)clamp16(l);
        out[2 * i + 1] = (int16_t)clamp16(r);
    }
}

void Spu2::mix_float(float *out, int frames)
{
    const float k = 1.0f / 32768.0f;
    for (int i = 0; i < frames; i++) {
        int32_t l, r;
        mix_frame(l, r);
        out[2 * i] = (float)l * k;
        out[2 * i + 1] = (float)r * k;
    }
}

} /* namespace spu2 */
