/* gs_tex_format.h - GS texture formats and texture registers, for the native
 * texture decoder (GT3 PC).
 *
 * The PS2 game describes every texture with GS registers (TEX0, TEX1, CLAMP,
 * TEXA, MIPTBP1/2, TEXCLUT) and moves pixels with host -> local transfers
 * (BITBLTBUF / TRXPOS / TRXREG, gs_build_image_transfer_packet 0x00254A30).
 * This header gives those registers as plain structs (decoded from / encoded
 * to their 64-bit values, GS User's Manual field layout), the pixel storage
 * modes and the size / footprint helpers the texture registry needs.
 *
 * Plain C++17, no Windows or game dependency (tools/texdump uses it alone).
 * See docs/graphics.md for how the renderer uses it.
 */
#ifndef GT3PC_GS_TEX_FORMAT_H
#define GT3PC_GS_TEX_FORMAT_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gstex {

/* Pixel storage modes (TEX0.PSM, FRAME.PSM, BITBLTBUF.DPSM, TEX0.CPSM). */
enum Psm : uint32_t {
    PSMCT32  = 0x00,
    PSMCT24  = 0x01,
    PSMCT16  = 0x02,
    PSMCT16S = 0x0A,
    PSMT8    = 0x13,
    PSMT4    = 0x14,
    PSMT8H   = 0x1B,    /* 8-bit index in bits 24..31 of a 32-bit pixel */
    PSMT4HL  = 0x24,    /* 4-bit index in bits 24..27 */
    PSMT4HH  = 0x2C,    /* 4-bit index in bits 28..31 */
    PSMZ32   = 0x30,
    PSMZ24   = 0x31,
    PSMZ16   = 0x32,
    PSMZ16S  = 0x3A
};

/* GS local memory: 4 MB = 0x4000 blocks of 256 bytes (64 words), 32 blocks
 * a page.  Addresses in the registers: TBP0 / CBP / DBP in blocks, FBP / ZBP
 * in pages; pgl passes word addresses (block * 64). */
enum : uint32_t {
    GS_BLOCK_BYTES = 256,
    GS_PAGE_BLOCKS = 32,
    GS_PAGE_BYTES  = 8192,
    GS_MEM_BLOCKS  = 0x4000,
    GS_MEM_BYTES   = 0x400000
};

bool        psm_valid(uint32_t psm);
const char *psm_name(uint32_t psm);          /* "CT32", "T4HH", ... ("?" if invalid) */
bool        psm_indexed(uint32_t psm);       /* T8, T4, T8H, T4HL, T4HH: through the CLUT */
bool        psm_depth(uint32_t psm);         /* Z32 / Z24 / Z16 / Z16S */
/* Bits of one pixel in a host transfer (CT24 / Z24: 24, T8H: 8, T4HL / T4HH: 4). */
uint32_t    psm_transfer_bits(uint32_t psm);
/* Bits of the field the pixel occupies in local memory (CT24: 24 of 32, T8H: 8). */
uint32_t    psm_field_bits(uint32_t psm);

/* Page / block geometry of a format (pixels). */
struct PsmGeom {
    uint32_t pageW, pageH;      /* CT32 64x32, CT16 64x64, T8 128x64, T4 128x128 */
    uint32_t blockW, blockH;    /* CT32 8x8, CT16 16x8, T8 16x16, T4 32x16 */
};
PsmGeom psm_geom(uint32_t psm);

/* ------------------------------------------------------------------------ */
/* Registers                                                                */
/* ------------------------------------------------------------------------ */

