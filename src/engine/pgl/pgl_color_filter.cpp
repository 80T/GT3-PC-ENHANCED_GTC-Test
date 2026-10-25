/* pgl_color_filter.cpp - the frame-buffer filters of pgl: the decompiled
 * color_filter.c (0x002582F8..0x0025A038), ported.
 *
 * On the PS2 both filters are VIF1 packets for VU1 micro programs (MSCALF
 * 0x763 / 0x78D) that patch and kick GIF data page by page, reading the
 * CT32 frame as PSMT8 bytes through CLUTs.  The port keeps the CPU part -
 * the tone-curve CLUT and the saturation matrix of pfx_color_correction,
 * the CLUT and index-texture uploads (the afterimage reads the identity
 * CLUT too) - and hands the per-pixel result to the renderer's recipes
 * (graphics.md 8.5 / 8.6): pglr_pfx_color_correction computes for every
 * pixel what the three additive CLUT passes and the write-back produce,
 * pglr_pfx_split_rgb writes the colour bytes into the planes' alpha. */
#include "engine/pgl/pgl_port.h"

#define U118_F_LUMA_R 0.300000012f
#define U118_F_LUMA_G 0.589999974f
#define U118_F_LUMA_B 0.11f
#define U118_F_1_255  0.00392156886f

/* CSM1 CLUT index order: bits 3 and 4 of the index swapped. */
#define PFX_CLUT_SWIZZLE(i)  (((i) & 0xE7) | (((i) & 8) << 1) | (((i) & 0x10) >> 1))
#define GIF_AD_TAG_N(n)     (GIF_NREG(1) | (u64)(n))
#define GIF_IMAGE_TAG(qwc)  (GIF_FLG_IMAGE | GIF_EOP | (u64)(qwc))

/* The GS registers the packets clobbered (re-sent on the next flush). */
#define PFX_DIRTY_CTX_REGS  (GS_DIRTY_BOTH(GS_REG_FRAME) | GS_DIRTY_BOTH(GS_REG_XYOFFSET) | \
                             GS_DIRTY_BOTH(GS_REG_ZBUF) | GS_DIRTY_BOTH(GS_REG_SCISSOR) |     \
                             GS_DIRTY_BOTH(GS_REG_TEST) | GS_DIRTY_BOTH(GS_REG_CLAMP) |       \
                             GS_DIRTY_BOTH(GS_REG_TEX2) | GS_DIRTY_BOTH(GS_REG_TEX1) |        \
                             GS_DIRTY_BOTH(GS_REG_TEX0))
#define PFX_COLOR_DIRTY     (PFX_DIRTY_CTX_REGS | GS_DIRTY_BOTH(GS_REG_ALPHA) |               \
                             GS_DIRTY_GEN(GS_GEN_COLCLAMP) | GS_DIRTY_GEN(GS_GEN_PABE) |      \
                             GS_DIRTY_GEN(GS_GEN_PRMODECONT))
#define PFX_SPLIT_DIRTY     (PFX_DIRTY_CTX_REGS | GS_DIRTY_BOTH(GS_REG_FBA) |                 \
                             GS_DIRTY_GEN(GS_GEN_PABE) | GS_DIRTY_GEN(GS_GEN_PRMODECONT))
static_assert(PFX_COLOR_DIRTY == 0x0000003107FB07FBull, "PFX_COLOR_DIRTY");
static_assert(PFX_SPLIT_DIRTY == 0x0000003007DF07DFull, "PFX_SPLIT_DIRTY");

extern "C" {
void pfx_build_curve_clut(u32 clutAddr, f32 r, f32 g, f32 b, f32 lo, f32 hi);
void pfx_upload_index_texture(u32 vramAddr);
}

/* The curve of the last pfx_build_curve_clut, by CLUT index (R, G, B). */
static u8 s_curve[256 * 3];

// FUNC 0x002582F8 pfx_color_correction
/* workAddr: free VRAM (the work page, the CLUT at +0x800, the index texture
 * at +0x900); r, g, b the curve control points, [lo, hi] the input range,
 * saturation 1.0 = unchanged, 0.0 = grey.  Source: the TEX0 texture,
 * destination: the FRAME buffer, tgtA's size. */
