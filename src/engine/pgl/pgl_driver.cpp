/* pgl_driver.cpp - the GS driver of pgl (G001): the decompiled
 * gs_display_driver.c (0x00250A58..0x00254828), gs_driver_core.c
 * (0x00250208..0x00250A58), gs_driver.c (0x0025A038..0x0025ABD0) and
 * gs_context.c (0x00258150..0x002582F8), ported.
 *
 * Kept as the decomp has it: the context init, every register setter and
 * switch (they write GsContext's shadows and dirty bits), the packet
 * bookkeeping (frame / sub-packet headers, list tails: plain memory), the
 * vsync wait and the vblank handler's waiter lists.  Native (pgl_bridge.h):
 * the display (gfx_set_display_fb presents), the register flush (the
 * emitted shadows are recorded), the kick (executes the recorded frame), the
 * clears and frame-buffer copies (recorded as the register writes and
 * sprites the PS2 packets held), DMA / CRTC / interrupt set-up (gone).
 *
 * PC: progressive (docs/ROADMAP.md, "Full progressive scan rendering
 * everywhere"; docs/renderer.md, "Fields").  The display mode is
 * non-interlaced, so there is no field: ctx->field stays 0,
 * gs_get_field_mode answers PGL_FIELD_FRAME and gfx_set_field_mode drops
 * the half-line field offsets.  The one-field (224-line) buffers keep their
 * GS size; the renderer draws them at full height. */
#include "engine/pgl/pgl_port.h"
#include "platform/frame_step.h"            /* PC: the tick step's pause (gfx_tick_vsync) */

/* ======================================================================== */
/* Data                                                                     */
/* ======================================================================== */

extern "C" {

GsContext g_pglCtx;                         /* the scratchpad context (0x70003000) */
GfxDriverState g_gfx;                       /* 0x004A69C0 */

/* The DMAC register blocks the driver addressed (D0 / D1 / D2).  No DMA in
 * the port: three inert blocks keep the pointers valid. */
static EeDmaChanRegs s_dmaChanRegs[3];
EeDmaChanRegs *g_gsDmaChan[3] = { &s_dmaChanRegs[0], &s_dmaChanRegs[1], &s_dmaChanRegs[2] };

/* gfx_depth_func's PGL_DEPTHFUNC_* -> TEST.ZTST (0x002FC1F8). */
u8 g_gsDepthFuncZtst[] = { GS_ZTST_NEVER, GS_ZTST_ALWAYS, GS_ZTST_GREATER, GS_ZTST_GEQUAL };

/* The byte stack of pgl_push_attrib / pgl_pop_attrib (0x002FB1E8). */
u8 g_pglAttribStack[0x1000];
u8 *g_pglAttribSp = NULL;                   /* 0x003530DC */
u8 g_gfxInitialized = 0;                    /* 0x003530D4 */

/* gs_driver.c's .sdata: the LCG of the VU1 microcode unscrambling. */
s8  g_003530E0 = 0;
u32 g_003530E4 = 0x32277070;

/* The GS register numbers of the shadows (gs_collect_dirty_regs). */
u8 g_gsRegsGeneral[GS_GEN_COUNT] = {
    SCE_GS_COLCLAMP, SCE_GS_DIMX, SCE_GS_DTHE, SCE_GS_FOGCOL, SCE_GS_PABE,
    SCE_GS_PRMODECONT, SCE_GS_SCANMSK, SCE_GS_TEXA, SCE_GS_TEXCLUT, SCE_GS_TEXFLUSH,
};
u8 g_gsRegsCtx1[GS_REG_COUNT] = {
    SCE_GS_FRAME_1, SCE_GS_XYOFFSET_1, SCE_GS_FBA_1, SCE_GS_ZBUF_1, SCE_GS_SCISSOR_1,
    SCE_GS_ALPHA_1, SCE_GS_TEST_1, SCE_GS_CLAMP_1, SCE_GS_TEX2_1, SCE_GS_TEX1_1,
    SCE_GS_TEX0_1, SCE_GS_MIPTBP1_1, SCE_GS_MIPTBP2_1,
};
u8 g_gsRegsCtx2[GS_REG_COUNT] = {
    SCE_GS_FRAME_2, SCE_GS_XYOFFSET_2, SCE_GS_FBA_2, SCE_GS_ZBUF_2, SCE_GS_SCISSOR_2,
    SCE_GS_ALPHA_2, SCE_GS_TEST_2, SCE_GS_CLAMP_2, SCE_GS_TEX2_2, SCE_GS_TEX1_2,
    SCE_GS_TEX0_2, SCE_GS_MIPTBP1_2, SCE_GS_MIPTBP2_2,
};

}  /* extern "C" */

/* gfx_init_vu0_constants' FLT_MAX (.sdata 0x003530D8). */
#define GS_F_FLT_MAX    3.40282347e+38f
#define GS_F_0_2        0.200000003f
#define GS_F_0_8        0.800000012f

/* GS IMR shadow of gfx_init: only FINISH unmasked. */
#define GS_IMR_FINISH_ONLY  ((u64)(s64)-0x201)
#define GS_PMODE_INIT       0x66
#define GS_SMODE2_FIELD     1
#define GS_SMODE2_FRAME     3
#define GS_SMODE2_NOINTER   2

/* The VU1 state of a draw: the combined matrix vf24..vf27 and the viewport
 * vectors (pgl_send_matrix's block) and the program. */
void pgl_vu1_state(GsContext *ctx, PglrVu1 *out)
{
    for (int r = 0; r < 4; r++) {
        out->m[r * 4 + 0] = VU0.vf[24 + r].x;
        out->m[r * 4 + 1] = VU0.vf[24 + r].y;
        out->m[r * 4 + 2] = VU0.vf[24 + r].z;
        out->m[r * 4 + 3] = VU0.vf[24 + r].w;
    }
    memcpy(out->viewScale, &ctx->viewScale, 16);
    memcpy(out->viewCenter, &ctx->viewCenter, 16);
    memcpy(out->limits, &ctx->limits, 16);
    memcpy(out->vBB0, &ctx->vBB0, 16);
    out->prog = ctx->vu1Prog;
    /* PC: the projection's clip z = A w + B (docs/renderer.md 4,
     * "Precision"): the renderer takes the depth from w with them when the
     * combined matrix agrees (pgl_exec.cpp vu1_depth) */
    pgl::Vec4 p[4];
    pgl::vu0GetModeMatrix(VU0, ctx->matrixMode, pgl::MTX_PROJECTION, p);
    double ww = 0.0, zw = 0.0, zz = 0.0;
    for (int r = 0; r < 3; r++) {
        ww += (double)p[r].w * p[r].w;
        zw += (double)p[r].z * p[r].w;
        zz += (double)p[r].z * p[r].z;
    }
    out->projZ[0] = out->projZ[1] = out->projZ[2] = 0.0f;
    if (ww > 1e-20) {
        const double a = zw / ww;
        double res = 0.0;
        for (int r = 0; r < 3; r++) {
            const double d = p[r].z - a * p[r].w;
            res += d * d;
        }
        if (res <= 1e-10 * zz) {
            out->projZ[0] = (float)a;
            out->projZ[1] = (float)((double)p[3].z - a * p[3].w);
            out->projZ[2] = 1.0f;
        }
    }
}

/* ======================================================================== */
/* gs_driver_core.c                                                         */
/* ======================================================================== */

// FUNC 0x00250208 gs_flush_dirty_regs
/* Without an active packet forgets the dirty set; otherwise emits the dirty
 * & enabled shadows.  PC: the emitted registers are recorded for the
 * renderer (the A+D block the PS2 packet held). */
void gs_flush_dirty_regs(void)
{
    GsContext *ctx = GS_SPR_CTX;
    u64 mask;

    if (!(ctx->packetOpen || ctx->subPacketOpen)) {
        ctx->dirty = 0;
        return;
    }
    mask = ctx->dirty & ctx->enable;
    if (mask == 0)
        return;
    ctx->dirty &= ~mask;
    ctx->written |= mask;
    pglr_regs(mask, ctx->reg, ctx->gen);
}

// FUNC 0x002502C8 gfx_perf_counter0
/* The CPU-cycle count (the per-vblank accumulator + COP0 PCR0 on the PS2;
 * the callers seed random generators with it).  PC: the CPU's counter. */
u64 gfx_perf_counter0(void)
{
    return pglr_cycles();
}

// FUNC 0x002502F8 gfx_perf_counter1
u64 gfx_perf_counter1(void)
{
    return pglr_cycles();
}

// FUNC 0x00250328 gfx_wait_vsync_poll
/* Polling vblank wait (CSR.VSINT).  PC: waits for the next vblank. */
void gfx_wait_vsync_poll(void)
{
    u64 seen = pglr_load64(&g_gfx.vblankCount);

    while (pglr_load64(&g_gfx.vblankCount) == seen)
        DelayThread(1000);
}

// FUNC 0x00250378 gfx_reset_paths
/* Resets VIF0 / VIF1 / VU0 / VU1 / GIF.  PC: nothing to reset. */
void gfx_reset_paths(void)
{
}

// FUNC 0x002503E0 gfx_init_vu0_constants
/* Identity in vf28..vf31 and parked for all three matrix modes, stack
 * pointers vi11..vi13; the context's constant floats; the attribute stack
 * pointer. */
void gfx_init_vu0_constants(GsContext *ctx)
{
    pgl::ctxInitVu0Constants(*ctx, VU0);
    ctx->fogStart = 0.0f;
    ctx->fogAmount = 1.0f;
    g_pglAttribSp = g_pglAttribStack;
}

// FUNC 0x002504E8 gfx_init_lights
/* The 8 lights cleared, light 0 with the OpenGL GL_LIGHT0 defaults (but
 * quadAtten 1.0); eyeWorld {0,0,0,1}, global ambient {0.2,0.2,0.2,1}, every
 * lighting flag dirty, lightMode 2. */
void gfx_init_lights(GsContext *ctx)
{
    f32 zero;

    memset(ctx->lights, 0, sizeof(ctx->lights));
    ctx->lights[0].linearAtten = 0.0f;
    zero = ctx->lights[0].linearAtten;
    ctx->lights[0].ambient.w  = 1.0f;
    ctx->lights[0].diffuse.x  = 1.0f;
    ctx->lights[0].diffuse.y  = 1.0f;
    ctx->lights[0].diffuse.z  = 1.0f;
    ctx->lights[0].diffuse.w  = 1.0f;
    ctx->lights[0].specular.x = 1.0f;
    ctx->lights[0].specular.y = 1.0f;
    ctx->lights[0].specular.z = 1.0f;
    ctx->lights[0].specular.w = 1.0f;
    ctx->lights[0].position.z = 1.0f;
    ctx->lights[0].constAtten = 1.0f;
    ctx->lights[0].quadAtten  = 1.0f;
    ctx->lights[0].spotDir.z  = -1.0f;
    ctx->lights[0].spotCutoff = 180.0f;
    ctx->lights[0].spotExpEnc = float_to_gs_param_encode(zero);
    ctx->globalAmbient.x = GS_F_0_2;
    ctx->globalAmbient.y = GS_F_0_2;
    ctx->globalAmbient.z = GS_F_0_2;
    ctx->globalAmbient.w = 1.0f;
    ctx->eyeWorld.x = zero;
    ctx->eyeWorld.y = zero;
    ctx->eyeWorld.z = zero;
    ctx->eyeWorld.w = 1.0f;
    ctx->lightFlags = PGL_LF_DIRTY;
    ctx->lightMode = 2;
}

// FUNC 0x002505C0 gfx_init_material
/* The OpenGL material defaults on face 0; qAF0 = qB00 = 1, qB10 = 0;
 * colour material off, kind 2 (ambient and diffuse) on face mask 1. */
void gfx_init_material(GsContext *ctx)
{
    memset(ctx->material, 0, sizeof(ctx->material));
    ctx->material[0].ambient.x  = GS_F_0_2;
    ctx->material[0].ambient.y  = GS_F_0_2;
    ctx->material[0].ambient.z  = GS_F_0_2;
    ctx->material[0].ambient.w  = 1.0f;
    ctx->material[0].diffuse.x  = GS_F_0_8;
    ctx->material[0].diffuse.y  = GS_F_0_8;
    ctx->material[0].diffuse.z  = GS_F_0_8;
    ctx->material[0].diffuse.w  = 1.0f;
    ctx->material[0].specular.w = 1.0f;
    ctx->material[0].emission.w = 1.0f;
    ctx->material[0].shininessEnc = float_to_gs_param_encode(0.0f);
    ctx->qAF0.x = 1.0f;
    ctx->qAF0.y = 1.0f;
    ctx->qAF0.z = 1.0f;
    ctx->qAF0.w = 1.0f;
    ctx->qB00.x = 1.0f;
    ctx->qB00.y = 1.0f;
    ctx->qB00.z = 1.0f;
    ctx->qB00.w = 1.0f;
    ctx->colorMaterialKind = 2;
    ctx->colorMaterialFaces = 1;
    ctx->colorMaterial = 0;
    ctx->qB10.x = 0.0f;
    ctx->qB10.y = 0.0f;
    ctx->qB10.z = 0.0f;
    ctx->qB10.w = 0.0f;
}

// FUNC 0x00250690 gfx_reset_texture_state
/* TEXA {0, AEM 0, 0x80}; TEX0 = TCC; TEX1 bilinear; TEX2 = PSMT8 + CLD;
 * MIPTBP1 / 2, CLAMP = 0.  Quirk kept: TEX2 is not marked dirty. */
void gfx_reset_texture_state(GsContext *ctx)
{
    u8 *texa = (u8 *)&ctx->gen[GS_GEN_TEXA];

    texa[0] = 0;
    texa[4] = GS_TEXA_TA1_DEFAULT;
    ctx->gen[GS_GEN_TEXA] &= ~GS_TEXA_AEM;
    ctx->dirty |= GS_DIRTY_TEX_STATE;
    ctx->reg[GS_REG_TEX0] = GS_TEX0_TCC;
    ctx->reg[GS_REG_TEX1] = GS_TEX1_DEFAULT;
    ctx->reg[GS_REG_TEX2] = GS_TEX2_DEFAULT;
    ctx->reg[GS_REG_MIPTBP2] = 0;
    ctx->reg[GS_REG_CLAMP] = 0;
    ctx->reg[GS_REG_MIPTBP1] = 0;
}

