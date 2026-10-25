/* pgl_prim.cpp - pgl's immediate VU1 primitives and model drawing: the
 * decompiled vu1_prim.c (0x0024FA98..0x00250208) and render.c (unit u114,
 * 0x0024BF10..0x0024D0C0), ported.
 *
 * The CPU side is the decomp's (texture-coordinate and colour generation,
 * the texture matrix, CPU lighting, the strip batcher's bookkeeping, the
 * texture / material selection, the material colour scales, model_draw's
 * path choice).  PC: what went to VU1 by DMA is recorded for the renderer
 * (pglr_strip / pglr_tri16 / pglr_model_part, the arrays read when the
 * frame executes); box_visible's VU0 clip test is pgl_math's. */
#include "engine/pgl/pgl_port.h"

using pgl::prim::GsVertex;

static inline u32 draw_flags(const GsContext *ctx)
{
    return ctx->halfHeight ? PGLR_HALF_HEIGHT : 0;
}

/* ======================================================================== */
/* vu1_prim.c                                                               */
/* ======================================================================== */

// FUNC 0x0024FA98 pgl_gen_colors
/* A colour buffer (n words) from the packet end, filled by colorGenFunc. */
void *pgl_gen_colors(GsContext *ctx, const void *xyz, const void *normals, const void *colors, int n)
{
    u32 *buf;

    buf = (u32 *)((u8 *)ctx->end - ((n + 3) >> 2) * 16);
    ctx->end = buf;
    ctx->colorGenFunc(buf, xyz, normals, colors, n);
    return buf;
}

// FUNC 0x0024FAE0 pgl_gen_texcoords
/* A texture-coordinate buffer from the packet end; texGenFunc is called
 * once with all zeros, then to fill it. */
void *pgl_gen_texcoords(GsContext *ctx, const void *xyz, const void *normals, const void *uvs, int n)
{
    u32 *buf;

    buf = (u32 *)((u8 *)ctx->end - ((n + 1) >> 1) * 16);
    ctx->end = buf;
    ctx->texGenFunc(NULL, NULL, NULL, NULL, 0);
    ctx->texGenFunc(buf, xyz, normals, uvs, n);
    return buf;
}

// FUNC 0x0024FB90 pgl_xform_texcoords
/* (s, t) -> the texture matrix x (s, t, 0, 1), x and y kept, into a buffer
 * from the packet end. */
void *pgl_xform_texcoords(GsContext *ctx, const void *uvs, int n)
{
    f32 *buf;
    pgl::Vec4 tex[4];

    buf = (f32 *)((u8 *)ctx->end - ((n + 1) >> 1) * 16);
    ctx->end = (u32 *)buf;
    pgl::vu0GetModeMatrix(VU0, ctx->matrixMode, pgl::MTX_TEXTURE, tex);
    for (int r = 0; r < 4; r++)
        VU0.vf[20 + r] = tex[r];
    pgl::xformTexcoords(tex, buf, (const f32 *)uvs, n);
    return buf;
}

// FUNC 0x0024FC48 pgl_light_vertices
/* CPU lighting (PGL_LF_CPU_LIGHTING): colour scale 128 textured, else 255;
 * the light state brought up to date; __pglShading's colour buffer. */
void *pgl_light_vertices(GsContext *ctx, const void *xyz, const void *normals, const void *colors,
                         const void *uvs, int n)
{
    if (uvs != NULL)
        ctx->colorScale = 128.0f;
    else
        ctx->colorScale = 255.0f;
    if (ctx->lightFlags & PGL_LF_LIGHTS)
        pgl_build_light_list(ctx);
    if (ctx->lightFlags & PGL_LF_MODELVIEW)
        pgl_update_eye_lights(ctx);
    if (ctx->lightFlags & PGL_LF_MATERIAL)
        pgl_update_light_colors(ctx);
    return __pglShading(ctx, (const float *)xyz, (const float *)normals, (const u32 *)colors, n);
}