void pfx_color_correction(f32 r, f32 g, f32 b, f32 lo, f32 hi, f32 saturation, u32 workAddr)
{
    GsContext *ctx = GS_SPR_CTX;
    GsRenderTarget *tgt = &ctx->tgtA;
    u32 clutAddr = workAddr + 0x800;
    u32 idxAddr = workAddr + 0x900;
    u32 texAddr = (u32)(s32)(ctx->reg[GS_REG_TEX0] & 0x3FFF) << 6;
    u32 fbAddr = (u32)SHADOW_U16(ctx->reg[GS_REG_FRAME])[0] << 11;
    u32 fbw = SHADOW_U8(ctx->reg[GS_REG_FRAME])[2];
    f32 keep, rest, lumR, lumG, lumB, diagR, diagG, diagB;
    u32 colR, colG, colB;

    pfx_build_curve_clut(clutAddr, r, g, b, lo, hi);
    pfx_upload_index_texture(idxAddr);

    keep = ee_minf(saturation * 128.0f, 255.0f);
    rest = ee_maxf(128.0f - keep, 0.0f);
    lumR = rest * U118_F_LUMA_R;
    lumG = rest * U118_F_LUMA_G;
    lumB = rest * U118_F_LUMA_B;
    diagR = ee_minf(ee_maxf(keep + lumR, 0.0f), 255.0f);
    diagG = ee_minf(ee_maxf(keep + lumG, 0.0f), 255.0f);
    diagB = ee_minf(ee_maxf(keep + lumB, 0.0f), 255.0f);
    colR = (u32)(s32)diagR | ((u32)(s32)lumR << 8) | ((u32)(s32)lumR << 16);
    colG = (u32)(s32)lumG | ((u32)(s32)diagG << 8) | ((u32)(s32)lumG << 16);
    colB = (u32)(s32)lumB | ((u32)(s32)lumB << 8) | ((u32)(s32)diagB << 16);
    pglr_pfx_color_correction(texAddr >> 6, fbAddr >> 6, fbw, tgt->width, tgt->height, s_curve,
                              colR, colG, colB);
    ctx->dirty |= PFX_COLOR_DIRTY;
}

// FUNC 0x002584A0 pfx_build_color_packet
/* The VU1 packet of the colour correction (the renderer's recipe does its
 * work: pfx_color_correction).  Kept for the record: nothing to queue. */
extern "C" void pfx_build_color_packet(u32 texAddr, u32 fbAddr, u32 fbw, u32 workAddr, u32 clutAddr,
                                       u32 idxAddr, u64 scissor, u32 colR, u32 colG, u32 colB)
{
    (void)texAddr; (void)fbAddr; (void)fbw; (void)workAddr; (void)clutAddr;
    (void)idxAddr; (void)scissor; (void)colR; (void)colG; (void)colB;
}

// FUNC 0x00258DB0 pfx_build_curve_clut
/* Uploads (an image transfer in the packet) the 16 x 16 PSMCT24 tone-curve
 * CLUT: entry k (CSM1 order), per channel c (clamped to [0, 1]), 255 B(t)
 * with B(t) = t t + 2 c t (1 - t), t = (swz(k) / 255 - lo) / (hi - lo)
 * clamped to [0, 1] (0 when lo == hi); quirk: entry 0's t is not clamped.
 * The FPU accumulator arithmetic in the original's order.  PC: also kept
 * by index for the renderer's recipe. */