/* TEX0 (also TEX2: PSM and the CLUT fields; CLD as for TEX0). */
struct Tex0 {
    uint32_t tbp0 = 0;          /* bits  0..13 texture base, blocks */
    uint32_t tbw  = 0;          /* bits 14..19 buffer width, 64 pixels */
    uint32_t psm  = 0;          /* bits 20..25 */
    uint32_t tw   = 0;          /* bits 26..29 log2 width (the ST space: 1 << tw) */
    uint32_t th   = 0;          /* bits 30..33 log2 height */
    uint32_t tcc  = 0;          /* bit  34 1: RGBA (texture alpha used), 0: RGB */
    uint32_t tfx  = 0;          /* bits 35..36 0 modulate, 1 decal, 2 highlight, 3 highlight2 */
    uint32_t cbp  = 0;          /* bits 37..50 CLUT base, blocks */
    uint32_t cpsm = 0;          /* bits 51..54 CLUT format: CT32 (0), CT16 (2), CT16S (0xA) */
    uint32_t csm  = 0;          /* bit  55 0: CSM1 (the swizzled 16x16 / 8x2 CLUT), 1: CSM2 (a line, TEXCLUT) */
    uint32_t csa  = 0;          /* bits 56..60 CLUT entry offset, 16 entries */
    uint32_t cld  = 0;          /* bits 61..63 CLUT load control */

    static Tex0 decode(uint64_t v);
    uint64_t encode() const;
    uint32_t width() const { return 1u << tw; }
    uint32_t height() const { return 1u << th; }
};

/* TEX1 */
struct Tex1 {
    uint32_t lcm  = 0;          /* bit 0 LOD: 0 from Q (K + L), 1 fixed K */
    uint32_t mxl  = 0;          /* bits 2..4 maximum mip level (0: no mipmaps) */
    uint32_t mmag = 0;          /* bit 5 0 nearest, 1 linear */
    uint32_t mmin = 0;          /* bits 6..8 0 nearest, 1 linear, 2..5 the mipmap filters */
    uint32_t mtba = 0;          /* bit 9 MIPTBP1 computed by the GS */
    uint32_t l    = 0;          /* bits 19..20 */
    int32_t  k    = 0;          /* bits 32..43 signed 7.4 fixed point (k / 16.0) */

    static Tex1 decode(uint64_t v);
    uint64_t encode() const;
};

/* CLAMP */
struct Clamp {
    uint32_t wms  = 0;          /* bits 0..1 0 repeat, 1 clamp, 2 region clamp, 3 region repeat */
    uint32_t wmt  = 0;          /* bits 2..3 */
    uint32_t minu = 0;          /* bits  4..13 */
    uint32_t maxu = 0;          /* bits 14..23 */
    uint32_t minv = 0;          /* bits 24..33 */
    uint32_t maxv = 0;          /* bits 34..43 */

    static Clamp decode(uint64_t v);
    uint64_t encode() const;
};

/* TEXA: the alpha of CT24 / CT16 texels.  pgl keeps it at TA0 0, AEM 0,
 * TA1 0x80 (gfx_reset_texture_state 0x00250690; no game code changes it). */
struct Texa {
    uint32_t ta0 = 0;           /* bits  0..7 alpha of CT24 texels and of CT16 texels with A = 0 */
    uint32_t aem = 0;           /* bit  15 1: RGB = 0 (and A = 0) gives alpha 0 */
    uint32_t ta1 = 0x80;        /* bits 32..39 alpha of CT16 texels with A = 1 */

    static Texa decode(uint64_t v);
    uint64_t encode() const;
};

/* MIPTBP1 (levels 1..3) / MIPTBP2 (levels 4..6). */
struct Miptbp {
    uint32_t tbp[3] = {0, 0, 0};    /* bits 0..13, 20..33, 40..53 */
    uint32_t tbw[3] = {0, 0, 0};    /* bits 14..19, 34..39, 54..59 */

    static Miptbp decode(uint64_t v);
    uint64_t encode() const;
};

/* TEXCLUT (CSM2 only; GT3 never uses CSM2). */
struct TexClut {
    uint32_t cbw = 0;           /* bits 0..5 CLUT buffer width, 64 pixels */
    uint32_t cou = 0;           /* bits 6..11 x offset / 16 */
    uint32_t cov = 0;           /* bits 12..21 y offset */

    static TexClut decode(uint64_t v);
};

/* The destination of a host -> local transfer: BITBLTBUF.DBP / DBW / DPSM
 * (the upper word, Tex1Xfer.dbp / dbw / dpsm), TRXPOS.DSAX / DSAY and
 * TRXREG (w, h). */