// FUNC 0x0024FE00 pgl_put_texcoords
/* The texture coordinates' DMA ref.  PC: the strip record carries them. */
void pgl_put_texcoords(GsContext *ctx, const void *uvs, int n)
{
    (void)uvs;
    (void)n;
    ctx->stmask = STMASK_DEFAULT;
}

// FUNC 0x0024FEA8 pgl_draw_strip_vu1
/* A vertex array as one strip of the VU1 primitive program (nothing for n >
 * 64 or without a program): generated arrays first, then the strip with
 * PRIM = primAttr | TRISTRIP, TME by uvs, the colours or one constant
 * colour (colorTex textured, else color).  Statistics: n vertices, n - 2
 * primitives, 1 part. */
void pgl_draw_strip_vu1(const void *xyz, const void *normals, const void *colors, const void *uvs, int n)
{
    GsContext *ctx = GS_SPR_CTX;
    u32 prim;
    u32 constRgba;

    if (n >= VU1_MAX_VERTS + 1)
        return;
    if (ctx->vu1Prog == 0)
        return;
    ctx->nVerts += n;
    ctx->nPrims += n - 2;
    ctx->nParts++;
    if (ctx->matrixDirty != 0)
        pgl_sync_matrices(ctx);
    if (ctx->texGenEnable != 0 && ctx->texGenFunc != NULL)
        uvs = pgl_gen_texcoords(ctx, xyz, normals, uvs, n);
    if (ctx->texMatrixEnable != 0)
        uvs = pgl_xform_texcoords(ctx, uvs, n);
    if ((s8)ctx->lightFlags & PGL_LF_CPU_LIGHTING)
        colors = pgl_light_vertices(ctx, xyz, normals, colors, uvs, n);
    if (ctx->colorGenEnable != 0 && ctx->colorGenFunc != NULL)
        colors = pgl_gen_colors(ctx, xyz, normals, colors, n);
    prim = ctx->primAttr | GS_PRIM_TRIANGLE_STRIP;
    if (uvs != NULL)
        prim |= GS_PRIM_TME;
    else
        prim &= ~GS_PRIM_TME;
    constRgba = uvs != NULL ? ctx->colorTex : ctx->color;
    if (uvs != NULL)
        pgl_put_texcoords(ctx, uvs, n);
    if (colors == NULL)
        ctx->stmask = 0x55555555u;              /* pgl_put_const_color's STROW mask */
    pgl_vu1_matrix_due(ctx);
    if (ctx->dirty & ctx->enable)
        gs_collect_dirty_regs(ctx, NULL);
    g_pglVu1Sent.prog = ctx->vu1Prog;
    pglr_strip(&g_pglVu1Sent, prim, (const float *)xyz, (const float *)uvs, (const u32 *)colors,
               constRgba, n, NULL, 0, draw_flags(ctx));
}

// FUNC 0x002501A8 pgl_draw_strip
/* Strips of 3 vertices or more, only while list 0 (VIF1) is written. */
void pgl_draw_strip(const void *xyz, const void *normals, const void *colors, const void *uvs, int n)
{
    GsContext *ctx = GS_SPR_CTX;

    if (n < 3)
        return;
    if (ctx->curList == 0)
        pgl_draw_strip_vu1(xyz, normals, colors, uvs, n);
}

// FUNC 0x002501E0 gs_flush_all_regs
void gs_flush_all_regs(void)
{
    GS_SPR_CTX->dirty = GS_DIRTY_ALL;
    gs_flush_dirty_regs();
}

/* ======================================================================== */
/* render.c                                                                 */
/* ======================================================================== */

// FUNC 0x0024BF10 vu0_clip_point
/* The clip flags of (x, y, z) (the float bits) through vf24..vf27 against
 * its own w, remapped for w < 0. */
u32 vu0_clip_point(u64 x, u64 y, u64 z)
{
    return pgl::clipPointCombined(VU0, pgl_u2f((u32)x), pgl_u2f((u32)y), pgl_u2f((u32)z));
}

// FUNC 0x0024BF80 box_visible
/* 0 if the box lo..hi lies wholly outside one clip plane, else 1. */
int box_visible(const f32 *lo, const f32 *hi)
{
    vu0_load_screen_regs();
    return pgl::boxVisible(VU0, lo, hi) ? 1 : 0;
}