void pfx_build_curve_clut(u32 clutAddr, f32 r, f32 g, f32 b, f32 lo, f32 hi)
{
    u64 *p;
    u8 *out;
    f32 negLo, invRange, t, t2, outR, outG, outB, in;
    f32 acc;
    int i, idx;

    gs_flush_dirty_regs();
    p = (u64 *)gfx_alloc_direct(0x36);
    negLo = -lo;
    if (lo != hi)
        invRange = 1.0f / (hi - lo);
    else
        invRange = 0.0f;
    p[0] = GIF_AD_TAG_N(4);
    p[1] = GIF_REG_AD;
    ((u32 *)(p + 2))[0] = 0;
    ((u32 *)(p + 2))[1] = ((clutAddr >> 6) & 0x3FFF) | (SCE_GS_PSMCT24 << 24) | (1 << 16);
    p[3] = SCE_GS_BITBLTBUF;
    ((u32 *)(p + 4))[0] = 0;
    ((u32 *)(p + 4))[1] = 0;
    p[5] = SCE_GS_TRXPOS;
    ((u32 *)(p + 6))[0] = 0x10;
    ((u32 *)(p + 6))[1] = 0x10;
    p[7] = SCE_GS_TRXREG;
    p[8] = 0;
    p[9] = SCE_GS_TRXDIR;
    p[10] = GIF_IMAGE_TAG(0x30);
    p[11] = 0;
    out = (u8 *)(p + 12);

    r = ee_minf(r, 1.0f);
    g = ee_minf(g, 1.0f);
    b = ee_minf(b, 1.0f);
    r = ee_maxf(r, 0.0f);
    g = ee_maxf(g, 0.0f);
    b = ee_maxf(b, 0.0f);
    t = negLo * invRange;
    r += r;
    g += g;
    b += b;
    t2 = t * t;
    i = 0;
    do {
        int pos = i;                    /* this entry's position (CSM1 order) */

        i++;
        acc = t * t;
        idx = PFX_CLUT_SWIZZLE(i);
        acc += r * t;
        outR = acc - r * t2;
        acc = t * t;
        in = (f32)idx;
        acc += g * t;
        outG = acc - g * t2;
        in = in * U118_F_1_255;
        outR *= 255.0f;
        acc = t * t;
        acc += b * t;
        outB = acc - b * t2;
        in = in + negLo;
        outG *= 255.0f;
        outB *= 255.0f;
        t = in * invRange;
        outR = ee_minf(outR, 255.0f);
        outG = ee_minf(outG, 255.0f);
        outB = ee_minf(outB, 255.0f);
        t = ee_minf(t, 1.0f);
        outR = ee_maxf(outR, 0.0f);
        outG = ee_maxf(outG, 0.0f);
        outB = ee_maxf(outB, 0.0f);
        t = ee_maxf(t, 0.0f);
        t2 = t * t;
        out[0] = (u8)(s32)outR;
        out[1] = (u8)(s32)outG;
        out[2] = (u8)(s32)outB;
        out += 3;
        {
            int index = PFX_CLUT_SWIZZLE(pos);     /* the CLUT index reading this position */

            s_curve[index * 3 + 0] = (u8)(s32)outR;
            s_curve[index * 3 + 1] = (u8)(s32)outG;
            s_curve[index * 3 + 2] = (u8)(s32)outB;
        }
    } while (i < 0x100);
}

/* The 8x32 PSMT8 index texture (0x00345590). */
extern "C" const u8 g_00345590[0x100] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x0C, 0x0D, 0x0E, 0x0F, 0x08, 0x09, 0x0A, 0x0B, 0x1C, 0x1D, 0x1E, 0x1F, 0x18, 0x19, 0x1A, 0x1B,
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
    0x2C, 0x2D, 0x2E, 0x2F, 0x28, 0x29, 0x2A, 0x2B, 0x3C, 0x3D, 0x3E, 0x3F, 0x38, 0x39, 0x3A, 0x3B,
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
    0x4C, 0x4D, 0x4E, 0x4F, 0x48, 0x49, 0x4A, 0x4B, 0x5C, 0x5D, 0x5E, 0x5F, 0x58, 0x59, 0x5A, 0x5B,
    0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77,
    0x6C, 0x6D, 0x6E, 0x6F, 0x68, 0x69, 0x6A, 0x6B, 0x7C, 0x7D, 0x7E, 0x7F, 0x78, 0x79, 0x7A, 0x7B,
    0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x8C, 0x8D, 0x8E, 0x8F, 0x88, 0x89, 0x8A, 0x8B, 0x9C, 0x9D, 0x9E, 0x9F, 0x98, 0x99, 0x9A, 0x9B,
    0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7,
    0xAC, 0xAD, 0xAE, 0xAF, 0xA8, 0xA9, 0xAA, 0xAB, 0xBC, 0xBD, 0xBE, 0xBF, 0xB8, 0xB9, 0xBA, 0xBB,
    0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7,
    0xCC, 0xCD, 0xCE, 0xCF, 0xC8, 0xC9, 0xCA, 0xCB, 0xDC, 0xDD, 0xDE, 0xDF, 0xD8, 0xD9, 0xDA, 0xDB,
    0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7,
    0xEC, 0xED, 0xEE, 0xEF, 0xE8, 0xE9, 0xEA, 0xEB, 0xFC, 0xFD, 0xFE, 0xFF, 0xF8, 0xF9, 0xFA, 0xFB,
};