// FUNC 0x00250708 gfx_context_init
void gfx_context_init(GsContext *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->gen[GS_GEN_COLCLAMP] |= GS_COLCLAMP_CLAMP;
    ctx->gen[GS_GEN_PRMODECONT] |= GS_PRMODECONT_AC;
    ctx->reg[GS_REG_ZBUF] &= ~GS_ZBUF_ZMSK;
    ctx->uncachedBuf = 0;
    ctx->depthFar = 1.0f;
    ctx->dirty = GS_DIRTY_ALL;
    ctx->primAttr = GS_PRIM_IIP;
    ctx->depthNear = 0.0f;
    ctx->normal[0] = 0.0f;
    ctx->color = 0xFFFFFFFF;
    ctx->normal[1] = 0.0f;
    ctx->fB4C = 0;
    ctx->frontFace = 0;
    ctx->cullEnable = 0;
    ctx->colorTex = 0x80808080;
    ctx->normal[2] = 1.0f;
    ctx->cullFace = 2;
    ctx->vu1ProgAlt = 1;
    ctx->vu1Prog = VU1_PRIM_PROG_DEFAULT;
    gfx_reset_texture_state(ctx);
    gfx_init_vu0_constants(ctx);
    gfx_init_lights(ctx);
    gfx_init_material(ctx);
}

// FUNC 0x00250808 gfx_init_default
/* PC: non-interlaced (the original: gfx_init(1, 1, omode, 1), interlaced). */
void gfx_init_default(int omode)
{
    gfx_init(1, SCE_GS_NOINTERLACE, omode, SCE_GS_FRAME);
}

// FUNC 0x00250820 gfx_init
/* One-shot driver init.  PC: the display mode and the context as on the
 * PS2; the vblank handler installed; no GS / VIF / VU / DMA hardware, no
 * microcode (the renderer is native: pglr_init). */
void gfx_init(int fullReset, int inter, int omode, int ffmd)
{
    GsContext *ctx = GS_SPR_CTX;
    GfxDriverState *g = &g_gfx;

    (void)fullReset;
    if (g_gfxInitialized == 1)
        return;

    memset((void *)g, 0, sizeof(*g));
    gfx_store_display_mode(inter, omode, ffmd);
    g->gsRevision = 0;
    g->imr = GS_IMR_FINISH_ONLY;
    gfx_set_screen_size(640, 448);
    gfx_set_display_area(0, 0, 640, 448);
    gfx_reset_paths();
    gfx_dma_init();
    g->vsyncCallback = NULL;
    pglr_init();
    gfx_context_init(ctx);
    g->intcIdVblank = AddIntcHandler(INTC_VBLANK_S, intc_vblank_handler, 0);
    EnableIntc(INTC_VBLANK_S);
    g_gfxInitialized = 1;
}

// FUNC 0x00250A30 gfx_context_init_spr
void gfx_context_init_spr(void)
{
    gfx_context_init(GS_SPR_CTX);
}

/* ======================================================================== */
/* gs_display_driver.c                                                      */
/* ======================================================================== */

// FUNC 0x00250A58 gfx_init_vu0_constants_spr
void gfx_init_vu0_constants_spr(void)
{
    gfx_init_vu0_constants(GS_SPR_CTX);
}

// FUNC 0x00250A80 gfx_init_lights_material_spr
void gfx_init_lights_material_spr(void)
{
    gfx_init_lights(GS_SPR_CTX);
    gfx_init_material(GS_SPR_CTX);
}

// FUNC 0x00250AB8 gfx_reset_texture_state_spr
void gfx_reset_texture_state_spr(void)
{
    gfx_reset_texture_state(GS_SPR_CTX);
}

// FUNC 0x00250AE0 gfx_packet_is_open
u8 gfx_packet_is_open(void)
{
    return GS_SPR_CTX->packetOpen;
}

// FUNC 0x00250AF0 gfx_change_display_mode
void gfx_change_display_mode(int inter, int omode, int ffmd)
{
    GfxDriverState *g = &g_gfx;

    if (g->inter == inter && g->omode == omode && g->ffmd == ffmd)
        return;
    gfx_store_display_mode(inter, omode, ffmd);
    gfx_update_display_regs(g);
    gfx_reset_graph_stored_mode();
}

// FUNC 0x00250B58 gfx_store_display_mode
void gfx_store_display_mode(int inter, int omode, int ffmd)
{
    GfxDriverState *g = &g_gfx;

    g->omode = (u8)omode;
    g->inter = (u8)inter;
    g->ffmd  = (u8)ffmd;
    if (inter == SCE_GS_INTERLACE) {
        if (ffmd == SCE_GS_FRAME)
            g->smode2 = GS_SMODE2_FRAME;
        else
            g->smode2 = GS_SMODE2_FIELD;
    } else {
        g->smode2 = GS_SMODE2_NOINTER;
    }
}

// FUNC 0x00250BA0 gfx_reset_graph_stored_mode
/* sceGsResetGraph(5, ...): re-programs the CRTC.  PC: the window keeps its
 * mode. */
void gfx_reset_graph_stored_mode(void)
{
}

// FUNC 0x00250BC0 gfx_set_display_enable
/* Read circuit 2 on / off (PMODE.EN2).  PC: black / the displayed buffer. */
void gfx_set_display_enable(u8 on)
{
    if (on != 0) {
        if (on == 1)
            pglr_display_enable(1);
    } else {
        pglr_display_enable(0);
    }
}

// FUNC 0x00250C10 gfx_update_display_regs
/* The DISPLAY2 / PMODE / BGCOLOR shadows (kept: the game reads none of
 * them, the presentation uses dbx / dby / dispW / dispH). */
void gfx_update_display_regs(GfxDriverState *g)
{
    s32 magh, magv, lines, top, dh;
    s32 dx, dy, dw;

    lines = (g->omode == SCE_GS_NTSC) ? 0x1C0 : 0x200;
    top   = (g->omode == SCE_GS_NTSC) ? 0x19 : 0x24;
    dx = g->dispDX;
    dy = g->dispDY;
    dw = g->dispW;
    dh = g->dispH;
    magh = g->screenW != 0 ? (s32)(g->screenW + 0x9FF) / (s32)g->screenW : 1;
    magv = g->screenH != 0 ? lines / (s32)g->screenH : 1;
    if (g->inter == SCE_GS_INTERLACE) {
        top <<= 1;
        dh <<= 1;
    }
    if (g->ffmd == SCE_GS_FIELD)
        dh >>= 1;
    g->display2 = (u64)((u32)(dx * magh + 0x28C) & 0xFFFF)
                | ((u64)((u32)(top + dy) & 0xFFF) << 12)
                | ((u64)(s64)(magh - 1) << 23)
                | ((u64)(s64)(magv - 1) << 27)
                | ((u64)(s64)(magh * dw - 1) << 32)
                | ((u64)(s64)(magv * dh - 1) << 44);
    g->pmode   = GS_PMODE_INIT;
    g->bgcolor = 0;
}

// FUNC 0x00250CF8 gfx_set_display_area
void gfx_set_display_area(int dbx, int dby, int w, int h)
{
    GfxDriverState *g = &g_gfx;

    g->dbx   = (u16)dbx;
    g->dby   = (u16)dby;
    g->dispW = (u16)w;
    g->dispH = (u16)h;
    gfx_update_display_regs(g);
}

// FUNC 0x00250D18 gfx_set_screen_size
void gfx_set_screen_size(int w, int h)
{
    GfxDriverState *g = &g_gfx;

    g->screenW = (u16)w;
    g->screenH = (u16)h;
    gfx_update_display_regs(g);
}

// FUNC 0x00250D30 gfx_set_display_fb
/* Frame flip: the DISPFB2 shadow, then the CRTC registers.  PC: presents
 * the buffer (pglr_display: its display rectangle at 4:3), a whole
 * progressive frame (no field to flip). */
void gfx_set_display_fb(u32 fbWordAddr, int psm, int fbw, int fbh)
{
    GfxDriverState *g = &g_gfx;

    (void)fbh;
    g->dispfb2 = (u64)(fbWordAddr >> 11)
               | ((u64)((fbw >> 6) & 0x3F) << 9)
               | ((u64)(psm & 0xF) << 15)
               | ((u64)(g->dbx & 0x7FF) << 32)
               | ((u64)(g->dby & 0x7FF) << 43);
    pglr_display(fbWordAddr >> 6, (u32)psm, (u32)fbw, g->dbx, g->dby, g->dispW, g->dispH);
}

// FUNC 0x00250DD8 gfx_dma_channel_init
/* Channel ch's mutex (the CHCR / address registers: no DMA in the port). */
void gfx_dma_channel_init(int ch, u32 chcr)
{
    GfxDriverState *g = &g_gfx;
    struct SemaParam param;

    g_gsDmaChan[ch]->chcr = chcr;
    g->chan[ch].busy = 0;
    g->chan[ch].tagIrq = 0;
    memset(&param, 0, sizeof(param));
    param.initCount = 1;
    param.maxCount  = 1;
    g->chan[ch].sema = CreateSema(&param);
}

// FUNC 0x00250E68 gfx_dma_init
void gfx_dma_init(void)
{
    gfx_dma_channel_init(DMAC_VIF0, 0x40);
    gfx_dma_channel_init(DMAC_VIF1, 0xC0);
    gfx_dma_channel_init(DMAC_GIF, 0x80);
}

// FUNC 0x00250F60 gfx_dma_lock
void gfx_dma_lock(int ch)
{
    WaitSema(g_gfx.chan[ch].sema);
}

// FUNC 0x00250F80 gfx_dma_unlock
void gfx_dma_unlock(int ch)
{
    SignalSema(g_gfx.chan[ch].sema);
}

// FUNC 0x00250FA0 gfx_dma_sync
/* Waits until channel ch is idle.  PC: channels are never busy (the
 * renderer executes synchronously): 0. */
int gfx_dma_sync(int ch)
{
    (void)ch;
    return 0;
}

// FUNC 0x00251150 gfx_dma_send
/* A normal-mode transfer from memory.  PC: only the image uploads used it
 * (gs_upload_image_sync, tex1_upload_to_vram: pglr_upload / pglr_tex1_upload
 * there). */
void gfx_dma_send(int ch, const void *madr, u32 qwc)
{
    (void)ch;
    (void)madr;
    (void)qwc;
    pglr_log_once("gfx_dma_send", "pgl: gfx_dma_send has no native meaning (ignored)");
}

// FUNC 0x00251210 gfx_kick_frame_lists
/* Kicks a finished frame (hdr, else the context's).  PC: executes it. */
void gfx_kick_frame_lists(GsFrameHeader *hdr)
{
    GfxDriverState *g = &g_gfx;
    GsContext *ctx = GS_SPR_CTX;

    if (hdr == NULL) {
        hdr = ctx->frameHdr;
        if (hdr == NULL)
            return;
    }
    g->pathSync16 = 0;
    g->pathSync17 = 0;
    g->pathSync18 = 0;
    g->finishCount = 0;
    pglr_kick();
}

// FUNC 0x002512D0 vu_upload_microcode
/* PC: there is no VU microcode (pgl_math.cpp and the renderer's shaders
 * compute what it did). */
void vu_upload_microcode(int ch, u32 vuAddr, int byteSize, const void *code)
{
    (void)ch;
    (void)vuAddr;
    (void)byteSize;
    (void)code;
}

// FUNC 0x002513E0 gfx_enable
/* glEnable-style switch (PGL_CAP_*); a register change marks it dirty. */
void gfx_enable(u32 what)
{
    GsContext *ctx = GS_SPR_CTX;

    switch (what) {
    case 0x00:
        break;
    case PGL_CAP_PATH3:
        if (GS_PACKET_ACTIVE(ctx))
            gfx_mskpath3(0);
        ctx->path3Masked = 0;
        break;
    case PGL_CAP_ALPHA_TEST:
        ctx->reg[GS_REG_TEST] |= GS_FIELD_MASK(GS_TEST_ATE);
        ctx->dirty |= GS_DIRTY_BOTH(GS_REG_TEST);
        break;
    case PGL_CAP_DEST_ALPHA:
        ctx->reg[GS_REG_TEST] |= GS_FIELD_MASK(GS_TEST_DATE);
        ctx->dirty |= GS_DIRTY_BOTH(GS_REG_TEST);
        break;
    case PGL_CAP_DEPTH_TEST:
        ((GsTest *)&ctx->reg[GS_REG_TEST])->ZTST = ctx->zTestMethod;
        ctx->depthTest = 1;
        ((GsZbuf *)&ctx->reg[GS_REG_ZBUF])->ZMSK = ctx->zMask;
        ((GsTest *)&ctx->reg[GS_REG_TEST])->ZTE = 1;
        ctx->dirty |= GS_DIRTY_BOTH(GS_REG_ZBUF) | GS_DIRTY_BOTH(GS_REG_TEST);
        break;
    case PGL_CAP_BLEND:
        ctx->primAttr |= GS_PRIM_ABE;
        break;
    case PGL_CAP_FOG:
        ctx->primAttr |= GS_PRIM_FGE;
        break;
    case PGL_CAP_ANTIALIAS:
        ctx->primAttr |= GS_PRIM_AA1;
        break;
    case PGL_CAP_TEXTURE:
        ctx->primAttr |= GS_PRIM_TME;
        break;
    case PGL_CAP_DITHER:
        ctx->gen[GS_GEN_DTHE] |= GS_DTHE_ON;
        ctx->dirty |= GS_DIRTY_GEN(GS_GEN_DTHE);
        break;
    case PGL_CAP_TEXGEN:
        ctx->texGenEnable = 1;
        break;
    case PGL_CAP_COLORGEN:
        ctx->colorGenEnable = 1;
        break;
    case PGL_CAP_COLOR_CLAMP:
        ctx->gen[GS_GEN_COLCLAMP] |= GS_COLCLAMP_CLAMP;
        ctx->dirty |= GS_DIRTY_GEN(GS_GEN_COLCLAMP);
        break;
    case PGL_CAP_COLOR_MATERIAL:
        ctx->colorMaterial = 1;
        break;
    case PGL_CAP_CPU_LIGHTING:
        ctx->lightFlags |= PGL_LF_CPU_LIGHTING;
        break;
    case PGL_CAP_LIGHT0 + 0: case PGL_CAP_LIGHT0 + 1:
    case PGL_CAP_LIGHT0 + 2: case PGL_CAP_LIGHT0 + 3:
    case PGL_CAP_LIGHT0 + 4: case PGL_CAP_LIGHT0 + 5:
    case PGL_CAP_LIGHT0 + 6: case PGL_CAP_LIGHT0 + 7:
        ctx->lights[what - PGL_CAP_LIGHT0].enable = 1;
        ctx->lightFlags |= PGL_LF_LIGHTS;
        break;
    case PGL_CAP_FLAG24:
        ctx->flag24 = 1;
        break;
    case PGL_CAP_CULL_FACE:
        ctx->cullEnable = 1;
        gs_select_vu1_prog(ctx);
        return;
    case PGL_CAP_VU1_PROG_33E:
        ctx->vu1ProgAlt = 1;
        gs_select_vu1_prog(ctx);
        return;
    case PGL_CAP_FLAG27:
        ctx->flag27 = 1;
        break;
    case PGL_CAP_TEX_MATRIX:
        ctx->texMatrixEnable = 1;
        break;
    case PGL_CAP_SCALE:
        ctx->scaleEnable = 1;
        ctx->matrixDirty = 1;
        break;
    case PGL_CAP_NORMAL_MATRIX:
        ctx->normalMtxEnable = 1;
        break;
    default:
        break;
    }
}

