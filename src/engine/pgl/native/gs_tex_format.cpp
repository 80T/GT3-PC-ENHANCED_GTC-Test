/* gs_tex_format.cpp - GS texture formats and registers (see the header). */
#include "engine/pgl/native/gs_tex_format.h"
#include "engine/pgl/native/gs_tex_layout.h"

#include <algorithm>

namespace gstex {

bool psm_valid(uint32_t psm)
{
    switch (psm) {
    case PSMCT32: case PSMCT24: case PSMCT16: case PSMCT16S:
    case PSMT8: case PSMT4: case PSMT8H: case PSMT4HL: case PSMT4HH:
    case PSMZ32: case PSMZ24: case PSMZ16: case PSMZ16S:
        return true;
    default:
        return false;
    }
}

const char *psm_name(uint32_t psm)
{
    switch (psm) {
    case PSMCT32:  return "CT32";
    case PSMCT24:  return "CT24";
    case PSMCT16:  return "CT16";
    case PSMCT16S: return "CT16S";
    case PSMT8:    return "T8";
    case PSMT4:    return "T4";
    case PSMT8H:   return "T8H";
    case PSMT4HL:  return "T4HL";
    case PSMT4HH:  return "T4HH";
    case PSMZ32:   return "Z32";
    case PSMZ24:   return "Z24";
    case PSMZ16:   return "Z16";
    case PSMZ16S:  return "Z16S";
    default:       return "?";
    }
}

bool psm_indexed(uint32_t psm)
{
    return psm == PSMT8 || psm == PSMT4 || psm == PSMT8H || psm == PSMT4HL || psm == PSMT4HH;
}

bool psm_depth(uint32_t psm)
{
    return psm == PSMZ32 || psm == PSMZ24 || psm == PSMZ16 || psm == PSMZ16S;
}

uint32_t psm_transfer_bits(uint32_t psm)
{
    switch (psm) {
    case PSMCT32: case PSMZ32:                      return 32;
    case PSMCT24: case PSMZ24:                      return 24;
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return 16;
    case PSMT8: case PSMT8H:                        return 8;
    case PSMT4: case PSMT4HL: case PSMT4HH:         return 4;
    default:                                        return 32;
    }
}

uint32_t psm_field_bits(uint32_t psm)
{
    return psm_transfer_bits(psm);
}

PsmGeom psm_geom(uint32_t psm)
{
    switch (psm) {
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S:
        return { 64, 64, 16, 8 };
    case PSMT8:
        return { 128, 64, 16, 16 };
    case PSMT4:
        return { 128, 128, 32, 16 };
    default:    /* the 32-bit layouts: CT32, CT24, Z32, Z24, T8H, T4HL, T4HH */
        return { 64, 32, 8, 8 };
    }
}

/* ------------------------------------------------------------------------ */

static inline uint32_t bits(uint64_t v, unsigned shift, unsigned width)
{
    return (uint32_t)((v >> shift) & ((1ull << width) - 1));
}

static inline uint64_t put(uint64_t v, unsigned shift, unsigned width)
{
    return (v & ((1ull << width) - 1)) << shift;
}

Tex0 Tex0::decode(uint64_t v)
{
    Tex0 t;
    t.tbp0 = bits(v, 0, 14);
    t.tbw  = bits(v, 14, 6);
    t.psm  = bits(v, 20, 6);
    t.tw   = bits(v, 26, 4);
    t.th   = bits(v, 30, 4);
    t.tcc  = bits(v, 34, 1);
    t.tfx  = bits(v, 35, 2);
    t.cbp  = bits(v, 37, 14);
    t.cpsm = bits(v, 51, 4);
    t.csm  = bits(v, 55, 1);
    t.csa  = bits(v, 56, 5);
    t.cld  = bits(v, 61, 3);
    return t;
}

uint64_t Tex0::encode() const
{
    return put(tbp0, 0, 14) | put(tbw, 14, 6) | put(psm, 20, 6) | put(tw, 26, 4) |
           put(th, 30, 4) | put(tcc, 34, 1) | put(tfx, 35, 2) | put(cbp, 37, 14) |
           put(cpsm, 51, 4) | put(csm, 55, 1) | put(csa, 56, 5) | put(cld, 61, 3);
}

Tex1 Tex1::decode(uint64_t v)
{
    Tex1 t;
    t.lcm  = bits(v, 0, 1);
    t.mxl  = bits(v, 2, 3);
    t.mmag = bits(v, 5, 1);
    t.mmin = bits(v, 6, 3);
    t.mtba = bits(v, 9, 1);
    t.l    = bits(v, 19, 2);
    uint32_t k = bits(v, 32, 12);
    t.k = (int32_t)(k << 20) >> 20;         /* signed 12 bits */
    return t;
}

uint64_t Tex1::encode() const
{
    return put(lcm, 0, 1) | put(mxl, 2, 3) | put(mmag, 5, 1) | put(mmin, 6, 3) |
           put(mtba, 9, 1) | put(l, 19, 2) | put((uint32_t)k, 32, 12);
}

Clamp Clamp::decode(uint64_t v)
{
    Clamp c;
    c.wms  = bits(v, 0, 2);
    c.wmt  = bits(v, 2, 2);
    c.minu = bits(v, 4, 10);
    c.maxu = bits(v, 14, 10);
    c.minv = bits(v, 24, 10);
    c.maxv = bits(v, 34, 10);
    return c;
}

uint64_t Clamp::encode() const
{
    return put(wms, 0, 2) | put(wmt, 2, 2) | put(minu, 4, 10) | put(maxu, 14, 10) |
           put(minv, 24, 10) | put(maxv, 34, 10);
}

Texa Texa::decode(uint64_t v)
{
    Texa t;
    t.ta0 = bits(v, 0, 8);
    t.aem = bits(v, 15, 1);
    t.ta1 = bits(v, 32, 8);
    return t;
}

uint64_t Texa::encode() const
{
    return put(ta0, 0, 8) | put(aem, 15, 1) | put(ta1, 32, 8);
}

Miptbp Miptbp::decode(uint64_t v)
{
    Miptbp m;
    for (int i = 0; i < 3; i++) {
        m.tbp[i] = bits(v, 20 * i, 14);
        m.tbw[i] = bits(v, 20 * i + 14, 6);
    }
    return m;
}

uint64_t Miptbp::encode() const
{
    uint64_t v = 0;
    for (int i = 0; i < 3; i++)
        v |= put(tbp[i], 20 * i, 14) | put(tbw[i], 20 * i + 14, 6);
    return v;
}

TexClut TexClut::decode(uint64_t v)
{
    TexClut t;
    t.cbw = bits(v, 0, 6);
    t.cou = bits(v, 6, 6);
    t.cov = bits(v, 12, 10);
    return t;
}

/* ------------------------------------------------------------------------ */

uint32_t image_bytes(uint32_t psm, uint32_t w, uint32_t h)
{
    uint64_t bitsTotal = (uint64_t)w * h * psm_transfer_bits(psm);
    return (uint32_t)((bitsTotal + 7) / 8);
}

uint32_t transfer_qwc(uint32_t psm, uint32_t w, uint32_t h)
{
    uint32_t n = w * h;
    switch (psm) {
    case PSMCT32:                       return (n * 4 + 15) >> 4;
    case PSMCT24:                       return (n * 3 + 15) >> 4;
    case PSMCT16: case PSMCT16S:        return (n * 2 + 15) >> 4;
    case PSMT8: case PSMT8H:            return (n + 15) >> 4;
    case PSMT4: case PSMT4HH: case PSMT4HL: return (n + 31) >> 5;
    default:                            return n;   /* gs_build_image_transfer_packet's quirk */
    }
}

int32_t vram_words(uint32_t psm, int32_t w, int32_t h, int32_t *alignedW)
{
    int32_t pageW, pageH, shift;

    switch (psm) {
    case PSMCT32: case PSMCT24: case PSMT8H: case PSMT4HL: case PSMT4HH:
    case PSMZ32: case PSMZ24:
        pageW = 0x40; pageH = 0x20; shift = 11;
        break;
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S:
        pageW = 0x40; pageH = 0x40; shift = 12;
        break;
    case PSMT8:
        pageW = 0x80; pageH = 0x40; shift = 13;
        break;
    case PSMT4:
        pageW = 0x80; pageH = 0x80; shift = 14;
        break;
    default:
        return -1;
    }
    int32_t roundW = (w + pageW - 1) & -pageW;
    int32_t roundH = (h + pageH - 1) & -pageH;
    if (alignedW != nullptr)
        *alignedW = roundW;
    return (int32_t)(((uint32_t)(roundW * roundH) << 11) >> shift);   /* tex_vram_size's arithmetic */
}

BlockRange image_blocks(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y,
                        uint32_t w, uint32_t h)
{
    BlockRange r;
    if (w == 0 || h == 0) {
        r.first = bp;
        r.last = bp - 1;
        return r;
    }
    /* one sample per block cell covered (the block grid of the format) */
    PsmGeom g = psm_geom(psm);
    uint32_t x0 = x & ~(g.blockW - 1), y0 = y & ~(g.blockH - 1);
    uint32_t lo = 0xFFFFFFFFu, hi = 0;
    for (uint32_t by = y0; by < y + h; by += g.blockH)
        for (uint32_t bx = x0; bx < x + w; bx += g.blockW) {
            uint32_t b = block_number(psm, bp, bw, bx, by);
            lo = std::min(lo, b);
            hi = std::max(hi, b);
        }
    r.first = lo;
    r.last = hi;
    return r;
}

void image_block_mask(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y,
                      uint32_t w, uint32_t h, uint32_t baseBlock, std::vector<uint8_t> &mask)
{
    if (w == 0 || h == 0)
        return;
    PsmGeom g = psm_geom(psm);
    uint32_t x0 = x & ~(g.blockW - 1), y0 = y & ~(g.blockH - 1);
    for (uint32_t by = y0; by < y + h; by += g.blockH)
        for (uint32_t bx = x0; bx < x + w; bx += g.blockW) {
            uint32_t rel = (block_number(psm, bp, bw, bx, by) - baseBlock) & (GS_MEM_BLOCKS - 1);
            if (rel < mask.size())
                mask[rel] = 1;
        }
}

BlockRange texture_blocks(const Tex0 &t, uint32_t w, uint32_t h)
{
    if (w == 0)
        w = t.width();
    if (h == 0)
        h = t.height();
    return image_blocks(t.psm, t.tbp0, t.tbw, 0, 0, w, h);
}

BlockRange clut_blocks(const Tex0 &t, bool t8Load, const TexClut *tc)
{
    BlockRange r;
    if (!psm_indexed(t.psm)) {
        r.first = t.cbp;
        r.last = t.cbp - 1;
        return r;
    }
    bool t8 = t8Load || t.psm == PSMT8 || t.psm == PSMT8H;
    if (t.csm == 0) {
        /* CSM1: 16 x 16 (T8) or 8 x 2 (T4) pixels of CPSM at CBP, width 64 */
        if (t8)
            return image_blocks(t.cpsm, t.cbp, 1, 0, 0, 16, 16);
        return image_blocks(t.cpsm, t.cbp, 1, 0, 0, 8, 2);
    }
    /* CSM2: one line of 256 / 16 CT16 entries at (COU * 16, COV), width CBW */
    uint32_t cbw = tc ? tc->cbw : 1, cou = tc ? tc->cou : 0, cov = tc ? tc->cov : 0;
    return image_blocks(PSMCT16, t.cbp, cbw, cou * 16, cov, t8 ? 256 : 16, 1);
}

MipLevel mip_level(const Tex0 &t, const Miptbp &m1, const Miptbp &m2, uint32_t level)
{
    MipLevel m;
    if (level == 0) {
        m.tbp = t.tbp0;
        m.tbw = t.tbw;
    } else if (level <= 3) {
        m.tbp = m1.tbp[level - 1];
        m.tbw = m1.tbw[level - 1];
    } else {
        uint32_t i = std::min(level, 6u) - 4;
        m.tbp = m2.tbp[i];
        m.tbw = m2.tbw[i];
    }
    m.w = std::max(1u, t.width() >> level);
    m.h = std::max(1u, t.height() >> level);
    return m;
}

} /* namespace gstex */