// FUNC 0x0024C098 mat_scale_set_grey
void mat_scale_set_grey(u32 kind, f32 v)
{
    mat_scale_set(kind, v, v, v, 1.0f);
}

// FUNC 0x0024C0B0 mat_scale_set_rgb
void mat_scale_set_rgb(u32 kind, f32 r, f32 g, f32 b)
{
    mat_scale_set(kind, r, g, b, 1.0f);
}

// FUNC 0x0024C0C0 mat_scale_set
/* The colour scale of a material component (0 ambient, 1 diffuse, 2 both,
 * 3 emission, 4 specular); material_load applies them when matScaleOn. */
void mat_scale_set(u32 kind, f32 r, f32 g, f32 b, f32 a)
{
    GsLibState *st = &g_002F72C0;

    switch (kind) {
    case PGL_MAT_AMBIENT:
    case PGL_MAT_AMBIENT_AND_DIFFUSE:
        st->matScale[MATSCALE_AMBIENT][0] = r;
        st->matScale[MATSCALE_AMBIENT][1] = g;
        st->matScale[MATSCALE_AMBIENT][2] = b;
        st->matScale[MATSCALE_AMBIENT][3] = a;
        if (kind == PGL_MAT_AMBIENT)
            break;
        /* fall through: ambient and diffuse */
    case PGL_MAT_DIFFUSE:
        st->matScale[MATSCALE_DIFFUSE][0] = r;
        st->matScale[MATSCALE_DIFFUSE][1] = g;
        st->matScale[MATSCALE_DIFFUSE][2] = b;
        st->matScale[MATSCALE_DIFFUSE][3] = a;
        break;
    case PGL_MAT_EMISSION:
        st->matScale[MATSCALE_EMISSION][0] = r;
        st->matScale[MATSCALE_EMISSION][1] = g;
        st->matScale[MATSCALE_EMISSION][2] = b;
        st->matScale[MATSCALE_EMISSION][3] = a;
        break;
    case PGL_MAT_SPECULAR:
        st->matScale[MATSCALE_SPECULAR][0] = r;
        st->matScale[MATSCALE_SPECULAR][1] = g;
        st->matScale[MATSCALE_SPECULAR][2] = b;
        st->matScale[MATSCALE_SPECULAR][3] = a;
        break;
    }
}

// FUNC 0x0024C148 draw_poly2d
/* A triangle fan of n window-pixel points (y * 8 for field rendering),
 * optional texels uv and colours rgba (else colorTex / color once). */
void draw_poly2d(const f32 *xy, const f32 *uv, const u32 *rgba, u32 n)
{
    GsContext *ctx = GS_SPR_CTX;
    GsVertex stackV[64];
    GsVertex *v = stackV;
    pgl::prim::Batch b;

    gs_flush_dirty_regs();
    if (n > 64)
        v = (GsVertex *)malloc(sizeof(GsVertex) * n);
    b = pgl::prim::polyFan2d(pgl::prim::primCtxOf(*ctx), xy, uv, rgba, (int)n, v);
    pglr_prim(b.prim, v, b.n, draw_flags(ctx));
    if (v != stackV)
        free(v);
}

/* The strip batcher's state (.sdata / .sbss / .bss). */
u8 g_stripBatchOpen = 0;                    /* 0x003530B4 */
static s32 g_stripBatchVerts;               /* 0x00353274 vertices in the batch (<= 64) */
static s32 g_stripBatchCount;               /* 0x00353278 strips in the batch */
static s32 g_0035327C;                      /* 0x0035327C only cleared */
static s32 g_stripBatchXyz2Addr;            /* 0x00353280 never set */
static u8  g_stripBatchLengths[VU1_MAX_VERTS];  /* 0x004A6880 */

/* PC: the arrays of the open batch (the DMA refs strip_batch_add queued),
 * each with its strips. */