// FUNC 0x00251670 gfx_disable
void gfx_disable(u32 what)
{
    GsContext *ctx = GS_SPR_CTX;

    switch (what) {
    case 0x00:
        break;
    case PGL_CAP_PATH3:
        if (GS_PACKET_ACTIVE(ctx))
            gfx_mskpath3(1);
        ctx->path3Masked = 1;
        break;
    case PGL_CAP_ALPHA_TEST:
        ctx->reg[GS_REG_TEST] &= ~GS_FIELD_MASK(GS_TEST_ATE);
        ctx->dirty |= GS_DIRTY_BOTH(GS_REG_TEST);
        break;
    case PGL_CAP_DEST_ALPHA:
        ctx->reg[GS_REG_TEST] &= ~GS_FIELD_MASK(GS_TEST_DATE);
        ctx->dirty |= GS_DIRTY_BOTH(GS_REG_TEST);
        break;
    case PGL_CAP_DEPTH_TEST: {
        u64 test = ctx->reg[GS_REG_TEST];
        u64 zbuf = ctx->reg[GS_REG_ZBUF];

        test &= ~GS_FIELD_MASK(GS_TEST_ZTST);
        test |= GS_FIELD_VAL(GS_TEST_ZTST, GS_ZTST_ALWAYS);
        zbuf |= GS_ZBUF_ZMSK;
        test |= GS_FIELD_MASK(GS_TEST_ZTE);
        ctx->reg[GS_REG_ZBUF] = zbuf;
        ctx->dirty |= GS_DIRTY_BOTH(GS_REG_ZBUF) | GS_DIRTY_BOTH(GS_REG_TEST);
        ctx->reg[GS_REG_TEST] = test;
        ctx->depthTest = 0;
        break;
    }
    case PGL_CAP_BLEND:
        ctx->primAttr &= ~GS_PRIM_ABE;
        break;
    case PGL_CAP_FOG:
        ctx->primAttr &= ~GS_PRIM_FGE;
        break;
    case PGL_CAP_ANTIALIAS:
        ctx->primAttr &= ~GS_PRIM_AA1;
        break;
    case PGL_CAP_TEXTURE:
        ctx->primAttr &= ~GS_PRIM_TME;
        break;
    case PGL_CAP_DITHER:
        ctx->gen[GS_GEN_DTHE] &= ~(u64)GS_DTHE_ON;
        ctx->dirty |= GS_DIRTY_GEN(GS_GEN_DTHE);
        break;
    case PGL_CAP_TEXGEN:
        ctx->texGenEnable = 0;
        break;
    case PGL_CAP_COLORGEN:
        ctx->colorGenEnable = 0;
        break;
    case PGL_CAP_COLOR_CLAMP:
        ctx->gen[GS_GEN_COLCLAMP] &= ~(u64)GS_COLCLAMP_CLAMP;
        ctx->dirty |= GS_DIRTY_GEN(GS_GEN_COLCLAMP);
        break;
    case PGL_CAP_COLOR_MATERIAL:
        ctx->colorMaterial = 0;
        break;
    case PGL_CAP_CPU_LIGHTING:
        ctx->lightFlags &= ~PGL_LF_CPU_LIGHTING;
        break;
    case PGL_CAP_LIGHT0 + 0: case PGL_CAP_LIGHT0 + 1:
    case PGL_CAP_LIGHT0 + 2: case PGL_CAP_LIGHT0 + 3:
    case PGL_CAP_LIGHT0 + 4: case PGL_CAP_LIGHT0 + 5:
    case PGL_CAP_LIGHT0 + 6: case PGL_CAP_LIGHT0 + 7:
        ctx->lights[what - PGL_CAP_LIGHT0].enable = 0;
        ctx->lightFlags |= PGL_LF_LIGHTS;
        break;
    case PGL_CAP_FLAG24:
        ctx->flag24 = 0;
        break;
    case PGL_CAP_CULL_FACE:
        ctx->cullEnable = 0;
        gs_select_vu1_prog(ctx);
        return;
    case PGL_CAP_VU1_PROG_33E:
        ctx->vu1ProgAlt = 0;
        gs_select_vu1_prog(ctx);
        return;
    case PGL_CAP_FLAG27:
        ctx->flag27 = 0;
        break;
    case PGL_CAP_TEX_MATRIX:
        ctx->texMatrixEnable = 0;
        break;
    case PGL_CAP_SCALE:
        ctx->scaleEnable = 0;
        ctx->matrixDirty = 1;
        break;
    case PGL_CAP_NORMAL_MATRIX:
        ctx->normalMtxEnable = 0;
        break;
    default:
        break;
    }
}

// FUNC 0x002518A8 gfx_mskpath3
/* List 0, dirty registers flushed, then MSKPATH3 (PATH3 masking has no
 * native meaning: the commands run in program order). */
void gfx_mskpath3(int mask)
{
    GsContext *ctx = GS_SPR_CTX;
    u32 *p;

    gfx_select_list(PGL_LIST_VIF1);
    gs_flush_dirty_regs();
    p = ctx->cur;
    ctx->cur = p + 4;
    p[0] = DMATAG_CNT(0);
    p[1] = 0;
    p[2] = VIF_FLUSH;
    p[3] = ((u32)(u8)mask << 15) | VIF_MSKPATH3;
}

// FUNC 0x00251918 gfx_cull_face
void gfx_cull_face(u32 mode)
{
    GsContext *ctx = GS_SPR_CTX;

    ctx->cullFace = mode;
    gs_select_vu1_prog(ctx);
}

// FUNC 0x00251930 gfx_front_face
void gfx_front_face(u32 mode)
{
    GsContext *ctx = GS_SPR_CTX;

    ctx->frontFace = mode;
    gs_select_vu1_prog(ctx);
}

// FUNC 0x00251948 gfx_set_packet_buffer
/* base, size, cur = the start, end = start + size.  PC: plain pointers (no
 * uncached segment). */
void gfx_set_packet_buffer(void *buf, u32 size)
{
    GsContext *ctx = GS_SPR_CTX;

    ctx->bufBase = (u8 *)buf;
    ctx->bufSize = size;
    ctx->cur = (u32 *)buf;
    ctx->end = buf != NULL ? (u32 *)((u8 *)buf + size) : NULL;
}

// FUNC 0x002519B8 gfx_get_packet_ptr
u32 *gfx_get_packet_ptr(void)
{
    return GS_SPR_CTX->cur;
}

// FUNC 0x002519C8 gfx_get_packet_space
int gfx_get_packet_space(void)
{
    GsContext *ctx = GS_SPR_CTX;

    return (int)((u8 *)ctx->end - (u8 *)ctx->cur);
}

// FUNC 0x002519E0 gfx_set_packet_access
/* Cached / uncached / uncached-accelerated access to the packet buffer.
 * PC: one kind of memory; the mode is stored with the PS2's quirk (with no
 * packet open only addrBits changes). */
void gfx_set_packet_access(u32 mode)
{
    GsContext *ctx = GS_SPR_CTX;

    if (ctx->uncachedBuf == mode)
        return;
    switch (mode) {
    case PGL_PKT_UNCACHED:
        ctx->addrBits = 0x20000000u;
        break;
    case PGL_PKT_CACHED:
        ctx->addrBits = 0;
        break;
    case PGL_PKT_UNCACHED_ACCEL:
        ctx->addrBits = 0x30000000u;
        break;
    default:
        return;
    }
    if (ctx->packetOpen == 0)
        return;
    ctx->uncachedBuf = mode;
}

// FUNC 0x00251B10 gfx_begin_packet
/* Opens a frame packet at ctx->cur (NULL if one is open): the header, the
 * list starts, every register dirty, list 0 current, statistics zeroed.
 * PC: and a new command list for the renderer. */
GsFrameHeader *gfx_begin_packet(void)
{
    GsContext *ctx = GS_SPR_CTX;
    GsFrameHeader *hdr;
    u32 *p;

    if (ctx->packetOpen)
        return NULL;

    p = ctx->cur;
    ctx->frameHdr = (GsFrameHeader *)p;
    p += 4;
    hdr = ctx->frameHdr;
    hdr->gifChain = p;
    hdr->save0 = NULL;
    hdr->save1 = NULL;
    ctx->tail1 = p;
    p += 4;
    p[0] = DMATAG_CNT(1);
    p[1] = 0;
    p[2] = VIF_BASE(0);
    p[3] = VIF_OFFSET(VU1_BUF_OFFSET);
    p[4] = VIF_STCYCL(4, 4);
    p[5] = VIF_STMOD(0);
    p[6] = VIF_STMASK;
    p[7] = STMASK_DEFAULT;
    hdr->vif1Chain = p;
    ctx->nParts = 0;
    ctx->tail0 = NULL;
    ctx->curList = PGL_LIST_VIF1;
    ctx->enableList0 = GS_ENABLE_GEN_CTX1;
    ctx->enableList1 = 0;
    ctx->matrixDirty = 1;
    ctx->dirty = GS_DIRTY_ALL;
    ctx->enable = GS_ENABLE_GEN_CTX1;
    ctx->stmask = STMASK_DEFAULT;
    ctx->vu1MatrixSent = 0;
    ctx->halfHeight = 0;
    ctx->fieldShift = 0;
    ctx->nVerts = 0;
    ctx->nPrims = 0;
    ctx->packetOpen = 1;
    ctx->cur = p + 8;
    pglr_begin_frame();
    return hdr;
}

// FUNC 0x00251C08 gfx_end_packet
void gfx_end_packet(void)
{
    GsContext *ctx = GS_SPR_CTX;
    u32 *end0, *end1, *cur;

    if (ctx->subPacketOpen)
        gfx_close_subpacket();
    if (ctx->packetOpen == 0)
        return;

    if (ctx->curList == PGL_LIST_VIF1) {
        end0 = ctx->cur;
        end1 = ctx->tail1;
        cur = end0;
    } else {
        end1 = ctx->cur;
        end0 = ctx->tail0;
        cur = end1;
    }
    gs_put_tag(end0, DMATAG_END);
    gs_put_tag(end1, DMATAG_END);
    ctx->cur = cur + 4;
    ctx->packetOpen = 0;
    pglr_end_frame();
}

// FUNC 0x00251CB8 gfx_begin_subpacket
/* Starts a sub-packet (replayed by gfx_call_subpacket).  PC: the renderer
 * records it as a command list of its own. */
GsSubPacketHeader *gfx_begin_subpacket(void)
{
    GsContext *ctx = GS_SPR_CTX;
    GsSubPacketHeader *sub;
    u32 *cur;

    if (ctx->subPacketOpen)
        gfx_close_subpacket();
    gs_flush_dirty_regs();
    cur = ctx->cur;
    if (ctx->packetOpen) {
        GsFrameHeader *hdr;
        if (ctx->curList == PGL_LIST_VIF1)
            ctx->tail0 = cur;
        else
            ctx->tail1 = cur;
        hdr = ctx->frameHdr;
        cur += 4;
        hdr->save0 = ctx->tail0;
        hdr->save1 = ctx->tail1;
    }
    ctx->subHdr = (GsSubPacketHeader *)cur;
    cur += 4;
    sub = ctx->subHdr;
    sub->gifChain = cur;
    sub->written = 0;
    ctx->tail1 = cur;
    cur += 4;
    ctx->enable = ctx->enableList0;
    sub->vif1Chain = cur;
    cur[0] = DMATAG_CNT(0);
    cur[2] = VIF_STMASK;
    cur[1] = 0;
    cur[3] = STMASK_DEFAULT;
    cur += 4;
    ctx->stmask = STMASK_DEFAULT;
    ctx->cur = cur;
    ctx->tail0 = NULL;
    if (ctx->curList == PGL_LIST_GIF) {
        ctx->curList = PGL_LIST_VIF1;
        gfx_select_list(PGL_LIST_GIF);
        sub = ctx->subHdr;
    }
    ctx->subPacketOpen = 1;
    ctx->vu1MatrixSent = 0;
    ctx->written = 0;
    pglr_sub_begin(sub);
    return sub;
}

// FUNC 0x00251DB8 gfx_close_subpacket
void gfx_close_subpacket(void)
{
    GsContext *ctx = GS_SPR_CTX;
    u32 *cur, *ret0, *ret1, *link;
    u64 written;

    if (ctx->subPacketOpen == 0)
        return;

    written = ctx->written;
    ctx->dirty = written;
    ctx->subHdr->written = written;
    cur = ctx->cur;
    if (ctx->curList == PGL_LIST_VIF1) {
        ret0 = cur;
        ret1 = ctx->tail1;
    } else {
        ret0 = ctx->tail0;
        ret1 = cur;
    }
    gs_put_tag(ret0, DMATAG_RET);
    gs_put_tag(ret1, DMATAG_RET);
    cur += 4;
    ctx->cur = cur;
    ctx->subPacketOpen = 0;
    pglr_sub_end();
    if (ctx->packetOpen == 0)
        return;

    ctx->tail0 = ctx->frameHdr->save0;
    ctx->tail1 = ctx->frameHdr->save1;
    link = ctx->curList ? ctx->tail1 : ctx->tail0;
    gs_put_next_tag(link, cur);
}

// FUNC 0x00251EB8 gfx_call_subpacket
/* Replays a sub-packet inside the open frame packet; the registers it
 * changed are dirty afterwards. */
void gfx_call_subpacket(const GsSubPacketHeader *sp)
{
    GsContext *ctx = GS_SPR_CTX;
    int prevList;

    if (ctx->packetOpen == 0)
        return;

    prevList = ctx->curList;
    gfx_select_list(PGL_LIST_VIF1);
    gs_flush_dirty_regs();
    pglr_sub_call(sp);
    ctx->dirty = sp->written;
    gfx_select_list(prevList);
}

// FUNC 0x00251FA8 gfx_select_list
/* Switches the packet writer to list `which'; returns the previous list.
 * PC: the renderer runs the commands of both lists in program order (which
 * satisfies every SIGNAL / WAIT / READBACK ordering of the PS2); the list
 * bookkeeping is kept. */
int gfx_select_list(int which)
{
    GsContext *ctx = GS_SPR_CTX;
    int prev = ctx->curList;
    u32 *cur;

    if (which == prev)
        return prev;
    if (ctx->path3Masked != 0 && which == PGL_LIST_GIF)
        return prev;
    if (!GS_PACKET_ACTIVE(ctx)) {
        ctx->curList = (u8)which;
        return prev;
    }

    cur = ctx->cur;
    if (ctx->curList == PGL_LIST_VIF1) {
        ctx->tail0 = cur;
        cur += 4;
        ctx->enableList0 = ctx->enable;
        ctx->enable = ctx->enableList1;
        gs_put_next_tag(ctx->tail1, cur);
    } else {
        ctx->tail1 = cur;
        cur += 4;
        ctx->enableList1 = ctx->enable;
        ctx->enable = ctx->enableList0;
        gs_put_next_tag(ctx->tail0, cur);
    }
    ctx->cur = cur;
    ctx->curList = (u8)which;
    return prev;
}

