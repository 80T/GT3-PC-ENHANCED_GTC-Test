/* mpeg2_block.h - DCT coefficient decoding and inverse quantisation of one
 * 8x8 block, shared by the movie decoder (mpeg2_video.cpp) and the IPU
 * still-image decoder (ipu_image.cpp).
 *
 * This is the block part of the IPU's BDEC / IDEC commands (ipu_bdec
 * 0x00263688 for the movie player, ipu_decode_image 0x00264E58 for the
 * pictures): the DC size / differential, the run / level codes of table
 * B.14 or B.15 (IPU_CTRL.IVF), the zigzag or alternate scan (IPU_CTRL.AS),
 * the inverse quantisation with the loaded matrix and the linear or
 * non-linear quantiser scale (IPU_CTRL.QST), saturation, and the
 * mismatch control of ISO 13818-2 7.4.4 - or, with IPU_CTRL.MP1, MPEG-1's
 * escape format and oddification instead.
 */
#ifndef GT3_MEDIA_NATIVE_MPEG2_BLOCK_H
#define GT3_MEDIA_NATIVE_MPEG2_BLOCK_H

#include "engine/media/native/mpeg2_bits.h"
#include "engine/media/native/mpeg2_tables.h"

namespace gt3media {

/* What a block decode needs. */
struct CoefParams {
    const DctVlc *tabA;     /* AC table (B.14 or B.15), see Mpeg2Tables::dctA */
    const DctVlc *tabB;
    const uint8_t *scan;    /* scan position -> natural index */
    const uint16_t *wq;     /* W[natural] * quantiser_scale */
    bool mpeg1;             /* MPEG-1 escapes and oddification (IPU MP1) */
};

/* Results of the block decoders. */
enum BlockResult {
    BLOCK_ERROR   = -1,     /* invalid code or coefficient past 63 */
    BLOCK_DC_ONLY = 0,      /* only blk[0] is nonzero */
    BLOCK_FULL    = 1       /* rowMask has the rows with nonzero coefficients */
};

/* dct_dc_differential of an intra block (cc 0 luma, else chroma). */
inline int read_dc_diff(BitReader &br, const Mpeg2Tables &t, int cc, bool &ok)
{
    const Vlc &e = cc == 0 ? t.dcLum[br.peek(9)] : t.dcChrom[br.peek(10)];
    if (e.len == 0) {
        ok = false;
        return 0;
    }
    br.skip(e.len);
    int size = e.value;
    if (size == 0)
        return 0;
    int bits = (int)br.get(size);
    if (bits < (1 << (size - 1)))
        bits -= (1 << size) - 1;
    return bits;
}

/* The next run / level code.  Returns false on an invalid code; eob set at
 * end_of_block. */
inline bool read_coef(BitReader &br, const CoefParams &p, int &run, int &level, bool &eob)
{
    uint32_t w = br.peek(16);
    const DctVlc *e = w >= 0x0400 ? &p.tabA[w >> 7] : &p.tabB[w & 0x3FF];
    if (e->len == 0)
        return false;
    if (e->run == DCT_EOB) {
        br.skip(e->len);
        eob = true;
        return true;
    }
    eob = false;
    if (e->run == DCT_ESCAPE) {
        br.skip(6);
        run = (int)br.get(6);
        if (!p.mpeg1) {
            level = br.peekSigned(12);
            br.skip(12);
            if ((level & 0x7FF) == 0)       /* 0 and -2048 are forbidden */
                return false;
        } else {
            level = br.peekSigned(8);
            br.skip(8);
            if (level == 0)
                level = (int)br.get(8);
            else if (level == -128)
                level = (int)br.get(8) - 256;
        }
    } else {
        br.skip(e->len);
        run = e->run;
        level = br.getBit() ? -(int)e->level : (int)e->level;
    }
    return true;
}

/* Saturate to [-2048, 2047] after applying the sign. */
inline int signed_sat(int mag, bool neg)
{
    if (neg)
        return mag > 2048 ? -2048 : -mag;
    return mag > 2047 ? 2047 : mag;
}

/* AC coefficients of an intra block whose DC (F''[0][0]) the caller has
 * put in blk[0]; blk[1..63] must be zero on entry.  The decoder clears
 * the block after use (clear_block). */
inline int decode_intra_ac(BitReader &br, const CoefParams &p, int16_t *blk, unsigned &rowMask)
{
    int i = 0;
    int parity = blk[0];
    bool any = false;
    rowMask = 1;
    for (;;) {
        int run, level;
        bool eob;
        if (!read_coef(br, p, run, level, eob))
            return BLOCK_ERROR;
        if (eob)
            break;
        i += run + 1;
        if (i > 63)
            return BLOCK_ERROR;
        int j = p.scan[i];
        bool neg = level < 0;
        int mag = neg ? -level : level;
        int v = (mag * p.wq[j]) >> 4;
        if (p.mpeg1 && v != 0)
            v = (v - 1) | 1;                /* oddification */
        v = signed_sat(v, neg);
        blk[j] = (int16_t)v;
        parity ^= v;
        rowMask |= 1u << (j >> 3);
        any = true;
    }
    if (!p.mpeg1 && (parity & 1) == 0) {
        blk[63] ^= 1;                       /* mismatch control */
        rowMask |= 0x80;
        any = true;
    }
    return any ? BLOCK_FULL : BLOCK_DC_ONLY;
}

/* A non-intra block (all 64 coefficients); blk must be zero on entry. */
inline int decode_non_intra(BitReader &br, const CoefParams &p, int16_t *blk, unsigned &rowMask)
{
    int i = -1;
    int parity = 0;
    bool acOrToggle = false;
    rowMask = 0;
    /* The first coefficient: "1s" is run 0, level +-1 (table zero only,
     * and non-intra blocks always use table zero). */
    bool first = true;
    for (;;) {
        int run, level;
        bool eob;
        if (first && br.peek(1)) {
            br.skip(1);
            run = 0;
            level = br.getBit() ? -1 : 1;
            eob = false;
        } else if (!read_coef(br, p, run, level, eob)) {
            return BLOCK_ERROR;
        }
        first = false;
        if (eob)
            break;
        i += run + 1;
        if (i > 63)
            return BLOCK_ERROR;
        int j = p.scan[i];
        bool neg = level < 0;
        int mag = neg ? -level : level;
        int v = ((2 * mag + 1) * p.wq[j]) >> 5;
        if (p.mpeg1 && v != 0)
            v = (v - 1) | 1;
        v = signed_sat(v, neg);
        blk[j] = (int16_t)v;
        parity ^= v;
        rowMask |= 1u << (j >> 3);
        if (j != 0)
            acOrToggle = true;
    }
    if (!p.mpeg1 && (parity & 1) == 0) {
        blk[63] ^= 1;
        rowMask |= 0x80;
        acOrToggle = true;
    }
    return acOrToggle ? BLOCK_FULL : BLOCK_DC_ONLY;
}

/* Zero the rows of blk named by rowMask. */
inline void clear_block(int16_t *blk, unsigned rowMask)
{
    for (int r = 0; r < 8; r++) {
        if (rowMask & (1u << r)) {
            int16_t *row = blk + r * 8;
            row[0] = row[1] = row[2] = row[3] = row[4] = row[5] = row[6] = row[7] = 0;
        }
    }
}

} /* namespace gt3media */

#endif