struct StripSegment {
    const void *xyz;
    const void *rgba;
    const void *st;
    s32 n;
    s32 firstStrip;
    s32 nStrips;
};
static StripSegment s_segments[VU1_MAX_VERTS];
static int s_nSegments;

// FUNC 0x0024C488 strip_batch_begin
void strip_batch_begin(void)
{
    if (g_stripBatchOpen == 0) {
        g_stripBatchVerts = 0;
        g_stripBatchOpen = 1;
        g_stripBatchCount = 0;
        g_0035327C = 0;
        g_stripBatchXyz2Addr = 0;
        s_nSegments = 0;
    }
}

// FUNC 0x0024C4B0 strip_batch_end
/* Closes the open batch and kicks it when it has vertices: matrices and
 * dirty registers as due, the strip lengths, the GIF tag (PRIM triangle
 * strip | primAttr), MSCAL.  PC: each queued array with its strips is
 * recorded under the batch's state. */
void strip_batch_end(void)
{
    GsContext *ctx = GS_SPR_CTX;
    u32 prim;

    if (g_stripBatchOpen == 0)
        return;
    g_stripBatchOpen = 0;
    if (g_stripBatchVerts == 0)
        return;
    ctx->nParts++;
    if (ctx->matrixDirty)
        pgl_sync_matrices(ctx);
    pgl_vu1_matrix_due(ctx);
    if (ctx->dirty & ctx->enable)
        gs_collect_dirty_regs(ctx, NULL);
    ctx->stmask = 0x3F3F3F3Fu;
    prim = ctx->primAttr | GS_PRIM_TRIANGLE_STRIP;
    g_pglVu1Sent.prog = ctx->vu1Prog;
    for (int i = 0; i < s_nSegments; i++) {
        const StripSegment &sg = s_segments[i];

        pglr_strip(&g_pglVu1Sent, prim, (const float *)sg.xyz, (const float *)sg.st,
                   (const u32 *)sg.rgba, ctx->color, sg.n, &g_stripBatchLengths[sg.firstStrip],
                   sg.nStrips, draw_flags(ctx));
    }
    s_nSegments = 0;
}

// FUNC 0x0024C6B0 strip_batch_add
/* n (3..64) vertices into the open batch (flushed first if they would not
 * fit; counts padded to 4); strips = {count, lengths...}, NULL for one strip
 * of n.  Nothing without a VU1 program. */
void strip_batch_add(const void *xyz, const void *xyz2, const void *rgba, const void *st, s32 n,
                     const u8 *strips)
{
    GsContext *ctx = GS_SPR_CTX;
    s32 n4;
    StripSegment *sg;

    (void)xyz2;
    if (ctx->vu1Prog == 0)
        return;
    if (n < 3)
        return;
    if (n > VU1_MAX_VERTS)
        return;
    n4 = (n + 3) & ~3;
    if (g_stripBatchVerts + n4 > VU1_MAX_VERTS) {
        strip_batch_end();
        g_stripBatchVerts = 0;
        g_stripBatchOpen = 1;
        g_stripBatchCount = 0;
        g_0035327C = 0;
        g_stripBatchXyz2Addr = 0;
        s_nSegments = 0;
    }
    sg = &s_segments[s_nSegments++];
    sg->xyz = xyz;
    sg->rgba = rgba;
    sg->st = st;
    sg->n = n;
    sg->firstStrip = g_stripBatchCount;
    ctx->nVerts += n;
    g_stripBatchVerts += n;
    if (strips != NULL) {
        int count = strips[0];
        int i;

        strips++;
        for (i = 0; i < count; i++) {
            g_stripBatchLengths[g_stripBatchCount] = strips[i];
            ctx->nPrims += strips[i] - 2;
            g_stripBatchCount++;
        }
    } else {
        ctx->nPrims += n - 2;
        g_stripBatchLengths[g_stripBatchCount] = (u8)n;
        g_stripBatchCount++;
    }
    sg->nStrips = g_stripBatchCount - sg->firstStrip;
}

// FUNC 0x0024C940 strip_draw
/* One strip of n vertices: into the open batch, or as a batch of its own.
 * Nothing while the GIF list is selected. */