// FUNC 0x00252078 gfx_put_irq_marker
/* An interrupt marker (PGL_MARK_*) on the current list; returns a pointer
 * to the word holding the code (NULL without a packet).  PC: nothing waits
 * on them (program order). */
u32 *gfx_put_irq_marker(u32 code)
{
    GsContext *ctx = GS_SPR_CTX;
    u32 *p;

    if (!GS_PACKET_ACTIVE(ctx))
        return NULL;
    p = ctx->cur;
    if (ctx->curList == PGL_LIST_VIF1) {
        p[0] = DMATAG_CNT(0);
        p[3] = code | VIF_IBIT | VIF_FLUSH;
        p[1] = VIF_NOP;
        p[2] = VIF_NOP;
        ctx->cur = p + 4;
        p += 3;
    } else {
        if (code & PGL_MARK_SIGNAL) {
            u32 *q = p + 4;
            gs_put_tag(p, DMATAG_CNT(1));
            ((u64 *)q)[0] = GIF_NREG(1) | GIF_EOP;
            ((u64 *)q)[1] = GIF_REG_AD;
            p = q + 4;
        }
        ctx->cur = p + 4;
        gs_put_tag(p, code | DMATAG_IRQ | DMATAG_CNT(0));
    }
    return p;
}

// FUNC 0x00252140 gfx_sync_pipeline
/* Waits for DMA, VIF1, VU1 and the GIF to go idle.  PC: the renderer has
 * executed everything kicked. */
void gfx_sync_pipeline(void)
{
    pglr_sync();
}

/* PC (no original): the vsyncs of the game's threads but the one that runs
 * the ticks (gfx_wait_vblank_event, so gfx_vsync) are the game's ticks
 * (gfx_tick_vsync, by the render thread after each update), not the vblank
 * interrupt.  The main thread's menu loop (ControlBase::waitFrame) runs once
 * a vsync and reads the pads the tick updated: when the frames were slower
 * than the ticks (a frame cap below 60, slow frames) a frame ran two ticks
 * back to back and the loop ran twice on the second tick's reads - every
 * press counted twice in the menus (user report).  The tick thread waits for
 * the interrupt's vblanks.  The interrupt wakes the others too before the
 * first tick and while the ticks stall (more than TICK_STALL vblanks behind:
 * a load, a long frame), but not while the tick step pauses
 * (platform/frame_step.h: the menus' loops pause with the game). */
enum { TICK_STALL = 4 };
static volatile int s_tickThread = -1;          /* the thread of the ticks (-1: none yet) */
static volatile u64 s_vsyncEvents;              /* the other threads' vsyncs so far */
static volatile s64 s_eventBase;                /* vblank count - events at the last tick's resync */

/* vblanks counted minus vsync events given (interrupts off) */
static s64 vsync_behind(void)
{
    return (s64)pglr_load64(&g_gfx.vblankCount) - s_eventBase - (s64)pglr_load64(&s_vsyncEvents);
}

void gfx_tick_vsync(void)
{
    GfxDriverState *g = &g_gfx;
    GfxWaitNode *w;
    s64 behind;

    DIntr();
    if (s_tickThread < 0) {
        s_tickThread = GetThreadId();
        s_eventBase = (s64)pglr_load64(&g->vblankCount) - (s64)pglr_load64(&s_vsyncEvents) - 1;
    }
    behind = vsync_behind();
    if (behind > 1) {
        /* the vblanks of a stall (the interrupt's events gave the others
         * theirs) or of the ticks caught up: from here */
        s_eventBase += behind - 1;
        behind = 1;
    }
    /* not ahead of the vblanks: one event a tick */
    if (behind >= 0) {
        pglr_add64(&s_vsyncEvents, 1);
        for (w = g->vsyncWaiters; w != NULL; w = w->next)
            if (w->tid != s_tickThread)
                iWakeupThread(w->tid);
    }
    EIntr();
}

// FUNC 0x00252260 gfx_wait_vblank_event
/* Sleeps until the vblank counter changes (a node in vsyncWaiters, woken by
 * the vblank handler).  PC: a thread other than the ticks' waits for the
 * next vsync event instead (gfx_tick_vsync). */
void gfx_wait_vblank_event(void)
{
    GfxDriverState *g = &g_gfx;
    GfxWaitNode node;
    GfxWaitNode *it, *prev;
    u64 seen;
    int tid;
    int events;

    tid = GetThreadId();
    node.tid = tid;
    DIntr();
    node.next = g->vsyncWaiters;
    g->vsyncWaiters = &node;
    events = s_tickThread >= 0 && tid != s_tickThread;     /* PC */
    seen = events ? pglr_load64(&s_vsyncEvents) : pglr_load64(&g->vblankCount);
    EIntr();

    while (seen == (events ? pglr_load64(&s_vsyncEvents) : pglr_load64(&g->vblankCount)))
        SleepThread();

    DIntr();
    prev = NULL;
    for (it = g->vsyncWaiters; it != NULL; it = it->next) {
        if (it == &node) {
            if (prev != NULL)
                prev->next = node.next;
            else
                g->vsyncWaiters = node.next;
            break;
        }
        prev = it;
    }
    EIntr();
    CancelWakeupThread(tid);
}

// FUNC 0x00252338 gfx_vsync
/* Frame pacing.  n == 0: the vblank count.  n > 0: waits until n-1
 * vblanks passed since the last call, then for one more; returns the
 * vblanks elapsed.  n < 0: 1 at once if -n vblanks passed, else waits for
 * them and returns 0.  Then lastVsync = count, ctx->field = g.field when
 * interlaced. */
int gfx_vsync(int n)
{
    GfxDriverState *g = &g_gfx;
    GsContext *ctx = GS_SPR_CTX;
    u64 last;
    int ret;

    if (n == 0)
        return (s32)pglr_load64(&g->vblankCount);

    last = g->lastVsync;
    if (n > 0) {
        u64 minElapsed = (u64)(s64)(n - 1);
        while (pglr_load64(&g->vblankCount) - last < minElapsed)
            gfx_wait_vblank_event();
        gfx_wait_vblank_event();
        ret = (s32)(pglr_load64(&g->vblankCount) - last);
    } else {
        u64 minElapsed = (u64)(s64)(s32)(0u - (u32)n);
        if (pglr_load64(&g->vblankCount) - last >= minElapsed) {
            ret = 1;
        } else {
            while (pglr_load64(&g->vblankCount) - last < minElapsed)
                gfx_wait_vblank_event();
            ret = 0;
        }
    }

    g->lastVsync = pglr_load64(&g->vblankCount);
    /* PC: never interlaced (gfx_init_default), so the field is 0 */
    if (g->inter == SCE_GS_INTERLACE)
        ctx->field = (s8)g->field;
    else
        ctx->field = 0;
    return ret;
}

/* PC (no original): what gfx_vsync(-1) leaves after a vblank it did not wait
 * for - lastVsync at the count, the field - for the render thread's ticks
 * while it draws frames between them (game/app/pc_frame.h). */
void gfx_vsync_mark(void)
{
    GfxDriverState *g = &g_gfx;
    GsContext *ctx = GS_SPR_CTX;

    g->lastVsync = pglr_load64(&g->vblankCount);
    if (g->inter == SCE_GS_INTERLACE)
        ctx->field = (s8)g->field;
    else
        ctx->field = 0;
}

// FUNC 0x00252460 gfx_dma_sync_frame
void gfx_dma_sync_frame(void)
{
    pglr_sync();
}

// FUNC 0x00252480 gfx_set_target_b
/* The secondary render target tgtB (no register shadow): base, fbw = w >>
 * 6 (rounded down), psm, size, offsets. */
void gfx_set_target_b(u32 fbWordAddr, int psm, int w, int h)
{
    GsContext *ctx = GS_SPR_CTX;
    s32 xoff = (0x800 - (w >> 1)) << 4;
    s32 yoff = (0x800 - (h >> 1)) << 4;

    ctx->tgtB.width  = w;
    ctx->tgtB.height = h;
    ctx->tgtB.xoff   = xoff;
    ctx->tgtB.yoff   = yoff;
    ctx->tgtB.xend   = xoff + (w << 4);
    ctx->tgtB.yend   = yoff + (h << 4);
    ctx->tgtB.base   = fbWordAddr >> 6;
    ctx->tgtB.fbw    = w >> 6;
    ctx->tgtB.psm    = psm;
    pglr_target(fbWordAddr >> 6, (u32)w, (u32)psm, w, h);
}

// FUNC 0x002524E8 gfx_set_draw_buffer
/* The draw buffer (target A): FRAME FBP / FBW / PSM, XYOFFSET centred, both
 * dirty; halfHeight = 0; tgtA; then the draw area (0, 0, w, h). */
void gfx_set_draw_buffer(u32 fbWordAddr, int psm, int w, int h)
{
    GsContext *ctx = GS_SPR_CTX;
    s32 xoff = (0x800 - (w >> 1)) << 4;
    s32 yoff = (0x800 - (h >> 1)) << 4;
    s32 fbw = (w + 0x3F) >> 6;

    ctx->tgtA.width  = w;
    ctx->tgtA.height = h;
    ctx->tgtA.xoff   = xoff;
    ctx->tgtA.yoff   = yoff;
    ctx->tgtA.xend   = xoff + (w << 4);
    ctx->tgtA.yend   = yoff + (h << 4);
    ctx->tgtA.base   = fbWordAddr >> 6;
    ctx->tgtA.fbw    = fbw;
    ctx->tgtA.psm    = psm;
    SHADOW_U16(ctx->reg[GS_REG_FRAME])[0] = (u16)(fbWordAddr >> 11);
    SHADOW_U8(ctx->reg[GS_REG_FRAME])[2] = (u8)fbw;
    SHADOW_U8(ctx->reg[GS_REG_FRAME])[3] = (u8)psm;
    ((GsXyoffset *)&ctx->reg[GS_REG_XYOFFSET])->OFX = xoff;
    ((GsXyoffset *)&ctx->reg[GS_REG_XYOFFSET])->OFY = yoff;
    ctx->dirty |= GS_DIRTY_BOTH(GS_REG_FRAME) | GS_DIRTY_BOTH(GS_REG_XYOFFSET);
    ctx->halfHeight = 0;
    pglr_target(fbWordAddr >> 6, (u32)(fbw << 6), (u32)psm, w, h);
    gfx_set_draw_area(0, 0, w, h);
}

// FUNC 0x002525A0 gs_update_depth_range
/* The z lanes of the VU1 viewport from the depth range, the Z format's
 * scale and the two offsets; vu1MatrixSent = 0. */
void gs_update_depth_range(GsContext *ctx)
{
    f32 far = ctx->depthFar;
    f32 near = ctx->depthNear;
    f32 mid = (near + far) * 0.5f;
    f32 offset = ctx->depthOffset + 1.0f;
    f32 scale = ctx->depthScale;

    ctx->vu1MatrixSent = 0;
    ctx->viewScale.z = (near - far) * scale * 0.5f;
    ctx->viewCenter.z = ((1.0f - mid) + ctx->depthBias) * scale + offset;
}

// FUNC 0x00252600 gfx_set_depth_buffer
void gfx_set_depth_buffer(u32 zbWordAddr, int psm)
{
    GsContext *ctx = GS_SPR_CTX;

    switch ((u32)psm) {
    case SCE_GS_PSMZ32:
        ctx->depthScale = 4294836224.0f;
        break;
    case SCE_GS_PSMZ24:
        ctx->depthScale = 16776191.0f;
        break;
    case SCE_GS_PSMZ16:
    case SCE_GS_PSMZ16S:
        ctx->depthScale = 65531.0f;
        break;
    }
    ctx->spriteZF = (u32)(s32)ctx->depthScale | 0xFF000000;
    ((GsZbuf *)&ctx->reg[GS_REG_ZBUF])->ZBP = zbWordAddr >> 11;
    ((GsZbuf *)&ctx->reg[GS_REG_ZBUF])->PSM = psm;
    ctx->dirty |= GS_DIRTY_BOTH(GS_REG_ZBUF);
    gs_update_depth_range(ctx);
}

// FUNC 0x002526D0 gfx_depth_func
void gfx_depth_func(int func)
{
    GsContext *ctx = GS_SPR_CTX;
    u32 ztst = g_gsDepthFuncZtst[func];

    ctx->zTestMethod = ztst;
    if (ctx->depthTest) {
        ctx->reg[GS_REG_TEST] = GS_FIELD_SET(ctx->reg[GS_REG_TEST], GS_TEST_ZTST, ztst);
        ctx->dirty |= GS_DIRTY_BOTH(GS_REG_TEST);
    }
}

// FUNC 0x00252730 gfx_alpha_func
void gfx_alpha_func(int atst, float ref)
{
    GsContext *ctx = GS_SPR_CTX;

    ((GsTest *)&ctx->reg[GS_REG_TEST])->ATST = atst;
    ref = ee_clampf(ref * 128.0f, 0.0f, 255.0f);
    ((GsTest *)&ctx->reg[GS_REG_TEST])->AREF = (s32)ref;
    ctx->dirty |= GS_DIRTY_BOTH(GS_REG_TEST);
}

// FUNC 0x002527A8 gfx_alpha_fail
void gfx_alpha_fail(int afail)
{
    GsContext *ctx = GS_SPR_CTX;

    ((GsTest *)&ctx->reg[GS_REG_TEST])->AFAIL = afail;
    ctx->dirty |= GS_DIRTY_BOTH(GS_REG_TEST);
}

// FUNC 0x002527E8 gfx_blend_func
void gfx_blend_func(int abcd, float fix)
{
    GsContext *ctx = GS_SPR_CTX;

    ((GsAlpha *)&ctx->reg[GS_REG_ALPHA])->ABCD = abcd;
    ((GsAlpha *)&ctx->reg[GS_REG_ALPHA])->FIX = (s32)ee_clampf(fix * 128.0f, 0.0f, 255.0f);
    ctx->dirty |= GS_DIRTY_BOTH(GS_REG_ALPHA);
}

// FUNC 0x00252838 gfx_dest_alpha_func
void gfx_dest_alpha_func(int mode, float ref)
{
    GsContext *ctx = GS_SPR_CTX;

    (void)ref;
    if (mode == PGL_ATEST_GEQUAL)
        ctx->reg[GS_REG_TEST] |= GS_FIELD_MASK(GS_TEST_DATM);
    else if (mode == PGL_ATEST_LESS)
        ctx->reg[GS_REG_TEST] &= ~GS_FIELD_MASK(GS_TEST_DATM);
    ctx->dirty |= GS_DIRTY_BOTH(GS_REG_TEST);
}

