/* spu2_reverb.h - the SPU2 reverb ("effect") of one core, as its own stage.
 *
 * What the hardware does (nocash psx-spx "SPU Reverb Formula" for the PS1
 * unit, which the SPU2 keeps per core; PCSX2's SPU2 Reverb.cpp as a
 * reference for the PS2 specifics):
 *   - the work area is a ring of 16-bit words in sound memory between ESA
 *     and EEA; every address register is an offset into it from a position
 *     that advances by one word every two output samples;
 *   - the unit runs at half rate, alternating left and right: the input
 *     (the core's wet mix) goes through a 39-tap half-band FIR on the way
 *     in, the output is zero-stuffed and filtered by the same FIR (x2) on
 *     the way out;
 *   - same-side and cross-side reflections (IIR + wall), four comb taps,
 *     two all-pass stages; results are written back into the ring only
 *     while the core's effect enable bit is set.
 * The 32 registers come from the effect presets of the IOP sound library
 * (libsd.irx's table, the same values as the PS1 libspu presets); libsd
 * writes the address registers as preset value x 4 (PS2 word units) and
 * places ESA = EEA - work size + 1.  GT3 uses mode 4 (STUDIO_3, "studio
 * large") on both cores, and mode 0 (off) at shutdown.
 *
 * Plain C++, no Windows or game dependency. */
#ifndef GT3PC_SPU2_REVERB_H
#define GT3PC_SPU2_REVERB_H

#include <cstdint>

namespace spu2 {

/* libsd effect modes (sceSdEffectAttr.mode, low byte). */
enum EffectMode {
    EFFECT_OFF = 0, EFFECT_ROOM, EFFECT_STUDIO_1, EFFECT_STUDIO_2, EFFECT_STUDIO_3,
    EFFECT_HALL, EFFECT_SPACE, EFFECT_ECHO, EFFECT_DELAY, EFFECT_PIPE,
    EFFECT_MODE_COUNT
};

/* A preset: the 32 reverb registers in their hardware order (dAPF1, dAPF2,
 * vIIR, vCOMB1..4, vWALL, vAPF1, vAPF2, mLSAME, mRSAME, mLCOMB1, mRCOMB1,
 * mLCOMB2, mRCOMB2, dLSAME, dRSAME, mLDIFF, mRDIFF, mLCOMB3, mRCOMB3,
 * mLCOMB4, mRCOMB4, dLDIFF, dRDIFF, mLAPF1, mRAPF1, mLAPF2, mRAPF2, vLIN,
 * vRIN) and the work area size in 8-byte units. */
struct ReverbPreset {
    uint16_t reg[32];
    uint32_t workSize8;
};

extern const ReverbPreset kReverbPresets[EFFECT_MODE_COUNT];

class Reverb {
public:
    Reverb() { reset(0xFFFFF); }

    /* Power-on state: mode off, the end address eea (word address). */
    void reset(uint32_t eeaWords);
    /* sceSdSetEffectAttr's register part: loads mode's preset and places
     * ESA below the current EEA. */
    void set_mode(int mode);
    void set_end(uint32_t eeaWords) { eea_ = eeaWords & 0xFFFFF; }
    int  mode() const { return mode_; }
    uint32_t start() const { return esa_; }
    uint32_t end() const { return eea_; }
    /* Zeroes the work area of `mode' below EEA (sceSdClearEffectWorkArea). */
    void clear_area(uint16_t *ram, int mode) const;

    /* One 48 kHz tick: in = the wet mix (clamped to 16 bits by the caller),
     * out = the reverb output before the effect volume (EVOL).  ram is the
     * sound memory; writes happen only when enabled.  The work area's
     * addresses are the hardware's 20-bit word addresses (0xFFFFF masks):
     * PC: in the first 2 MB of the port's larger memory (spu2.h), where GT3
     * places both areas (0x1D9020..0x1DFFFF, 0x1F9020..0x1FFFFF). */
    void tick(int32_t inL, int32_t inR, int32_t &outL, int32_t &outR, uint16_t *ram, bool enabled);

private:
    uint32_t addr(int32_t offset) const;

    int      mode_ = 0;
    uint32_t esa_ = 0, eea_ = 0;
    int32_t  apf1Size_ = 0, apf2Size_ = 0;
    int32_t  vIIR_ = 0, vCOMB_[4] = {}, vWALL_ = 0, vAPF1_ = 0, vAPF2_ = 0, vIN_[2] = {};
    int32_t  mSAME_[2] = {}, mCOMB1_[2] = {}, mCOMB2_[2] = {}, dSAME_[2] = {}, mDIFF_[2] = {};
    int32_t  mCOMB3_[2] = {}, mCOMB4_[2] = {}, dDIFF_[2] = {}, mAPF1_[2] = {}, mAPF2_[2] = {};

    uint32_t cycle_ = 0;            /* output samples since reset (position = cycle / 2) */
    int16_t  down_[2][128] = {};    /* input history (mirrored: [i] == [i + 64]) */
    int16_t  up_[2][128] = {};      /* output history, zero-stuffed */
    int      pos_ = 0;
};

} /* namespace spu2 */

#endif /* GT3PC_SPU2_REVERB_H */