void strip_draw(const void *xyz, const void *xyz2, const void *rgba, const void *st, s32 n)
{
    GsContext *ctx = GS_SPR_CTX;

    if (ctx->curList != 0)
        return;
    if (g_stripBatchOpen != 0) {
        strip_batch_add(xyz, xyz2, rgba, st, n, NULL);
        return;
    }
    g_stripBatchVerts = 0;
    g_stripBatchOpen = 1;
    g_stripBatchCount = 0;
    g_0035327C = 0;
    g_stripBatchXyz2Addr = 0;
    s_nSegments = 0;
    strip_batch_add(xyz, xyz2, rgba, st, n, NULL);
    strip_batch_end();
}

// FUNC 0x0024C9A8 tri_draw16
/* n triangles (3n <= 64 vertices) of 16-bit data (xyzw V4-16, st V2-16,
 * rgba), kicked at once with the lighting entry 0x48 (VU1L_DEFAULT, format
 * 3).  The second argument is not used.  (Memory card icons.) */
void tri_draw16(const void *xyzw16, const void *unused, const void *rgba, const void *st16, s32 n)
{
    GsContext *ctx = GS_SPR_CTX;
    s32 nVerts;

    (void)unused;
    if (ctx->vu1Prog == 0)
        return;
    if (n == 0)
        return;
    nVerts = n * 3;
    if (nVerts > VU1_MAX_VERTS)
        return;
    ctx->nParts++;
    if (ctx->matrixDirty)
        pgl_sync_matrices(ctx);
    pgl_vu1_matrix_due(ctx);
    if (ctx->dirty & ctx->enable)
        gs_collect_dirty_regs(ctx, NULL);
    ctx->stmask = 0x3F3F3F3Fu;
    g_pglVu1Sent.prog = ctx->vu1Prog;
    pglr_tri16(&g_pglVu1Sent, ctx->primAttr | GS_PRIM_TRIANGLE, (const int16_t *)xyzw16,
               (const int16_t *)st16, (const u32 *)rgba, n, draw_flags(ctx));
}

// FUNC 0x0024CCB0 pgl_set_part_tex_func
void pgl_set_part_tex_func(PglPartTexFunc fn)
{
    GS_SPR_CTX->partTexFunc = fn;
}

// FUNC 0x0024CCC0 material_load
/* A material record into the context's material 0 (scaled by the library's
 * matScale when matScaleOn and the record's flag bit 0); the lighting due
 * again. */
void material_load(const GsMaterial *m)
{
    GsLibState *st = &g_002F72C0;
    GsContext *ctx = GS_SPR_CTX;
    pgl::Vec4 amb = vec4_of(m->ambient), dif = vec4_of(m->diffuse);
    pgl::Vec4 spc = vec4_of(m->specular), emi = vec4_of(m->emission);

    if (st->matScaleOn && (m->x44.flags & 1)) {
        amb = pgl::vu::mul(amb, vec4_load(st->matScale[MATSCALE_AMBIENT]));
        dif = pgl::vu::mul(dif, vec4_load(st->matScale[MATSCALE_DIFFUSE]));
        spc = pgl::vu::mul(spc, vec4_load(st->matScale[MATSCALE_SPECULAR]));
        emi = pgl::vu::mul(emi, vec4_load(st->matScale[MATSCALE_EMISSION]));
    }
    VU0.vf[1] = amb;
    VU0.vf[2] = dif;
    VU0.vf[3] = spc;
    VU0.vf[4] = emi;
    ctx->material[0].shininess = m->shininess;
    ctx->lightFlags |= PGL_LF_MATERIAL;
    vec4_store(ctx->material[0].ambient, amb);
    vec4_store(ctx->material[0].diffuse, dif);
    vec4_store(ctx->material[0].specular, spc);
    vec4_store(ctx->material[0].emission, emi);
}

// FUNC 0x0024CD48 gfx_set_texture_regs
/* The bootstrap's gfx_set_texture_regs with the override block of
 * g_002F72C0: CLAMP, TEX0 (CLD stripped), TEX1 into the shadows; TEX0 with
 * CLD != 0 also to TEX2 (PSMT8, CSA 0 when CSA != 0) to load the CLUT; with
 * mipmapping TEX1.K + lodBias << L and MIPTBP1 (/ 2). */