// FUNC 0x00252890 gfx_alpha_func_raw
void gfx_alpha_func_raw(int atst, u8 ref)
{
    GsContext *ctx = GS_SPR_CTX;

    ((GsTest *)&ctx->reg[GS_REG_TEST])->ATST = atst;
    ((GsTest *)&ctx->reg[GS_REG_TEST])->AREF = ref;
    ctx->dirty |= GS_DIRTY_BOTH(GS_REG_TEST);
}

// FUNC 0x002528E0 gfx_blend_func_raw
void gfx_blend_func_raw(u8 abcd, u8 fix)
{
    GsContext *ctx = GS_SPR_CTX;

    SHADOW_U8(ctx->reg[GS_REG_ALPHA])[0] = abcd;
    SHADOW_U8(ctx->reg[GS_REG_ALPHA])[4] = fix;
    ctx->dirty |= GS_DIRTY_BOTH(GS_REG_ALPHA);
}

// FUNC 0x00252908 gfx_color_mask
void gfx_color_mask(u8 r, u8 g, u8 b, u8 a)
{
    u32 writable = 0;

    if (r)
        writable |= 0xFF;
    if (g)
        writable |= 0xFF00;
    if (b)
        writable |= 0xFF0000;
    if (a)
        writable |= 0xFF000000;
    gfx_set_frame_write_mask(writable);
}

// FUNC 0x00252950 gfx_set_frame_write_mask
void gfx_set_frame_write_mask(u32 mask)
{
    GsContext *ctx = GS_SPR_CTX;

    SHADOW_U32(ctx->reg[GS_REG_FRAME])[1] = ~mask;
    ctx->dirty |= GS_DIRTY_BOTH(GS_REG_FRAME);
}

// FUNC 0x00252978 gfx_depth_mask
void gfx_depth_mask(u8 write)
{
    GsContext *ctx = GS_SPR_CTX;

    ctx->zMask = (write == 0);
    if (ctx->depthTest) {
        ((GsZbuf *)&ctx->reg[GS_REG_ZBUF])->ZMSK = ctx->zMask;
        ctx->dirty |= GS_DIRTY_BOTH(GS_REG_ZBUF);
    }
}

// FUNC 0x002529E0 gfx_set_field_mode
/* Field rendering mode (PGL_FIELD_*): fieldShift = 0, XYOFFSET.OFY = tgtA.yoff
 * + a field offset, halfHeight.
 * PC: no field offsets.  The PS2 moved the odd field's drawing half a line
 * down (HALF_CUR / FRAME_SHIFT on the odd field, HALF_OTHER on the even one,
 * HALF_ODD: OFY + 8) because the display showed that field half a line
 * lower; the port draws every field buffer at full height and presents it
 * progressive, where the offset would shake the picture by a line every
 * other frame (docs/renderer.md, "Fields"). */
void gfx_set_field_mode(u32 mode)
{
    GsContext *ctx = GS_SPR_CTX;

    ctx->fieldShift = 0;
    switch (mode) {
    case PGL_FIELD_FRAME:
        ctx->halfHeight = 0;
        ctx->dirty |= GS_DIRTY_BOTH(GS_REG_XYOFFSET);
        ((GsXyoffset *)&ctx->reg[GS_REG_XYOFFSET])->OFY = ctx->tgtA.yoff;
        break;
    case PGL_FIELD_HALF_CUR:
    case PGL_FIELD_HALF_OTHER:
    case PGL_FIELD_HALF_EVEN:
    case PGL_FIELD_HALF_ODD:
        ctx->halfHeight = 1;
        ctx->dirty |= GS_DIRTY_BOTH(GS_REG_XYOFFSET);
        ((GsXyoffset *)&ctx->reg[GS_REG_XYOFFSET])->OFY = ctx->tgtA.yoff;
        break;
    case PGL_FIELD_FRAME_SHIFT:
        ctx->halfHeight = 0;
        ((GsXyoffset *)&ctx->reg[GS_REG_XYOFFSET])->OFY = ctx->tgtA.yoff;
        ctx->fieldShift = 1;
        ctx->dirty |= GS_DIRTY_BOTH(GS_REG_XYOFFSET);
        break;
    }
}

/* PC: the 2D design squeeze (gfx_set_design_scale) and the offset
 * gfx_offset_xy added under it, in design pixels (its running sum). */
static struct {
    int on;
    f32 kx, ky;
    f32 sumX, sumY;
} s_design = {0, 1.0f, 1.0f, 0.0f, 0.0f};

/* PC (G001_flare.h): a race view's 3D picture done in the buffer at
 * fbWordAddr (the FXAA point: docs/renderer.md "Anti-aliasing"). */
void gfx_post_antialias(u32 fbWordAddr, int w, int h)
{
    pglr_post_aa(fbWordAddr >> 6, w, h);
}

/* PC (2026-11-14, G001_flare.h): a buffer's detail (pglr_target_detail). */
void gfx_pc_target_detail(u32 fbWordAddr, int w, int h, f32 dx, f32 dy)
{
    pglr_target_detail(fbWordAddr >> 6, w, h, dx, dy);
}

void gfx_present_again(void)
{
    pglr_present_again();
}

static_assert(GFX_SEC_POST == PGLR_SEC_POST && GFX_SEC_REPLAY_BLUR == PGLR_SEC_REPLAY_BLUR &&
                  GFX_SEC_WEATHER == PGLR_SEC_WEATHER,
              "the GPU sections (G001_flare.h, pgl_bridge.h)");

void gfx_gpu_section(int id)
{
    pglr_gpu_section(id);
}

/* PC: v to the nearest integer (halves away from 0). */
static s32 design_round(f32 v)
{
    return v >= 0.0f ? (s32)(v + 0.5f) : -(s32)(0.5f - v);
}

/* PC (G001_flare.h): the 2D design squeeze, recorded in order with the
 * draws; the offsets that follow are design pixels. */
void gfx_set_design_scale(float kx, float ky)
{
    s_design.kx = kx > 0.0f ? kx : 1.0f;
    s_design.ky = ky > 0.0f ? ky : 1.0f;
    s_design.on = s_design.kx != 1.0f || s_design.ky != 1.0f;
    s_design.sumX = 0.0f;
    s_design.sumY = 0.0f;
    pglr_set_design_scale(s_design.kx, s_design.ky);
}

/* PC (G001_flare.h): the width of the lines that follow, recorded in order
 * with the draws. */
void gfx_set_line_width(float px)
{
    pglr_set_line_width(px);
}

/* PC (G001_flare.h): a model rebuilt in place, decoded again. */
void gfx_model_written(VifModel *m)
{
    pglr_model_written(m);
}

// FUNC 0x00252B40 gfx_offset_xy
void gfx_offset_xy(float dx, float dy)
{
    GsContext *ctx = GS_SPR_CTX;
    f32 yUnit = 16.0f;

    if (ctx->halfHeight)
        yUnit = 8.0f;
    if (s_design.on) {
        /* PC: under a design squeeze (a HUD draw of a race frame: the
         * dials' and the steering wheel's sub-pixel passes) the steps are
         * design pixels, so they spread as at 4:3 (in GS pixels a tall
         * window's 0.75 was 3 native pixels): the offset follows their
         * running sum squeezed and rounded, and steps summing to 0 come
         * back exactly */
        const f32 x0 = s_design.sumX * s_design.kx * 16.0f, y0 = s_design.sumY * s_design.ky * yUnit;
        s_design.sumX += dx;
        s_design.sumY += dy;
        const f32 x1 = s_design.sumX * s_design.kx * 16.0f, y1 = s_design.sumY * s_design.ky * yUnit;
        ((GsXyoffset *)&ctx->reg[GS_REG_XYOFFSET])->OFX += design_round(x1) - design_round(x0);
        ((GsXyoffset *)&ctx->reg[GS_REG_XYOFFSET])->OFY += design_round(y1) - design_round(y0);
        ctx->dirty |= GS_DIRTY_BOTH(GS_REG_XYOFFSET);
        return;
    }
    ((GsXyoffset *)&ctx->reg[GS_REG_XYOFFSET])->OFX += (s32)(dx * 16.0f);
    ((GsXyoffset *)&ctx->reg[GS_REG_XYOFFSET])->OFY += (s32)(dy * yUnit);
    ctx->dirty |= GS_DIRTY_BOTH(GS_REG_XYOFFSET);
}

// FUNC 0x00252BB8 gfx_set_draw_area
void gfx_set_draw_area(int x, int y, int w, int h)
{
    GsContext *ctx = GS_SPR_CTX;

    if (ctx->halfHeight) {
        y >>= 1;
        h >>= 1;
    }
    ctx->winX = x;
    ctx->winY = y;
    ctx->winW = w;
    ctx->winH = h;
    gs_update_scissor_and_viewport(ctx);
}

// FUNC 0x00252BE8 gs_update_scissor_and_viewport
void gs_update_scissor_and_viewport(GsContext *ctx)
{
    GsScissor *sc = (GsScissor *)&ctx->reg[GS_REG_SCISSOR];
    s32 x = ctx->winX, y = ctx->winY, w = ctx->winW, h = ctx->winH;

    sc->SCAX0 = x;
    sc->SCAY0 = y;
    sc->SCAX1 = x + w - 1;
    sc->SCAY1 = y + h - 1;
    ctx->dirty |= GS_DIRTY_BOTH(GS_REG_SCISSOR);
    ctx->viewScale.x  = (f32)w * 0.5f;
    ctx->viewScale.y  = (f32)(-h) * 0.5f;
    ctx->viewCenter.x = (f32)((s32)ctx->tgtA.xoff + (x << 4) + (w << 3)) * 0.0625f;
    ctx->viewCenter.y = (f32)((s32)ctx->tgtA.yoff + (y << 4) + (h << 3)) * 0.0625f;
    ctx->vu1MatrixSent = 0;
}

// FUNC 0x00252D30 gfx_set_scissor
void gfx_set_scissor(int x, int y, int w, int h)
{
    GsContext *ctx = GS_SPR_CTX;
    GsScissor *sc = (GsScissor *)&ctx->reg[GS_REG_SCISSOR];

    if (ctx->halfHeight) {
        y >>= 1;
        h >>= 1;
    }
    sc->SCAX0 = x;
    sc->SCAY0 = y;
    sc->SCAX1 = x + w - 1;
    sc->SCAY1 = y + h - 1;
    ctx->dirty |= GS_DIRTY_BOTH(GS_REG_SCISSOR);
}

/* PC: the scissor narrowed to its part below `top' of its height (0 .. 1;
 * PGL_ATTR_SCISSOR's push and pop round it). */
void gfx_scissor_below(f32 top)
{
    GsContext *ctx = GS_SPR_CTX;
    GsScissor *sc = (GsScissor *)&ctx->reg[GS_REG_SCISSOR];

    sc->SCAY0 += (int)((f32)(sc->SCAY1 - sc->SCAY0 + 1) * top);
    ctx->dirty |= GS_DIRTY_BOTH(GS_REG_SCISSOR);
}

// FUNC 0x00252DF8 gfx_set_depth_offset
void gfx_set_depth_offset(float ofs)
{
    GsContext *ctx = GS_SPR_CTX;

    ctx->depthOffset = ofs;
    gs_update_depth_range(ctx);
}

// FUNC 0x00252E08 gs_set_depth_bias_at
/* The depth bias of a view-space dz at (x, y, z): both points through the
 * modelview and the projection, divided; depthBias = the difference of the
 * w lanes (as the binary: ~0).  PC: pgl_math's ctxSetDepthBiasAt (the VU0
 * asm's computation). */
void gs_set_depth_bias_at(float dz, float x, float y, float z)
{
    pgl::ctxSetDepthBiasAt(*GS_SPR_CTX, VU0, dz, x, y, z);
}

// FUNC 0x00252F58 gs_set_clear_color_f
void gs_set_clear_color_f(float r, float g, float b, float a)
{
    GS_SPR_CTX->clearColor = pack_rgba_floats_to_u32(r, g, b, a, 255.0f);
}

// FUNC 0x00252F90 gs_set_clear_color
void gs_set_clear_color(u32 rgba)
{
    GS_SPR_CTX->clearColor = rgba;
}

// FUNC 0x00252FA0 gs_set_clear_depth
void gs_set_clear_depth(float d)
{
    GS_SPR_CTX->clearDepth = ee_clampf(1.0f - d, 0.0f, 1.0f);
}

/* A sprite pair (XYZ3 then XYZ2) as two GS vertices. */
static void put_sprite(pgl::prim::GsVertex *v, u32 xy0, u32 z0, u32 xy1, u32 z1, u32 rgba,
                       u32 uv0, u32 uv1)
{
    v[0].x = (u16)xy0;
    v[0].y = (u16)(xy0 >> 16);
    v[0].z = z0;
    v[0].f = 0xFF;
    v[0].rgba = rgba;
    v[0].u = (u16)(uv0 & 0x3FFF);
    v[0].v = (u16)((uv0 >> 16) & 0x3FFF);
    v[0].kick = false;
    v[1].x = (u16)xy1;
    v[1].y = (u16)(xy1 >> 16);
    v[1].z = z1;
    v[1].f = 0xFF;
    v[1].rgba = rgba;
    v[1].u = (u16)(uv1 & 0x3FFF);
    v[1].v = (u16)((uv1 >> 16) & 0x3FFF);
    v[1].kick = true;
}

// FUNC 0x00252FC8 gs_build_clear_packet
/* A clear of target `tgt': TEST_1 = test, ZBUF_1 = the shadow with ZMSK =
 * zMask, PRIM = SPRITE, RGBAQ = rgba, then 64-pixel strips of full height
 * at depth z (XYZ2: the strips' union is drawn as one sprite here, the
 * same pixels).  ZBUF_1 and TEST_1 dirty again. */
void gs_build_clear_packet(GsContext *ctx, u64 test, u32 rgba, u32 z, const GsRenderTarget *tgt)
{
    pgl::prim::GsVertex v[2];
    u32 xy0, xy1, x;

    pglr_ad(SCE_GS_TEST_1, test);
    pglr_ad(SCE_GS_ZBUF_1, (ctx->reg[GS_REG_ZBUF] & ~GS_ZBUF_ZMSK) | ((u64)ctx->zMask << 32));
    /* the strips start at xoff, xoff + 0x400 ... while < xend: their union
     * ends where the first start >= xend would be */
    x = tgt->xoff;
    do {
        x += 0x400;
    } while ((x & 0xFFFF) < tgt->xend);
    xy0 = (u32)tgt->xoff | ((u32)tgt->yoff << 16);
    xy1 = (x & 0xFFFF) | ((u32)tgt->yend << 16);
    put_sprite(v, xy0, z, xy1, z, rgba, 0, 0);
    pglr_prim(GS_PRIM_SPRITE, v, 2, ctx->halfHeight ? PGLR_HALF_HEIGHT : 0);
    ctx->dirty |= GS_DIRTY_CTX1(GS_REG_ZBUF) | GS_DIRTY_CTX1(GS_REG_TEST);
}

