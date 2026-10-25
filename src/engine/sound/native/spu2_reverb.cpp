/* spu2_reverb.cpp - the SPU2 reverb stage (see spu2_reverb.h). */
#include "engine/sound/native/spu2_reverb.h"

#include <cstring>

namespace spu2 {

/* The effect presets of GT3's libsd.irx (.data 0x41F0, 0x44 bytes each: a
 * register mask word (0: all) and the 32 registers), and the work area
 * sizes (.data 0x41C8, 8-byte units).  The same values as the PS1 libspu
 * presets. */
const ReverbPreset kReverbPresets[EFFECT_MODE_COUNT] = {
    /* 0 OFF */
    { { 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
        0x0000, 0x0000, 0x0001, 0x0001, 0x0000, 0x0000, 0x0000, 0x0000,
        0x0000, 0x0000, 0x0001, 0x0001, 0x0000, 0x0000, 0x0000, 0x0000,
        0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000 }, 0x0002 },
    /* 1 ROOM */
    { { 0x007D, 0x005B, 0x6D80, 0x54B8, 0xBED0, 0x0000, 0x0000, 0xBA80,
        0x5800, 0x5300, 0x04D6, 0x0333, 0x03F0, 0x0227, 0x0374, 0x01EF,
        0x0336, 0x01B7, 0x0335, 0x01B6, 0x0334, 0x01B5, 0x0334, 0x01B5,
        0x0334, 0x01B5, 0x01B4, 0x0136, 0x00B8, 0x005C, 0x8000, 0x8000 }, 0x04D8 },
    /* 2 STUDIO_1 (small) */
    { { 0x0033, 0x0025, 0x70F0, 0x4FA8, 0xBCE0, 0x4410, 0xC0F0, 0x9C00,
        0x5280, 0x4EC0, 0x03E4, 0x031B, 0x03A4, 0x02AF, 0x0372, 0x0266,
        0x031C, 0x025D, 0x025C, 0x018E, 0x022F, 0x0135, 0x01D2, 0x00B7,
        0x018F, 0x00B5, 0x00B4, 0x0080, 0x004C, 0x0026, 0x8000, 0x8000 }, 0x03E8 },
    /* 3 STUDIO_2 (medium) */
    { { 0x00B1, 0x007F, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xB4C0,
        0x5280, 0x4EC0, 0x0904, 0x076B, 0x0824, 0x065F, 0x07A2, 0x0616,
        0x076C, 0x05ED, 0x05EC, 0x042E, 0x050F, 0x0305, 0x0462, 0x02B7,
        0x042F, 0x0265, 0x0264, 0x01B2, 0x0100, 0x0080, 0x8000, 0x8000 }, 0x0908 },
    /* 4 STUDIO_3 (large): GT3's mode */
    { { 0x00E3, 0x00A9, 0x6F60, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xA680,
        0x5680, 0x52C0, 0x0DFB, 0x0B58, 0x0D09, 0x0A3C, 0x0BD9, 0x0973,
        0x0B59, 0x08DA, 0x08D9, 0x05E9, 0x07EC, 0x04B0, 0x06EF, 0x03D2,
        0x05EA, 0x031D, 0x031C, 0x0238, 0x0154, 0x00AA, 0x8000, 0x8000 }, 0x0DFC },
    /* 5 HALL */
    { { 0x01A5, 0x0139, 0x6000, 0x5000, 0x4C00, 0xB800, 0xBC00, 0xC000,
        0x6000, 0x5C00, 0x15BA, 0x11BB, 0x14C2, 0x10BD, 0x11BC, 0x0DC1,
        0x11C0, 0x0DC3, 0x0DC0, 0x09C1, 0x0BC4, 0x07C1, 0x0A00, 0x06CD,
        0x09C2, 0x05C1, 0x05C0, 0x041A, 0x0274, 0x013A, 0x8000, 0x8000 }, 0x15BC },
    /* 6 SPACE */
    { { 0x033D, 0x0231, 0x7E00, 0x5000, 0xB400, 0xB000, 0x4C00, 0xB000,
        0x6000, 0x5400, 0x1ED6, 0x1A31, 0x1D14, 0x183B, 0x1BC2, 0x16B2,
        0x1A32, 0x15EF, 0x15EE, 0x1055, 0x1334, 0x0F2D, 0x11F6, 0x0C5D,
        0x1056, 0x0AE1, 0x0AE0, 0x07A2, 0x0464, 0x0232, 0x8000, 0x8000 }, 0x1ED8 },
    /* 7 ECHO (libsd also sets the delay / feedback registers from the
     * attributes; GT3 never selects it: the preset's defaults are used) */
    { { 0x0003, 0x0003, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x8100,
        0x0000, 0x0000, 0x1FFD, 0x0FFD, 0x1009, 0x0009, 0x0000, 0x0000,
        0x1009, 0x0009, 0x1FFF, 0x1FFF, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE,
        0x1FFE, 0x1FFE, 0x1008, 0x1004, 0x0008, 0x0004, 0x8000, 0x8000 }, 0x3008 },
    /* 8 DELAY (idem) */
    { { 0x0003, 0x0003, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x0000,
        0x0000, 0x0000, 0x1FFD, 0x0FFD, 0x1009, 0x0009, 0x0000, 0x0000,
        0x1009, 0x0009, 0x1FFF, 0x1FFF, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE,
        0x1FFE, 0x1FFE, 0x1008, 0x1004, 0x0008, 0x0004, 0x8000, 0x8000 }, 0x3008 },
    /* 9 PIPE */
    { { 0x0017, 0x0013, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0x8500,
        0x5F80, 0x54C0, 0x0371, 0x02AF, 0x02E5, 0x01DF, 0x02B0, 0x01D7,
        0x0358, 0x026A, 0x01D6, 0x011E, 0x012D, 0x00B1, 0x011F, 0x0059,
        0x01A0, 0x00E3, 0x0058, 0x0040, 0x0028, 0x0014, 0x8000, 0x8000 }, 0x0780 },
};

/* The half-band FIR of the reverb's input decimation and output
 * interpolation (39 taps; the hardware coefficients listed by nocash's
 * psx-spx, "Reverb Buffer Resampling").  The output side uses them x 2. */
static const int32_t kFir[39] = {
       -1,    0,     2,    0,   -10,    0,    35,    0,  -103,    0,
      266,    0,  -616,    0,  1332,    0, -2960,    0, 10246, 16384,
    10246,    0, -2960,    0,  1332,    0,  -616,    0,   266,    0,
     -103,    0,    35,    0,   -10,    0,     2,    0,    -1,
};

static inline int32_t clamp16(int32_t v)
{
    return v < -32768 ? -32768 : (v > 32767 ? 32767 : v);
}

static inline int32_t mul15(int32_t a, int32_t b)
{
    return (a * b) >> 15;
}

void Reverb::reset(uint32_t eeaWords)
{
    eea_ = eeaWords & 0xFFFFF;
    cycle_ = 0;
    pos_ = 0;
    std::memset(down_, 0, sizeof(down_));
    std::memset(up_, 0, sizeof(up_));
    set_mode(EFFECT_OFF);
}

void Reverb::set_mode(int mode)
{
    if (mode < 0 || mode >= EFFECT_MODE_COUNT)
        return;
    const ReverbPreset &p = kReverbPresets[mode];
    const uint16_t *r = p.reg;
    /* address registers: preset x 4 (words); volumes: signed 16 bits */
    auto A = [&](int i) { return (int32_t)r[i] * 4; };
    auto V = [&](int i) { return (int32_t)(int16_t)r[i]; };

    mode_ = mode;
    apf1Size_ = A(0);
    apf2Size_ = A(1);
    vIIR_ = V(2);
    vCOMB_[0] = V(3);
    vCOMB_[1] = V(4);
    vCOMB_[2] = V(5);
    vCOMB_[3] = V(6);
    vWALL_ = V(7);
    vAPF1_ = V(8);
    vAPF2_ = V(9);
    mSAME_[0] = A(10);  mSAME_[1] = A(11);
    mCOMB1_[0] = A(12); mCOMB1_[1] = A(13);
    mCOMB2_[0] = A(14); mCOMB2_[1] = A(15);
    dSAME_[0] = A(16);  dSAME_[1] = A(17);
    mDIFF_[0] = A(18);  mDIFF_[1] = A(19);
    mCOMB3_[0] = A(20); mCOMB3_[1] = A(21);
    mCOMB4_[0] = A(22); mCOMB4_[1] = A(23);
    dDIFF_[0] = A(24);  dDIFF_[1] = A(25);
    mAPF1_[0] = A(26);  mAPF1_[1] = A(27);
    mAPF2_[0] = A(28);  mAPF2_[1] = A(29);
    vIN_[0] = V(30);
    vIN_[1] = V(31);
    /* libsd: ESA = EEA - (size * 8 - 2) bytes, i.e. a ring of size * 4 words
     * ending at EEA */
    uint32_t words = p.workSize8 * 4;
    esa_ = (eea_ + 1 - words) & 0xFFFFF;
}

void Reverb::clear_area(uint16_t *ram, int mode) const
{
    if (mode < 0 || mode >= EFFECT_MODE_COUNT)
        return;
    uint32_t words = kReverbPresets[mode].workSize8 * 4;
    uint32_t start = (eea_ + 1 - words) & 0xFFFFF;
    for (uint32_t i = 0; i < words; i++)
        ram[(start + i) & 0xFFFFF] = 0;
}

uint32_t Reverb::addr(int32_t offset) const
{
    uint32_t size = eea_ >= esa_ ? eea_ - esa_ + 1 : 1;
    int64_t x = (int64_t)(cycle_ >> 1) + offset;
    x %= (int64_t)size;
    if (x < 0)
        x += size;
    return (esa_ + (uint32_t)x) & 0xFFFFF;
}

void Reverb::tick(int32_t inL, int32_t inR, int32_t &outL, int32_t &outR, uint16_t *ram, bool enabled)
{
    /* input history for the decimation FIR */
    down_[0][pos_] = down_[0][pos_ + 64] = (int16_t)clamp16(inL);
    down_[1][pos_] = down_[1][pos_ + 64] = (int16_t)clamp16(inR);

    const int R = cycle_ & 1;               /* this tick's side */
    const int first = pos_ + 64 - 38;        /* the 39 most recent inputs */
    int32_t acc = 0;
    for (int i = 0; i < 39; i++)
        acc += down_[R][first + i] * kFir[i];
    const int32_t input = clamp16(acc >> 15);

    auto rd = [&](int32_t off) { return (int32_t)(int16_t)ram[addr(off)]; };

    const int32_t in = mul15(vIN_[R], input);
    /* same side: L <- L, R <- R; different side: L <- R, R <- L */
    const int32_t sameSrc = rd(dSAME_[R]);
    const int32_t samePrv = rd(mSAME_[R] - 1);
    const int32_t diffSrc = rd(dDIFF_[R ^ 1]);
    const int32_t diffPrv = rd(mDIFF_[R] - 1);
    const int32_t same = mul15(vIIR_, in + mul15(vWALL_, sameSrc) - samePrv) + samePrv;
    const int32_t diff = mul15(vIIR_, in + mul15(vWALL_, diffSrc) - diffPrv) + diffPrv;

    int32_t out = mul15(vCOMB_[0], rd(mCOMB1_[R])) + mul15(vCOMB_[1], rd(mCOMB2_[R]))
                + mul15(vCOMB_[2], rd(mCOMB3_[R])) + mul15(vCOMB_[3], rd(mCOMB4_[R]));

    const int32_t apf1Src = rd(mAPF1_[R] - apf1Size_);
    const int32_t apf1 = out - mul15(vAPF1_, apf1Src);
    out = apf1Src + mul15(vAPF1_, apf1);
    const int32_t apf2Src = rd(mAPF2_[R] - apf2Size_);
    const int32_t apf2 = out - mul15(vAPF2_, apf2Src);
    out = apf2Src + mul15(vAPF2_, apf2);

    if (enabled) {
        ram[addr(mSAME_[R])] = (uint16_t)clamp16(same);
        ram[addr(mDIFF_[R])] = (uint16_t)clamp16(diff);
        ram[addr(mAPF1_[R])] = (uint16_t)clamp16(apf1);
        ram[addr(mAPF2_[R])] = (uint16_t)clamp16(apf2);
    }

    /* output: this side's value, a zero on the other (zero stuffing) */
    up_[R][pos_] = up_[R][pos_ + 64] = (int16_t)clamp16(out);
    up_[R ^ 1][pos_] = up_[R ^ 1][pos_ + 64] = 0;
    int32_t l = 0, r = 0;
    for (int i = 0; i < 39; i++) {
        l += up_[0][first + i] * (kFir[i] * 2);
        r += up_[1][first + i] * (kFir[i] * 2);
    }
    outL = clamp16(l >> 15);
    outR = clamp16(r >> 15);

    pos_ = (pos_ + 1) & 63;
    cycle_++;
}

} /* namespace spu2 */