void gfx_set_texture_regs(const GsTexRegs *t)
{
    GsContext *ctx = GS_SPR_CTX;
    u64 tex0 = t->tex0;
    u64 tex1 = t->tex1;
    u64 clamp = t->clamp;
    u64 dirty = ctx->dirty;
    s32 mxl4;

    if (g_002F72C0.enable) {
        GsLibState *o = &g_002F72C0;

        tex0 &= ~o->tex0Mask;
        tex0 |= ctx->reg[GS_REG_TEX0] & o->tex0Mask;
        tex1 &= ~o->tex1Mask;
        tex1 |= ctx->reg[GS_REG_TEX1] & o->tex1Mask;
        clamp &= ~o->clampMask;
        clamp |= ctx->reg[GS_REG_CLAMP] & o->clampMask;
    }
    ctx->reg[GS_REG_CLAMP] = clamp;
    ctx->reg[GS_REG_TEX0] = tex0 & (~0ull >> 3);
    dirty |= GS_DIRTY_REG_BOTH(GS_REG_CLAMP) | GS_DIRTY_REG_BOTH(GS_REG_TEX1) | GS_DIRTY_REG_BOTH(GS_REG_TEX0);
    ctx->reg[GS_REG_TEX1] = tex1;
    if ((tex0 >> 61) != 0) {
        if (((tex0 >> 56) & 0x1F) != 0) {
            tex0 &= ~0x1F00000003F00000ull;
            tex0 |= (u64)GS_PSMT8 << 20;
        }
        ctx->reg[GS_REG_TEX2] = tex0;
        dirty |= GS_DIRTY_REG_BOTH(GS_REG_TEX2);
    }
    mxl4 = (s32)(tex1 & 0x1C);
    if (mxl4 != 0) {
        s32 k = (s32)((tex1 >> 32) & 0xFFF);
        s32 l = (s32)((tex1 >> 19) & 3);

        k = (s32)((u32)k << 20) >> 16;
        k += ctx->lodBias << l;
        k >>= 4;
        ctx->reg[GS_REG_TEX1] = (ctx->reg[GS_REG_TEX1] & 0xFFFFF000FFFFFFFFull) | ((u64)(k & 0xFFF) << 32);
        dirty |= GS_DIRTY_REG_BOTH(GS_REG_MIPTBP1);
        ctx->reg[GS_REG_MIPTBP1] = t->miptbp1;
        if ((u32)mxl4 >= 0xD) {
            dirty |= GS_DIRTY_REG_BOTH(GS_REG_MIPTBP2);
            ctx->reg[GS_REG_MIPTBP2] = t->miptbp2;
        }
    }
    ctx->dirty = dirty;
    tex1_texture_source(t, ctx->reg[GS_REG_TEX0]);  /* PC: a set without VRAM (ROADMAP 18) */
}

/* The texture / material selection. */
s32 g_texselTex = 0;                        /* 0x003530B8 */
s32 g_texselMat = 0;                        /* 0x003530BC */
s32 g_texselTexBase2 = 0;                   /* 0x003530C0 */
s32 g_texselMatBase2 = 0;                   /* 0x003530C4 */
static GsTexRegs  *g_texselTexTable;        /* 0x00353284 */
static GsMaterial *g_texselMatTable;        /* 0x00353288 */
static GsTexRegs  *g_texselTexTable2;       /* 0x0035328C */
static GsMaterial *g_texselMatTable2;       /* 0x00353290 */

// FUNC 0x0024CED0 texsel_reset
void texsel_reset(void)
{
    g_texselMat = 0;
    g_texselTex = 0;
    g_texselTexBase2 = 0;
    g_texselMatBase2 = 0;
}

// FUNC 0x0024CEE8 texsel_get_table2
GsTexRegs *texsel_get_table2(void)
{
    return g_texselTexTable2;
}

