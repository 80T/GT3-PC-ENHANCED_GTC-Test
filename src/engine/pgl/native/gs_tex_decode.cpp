/* gs_tex_decode.cpp - GS texels -> RGBA8 (see the header). */
#include "engine/pgl/native/gs_tex_decode.h"
#include "engine/pgl/native/gs_tex_layout.h"

#include <algorithm>

namespace gstex {

uint32_t expand_ct16(uint32_t c, const Texa &texa)
{
    uint32_t r = (c & 0x1F) << 3;
    uint32_t g = ((c >> 5) & 0x1F) << 3;
    uint32_t b = ((c >> 10) & 0x1F) << 3;
    uint32_t a;
    if (c & 0x8000)
        a = texa.ta1;
    else
        a = (texa.aem && (c & 0x7FFF) == 0) ? 0 : texa.ta0;
    return r | (g << 8) | (b << 16) | (a << 24);
}

uint32_t expand_ct24(uint32_t c, const Texa &texa)
{
    c &= 0x00FFFFFF;
    uint32_t a = (texa.aem && c == 0) ? 0 : texa.ta0;
    return c | (a << 24);
}

uint32_t expand_direct(uint32_t psm, uint32_t raw, const Texa &texa)
{
    switch (psm) {
    case PSMCT32: case PSMZ32:
        return raw;
    case PSMCT24: case PSMZ24:
        return expand_ct24(raw, texa);
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S:
        return expand_ct16(raw, texa);
    default:
        return raw;
    }
}

/* ------------------------------------------------------------------------ */

static inline bool psm_t8_family(uint32_t psm) { return psm == PSMT8 || psm == PSMT8H; }

void ClutBuffer::load(const TexSpace &space, const Tex0 &t, const TexClut *texclut)
{
    bool ct32 = (t.cpsm == PSMCT32 || t.cpsm == PSMCT24);
    uint32_t n = psm_t8_family(t.psm) ? 256 : 16;
    uint32_t base = t.csa * 16;

    cpsm_ = ct32 ? PSMCT32 : t.cpsm;
    loaded_ = true;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t x, y, bw, psm;
        if (t.csm == 0) {
            /* CSM1: 16 x 16 swizzled (T8) or 8 x 2 (T4), buffer width 64 */
            uint32_t p = (n == 256) ? clut_csm1_position(i) : i;
            x = (n == 256) ? (p & 15) : (p & 7);
            y = (n == 256) ? (p >> 4) : (p >> 3);
            bw = 1;
            psm = ct32 ? PSMCT32 : t.cpsm;
        } else {
            /* CSM2: a line of CT16 entries */
            x = (texclut ? texclut->cou * 16 : 0) + i;
            y = texclut ? texclut->cov : 0;
            bw = texclut ? texclut->cbw : 1;
            psm = PSMCT16;
        }
        uint32_t v = space.read(psm, t.cbp, bw, x, y);
        if (ct32)
            e32_[(base + i) & 255] = v;
        else
            e16_[(base + i) & 511] = (uint16_t)v;
    }
}

uint32_t ClutBuffer::lookup_raw(uint32_t psm, uint32_t csa, uint32_t i) const
{
    uint32_t e = csa * 16 + (psm_t8_family(psm) ? (i & 255) : (i & 15));
    if (cpsm_ == PSMCT32)
        return e32_[e & 255];
    return e16_[e & 511];
}

uint32_t ClutBuffer::lookup(uint32_t psm, uint32_t csa, uint32_t i, const Texa &texa) const
{
    uint32_t raw = lookup_raw(psm, csa, i);
    if (cpsm_ == PSMCT32)
        return raw;
    return expand_ct16(raw, texa);
}

Tex0 pgl_clut_load(const Tex0 &tex0)
{
    Tex0 t2 = tex0;
    if (t2.csa != 0) {
        t2.psm = PSMT8;
        t2.csa = 0;
    }
    return t2;
}

/* ------------------------------------------------------------------------ */

void decode_rect(const TexSpace &space, uint32_t psm, uint32_t tbp, uint32_t tbw,
                 uint32_t x0, uint32_t y0, uint32_t w, uint32_t h,
                 const ClutBuffer *clut, uint32_t csa, const Texa &texa,
                 uint32_t *out, size_t stride)
{
    bool indexed = psm_indexed(psm);
    for (uint32_t y = 0; y < h; y++) {
        uint32_t *row = out + (size_t)y * stride;
        for (uint32_t x = 0; x < w; x++) {
            uint32_t raw = space.read(psm, tbp, tbw, x0 + x, y0 + y);
            if (indexed)
                row[x] = clut ? clut->lookup(psm, csa, raw, texa) : (raw * 0x01010101u);
            else
                row[x] = expand_direct(psm, raw, texa);
        }
    }
}

Rect decode_region(const Tex0 &t, const Clamp &c)
{
    Rect r;
    r.w = t.width();
    r.h = t.height();
    if (c.wms == 2 && c.maxu >= c.minu) {
        r.x = c.minu;
        r.w = std::min(c.maxu - c.minu + 1, t.width() - std::min(c.minu, t.width()));
    }
    if (c.wmt == 2 && c.maxv >= c.minv) {
        r.y = c.minv;
        r.h = std::min(c.maxv - c.minv + 1, t.height() - std::min(c.minv, t.height()));
    }
    if (r.w == 0) r.w = 1;
    if (r.h == 0) r.h = 1;
    return r;
}

void decode_linear(uint32_t psm, const void *data, size_t bytes, uint32_t w, uint32_t h,
                   const uint32_t *palette, const Texa &texa, uint32_t *out, size_t stride)
{
    const uint8_t *s = static_cast<const uint8_t *>(data);
    uint32_t bitsPer = psm_transfer_bits(psm);
    uint64_t totalBits = (uint64_t)bytes * 8;
    uint64_t bitPos = 0;

    for (uint32_t y = 0; y < h; y++) {
        uint32_t *row = out + (size_t)y * stride;
        for (uint32_t x = 0; x < w; x++, bitPos += bitsPer) {
            if (bitPos + bitsPer > totalBits) {
                row[x] = 0;
                continue;
            }
            const uint8_t *p = s + (bitPos >> 3);
            uint32_t v;
            switch (bitsPer) {
            case 32: v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); break;
            case 24: v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16); break;
            case 16: v = (uint32_t)p[0] | ((uint32_t)p[1] << 8); break;
            case 8:  v = p[0]; break;
            default: v = (p[0] >> (bitPos & 4)) & 0xF; break;
            }
            if (psm_indexed(psm))
                row[x] = palette ? palette[v] : v * 0x01010101u;
            else
                row[x] = expand_direct(psm, v, texa);
        }
    }
}

} /* namespace gstex */