// FUNC 0x00253138 gs_clear_buffers
/* COLOR / DEPTH clear target A (depth clearDepth * depthScale, colour
 * clearColor; TEST: colour only 0, both ZTE + ALWAYS, depth only that +
 * ATST NEVER with AFAIL ZB_ONLY); TARGET_B re-points FRAME / SCISSOR /
 * XYOFFSET_1 to target B and clears it with clearColorB. */
void gs_clear_buffers(int mask)
{
    GsContext *ctx = GS_SPR_CTX;

    if (mask == 0)
        return;
    gs_flush_dirty_regs();
    if (mask & (PGL_CLEAR_COLOR | PGL_CLEAR_DEPTH)) {
        u32 z = (u32)(s32)(ctx->clearDepth * ctx->depthScale);
        u64 test;
        if ((mask & PGL_CLEAR_COLOR) == 0)
            test = GS_FIELD_VAL(GS_TEST_ZTST, GS_ZTST_ALWAYS) | GS_FIELD_MASK(GS_TEST_ZTE)
                 | GS_FIELD_VAL(GS_TEST_AFAIL, PGL_AFAIL_ZB_ONLY)
                 | GS_FIELD_MASK(GS_TEST_ATE);
        else
            test = (mask & PGL_CLEAR_DEPTH)
                 ? GS_FIELD_VAL(GS_TEST_ZTST, GS_ZTST_ALWAYS) | GS_FIELD_MASK(GS_TEST_ZTE)
                 : 0;
        gs_build_clear_packet(ctx, test, ctx->clearColor, z, &ctx->tgtA);
    }
    if (mask & PGL_CLEAR_TARGET_B) {
        gs_emit_render_target_regs(ctx);
        gs_build_clear_packet(ctx, 0, ctx->clearColorB, 0, &ctx->tgtB);
    }
}

// FUNC 0x002531F8 gs_emit_render_target_regs
/* FRAME_1 = target B, SCISSOR_1 = the scissor rescaled from target A to
 * target B, XYOFFSET_1 = target B's offsets; the three dirty again. */
void gs_emit_render_target_regs(GsContext *ctx)
{
    GsScissor *sc = (GsScissor *)&ctx->reg[GS_REG_SCISSOR];
    s32 x0, x1, y0, y1;
    u32 frame;

    frame = (ctx->tgtB.psm << 24) | (ctx->tgtB.fbw << 16) | (ctx->tgtB.base >> 5);
    if (ctx->tgtA.width == 0 || ctx->tgtA.height == 0) {
        x0 = y0 = 0;
        x1 = ctx->tgtB.width - 1;
        y1 = ctx->tgtB.height - 1;
    } else {
        x0 = (s32)sc->SCAX0 * ctx->tgtB.width / ctx->tgtA.width;
        x1 = ((s32)sc->SCAX1 + 1) * ctx->tgtB.width / ctx->tgtA.width - 1;
        y0 = (s32)sc->SCAY0 * ctx->tgtB.height / ctx->tgtA.height;
        y1 = ((s32)sc->SCAY1 + 1) * ctx->tgtB.height / ctx->tgtA.height - 1;
    }
    pglr_ad(SCE_GS_FRAME_1, (u64)(s64)(s32)frame);
    pglr_ad(SCE_GS_SCISSOR_1, ((u64)(u32)((x1 << 16) | x0)) | ((u64)(u32)((y1 << 16) | y0) << 32));
    pglr_ad(SCE_GS_XYOFFSET_1, (u64)ctx->tgtB.xoff | ((u64)ctx->tgtB.yoff << 32));
    ctx->dirty |= GS_DIRTY_CTX1(GS_REG_FRAME) | GS_DIRTY_CTX1(GS_REG_XYOFFSET)
                | GS_DIRTY_CTX1(GS_REG_SCISSOR);
}

// FUNC 0x00253358 gs_fill_target_b
/* A flat white sprite blended over target B (64-pixel strips, full height):
 * DTHE 0, TEST_1 0, ALPHA_1 {blend, FIX}, PRIM SPRITE | ABE, white. */
void gs_fill_target_b(GsContext *ctx, u32 blend, float fix)
{
    pgl::prim::GsVertex v[2];
    s64 f = (s32)ee_clampf(fix * 128.0f, 0.0f, 255.0f);
    u32 x;

    pglr_ad(SCE_GS_DTHE, 0);
    pglr_ad(SCE_GS_TEST_1, 0);
    pglr_ad(SCE_GS_ALPHA_1, (u64)blend | ((u64)f << 32));
    /* the strips start at xoff, xoff + 0x400 ... while < xend: their union */
    x = ctx->tgtB.xoff;
    do {
        x += 0x400;
    } while ((x & 0xFFFF) < ctx->tgtB.xend);
    put_sprite(v, (u32)ctx->tgtB.xoff | ((u32)ctx->tgtB.yoff << 16), 0,
               (x & 0xFFFF) | ((u32)ctx->tgtB.yend << 16), 0, 0xFFFFFFFF, 0, 0);
    pglr_prim(GS_PRIM_SPRITE | GS_PRIM_ABE, v, 2, 0);
    ctx->dirty |= GS_DIRTY_GEN(GS_GEN_DTHE) | GS_DIRTY_CTX1(GS_REG_ALPHA) | GS_DIRTY_CTX1(GS_REG_TEST);
}

// FUNC 0x002534B8 gs_copy_target
/* Target src as a texture over target dst's area (64-pixel sprite strips,
 * UVs scaled from dst to src size with a +6 (0.375 px) sample offset):
 * TEXFLUSH, DTHE 0, TEST_1 0, ALPHA_1 {blend, FIX}, TEX1_1 = LCM, TEX0_1 =
 * {src base, fbw, psm, 1024 x 1024, TCC}, CLAMP_1 REPEAT, PRIM SPRITE | TME
 * | ABE | FST, RGBAQ 0x80808080; dy shifts the sprite (12.4), dv the
 * texture (10.4).  Each strip as the binary computes it.
 * PC: +8 (the texel's centre) instead of +6.  On the GS the nearest texel of
 * a pixel is the same either way; drawn at s native pixels per GS pixel, the
 * renderer spreads a pixel's samples half a texel to each side of its
 * coordinate (pgl_exec.cpp "Coordinates"), and from s > 4 the first eighth
 * of every texel read the one before it: each copy moved the picture a
 * native line (or column), and with REPEAT the top line took the bottom one
 * - the menus' 640 x 224 buffers are s > 4 high from about 1080 lines on (the
 * car views' copies: a ragged top line, user report).  At the centre the
 * copies are exact at any s. */
#define COPY_SAMPLE_OFFSET 8                /* (12.4: half a pixel; the binary's 6) */
void gs_copy_target(GsContext *ctx, const GsRenderTarget *dst, const GsRenderTarget *src,
                    u32 blend, int dy, int dv, float fix)
{
    pgl::prim::GsVertex v[2 * 64];
    s64 f = (s32)ee_clampf(fix * 128.0f, 0.0f, 255.0f);
    u32 x, x1, vTop, vBottom, y0, y1;
    u32 sw, dw;
    int n = 0;

    pglr_ad(SCE_GS_TEXFLUSH, 0);
    pglr_ad(SCE_GS_DTHE, 0);
    pglr_ad(SCE_GS_TEST_1, 0);
    pglr_ad(SCE_GS_ALPHA_1, (u64)blend | ((u64)f << 32));
    pglr_ad(SCE_GS_TEX1_1, 1);                              /* LCM: LOD 0, nearest */
    pglr_ad(SCE_GS_TEX0_1, GS_SET_TEX0(src->base, src->fbw, src->psm, 10, 10, 1, 0, 0, 0, 0, 0, 0));
    pglr_ad(SCE_GS_CLAMP_1, PGL_WRAP_REPEAT);

    if (dst->height == 0 || dst->width == 0)
        return;
    x  = dst->xoff;
    x1 = x + 0x400;
    y0 = dst->yoff;
    y1 = dst->yend;
    vTop    = (((u32)src->height * COPY_SAMPLE_OFFSET) / (u32)dst->height + dv) << 16;
    vBottom = (((u32)(y1 - y0 + COPY_SAMPLE_OFFSET) * (u32)src->height) / (u32)dst->height + dv) << 16;
    sw = src->width;
    dw = dst->width;
    do {
        u32 u0 = ((x - dst->xoff + COPY_SAMPLE_OFFSET) * sw) / dw;
        u32 u1 = ((x1 - dst->xoff + COPY_SAMPLE_OFFSET) * sw) / dw;

        if (n < 2 * 64) {
            put_sprite(&v[n], (((y0 + dy) << 16) | x), 0, (((y1 + dy) << 16) | x1), 0, 0x80808080,
                       vTop | (u0 & 0xFFFF), vBottom | (u1 & 0xFFFF));
            n += 2;
        }
        x += 0x400;
        x1 += 0x400;
    } while (x < dst->xend);
    pglr_prim(GS_PRIM_SPRITE | GS_PRIM_TME | GS_PRIM_ABE | GS_PRIM_FST, v, n, 0);
    ctx->dirty |= GS_DIRTY_GEN(GS_GEN_DTHE)
                | GS_DIRTY_CTX1(GS_REG_TEX0) | GS_DIRTY_CTX1(GS_REG_TEX1)
                | GS_DIRTY_CTX1(GS_REG_CLAMP) | GS_DIRTY_CTX1(GS_REG_TEST)
                | GS_DIRTY_CTX1(GS_REG_ALPHA);
}

// FUNC 0x002537A0 gs_emit_xyoffset_integer
/* XYOFFSET_1 = the shadow without the sub-pixel bits; dirty again. */
void gs_emit_xyoffset_integer(GsContext *ctx)
{
    pglr_ad(SCE_GS_XYOFFSET_1, ctx->reg[GS_REG_XYOFFSET] & 0xFFFFFFF0FFFFFFF0ull);
    ctx->dirty |= GS_DIRTY_CTX1(GS_REG_XYOFFSET);
}

/* gfx_frame_effect's ALPHA selectors */
#define BLEND_ADD_FIX   PGL_BLEND_SEL(PGL_SEL_CS, PGL_SEL_ZERO, PGL_SEL_FIX, PGL_SEL_CD)    /* 0x68 */
#define BLEND_SUB_FIX   PGL_BLEND_SEL(PGL_SEL_ZERO, PGL_SEL_CS, PGL_SEL_FIX, PGL_SEL_CD)    /* 0x62 */
#define BLEND_SRC_FIX   PGL_BLEND_SEL(PGL_SEL_CS, PGL_SEL_ZERO, PGL_SEL_FIX, PGL_SEL_ZERO)  /* 0xA8 */
#define BLEND_DST_FIX   PGL_BLEND_SEL(PGL_SEL_CD, PGL_SEL_ZERO, PGL_SEL_FIX, PGL_SEL_ZERO)  /* 0xA9 */
#define BLEND_MIX_FIX   PGL_BLEND_SEL(PGL_SEL_CS, PGL_SEL_CD, PGL_SEL_FIX, PGL_SEL_CD)      /* 0x64 */
#define BLEND_MIX_ALPHA PGL_BLEND_SEL(PGL_SEL_CS, PGL_SEL_CD, PGL_SEL_AS, PGL_SEL_CD)       /* 0x44 */
#define LINE_12_4       0x10
#define LINE_10_4       0x10

// FUNC 0x00253828 gfx_frame_effect
/* Full-screen frame-buffer effects between the draw buffer (tgtA) and the
 * secondary target (tgtB) (PGL_FX_*), as the decomp has them: the PS2's
 * register writes and sprite strips, which the renderer draws natively
 * (the source target sampled as a texture). */
void gfx_frame_effect(u32 kind, float amount)
{
    GsContext *ctx = GS_SPR_CTX;

    gfx_texflush();
    gs_flush_dirty_regs();
    switch (kind) {
    case PGL_FX_ADD_A_TO_B:
        gs_emit_render_target_regs(ctx);
        if (0.0f < amount)
            gs_copy_target(ctx, &ctx->tgtB, &ctx->tgtA, BLEND_ADD_FIX, 0, 0, amount);
        else
            gs_copy_target(ctx, &ctx->tgtB, &ctx->tgtA, BLEND_SUB_FIX, 0, 0, -amount);
        return;
    case PGL_FX_COPY_A_TO_B:
        gs_emit_render_target_regs(ctx);
        gs_copy_target(ctx, &ctx->tgtB, &ctx->tgtA, BLEND_SRC_FIX, 0, 0, amount);
        return;
    case PGL_FX_ADD_WHITE:
        gs_emit_render_target_regs(ctx);
        if (0.0f < amount)
            gs_fill_target_b(ctx, BLEND_ADD_FIX, amount);
        else
            gs_fill_target_b(ctx, BLEND_SUB_FIX, -amount);
        return;
    case PGL_FX_SCALE_B:
        gs_emit_render_target_regs(ctx);
        gs_fill_target_b(ctx, BLEND_DST_FIX, amount);
        return;
    case PGL_FX_COPY_B_TO_A:
        gs_emit_xyoffset_integer(ctx);
        gs_copy_target(ctx, &ctx->tgtA, &ctx->tgtB, BLEND_SRC_FIX, 0, 0, amount);
        return;
    case PGL_FX_MIX_A_INTO_B:
        gs_emit_render_target_regs(ctx);
        gs_copy_target(ctx, &ctx->tgtB, &ctx->tgtA, BLEND_MIX_FIX, 0, 0, amount);
        return;
    case PGL_FX_ALPHA_A_INTO_B:
        gs_emit_render_target_regs(ctx);
        gs_copy_target(ctx, &ctx->tgtB, &ctx->tgtA, BLEND_MIX_ALPHA, 0, 0, amount);
        return;
    case PGL_FX_FIELD_BLEND:
        gs_emit_render_target_regs(ctx);
        if (ctx->field == 0) {
            gs_copy_target(ctx, &ctx->tgtB, &ctx->tgtA, BLEND_SRC_FIX, 0, LINE_10_4, 1.0f);
            gs_copy_target(ctx, &ctx->tgtB, &ctx->tgtA, BLEND_MIX_FIX, LINE_12_4, LINE_10_4, 0.5f);
            gs_copy_target(ctx, &ctx->tgtB, &ctx->tgtA, BLEND_MIX_FIX, 0, 0, amount);
        } else if (ctx->fieldShift != 0) {
            gs_copy_target(ctx, &ctx->tgtB, &ctx->tgtA, BLEND_SRC_FIX, 0, 0, 1.0f);
            gs_copy_target(ctx, &ctx->tgtB, &ctx->tgtA, BLEND_MIX_FIX, 0, LINE_10_4, 0.5f);
        } else {
            gs_copy_target(ctx, &ctx->tgtB, &ctx->tgtA, BLEND_SRC_FIX, 0, 0, 1.0f);
            gs_copy_target(ctx, &ctx->tgtB, &ctx->tgtA, BLEND_MIX_FIX, -LINE_12_4, 0, 0.5f);
            gs_copy_target(ctx, &ctx->tgtB, &ctx->tgtA, BLEND_MIX_FIX, 0, LINE_10_4, amount);
        }
        return;
    case PGL_FX_FIELD_BLEND_INV:
        gs_emit_render_target_regs(ctx);
        if (ctx->field == 0) {
            gs_copy_target(ctx, &ctx->tgtB, &ctx->tgtA, BLEND_SRC_FIX, 0, 0, 1.0f);
            gs_copy_target(ctx, &ctx->tgtB, &ctx->tgtA, BLEND_MIX_FIX, -LINE_12_4, 0, 0.5f);
            gs_copy_target(ctx, &ctx->tgtB, &ctx->tgtA, BLEND_MIX_FIX, 0, LINE_10_4, amount);
        } else if (ctx->fieldShift != 0) {
            gs_copy_target(ctx, &ctx->tgtB, &ctx->tgtA, BLEND_SRC_FIX, 0, 0, 1.0f);
            gs_copy_target(ctx, &ctx->tgtB, &ctx->tgtA, BLEND_MIX_FIX, LINE_12_4, LINE_10_4, 0.5f);
        } else {
            gs_copy_target(ctx, &ctx->tgtB, &ctx->tgtA, BLEND_SRC_FIX, 0, LINE_10_4, 1.0f);
            gs_copy_target(ctx, &ctx->tgtB, &ctx->tgtA, BLEND_MIX_FIX, LINE_12_4, LINE_10_4, 0.5f);
            gs_copy_target(ctx, &ctx->tgtB, &ctx->tgtA, BLEND_MIX_FIX, 0, 0, amount);
        }
        return;
    default:
        return;
    }
}