// FUNC 0x00259008 pfx_upload_index_texture
void pfx_upload_index_texture(u32 vramAddr)
{
    gs_upload_image(vramAddr, SCE_GS_PSMT8, 0x40, 0, 0, 8, 0x20, g_00345590);
}

// FUNC 0x00259240 pfx_build_identity_clut32
/* Uploads a 16 x 16 PSMCT32 CLUT whose entry k (CSM1 order) is k in all
 * four bytes.  (RacePS2Base::drawAfterimage reads it directly.) */
void pfx_build_identity_clut32(u32 clutAddr)
{
    u64 *p;
    u8 *out;
    int i;

    gs_flush_dirty_regs();
    p = (u64 *)gfx_alloc_direct(0x46);
    ((u32 *)p)[-2] = VIF_FLUSH;
    p[0] = GIF_AD_TAG_N(4);
    p[1] = GIF_REG_AD;
    ((u32 *)(p + 2))[0] = 0;
    ((u32 *)(p + 2))[1] = ((clutAddr >> 6) & 0x3FFF) | (SCE_GS_PSMCT32 << 24) | (1 << 16);
    p[3] = SCE_GS_BITBLTBUF;
    ((u32 *)(p + 4))[0] = 0;
    ((u32 *)(p + 4))[1] = 0;
    p[5] = SCE_GS_TRXPOS;
    ((u32 *)(p + 6))[0] = 0x10;
    ((u32 *)(p + 6))[1] = 0x10;
    p[7] = SCE_GS_TRXREG;
    p[8] = 0;
    p[9] = SCE_GS_TRXDIR;
    p[10] = GIF_IMAGE_TAG(0x40);
    p[11] = 0;
    out = (u8 *)(p + 12);
    for (i = 0; i < 0x100; i++) {
        int v = PFX_CLUT_SWIZZLE(i);

        out[0] = (u8)v;
        out[1] = (u8)v;
        out[2] = (u8)v;
        out[3] = (u8)v;
        out += 4;
    }
}

// FUNC 0x00259368 pfx_split_rgb_planes
/* The plane split (RacePS2Base::captureAfterimage): the identity CLUT and
 * the index texture uploaded, then the R, G, B bytes of the TEX0 texture
 * into the alpha bytes of the w x h planes at fbAddrR / G / B. */
void pfx_split_rgb_planes(u32 fbAddrR, u32 fbAddrG, u32 fbAddrB, s32 w, s32 h, u32 workAddr)
{
    GsContext *ctx = GS_SPR_CTX;
    u32 clutAddr = workAddr + 0x1800;
    u32 idxAddr = workAddr + 0x1900;
    u32 texAddr = (u32)(s32)(ctx->reg[GS_REG_TEX0] & 0x3FFF) << 6;

    pfx_build_identity_clut32(clutAddr);
    pfx_upload_index_texture(idxAddr);
    pglr_pfx_split_rgb(texAddr >> 6, fbAddrR >> 6, fbAddrG >> 6, fbAddrB >> 6, w, h);
    ctx->dirty |= PFX_SPLIT_DIRTY;
}