// FUNC 0x0024CEF0 matsel_set_base
void matsel_set_base(s32 base)
{
    g_texselMatBase2 = base;
}

// FUNC 0x0024CEF8 matsel_set_table2
void matsel_set_table2(GsMaterial *t)
{
    g_texselMatTable2 = t;
    g_texselMatBase2 = 0;
    g_texselMat = 0;
}

// FUNC 0x0024CF08 matsel_set_table
void matsel_set_table(GsMaterial *t)
{
    g_texselMatTable = t;
    g_texselMat = 0;
}

// FUNC 0x0024CF18 texsel_set_base
void texsel_set_base(s32 base)
{
    g_texselTexBase2 = base;
}

// FUNC 0x0024CF20 texsel_set_table2
void texsel_set_table2(GsTexRegs *t)
{
    g_texselTexTable2 = t;
    g_texselTexBase2 = 0;
    g_texselTex = 0;
}

// FUNC 0x0024CF30 texsel_set_table
void texsel_set_table(GsTexRegs *t)
{
    g_texselTexTable = t;
    g_texselTex = 0;
}

// FUNC 0x0024CF40 texmat_select
/* id bits 0..8 the texture (1-based in table 1; 0x1FF = table 2 at its
 * base), bits 9..15 the material (0x7F = table 2); 0 keeps the current, a
 * repeated index is not reloaded.  (The material's 0x1FF test is dead, as
 * in the code.) */
void texmat_select(s32 id)
{
    s32 mat = (id >> 9) & 0x7F;
    s32 tex = id & 0x1FF;

    if (tex == 0x1FF)
        tex = g_texselTexBase2 + 0x1FF;
    if (tex != 0 && tex != g_texselTex) {
        if (tex >= 0x1FF) {
            if (g_texselTexTable2 != NULL)
                gfx_set_texture_regs(&g_texselTexTable2[g_texselTexBase2]);
        } else if (g_texselTexTable != NULL) {
            gfx_set_texture_regs(g_texselTexTable + tex - 1);
        }
        g_texselTex = tex;
    }
    if (mat == 0x1FF)
        mat = g_texselMatBase2 + 0x1FF;
    if (mat != 0 && mat != g_texselMat) {
        if (mat >= 0x7F) {
            if (g_texselMatTable2 != NULL)
                material_load(&g_texselMatTable2[g_texselMatBase2]);
        } else if (g_texselMatTable != NULL) {
            material_load(g_texselMatTable + mat - 1);
        }
        g_texselMat = mat;
    }
}

// FUNC 0x0024D048 model_draw
/* A prebuilt VU1 model (when a program is selected and it has parts): the
 * general path when lit, overridden or without a part-texture function,
 * else the fast path. */
void model_draw(VifModel *m)
{
    GsContext *ctx = GS_SPR_CTX;
    GsLibState *st = &g_002F72C0;
    u32 prog = ctx->vu1Prog;

    if (prog == 0)
        return;
    if (m->nParts == 0)
        return;
    if (m->mode != 0 || st->enable != 0 || ctx->partTexFunc == NULL)
        vifmodel_draw_lit(m, prog);
    else
        vifmodel_draw(m, prog);
}

// FUNC 0x0024D0C0 vu1_unpack_xyz
/* Room for n (rounded up to 4) positions unpacked to VU1 0 in front of the
 * next model part; returns it (the caller fills it).  PC: the next
 * model_draw's first part takes them (read at execution). */
u32 *vu1_unpack_xyz(s32 n)
{
    GsContext *ctx = GS_SPR_CTX;
    s32 n4 = (n + 3) & ~3;
    s32 words = n4 * 3;
    u32 *cur = ctx->cur;

    cur[0] = DMATAG_CNT(words >> 2);
    ctx->cur = cur + 4 + words;
    cur[3] = ((u32)n4 << 16) | VIF_UNPACK(0x68, 0, 0);
    cur[1] = 0;
    cur[2] = VIF_NOP;
    g_pglPreXyz = (const float *)(cur + 4);
    g_pglPreXyzCount = n;
    return cur + 4;
}
