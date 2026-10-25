/* gs_tex_decode.h - GS texels -> RGBA8.
 *
 * Output pixels are uint32 0xAABBGGRR (bytes R, G, B, A in memory: the
 * order of a PSMCT32 pixel and of DXGI_FORMAT_R8G8B8A8_UNORM).  Alpha is the
 * GS value: 0x80 is 1.0 for the texture function and the blend (As / 128),
 * values above 0x80 are possible.  The renderer keeps that convention in its
 * textures and scales in the shader (docs/graphics.md, "Alpha"); viewers
 * (tools/texdump) expand it with alpha_to_unorm.
 *
 * What the GS does on a texture read, as this decoder does it:
 *   - CT32: the pixel.  CT24: RGB, A = TA0 (0 when TEXA.AEM and RGB = 0).
 *   - CT16 / CT16S: R, G, B 5 bits each shifted left by 3 (no low bits),
 *     A = TA1 when the A bit is set, else TA0 (0 when AEM and RGB = 0).
 *   - T8 / T4 / T8H / T4HL / T4HH: the index through the CLUT buffer (the
 *     entry is CT32 or CT16 / CT16S and expands the same way).
 *   - Z32 / Z24 / Z16 / Z16S read as textures: as CT32 / CT24 / CT16 / CT16S
 *     (their Z layout, gs_tex_layout.h).
 * TCC is not applied here (TCC 0 makes the texture function ignore the
 * texel alpha; that is the shader's), nor TFX or the filters.
 */
#ifndef GT3PC_GS_TEX_DECODE_H
#define GT3PC_GS_TEX_DECODE_H

#include <cstddef>
#include <cstdint>

#include "engine/pgl/native/gs_tex_format.h"
#include "engine/pgl/native/gs_tex_space.h"

namespace gstex {

/* 16 / 24-bit texel -> RGBA8 with TEXA (see above). */
uint32_t expand_ct16(uint32_t c16, const Texa &texa);
uint32_t expand_ct24(uint32_t c24, const Texa &texa);
/* A raw value of a direct format (CT32 / CT24 / CT16 / CT16S and the Z
 * formats) -> RGBA8. */
uint32_t expand_direct(uint32_t psm, uint32_t raw, const Texa &texa);

/* GS alpha (0x80 = 1.0) -> 0..255 (min(2a, 255)), for viewing. */
inline uint32_t alpha_to_unorm(uint32_t rgba)
{
    uint32_t a = rgba >> 24;
    a = a >= 0x80 ? 0xFF : a * 2;
    return (rgba & 0x00FFFFFFu) | (a << 24);
}

/* The GS CLUT buffer (the on-chip copy the indexed formats read): 256
 * 32-bit entries for a CT32 CLUT, 512 16-bit entries for CT16 / CT16S.
 * load() is what a TEX0 / TEX2 write with CLD != 0 does: it reads the CLUT
 * at CBP and stores it at CSA * 16:
 *   - CSM1, T8 / T8H: 256 entries from the 16 x 16 CPSM image at CBP
 *     (buffer width 64), entry i at clut_csm1_position(i);
 *   - CSM1, T4 / T4HL / T4HH: 16 entries from the 8 x 2 image at CBP;
 *   - CSM2: 256 or 16 CT16 entries from the line (COU * 16, COV) of the
 *     buffer at CBP, width TEXCLUT.CBW.
 * lookup(): T8-family index i -> entry (CSA * 16 + i), T4-family index i ->
 * entry (CSA * 16 + i).
 *
 * pgl's way (gfx_set_texture_regs 0x0024CD48): TEX0 goes out without CLD and
 * TEX2 = TEX0 with its CLD loads the CLUT; when TEX0.CSA != 0 TEX2 has PSM
 * forced to T8 and CSA 0, so the whole 256-entry CLUT is loaded and the
 * 4-bit texture takes entries CSA * 16 ... of it (the 8 x 2 block at
 * ((CSA & 1) * 8, (CSA >> 1) * 2) of the 16 x 16 image).  pgl_clut_load()
 * gives that TEX2.  The kanji font loads its own way (TEX0_2 with CLD 1 and
 * CSA 1: the 8 x 2 CLUT at CBP lands at entries 16..31 and is read there). */
class ClutBuffer {
public:
    void load(const TexSpace &space, const Tex0 &t, const TexClut *texclut = nullptr);
    /* Raw entry for index i of a texture of psm with CLUT offset csa. */
    uint32_t lookup_raw(uint32_t psm, uint32_t csa, uint32_t i) const;
    /* RGBA8 of index i (the entry expanded with TEXA when CT16). */
    uint32_t lookup(uint32_t psm, uint32_t csa, uint32_t i, const Texa &texa) const;
    uint32_t cpsm() const { return cpsm_; }
    bool loaded() const { return loaded_; }

private:
    uint32_t cpsm_ = PSMCT32;
    bool loaded_ = false;
    uint32_t e32_[256] = {};
    uint16_t e16_[512] = {};
};

/* The TEX2 pgl writes to load the CLUT of a texture (see ClutBuffer). */
Tex0 pgl_clut_load(const Tex0 &tex0);

/* Decodes the w x h rectangle at (x0, y0) of the texture (psm, tbp, tbw)
 * of the space into out (rows of `stride' pixels).  Indexed formats read
 * clut (must be loaded) with offset csa. */
void decode_rect(const TexSpace &space, uint32_t psm, uint32_t tbp, uint32_t tbw,
                 uint32_t x0, uint32_t y0, uint32_t w, uint32_t h,
                 const ClutBuffer *clut, uint32_t csa, const Texa &texa,
                 uint32_t *out, size_t stride);

/* The same for TEX0's level 0 (psm, TBP0, TBW, CSA). */
inline void decode_tex0(const TexSpace &space, const Tex0 &t, const ClutBuffer *clut,
                        const Texa &texa, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h,
                        uint32_t *out, size_t stride)
{
    decode_rect(space, t.psm, t.tbp0, t.tbw, x0, y0, w, h, clut, t.csa, texa, out, stride);
}

/* The rectangle of the 2^TW x 2^TH texture space worth decoding: the clamp
 * region when both axes are REGION_CLAMP (texels outside are never read),
 * else the whole space. */
struct Rect {
    uint32_t x = 0, y = 0, w = 0, h = 0;
};
Rect decode_region(const Tex0 &t, const Clamp &c);

/* A linear host image (an upload's data as the game holds it, before it is
 * laid out in VRAM) of psm -> RGBA8: for an upload whose format and buffer
 * width are the texture's (no layout change: the texture is the image).
 * Indexed formats take a 256- (T8) or 16-entry (T4) RGBA palette. */
void decode_linear(uint32_t psm, const void *data, size_t bytes, uint32_t w, uint32_t h,
                   const uint32_t *palette, const Texa &texa, uint32_t *out, size_t stride);

} /* namespace gstex */

#endif /* GT3PC_GS_TEX_DECODE_H */