// FUNC 0x00253DD0 gfx_set_fog_color_f
u32 gfx_set_fog_color_f(float r, float g, float b)
{
    GsContext *ctx = GS_SPR_CTX;
    u32 old = gs_get_fog_color();

    r = ee_clampf(r, 0.0f, 1.0f);
    g = ee_clampf(g, 0.0f, 1.0f);
    b = ee_clampf(b, 0.0f, 1.0f);
    ((GsFogcol *)&ctx->gen[GS_GEN_FOGCOL])->FCR = (s32)(r * 255.0f);
    ((GsFogcol *)&ctx->gen[GS_GEN_FOGCOL])->FCG = (s32)(g * 255.0f);
    ((GsFogcol *)&ctx->gen[GS_GEN_FOGCOL])->FCB = (s32)(b * 255.0f);
    ctx->dirty |= GS_DIRTY_GEN(GS_GEN_FOGCOL);
    return old;
}

// FUNC 0x00253E90 gfx_set_fog_color
u32 gfx_set_fog_color(u32 rgb)
{
    GsContext *ctx = GS_SPR_CTX;
    u32 old = gs_get_fog_color();

    ((GsFogcol *)&ctx->gen[GS_GEN_FOGCOL])->FCR = rgb;
    ((GsFogcol *)&ctx->gen[GS_GEN_FOGCOL])->FCG = rgb >> 8;
    ((GsFogcol *)&ctx->gen[GS_GEN_FOGCOL])->FCB = rgb >> 16;
    ctx->dirty |= GS_DIRTY_GEN(GS_GEN_FOGCOL);
    return old;
}

// FUNC 0x00253EF0 gfx_set_fog_params
void gfx_set_fog_params(float start, float amount)
{
    GsContext *ctx = GS_SPR_CTX;

    ctx->fogStart = start;
    ctx->fogAmount = amount;
    start = ee_maxf(start, 0.0f);
    amount = ee_clampf(amount * 255.0f, 0.0f, 255.0f);
    ctx->limits.w = start;
    ctx->viewScale.w = 255.0f - amount;
    ctx->viewCenter.w = amount + 0.100000001f;     /* 0x3DCCCCCD */
}

// FUNC 0x00253F38 gfx_set_texture_buffer
void gfx_set_texture_buffer(int level, u32 addr, int psm, int width)
{
    GsContext *ctx = GS_SPR_CTX;

    switch ((u32)level) {
    case 0:
        ((GsTex0 *)&ctx->reg[GS_REG_TEX0])->TBP0 = addr >> 6;
        ((GsTex0 *)&ctx->reg[GS_REG_TEX0])->TBW = width >> 6;
        ((GsTex0 *)&ctx->reg[GS_REG_TEX0])->PSM = psm;
        gs_fix_tex0_tcc(ctx);
        ctx->dirty |= GS_DIRTY_BOTH(GS_REG_TEX0);
        break;
    case 1:
        ((GsMiptbp *)&ctx->reg[GS_REG_MIPTBP1])->TBP1 = addr >> 6;
        ((GsMiptbp *)&ctx->reg[GS_REG_MIPTBP1])->TBW1 = width >> 6;
        ctx->dirty |= GS_DIRTY_BOTH(GS_REG_MIPTBP1);
        break;
    case 2:
        ((GsMiptbp *)&ctx->reg[GS_REG_MIPTBP1])->TBP2 = addr >> 6;
        ((GsMiptbp *)&ctx->reg[GS_REG_MIPTBP1])->TBW2 = width >> 6;
        ctx->dirty |= GS_DIRTY_BOTH(GS_REG_MIPTBP1);
        break;
    case 3:
        ((GsMiptbp *)&ctx->reg[GS_REG_MIPTBP1])->TBP3 = addr >> 6;
        ((GsMiptbp *)&ctx->reg[GS_REG_MIPTBP1])->TBW3 = width >> 6;
        ctx->dirty |= GS_DIRTY_BOTH(GS_REG_MIPTBP1);
        break;
    case 4:
        ((GsMiptbp *)&ctx->reg[GS_REG_MIPTBP2])->TBP1 = addr >> 6;
        ((GsMiptbp *)&ctx->reg[GS_REG_MIPTBP2])->TBW1 = width >> 6;
        ctx->dirty |= GS_DIRTY_BOTH(GS_REG_MIPTBP2);
        break;
    case 5:
        ((GsMiptbp *)&ctx->reg[GS_REG_MIPTBP2])->TBP2 = addr >> 6;
        ((GsMiptbp *)&ctx->reg[GS_REG_MIPTBP2])->TBW2 = width >> 6;
        ctx->dirty |= GS_DIRTY_BOTH(GS_REG_MIPTBP2);
        break;
    case 6:
        ((GsMiptbp *)&ctx->reg[GS_REG_MIPTBP2])->TBP3 = addr >> 6;
        ((GsMiptbp *)&ctx->reg[GS_REG_MIPTBP2])->TBW3 = width >> 6;
        ctx->dirty |= GS_DIRTY_BOTH(GS_REG_MIPTBP2);
        break;
    }
}

// FUNC 0x00254228 gfx_set_texture_size
void gfx_set_texture_size(int w, int h)
{
    GsContext *ctx = GS_SPR_CTX;
    int log2W, log2H;

    for (log2W = 1; log2W < 10; log2W++)
        if ((1 << log2W) >= w)
            break;
    for (log2H = 1; log2H < 10; log2H++)
        if ((1 << log2H) >= h)
            break;
    ((GsTex0 *)&ctx->reg[GS_REG_TEX0])->TW = log2W;
    ((GsTex0 *)&ctx->reg[GS_REG_TEX0])->TH = log2H;
    ctx->dirty |= GS_DIRTY_BOTH(GS_REG_TEX0);
}

// FUNC 0x00254308 gfx_set_texture_function
void gfx_set_texture_function(int tfx)
{
    GsContext *ctx = GS_SPR_CTX;

    ((GsTex0 *)&ctx->reg[GS_REG_TEX0])->TFX = tfx;
    ctx->dirty |= GS_DIRTY_BOTH(GS_REG_TEX0);
}

// FUNC 0x00254358 gfx_tex_param
void gfx_tex_param(int pname, int value)
{
    GsContext *ctx = GS_SPR_CTX;

    switch ((u32)pname) {
    case PGL_TEXPARAM_TCC:
        ((GsTex0 *)&ctx->reg[GS_REG_TEX0])->TCC = value;
        ctx->dirty |= GS_DIRTY_BOTH(GS_REG_TEX0);
        break;
    case PGL_TEXPARAM_TFX:
        ((GsTex0 *)&ctx->reg[GS_REG_TEX0])->TFX = value;
        ctx->dirty |= GS_DIRTY_BOTH(GS_REG_TEX0);
        break;
    case PGL_TEXPARAM_MMIN:
        ((GsTex1 *)&ctx->reg[GS_REG_TEX1])->MMIN = value;
        ctx->dirty |= GS_DIRTY_BOTH(GS_REG_TEX1);
        break;
    case PGL_TEXPARAM_MMAG:
        ((GsTex1 *)&ctx->reg[GS_REG_TEX1])->MMAG = value;
        ctx->dirty |= GS_DIRTY_BOTH(GS_REG_TEX1);
        break;
    case PGL_TEXPARAM_WMS:
        ((GsClamp *)&ctx->reg[GS_REG_CLAMP])->WMS = value;
        ctx->dirty |= GS_DIRTY_BOTH(GS_REG_CLAMP);
        break;
    case PGL_TEXPARAM_WMT:
        ((GsClamp *)&ctx->reg[GS_REG_CLAMP])->WMT = value;
        ctx->dirty |= GS_DIRTY_BOTH(GS_REG_CLAMP);
        break;
    case PGL_TEXPARAM_LCM:
        ((GsTex1 *)&ctx->reg[GS_REG_TEX1])->LCM = value;
        ctx->dirty |= GS_DIRTY_BOTH(GS_REG_TEX1);
        break;
    case PGL_TEXPARAM_MXL:
        ((GsTex1 *)&ctx->reg[GS_REG_TEX1])->MXL = value;
        ctx->dirty |= GS_DIRTY_BOTH(GS_REG_TEX1);
        break;
    case PGL_TEXPARAM_K:
        ((GsTex1 *)&ctx->reg[GS_REG_TEX1])->K = value >> 4;
        ctx->dirty |= GS_DIRTY_BOTH(GS_REG_TEX1);
        break;
    case PGL_TEXPARAM_L:
        ((GsTex1 *)&ctx->reg[GS_REG_TEX1])->L = value;
        ctx->dirty |= GS_DIRTY_BOTH(GS_REG_TEX1);
        break;
    case PGL_TEXPARAM_LOD_BIAS:
        ctx->lodBias = value;
        break;
    case PGL_TEXPARAM_TA0:
        ((GsTexa *)&ctx->gen[GS_GEN_TEXA])->TA0 = value;
        ctx->dirty |= GS_DIRTY_GEN(GS_GEN_TEXA);
        break;
    case PGL_TEXPARAM_TA1:
        ((GsTexa *)&ctx->gen[GS_GEN_TEXA])->TA1 = value;
        ctx->dirty |= GS_DIRTY_GEN(GS_GEN_TEXA);
        break;
    case PGL_TEXPARAM_AEM:
        ((GsTexa *)&ctx->gen[GS_GEN_TEXA])->AEM = value;
        ctx->dirty |= GS_DIRTY_GEN(GS_GEN_TEXA);
        break;
    default:
        break;
    }
}

// FUNC 0x002545D8 gfx_set_texture_region
void gfx_set_texture_region(int axis, u32 min, u32 max)
{
    GsContext *ctx = GS_SPR_CTX;

    switch (axis) {
    case PGL_REGION_U:
        ((GsClamp *)&ctx->reg[GS_REG_CLAMP])->MINU = min;
        ((GsClamp *)&ctx->reg[GS_REG_CLAMP])->MAXU = max;
        ctx->dirty |= GS_DIRTY_BOTH(GS_REG_CLAMP);
        break;
    case PGL_REGION_V:
        ((GsClamp *)&ctx->reg[GS_REG_CLAMP])->MINV = min;
        ((GsClamp *)&ctx->reg[GS_REG_CLAMP])->MAXV = max;
        ctx->dirty |= GS_DIRTY_BOTH(GS_REG_CLAMP);
        break;
    }
}

// FUNC 0x00254690 gfx_texflush
void gfx_texflush(void)
{
    GS_SPR_CTX->dirty |= GS_DIRTY_GEN(GS_GEN_TEXFLUSH);
}

// FUNC 0x002546B0 gfx_set_clut_mode
void gfx_set_clut_mode(int mode)
{
    GsContext *ctx = GS_SPR_CTX;

    switch (mode) {
    case PGL_CLUT_FROM_TEX0:
        ((GsTex2 *)&ctx->reg[GS_REG_TEX2])->PSM = ((GsTex0 *)&ctx->reg[GS_REG_TEX0])->PSM;
        ((GsTex2 *)&ctx->reg[GS_REG_TEX2])->CSA = ((GsTex0 *)&ctx->reg[GS_REG_TEX0])->CSA;
        break;
    case PGL_CLUT_T4:
        ((GsTex2 *)&ctx->reg[GS_REG_TEX2])->PSM = SCE_GS_PSMT4;
        ((GsTex2 *)&ctx->reg[GS_REG_TEX2])->CSA = ((GsTex0 *)&ctx->reg[GS_REG_TEX0])->CSA;
        break;
    case PGL_CLUT_T8:
        ((GsTex2 *)&ctx->reg[GS_REG_TEX2])->PSM = SCE_GS_PSMT8;
        ((GsTex2 *)&ctx->reg[GS_REG_TEX2])->CSA = ((GsTex0 *)&ctx->reg[GS_REG_TEX0])->CSA;
        break;
    case PGL_CLUT_T4_CSA0:
        ((GsTex2 *)&ctx->reg[GS_REG_TEX2])->PSM = SCE_GS_PSMT4;
        ((GsTex2 *)&ctx->reg[GS_REG_TEX2])->CSA = 0;
        break;
    case PGL_CLUT_T8_CSA0:
        ((GsTex2 *)&ctx->reg[GS_REG_TEX2])->PSM = SCE_GS_PSMT8;
        ((GsTex2 *)&ctx->reg[GS_REG_TEX2])->CSA = 0;
        break;
    }
    ctx->dirty |= GS_DIRTY_BOTH(GS_REG_TEX2) | GS_DIRTY_BOTH(GS_REG_TEX0);
}

/* ======================================================================== */
/* gs_driver.c                                                              */
/* ======================================================================== */

// FUNC 0x0025A038 pgl_color
/* color = rgba; colorTex = each colour byte x 129 >> 8, alpha kept; with
 * colorMaterial gs_set_material_param(faces, kind, the alpha byte only).
 * PC: of colorTex only the alpha reaches a draw now (pgl_state_prim.h
 * fullColor, docs/renderer.md "Full colours"). */