struct Transfer {
    uint32_t dbp  = 0;          /* blocks */
    uint32_t dbw  = 0;          /* 64 pixels */
    uint32_t dpsm = 0;
    uint32_t dsax = 0;
    uint32_t dsay = 0;
    uint32_t w    = 0;
    uint32_t h    = 0;
};

/* The register block of one texture (G001_flare.h GsTexRegs, 0x28 bytes:
 * TEX0, TEX1, MIPTBP1, MIPTBP2, CLAMP), the unit Tex1 sets and pgl's
 * gfx_set_texture_regs work with. */
struct TexRegs {
    uint64_t tex0    = 0;
    uint64_t tex1    = 0;
    uint64_t miptbp1 = 0;
    uint64_t miptbp2 = 0;
    uint64_t clamp   = 0;
};

/* ------------------------------------------------------------------------ */
/* Sizes and footprints                                                     */
/* ------------------------------------------------------------------------ */

/* Bytes of a w x h host image of psm (CT24 3 per pixel, T4 half). */
uint32_t image_bytes(uint32_t psm, uint32_t w, uint32_t h);

/* Quadwords of the IMAGE transfer, as gs_build_image_transfer_packet counts
 * them (a psm it does not know: w * h, its quirk). */
uint32_t transfer_qwc(uint32_t psm, uint32_t w, uint32_t h);

/* tex_vram_size (0x0024BE68): words of a w x h image of psm rounded up to
 * whole pages (the pgl allocator's footprint), *alignedW the rounded width;
 * -1 for a psm it does not know. */
int32_t vram_words(uint32_t psm, int32_t w, int32_t h, int32_t *alignedW);

/* The blocks the rectangle (x, y, w, h) of a buffer (bp, bw, psm) touches:
 * [first, last] as plain (not wrapped) block numbers, last >= first; for
 * w or h 0: last = first - 1.  Exact for any alignment. */
struct BlockRange {
    uint32_t first = 0;
    uint32_t last  = 0;         /* inclusive */
    bool empty() const { return last < first; }
    uint32_t count() const { return empty() ? 0 : last - first + 1; }
    bool overlaps(const BlockRange &o) const {
        return !empty() && !o.empty() && first <= o.last && o.first <= last;
    }
};
BlockRange image_blocks(uint32_t psm, uint32_t bp, uint32_t bw,
                        uint32_t x, uint32_t y, uint32_t w, uint32_t h);

/* Marks in mask (one byte per block from baseBlock, wrapping at 0x4000;
 * blocks past mask.size() ignored) every block the rectangle touches: the
 * exact block set, for layouts where ranges interleave. */
void image_block_mask(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y,
                      uint32_t w, uint32_t h, uint32_t baseBlock, std::vector<uint8_t> &mask);

/* The blocks a texture reads: level 0 at TBP0 (the rectangle given, or the
 * whole 2^TW x 2^TH when w / h are 0). */
BlockRange texture_blocks(const Tex0 &t, uint32_t w = 0, uint32_t h = 0);

/* The blocks of its CLUT: CSM1 T8 16x16, T4 8x2 entries of CPSM at CBP
 * (plus the CSA offset), CSM2 the line of TEXCLUT.  Empty for a direct
 * colour format.  t8Load: the whole 256-entry CLUT (pgl's TEX2 trick, see
 * gs_tex_decode.h). */
BlockRange clut_blocks(const Tex0 &t, bool t8Load, const TexClut *tc = nullptr);

/* Mip level `level' (0..6) of a texture: its base, buffer width and size.
 * Levels 1..3 from MIPTBP1, 4..6 from MIPTBP2 (TEX1.MTBA is not modelled:
 * pgl never sets it).  The size halves per level, at least 1. */
struct MipLevel {
    uint32_t tbp = 0, tbw = 0, w = 0, h = 0;
};
MipLevel mip_level(const Tex0 &t, const Miptbp &m1, const Miptbp &m2, uint32_t level);

} /* namespace gstex */

#endif /* GT3PC_GS_TEX_FORMAT_H */