void pgl_color(u32 rgba)
{
    GsContext *ctx = GS_SPR_CTX;
    u32 scaled = 0;
    u32 alpha;
    int i;

    ctx->color = rgba;
    for (i = 0; i < 4; i++) {
        u32 c = (rgba >> (8 * i)) & 0xFF;
        scaled |= (((c * 129) >> 8) & 0xFF) << (8 * i);
    }
    alpha = rgba & 0xFF000000;
    ctx->colorTex = (scaled & 0x00FFFFFF) | alpha;
    if (ctx->colorMaterial)
        gs_set_material_param(ctx->colorMaterialFaces, ctx->colorMaterialKind, alpha);
}

// FUNC 0x0025A090 pack_rgba_floats_to_u32
u32 pack_rgba_floats_to_u32(float r, float g, float b, float a, float scale)
{
    s32 ir, ig, ib, ia;

    r *= scale;
    g *= scale;
    b *= scale;
    a *= 128.0f;
    r = ee_minf(ee_maxf(r, 0.0f) + 0.5f, 255.0f);
    g = ee_minf(ee_maxf(g, 0.0f) + 0.5f, 255.0f);
    b = ee_minf(ee_maxf(b, 0.0f) + 0.5f, 255.0f);
    a = ee_minf(ee_maxf(a, 0.0f) + 0.5f, 255.0f);
    ir = (s32)r;
    ig = (s32)g;
    ib = (s32)b;
    ia = (s32)a;
    ir = ir | (ig << 8) | (ib << 16);
    return (u32)ir | ((u32)ia << 24);
}

// FUNC 0x0025A128 pgl_color4f
void pgl_color4f(float r, float g, float b, float a)
{
    pgl_color(pack_rgba_floats_to_u32(r, g, b, a, 255.0f));
}

// FUNC 0x0025A150 pgl_color3f
void pgl_color3f(float r, float g, float b)
{
    pgl_color(pack_rgba_floats_to_u32(r, g, b, 1.0f, 255.0f));
}

// FUNC 0x0025A180 pgl_color4fv
void pgl_color4fv(const f32 *rgba)
{
    pgl_color(pack_rgba_floats_to_u32(rgba[0], rgba[1], rgba[2], rgba[3], 255.0f));
}

// FUNC 0x0025A1B8 lcg_rand
int lcg_rand(void)
{
    g_003530E4 = (u32)(g_003530E4 * 0x5D588B65ull + 1);
    return (s32)g_003530E4;
}

// FUNC 0x0025A1F8 xor_unscramble_with_lcg
void xor_unscramble_with_lcg(u8 *buf, int len)
{
    int i;

    if (g_003530E0 == 0) {
        for (i = 0; i < len; i++) {
            *buf ^= (u8)lcg_rand();
            buf++;
        }
        g_003530E0 = 1;
    }
}

// FUNC 0x0025A258 gs_emit_masked_regs
u64 *gs_emit_masked_regs(u64 *out, u64 mask, const u8 *regNums, const u64 *shadow)
{
    do {
        shadow++;
        if ((mask & 1) == 1) {
            out[0] = *(shadow - 1);
            out[1] = *regNums;
            out += 2;
        }
        mask >>= 1;
        regNums++;
    } while (mask != 0);
    return out;
}

// FUNC 0x0025A290 gs_collect_dirty_regs
/* mask = dirty & enable (0: returns 0); removes it from dirty, adds it to
 * written; the A+D pairs at out (if any); returns their count.  PC: the
 * emitted registers are recorded for the renderer. */
int gs_collect_dirty_regs(GsContext *ctx, u64 *out)
{
    u64 dirty = ctx->dirty;
    u64 mask = dirty & ctx->enable;
    u64 *p = out;
    u64 bits;

    if (mask == 0)
        return 0;
    ctx->dirty = dirty & ~mask;
    ctx->written |= mask;
    pglr_regs(mask, ctx->reg, ctx->gen);
    if (out == NULL) {
        int n = 0;
        for (bits = mask; bits != 0; bits &= bits - 1)
            n++;
        return n;
    }
    bits = (mask >> 32) & ((1u << GS_GEN_COUNT) - 1);
    if (bits != 0)
        p = gs_emit_masked_regs(p, bits, g_gsRegsGeneral, ctx->gen);
    bits = mask & ((1u << GS_REG_COUNT) - 1);
    if (bits != 0)
        p = gs_emit_masked_regs(p, bits, g_gsRegsCtx1, ctx->reg);
    bits = (mask >> 16) & ((1u << GS_REG_COUNT) - 1);
    if (bits != 0)
        p = gs_emit_masked_regs(p, bits, g_gsRegsCtx2, ctx->reg);
    return (int)((u8 *)p - (u8 *)out) >> 4;
}

// FUNC 0x0025A360 intc_vblank_handler
/* INTC_VBLANK_S: the field (CSR.FIELD; PC: none, the display is
 * progressive: gfx_vsync), the 64-bit vblank counter,
 * vsyncCallback(count), then iWakeupThread for every thread in the vsync
 * waiter list and the DMA channel lists.  Runs on the port's timer thread
 * (never inside a game thread's DI section). */
int intc_vblank_handler(int cause)
{
    GfxDriverState *g = &g_gfx;
    GfxVsyncFunc cb;
    GfxWaitNode *w;
    int i;
    int all;

    (void)cause;
    cb = g->vsyncCallback;
    pglr_add64(&g->vblankCount, 1);
    if (cb != NULL)
        cb((s32)pglr_load64(&g->vblankCount));
    /* PC: the thread of the ticks; the others before the first tick, or
     * while the ticks stall (gfx_tick_vsync) */
    all = s_tickThread < 0;
    if (!all && !frame_step::paused() && vsync_behind() > TICK_STALL) {
        pglr_add64(&s_vsyncEvents, 1);
        all = 1;
    }
    for (w = g->vsyncWaiters; w != NULL; w = w->next)
        if (all || w->tid == s_tickThread)
            iWakeupThread(w->tid);
    for (i = 0; i < 3; i++)
        for (w = g->chan[i].waiters; w != NULL; w = w->next)
            iWakeupThread(w->tid);
    return 1;
}

// FUNC 0x0025A4A0 intc_vif1_handler
/* VIF1 i-bit markers.  PC: no VIF1; never installed. */
int intc_vif1_handler(int cause)
{
    (void)cause;
    return 1;
}

// FUNC 0x0025A5B8 intc_gs_finish_handler
/* GS FINISH.  PC: no GS events; never installed. */
int intc_gs_finish_handler(int cause)
{
    (void)cause;
    return 1;
}

// FUNC 0x0025A618 dmac_channel_handler
/* DMAC channels 0..2.  PC: no DMA; never installed. */
int dmac_channel_handler(int ch)
{
    (void)ch;
    return 1;
}

// FUNC 0x0025A830 gs_readback_begin
/* PC: readbacks run in the renderer (pglr_readback). */
void gs_readback_begin(GfxDriverState *g)
{
    (void)g;
}

// FUNC 0x0025A8A8 gs_readback_end
void gs_readback_end(GfxDriverState *g)
{
    (void)g;
}

// FUNC 0x0025A950 dma_start_source_chain
/* PC: no DMA (the kick executes the recorded frame). */
void dma_start_source_chain(int ch, const void *tadr)
{
    (void)ch;
    (void)tadr;
}

// FUNC 0x0025AA00 gs_select_vu1_prog
/* vu1Prog: 0x33E with PGL_CAP_VU1_PROG_33E, else 0x320; with culling on
 * + 0xA (the clockwise faces) or + 0x14; FRONT_AND_BACK gives 0. */
void gs_select_vu1_prog(GsContext *ctx)
{
    u32 prog = ctx->vu1ProgAlt ? VU1_PROG_BASE_33E : VU1_PROG_BASE;
    /* PC: under a mirrored world scale (the course scale with an odd number
     * of negative axes, pgl_set_world_scale) every triangle's screen winding
     * is turned: the front face taken as the other one */
    const int ccw = (ctx->frontFace == PGL_FRONT_CCW) != (pgl_world_scale_mirrored() != 0);

    if (ctx->cullEnable != 0) {
        switch (ctx->cullFace) {
        case PGL_CULL_FRONT:
            prog += ccw ? VU1_PROG_CULL_CCW : VU1_PROG_CULL_CW;
            break;
        case PGL_CULL_BACK:
            prog += ccw ? VU1_PROG_CULL_CW : VU1_PROG_CULL_CCW;
            break;
        case PGL_CULL_FRONT_AND_BACK:
            prog = 0;
            break;
        default:
            break;
        }
    }
    ctx->vu1Prog = prog;
}

// FUNC 0x0025AA88 gs_build_image_readback_packet
/* The local -> host transfer header; returns the image's qword count. */
int gs_build_image_readback_packet(u64 *pkt, const u32 *srcBitblt, int sx, int sy, int w, int h)
{
    u32 psm = ((const u8 *)srcBitblt)[3];
    int nPixels = w * h;
    int qwc;

    switch (psm) {
    case SCE_GS_PSMCT32:
    case SCE_GS_PSMZ32:
        qwc = (nPixels * 4 + 0xF) >> 4;
        break;
    case SCE_GS_PSMCT24:
    case SCE_GS_PSMZ24:
        qwc = (nPixels * 3 + 0xF) >> 4;
        break;
    case SCE_GS_PSMCT16:
    case SCE_GS_PSMCT16S:
        qwc = (nPixels * 2 + 0xF) >> 4;
        break;
    case SCE_GS_PSMT8:
    case SCE_GS_PSMT8H:
        qwc = (nPixels + 0xF) >> 4;
        break;
    case SCE_GS_PSMT4:
    case SCE_GS_PSMT4HL:
    case SCE_GS_PSMT4HH:
        qwc = (nPixels + 0x1F) >> 5;
        break;
    default:
        qwc = 0;
        break;
    }
    if (pkt != NULL) {
        pkt[0] = GIF_NREG(1) | GIF_EOP | 5;
        pkt[1] = GIF_REG_AD;
        ((u32 *)(pkt + 2))[0] = srcBitblt[0];
        ((u32 *)(pkt + 2))[1] = 0;
        pkt[3] = SCE_GS_BITBLTBUF;
        ((u32 *)(pkt + 4))[0] = ((u32)sy << 16) | (u32)sx;
        ((u32 *)(pkt + 4))[1] = 0;
        pkt[5] = SCE_GS_TRXPOS;
        ((u32 *)(pkt + 6))[0] = (u32)w;
        ((u32 *)(pkt + 6))[1] = (u32)h;
        pkt[7] = SCE_GS_TRXREG;
        pkt[8] = 0;
        pkt[9] = SCE_GS_FINISH;
        pkt[10] = 1;
        pkt[11] = SCE_GS_TRXDIR;
    }
    return qwc;
}

// FUNC 0x0025AB80 gfx_alloc_direct
/* qwc qwords at ctx->cur behind a DMA cnt + VIF DIRECTHL header; returns
 * the data (cur + 0x10).  The callers (the colour filters) write GIF data
 * there; PC: the block is recorded and decoded at execution. */
u32 *gfx_alloc_direct(s32 qwc)
{
    GsContext *ctx = GS_SPR_CTX;
    u32 *p = ctx->cur;

    if (qwc != 0) {
        p[0] = DMATAG_CNT(qwc);
        p[1] = 0;
        p[2] = VIF_NOP;
        p[3] = VIF_DIRECTHL(qwc);
        ctx->cur = (u32 *)((u8 *)p + ((qwc << 4) + 0x10));
        pglr_gif(p + 4, (u32)qwc, 0);
    }
    return p + 4;
}

/* ======================================================================== */
/* gs_context.c                                                             */
/* ======================================================================== */

// FUNC 0x00258150 pgl_set_morph_weight
void pgl_set_morph_weight(f32 weight)
{
    GS_SPR_CTX->morphWeight = weight;
}

// FUNC 0x00258160 gs_get_scanmask_state
int gs_get_scanmask_state(void)
{
    switch ((s32)(GS_SPR_CTX->gen[GS_GEN_SCANMSK] & 3)) {
    case 0:
        return PGL_SCANMASK_OFF;
    case 2:
        return PGL_SCANMASK_EVEN;
    case 3:
        return PGL_SCANMASK_ODD;
    }
    return PGL_SCANMASK_OFF;
}

// FUNC 0x002581C8 gs_get_fog_color
u32 gs_get_fog_color(void)
{
    GsContext *ctx = GS_SPR_CTX;

    return ((u8 *)&ctx->gen[GS_GEN_FOGCOL])[0]
         | (((u8 *)&ctx->gen[GS_GEN_FOGCOL])[1] << 8)
         | (((u8 *)&ctx->gen[GS_GEN_FOGCOL])[2] << 16);
}

// FUNC 0x002581F0 pgl_get_fog_constants
void pgl_get_fog_constants(f32 *out)
{
    GsContext *ctx = GS_SPR_CTX;

    out[0] = ctx->viewCenter.w;
    out[1] = ctx->viewScale.w;
    out[2] = ctx->limits.w;
}

// FUNC 0x00258218 gs_get_field_mode
/* PC: PGL_FIELD_FRAME while the display is not interlaced (always: the
 * port is progressive), so the game's field-paced code (movie halves, waits
 * for a field) never sees a field. */
int gs_get_field_mode(void)
{
    GsContext *ctx = GS_SPR_CTX;

    if (g_gfx.inter != SCE_GS_INTERLACE)
        return PGL_FIELD_FRAME;
    if (g_gfx.ffmd != SCE_GS_FRAME)
        return PGL_FIELD_FRAME;
    if (ctx->fieldShift)
        return PGL_FIELD_FRAME_SHIFT;
    return (ctx->field == 0) ? PGL_FIELD_HALF_EVEN : PGL_FIELD_HALF_ODD;
}

// FUNC 0x00258260 gfx_get_draw_buffer
void gfx_get_draw_buffer(u32 *fbWordAddr, u32 *psm, s32 *w, s32 *h)
{
    GsContext *ctx = GS_SPR_CTX;

    *w = ctx->tgtA.width;
    *h = ctx->tgtA.height;
    *fbWordAddr = (u32)ctx->tgtA.base << 6;
    *psm = ctx->tgtA.psm;
}

// FUNC 0x00258290 gfx_get_cull_face
u32 gfx_get_cull_face(void)
{
    return GS_SPR_CTX->cullFace;
}

// FUNC 0x002582A0 gfx_get_draw_area
void gfx_get_draw_area(s32 *x, s32 *y, s32 *w, s32 *h)
{
    GsContext *ctx = GS_SPR_CTX;

    *x = ctx->winX;
    *y = ctx->winY;
    *w = ctx->winW;
    *h = ctx->winH;
    if (ctx->halfHeight) {
        *y = *y << 1;
        *h = *h << 1;
    }
}
