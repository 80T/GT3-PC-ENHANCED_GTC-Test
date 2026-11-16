/* pgl_lighting.cpp - prebuilt VU1 models and the lighting state of pgl: the
 * decompiled vu1_lighting.c (0x0024DF28..0x0024FA98) and gs_lights.c
 * (0x0025ABD0..0x0025B0C8), ported.
 *
 * The lighting state is the decomp's: the light list, the eye-space light
 * vectors, the light x material colours, the per-mode blocks the EE built
 * for the VU1 lighting programs (pgl_send_lights_*, pgl_send_mode*), the
 * environment-map matrices.  PC: the VU0 asm is pgl_math's C++; a block the
 * PS2 sent to VU1 0xC9 is kept in g_pglLightBlock and handed to the renderer
 * with every model part (its shaders compute what the VU1 programs did,
 * docs/models.md 5); the parts themselves are recorded (pglr_model_part)
 * where vifmodel_draw / vifmodel_draw_lit queued their DMA refs.
 *
 * Lighting follows OpenGL (8 lights, glMaterial on face 0, a global
 * ambient, the local-viewer switch); up to 4 enabled directional lights
 * with a colour get a VU1 slot (transposed vectors at lightDirX.. / halfX..,
 * colours at lightDiffuse[] / lightSpecular[]). */
#include "engine/pgl/pgl_port.h"

using pgl::Vec4;

/* The lighting block of the last pgl_send_* (VU1 0xC9...). */
PglLightBlock g_pglLightBlock;
/* The matrix block VU1 holds (pgl_send_matrix). */
PglrVu1 g_pglVu1Sent;
/* The positions vu1_unpack_xyz put in front of the next model part. */
const float *g_pglPreXyz = NULL;
int g_pglPreXyzCount = 0;

/* 1 / 255 as the VU0 multiplies by it (0x3B808081). */
#define U118_F_1_255    0.00392156886f
#define GS_F_M1_6       (-0.166666672f)
#define PGL_EXPONENT_MAX 128.0f

/* The two scale vectors of the blocks: (cs, cs, cs, as) and (255, 255, 255,
 * as). */
static inline Vec4 scale_vec(const GsContext *ctx)
{
    return Vec4{ctx->colorScale, ctx->colorScale, ctx->colorScale, ctx->alphaScale};
}
static inline Vec4 lim_vec(const GsContext *ctx)
{
    return Vec4{255.0f, 255.0f, 255.0f, ctx->alphaScale};
}
static inline void block_put(int q, const Vec4 &v)
{
    g_pglLightBlock.q[q][0] = v.x;
    g_pglLightBlock.q[q][1] = v.y;
    g_pglLightBlock.q[q][2] = v.z;
    g_pglLightBlock.q[q][3] = v.w;
}

/* The specular variant for the material shininess. */
static inline u32 by_shininess(const GsContext *ctx, u32 lo, u32 mid, u32 hi)
{
    return ctx->material[0].shininess < 6.0f ? lo : ctx->material[0].shininess < 12.0f ? mid : hi;
}

// FUNC 0x0024DF28 vifmodel_setup_lighting
/* Per model: the colour / alpha scales (scaleFlags bits 0 / 1: 255, else
 * 128), the matrices of lighting modes 2 and 3, the vertex-format offset of
 * the lighting entry (format 1..3: -2, -4, -6). */
void vifmodel_setup_lighting(GsContext *ctx, const VifModel *m)
{
    int format;
    u32 lmode;
    f32 scale;

    lmode = m->mode;
    scale = 128.0f;
    ctx->vu1LightProg = VU1L_DEFAULT;
    ctx->f014 = 0;
    format = lmode >> 5;
    lmode &= 0x1F;
    if (m->scaleFlags & VIFMODEL_COLOR_255)
        scale = 255.0f;
    ctx->colorScale = scale;
    scale = 128.0f;
    if (m->scaleFlags & VIFMODEL_ALPHA_255)
        scale = 255.0f;
    ctx->alphaScale = scale;
    switch (lmode) {
    case PGL_LMODE_ENVMTX:
        pgl_build_env_matrix(ctx);
        break;
    case PGL_LMODE_ENVMAP:
        pgl_build_envmap_matrix(ctx);
        break;
    default:
        break;
    }
    ctx->vu1LightOfs = 0;
    switch (format) {
    case 1:
        ctx->vu1LightOfs = -2;
        break;
    case 2:
        ctx->vu1LightOfs = -4;
        break;
    case 3:
        ctx->vu1LightOfs = -6;
        break;
    default:
        break;
    }
}

// FUNC 0x0024E028 vifmodel_send_lighting
/* The lighting block of the model's mode while resend (one per VU1 data
 * buffer) is not exhausted; modes 1 and 4 start again at 2 when a light,
 * the modelview or the material changed.  (Formats 1 / 2 also unpacked
 * morphWeight to VU1 0xF2: the renderer gets it with each part.)  Returns
 * the new count. */
int vifmodel_send_lighting(GsContext *ctx, u32 mode, int resend)
{
    switch (VIFMODEL_LMODE(mode)) {
    case PGL_LMODE_LOCAL:
        if (ctx->lightFlags & PGL_LF_DIRTY)
            resend = 2;
        if (resend != 0) {
            pgl_send_lights_mode4(ctx);
            resend--;
        }
        break;
    case PGL_LMODE_LIGHTS:
        if (ctx->lightFlags & PGL_LF_DIRTY)
            resend = 2;
        if (resend != 0) {
            resend--;
            pgl_send_lights_mode1(ctx);
        }
        break;
    case PGL_LMODE_ENVMTX:
        if (resend != 0) {
            resend--;
            pgl_send_mode2(ctx);
        }
        break;
    case PGL_LMODE_ENVMAP:
        if (resend != 0) {
            resend--;
            pgl_send_mode3(ctx);
        }
        break;
    case PGL_LMODE_CONST:
        if (resend != 0) {
            pgl_send_mode5(ctx);
            resend--;
        }
        break;
    }
    return resend;
}

/* PC (2026-11-07): the hide box (vifmodel_pc_hide; the game thread's, set
 * round the draws it is for): every part recorded carries it. */
static float g_pcHideC[3], g_pcHideH[3];
static int g_pcHideOn;
static const VifPartRef *g_pcSkip;          /* the parts not drawn at all (hideOn 2) */
static int g_pcSkipCount;

void vifmodel_pc_hide(const float *centre, const float *half, const VifPartRef *skip, int n)
{
    g_pcHideOn = centre != NULL && half != NULL;
    for (int k = 0; k < 3; k++) {
        g_pcHideC[k] = g_pcHideOn ? centre[k] : 0.0f;
        g_pcHideH[k] = g_pcHideOn ? half[k] : 0.0f;
    }
    g_pcSkip = n > 0 ? skip : NULL;
    g_pcSkipCount = n > 0 ? n : 0;
}

static int pc_skipped(const VifModel *m, int index)
{
    for (int k = 0; k < g_pcSkipCount; k++)
        if (g_pcSkip[k].model == m && g_pcSkip[k].part == index)
            return 1;
    return 0;
}

/* Records one part of a model for the renderer. */
static void put_part(GsContext *ctx, const VifModel *m, int index, u32 flags)
{
    GsLibState *lib = &g_002F72C0;
    PglrModelPart part;

    part.model = m;
    part.part = index;
    part.lightProg = m->mode != 0 ? ctx->vu1LightProg : VU1L_DEFAULT;
    part.lightOfs = ctx->vu1LightOfs;
    memcpy(part.light, g_pglLightBlock.q, sizeof(part.light));
    part.lightQwords = g_pglLightBlock.count;
    part.colorScale = ctx->colorScale;
    part.alphaScale = ctx->alphaScale;
    part.morphWeight = ctx->morphWeight;
    part.primMask = lib->enable ? lib->primMask : 0;
    part.primAttr = ctx->primAttr;
    part.preXyz = NULL;
    part.preXyzCount = 0;
    if (index == 0 && g_pglPreXyz != NULL) {
        part.preXyz = g_pglPreXyz;
        part.preXyzCount = g_pglPreXyzCount;
        g_pglPreXyz = NULL;
        g_pglPreXyzCount = 0;
    }
    part.flareResults = NULL;
    part.flareIndex = 0;
    part.flareStripBlock = 0;               /* PC: (no GPU texel: vif_model_draw's) */
    part.flareStripH = 0;
    part.flareQ = 0;
    /* PC (2026-11-07): the hide box (vifmodel_pc_hide); 2: the part whole */
    part.hideOn = g_pcSkipCount != 0 && pc_skipped(m, index) ? 2.0f : g_pcHideOn ? 1.0f : 0.0f;
    part.hidePad = 0.0f;
    for (int k = 0; k < 3; k++) {
        part.hideC[k] = g_pcHideC[k];
        part.hideH[k] = g_pcHideH[k];
    }
    g_pglVu1Sent.prog = ctx->vu1Prog;
    pglr_model_part(&g_pglVu1Sent, &part, flags);
}

// FUNC 0x0024E1B8 vifmodel_put_part
/* DMA ref of one part + ITOP + MSCAL.  PC: model_draw records each part
 * itself (put_part); kept for its callers' sake: nothing to queue. */
void vifmodel_put_part(GsContext *ctx, const VifPart *part, u32 mscal)
{
    (void)ctx;
    (void)part;
    (void)mscal;
}

// FUNC 0x0024E2B8 vifmodel_draw_lit
/* The general path of model_draw: a lit model, the texture override on, or
 * no part-texture function.  Per part: the texture / material callback,
 * the lighting block, the dirty GS registers, then the part. */
void vifmodel_draw_lit(const VifModel *m, u32 prog)
{
    GsContext *ctx = GS_SPR_CTX;
    int resend;
    int i;

    (void)prog;
    ctx->stmask = STMASK_DEFAULT;
    if (ctx->matrixDirty != 0)
        pgl_sync_matrices(ctx);
    if (m->mode != 0)
        vifmodel_setup_lighting(ctx, m);
    ctx->nVerts += m->nVerts;
    ctx->nPrims += m->nPrims;
    ctx->nParts += m->nParts;
    pgl_vu1_matrix_due(ctx);
    resend = 2;
    for (i = 0; i < m->nParts; i++) {
        const VifPart *part = &m->part[i];

        if (part->tex != 0 && ctx->partTexFunc != NULL)
            ctx->partTexFunc(part->tex);
        if (m->mode != 0)
            resend = vifmodel_send_lighting(ctx, m->mode, resend);
        if (ctx->dirty & ctx->enable)
            gs_collect_dirty_regs(ctx, NULL);
        pgl_vu1_matrix_due(ctx);
        put_part(ctx, m, i, ctx->halfHeight ? PGLR_HALF_HEIGHT : 0);
    }
}

// FUNC 0x0024E4A0 vifmodel_draw
/* The fast path: an unlit model, override off, a part-texture function set
 * (called without a NULL test). */
void vifmodel_draw(const VifModel *m, u32 prog)
{
    GsContext *ctx = GS_SPR_CTX;
    int i;

    (void)prog;
    if (ctx->matrixDirty != 0)
        pgl_sync_matrices(ctx);
    ctx->nVerts += m->nVerts;
    ctx->nPrims += m->nPrims;
    ctx->nParts += m->nParts;
    ctx->stmask = STMASK_DEFAULT;
    pgl_vu1_matrix_due(ctx);
    for (i = 0; i < m->nParts; i++) {
        const VifPart *part = &m->part[i];

        if (part->tex != 0)
            ctx->partTexFunc(part->tex);
        if (ctx->dirty & ctx->enable)
            gs_collect_dirty_regs(ctx, NULL);
        pgl_vu1_matrix_due(ctx);
        put_part(ctx, m, i, ctx->halfHeight ? PGLR_HALF_HEIGHT : 0);
    }
}

// FUNC 0x0024E670 pgl_sync_matrices
/* vf24..vf27 = the combined matrix (modelview x projection by matrix mode,
 * the scale when scaleEnable); the matrix due for VU1 again, the eye-space
 * lights out of date. */
void pgl_sync_matrices(GsContext *ctx)
{
    pgl::ctxSyncMatrices(*ctx, VU0);
}

/* The matrix block when due (pgl_send_matrix + vu1MatrixSent). */
int pgl_vu1_matrix_due(GsContext *ctx)
{
    if (ctx->vu1MatrixSent != 0)
        return 0;
    pgl_send_matrix(ctx, 0xC1);
    ctx->vu1MatrixSent = 1;
    return 1;
}

// FUNC 0x0024E7F0 pgl_send_matrix
/* vf24..vf27 and the viewport vectors to VU1.  PC: what VU1 holds from now
 * on (g_pglVu1Sent). */
void pgl_send_matrix(GsContext *ctx, u32 vuAddr)
{
    (void)vuAddr;
    pgl_vu1_state(ctx, &g_pglVu1Sent);
}

// FUNC 0x0024E858 pgl_send_dirty_regs
/* The dirty GS registers sent to VU1 (XGKICKed before the vertices).  PC:
 * recorded (gs_collect_dirty_regs). */
void pgl_send_dirty_regs(GsContext *ctx, u32 vuAddr)
{
    (void)vuAddr;
    gs_collect_dirty_regs(ctx, NULL);
}

// FUNC 0x0024E8F8 pgl_send_lights_diffuse
/* Diffuse, 4 lights: the transposed directions, ambient and diffuse colours
 * (x the colour scales), (255, 255, 255, alpha scale); VU1L_DIFFUSE. */
void pgl_send_lights_diffuse(GsContext *ctx)
{
    Vec4 s = scale_vec(ctx);

    block_put(0, vec4_of(ctx->lightDirX));
    block_put(1, vec4_of(ctx->lightDirY));
    block_put(2, vec4_of(ctx->lightDirZ));
    block_put(3, pgl::vu::mul(vec4_of(ctx->ambientColor), s));
    for (int i = 0; i < 4; i++)
        block_put(4 + i, pgl::vu::mul(vec4_of(ctx->lightDiffuse[i]), s));
    block_put(8, lim_vec(ctx));
    g_pglLightBlock.count = 9;
    ctx->vu1LightProg = VU1L_DIFFUSE;
}

// FUNC 0x0024E9C8 pgl_send_lights_specular4
/* Diffuse + specular, 4 lights (16 qwords); the variant by shininess. */
void pgl_send_lights_specular4(GsContext *ctx)
{
    Vec4 s = scale_vec(ctx);

    block_put(0, vec4_of(ctx->lightDirX));
    block_put(1, vec4_of(ctx->lightDirY));
    block_put(2, vec4_of(ctx->lightDirZ));
    block_put(3, pgl::vu::mul(vec4_of(ctx->ambientColor), s));
    for (int i = 0; i < 4; i++)
        block_put(4 + i, pgl::vu::mul(vec4_of(ctx->lightDiffuse[i]), s));
    block_put(8, vec4_of(ctx->halfX));
    block_put(9, vec4_of(ctx->halfY));
    block_put(10, vec4_of(ctx->halfZ));
    for (int i = 0; i < 4; i++)
        block_put(11 + i, pgl::vu::mul(vec4_of(ctx->lightSpecular[i]), s));
    block_put(15, lim_vec(ctx));
    g_pglLightBlock.count = 16;
    ctx->vu1LightProg = by_shininess(ctx, VU1L_SPEC4_S6, VU1L_SPEC4_S12, VU1L_SPEC4);
}

// FUNC 0x0024EB30 pgl_send_lights_specular2
/* Diffuse + specular, 2 lights: the (x, y) lanes of the transposed
 * directions and half vectors in pairs, ambient, diffuse 0..1, specular
 * 0..1, (255, 255, 255, alpha scale) - 9 qwords. */
void pgl_send_lights_specular2(GsContext *ctx)
{
    Vec4 s = scale_vec(ctx);

    block_put(0, Vec4{ctx->lightDirX.x, ctx->lightDirX.y, ctx->halfX.x, ctx->halfX.y});
    block_put(1, Vec4{ctx->lightDirY.x, ctx->lightDirY.y, ctx->halfY.x, ctx->halfY.y});
    block_put(2, Vec4{ctx->lightDirZ.x, ctx->lightDirZ.y, ctx->halfZ.x, ctx->halfZ.y});
    block_put(3, pgl::vu::mul(vec4_of(ctx->ambientColor), s));
    block_put(4, pgl::vu::mul(vec4_of(ctx->lightDiffuse[0]), s));
    block_put(5, pgl::vu::mul(vec4_of(ctx->lightDiffuse[1]), s));
    block_put(6, pgl::vu::mul(vec4_of(ctx->lightSpecular[0]), s));
    block_put(7, pgl::vu::mul(vec4_of(ctx->lightSpecular[1]), s));
    block_put(8, lim_vec(ctx));
    g_pglLightBlock.count = 9;
    ctx->vu1LightProg = by_shininess(ctx, VU1L_SPEC2_S6, VU1L_SPEC2_S12, VU1L_SPEC2);
}

// FUNC 0x0024EC68 pgl_send_lights_mode1
/* Lighting mode 1: what the flags ask for recomputed, then the diffuse
 * program unless both a light and the material have a specular colour;
 * with specular and a local viewer: mode 4's packet (fewer than 2 VU
 * lights) or VU1L_DEFAULT; else the 2- or 4-light specular packet. */
void pgl_send_lights_mode1(GsContext *ctx)
{
    if (ctx->lightFlags & PGL_LF_LIGHTS)
        pgl_build_light_list(ctx);
    if (ctx->lightFlags & PGL_LF_MODELVIEW)
        pgl_update_eye_lights(ctx);
    if (ctx->lightFlags & PGL_LF_MATERIAL)
        pgl_update_light_colors(ctx);
    if ((ctx->lightFlags & (PGL_LF_MAT_SPECULAR | PGL_LF_SPECULAR)) != (PGL_LF_MAT_SPECULAR | PGL_LF_SPECULAR)) {
        pgl_send_lights_diffuse(ctx);
        return;
    }
    if (ctx->lightFlags & PGL_LF_LOCAL_VIEWER) {
        if (ctx->nVuLights < 2) {
            pgl_send_lights_mode4(ctx);
            return;
        }
        ctx->vu1LightProg = VU1L_DEFAULT;
        return;
    }
    if (ctx->nVuLights < 3) {
        pgl_send_lights_specular2(ctx);
        return;
    }
    pgl_send_lights_specular4(ctx);
}

// FUNC 0x0024ED50 pgl_send_lights_mode4
/* Lighting mode 4: the transposed directions, the eye in model space, the
 * specular colours of slots 0..1 (x the colour scales) - 6 qwords; the
 * variant by shininess. */
void pgl_send_lights_mode4(GsContext *ctx)
{
    Vec4 s;

    if (ctx->lightFlags & PGL_LF_LIGHTS)
        pgl_build_light_list(ctx);
    if (ctx->lightFlags & PGL_LF_MODELVIEW)
        pgl_update_eye_lights(ctx);
    if (ctx->lightFlags & PGL_LF_MATERIAL)
        pgl_update_light_colors(ctx);
    s = scale_vec(ctx);
    block_put(0, vec4_of(ctx->lightDirX));
    block_put(1, vec4_of(ctx->lightDirY));
    block_put(2, vec4_of(ctx->lightDirZ));
    block_put(3, vec4_of(ctx->eyeModel));
    block_put(4, pgl::vu::mul(vec4_of(ctx->lightSpecular[0]), s));
    block_put(5, pgl::vu::mul(vec4_of(ctx->lightSpecular[1]), s));
    g_pglLightBlock.count = 6;
    ctx->vu1LightProg = ctx->material[0].shininess < 6.0f ? VU1L_LOCAL_S6
                      : !(ctx->material[0].shininess < 12.0f) ? VU1L_LOCAL : VU1L_LOCAL_S12;
}

// FUNC 0x0024EE80 pgl_build_light_list
/* Links the enabled lights (lights[7] first), counts them, gives the
 * coloured directional ones VU1 slots 0..3.  "Has a colour" tests the bits
 * of r, g, b, not their values. */
void pgl_build_light_list(GsContext *ctx)
{
    GsLight *l;
    int i;
    int nSlots;
    s8 flags;
    int directional;
    u8 hasDiffuse;
    u8 hasSpecular;

    flags = (s8)ctx->lightFlags;
    flags |= PGL_LF_DIRTY;
    flags &= ~(PGL_LF_LIGHTS | PGL_LF_SPECULAR | PGL_LF_POINT);
    nSlots = 0;
    ctx->lightList = NULL;
    l = ctx->lights;
    ctx->nLights = 0;
    for (i = 0; i < 8; i++, l++) {
        l->vuIndex = -1;
        if (l->enable == 0)
            continue;
        directional = 1;
        hasDiffuse = (pgl_f2u(l->diffuse.x) | pgl_f2u(l->diffuse.y) | pgl_f2u(l->diffuse.z)) != 0;
        hasSpecular = (pgl_f2u(l->specular.x) | pgl_f2u(l->specular.y) | pgl_f2u(l->specular.z)) != 0;
        l->next = ctx->lightList;
        ctx->nLights++;
        ctx->lightList = l;
        if (!(l->position.w == 0.0f))
            directional = 0;
        if (directional == 0 && (hasDiffuse || hasSpecular))
            flags |= PGL_LF_POINT;
        if (hasSpecular)
            flags |= PGL_LF_SPECULAR;
        if (nSlots < 4 && (hasDiffuse || hasSpecular) && directional != 0) {
            l->vuIndex = (s8)nSlots;
            nSlots++;
        }
    }
    ctx->nVuLights = (s16)nSlots;
    ctx->lightFlags = (u8)flags;
}

/* The modelview's rigid inverse (vf20..vf23 after U115_VU_INVERT_VF20).
 * PC: an affine one under the course scale (pgl::g_worldScale). */
static void modelview_inverse(Vec4 out[4])
{
    pgl_modelview(out);
    pgl::invertModelview(out);
    for (int r = 0; r < 4; r++)
        VU0.vf[20 + r] = out[r];
}

/* normalize(v) as the VU0 does it (x, y, z summed; all four lanes scaled). */
static inline Vec4 vu_normalize(const Vec4 &v)
{
    float l = v.x * v.x + v.y * v.y;
    l = l + v.z * v.z;
    return pgl::vu::scale(v, pgl::vu::rsqrt(1.0f, l));
}

// FUNC 0x0024EF80 pgl_update_eye_lights
/* Model space for the lights: M = inverse(modelview) (rigid; the rows
 * divided by the scale when scaleEnable).  eyeModel / eyePos = M eyeWorld;
 * viewDir = normalize(M (0, 0, -1)); per light: directional - eyePosition
 * = normalize(M position), halfVector = normalize(eyePosition - viewDir);
 * positional - eyePosition = M position, eyeSpotDir = normalize(M
 * spotDir).  Lights with a VU slot go into the transposed block. */
void pgl_update_eye_lights(GsContext *ctx)
{
    Vec4 m[4];
    Vec4 eye, dir;
    GsLight *l;

    ctx->lightFlags &= ~PGL_LF_MODELVIEW;
    modelview_inverse(m);
    memset(&ctx->lightDirX, 0, sizeof(GsVec4) * 3);
    memset(&ctx->halfX, 0, sizeof(GsVec4) * 3);
    if (ctx->scaleEnable != 0) {
        Vec4 inv{pgl::vu::div(1.0f, ctx->scale.x), pgl::vu::div(1.0f, ctx->scale.y),
                 pgl::vu::div(1.0f, ctx->scale.z), 1.0f};
        for (int r = 0; r < 3; r++)
            m[r] = pgl::vu::mul(m[r], inv);
    }
    eye = pgl::vu::xform(m, vec4_of(ctx->eyeWorld));
    dir = pgl::vu::xform3(m, Vec4{0.0f, 0.0f, -1.0f, 0.0f});
    dir.w = m[0].w * 0.0f + m[1].w * 0.0f + m[2].w * -1.0f;
    vec4_store(ctx->eyeModel, eye);
    vec4_store(ctx->eyePos, eye);
    dir = vu_normalize(dir);
    vec4_store(ctx->viewDir, dir);
    for (l = ctx->lightList; l != NULL; l = l->next) {
        if (l->position.w == 0.0f) {
            Vec4 p = vu_normalize(pgl::vu::xform(m, vec4_of(l->position)));
            vec4_store(l->eyePosition, p);
            vec4_store(l->halfVector, vu_normalize(pgl::vu::sub(p, dir)));
        } else {
            vec4_store(l->eyePosition, pgl::vu::xform(m, vec4_of(l->position)));
            vec4_store(l->eyeSpotDir, vu_normalize(pgl::vu::xform(m, vec4_of(l->spotDir))));
        }
        if (l->vuIndex >= 0) {
            int slot = l->vuIndex;

            (&ctx->lightDirX.x)[slot] = l->eyePosition.x;
            (&ctx->lightDirY.x)[slot] = l->eyePosition.y;
            (&ctx->lightDirZ.x)[slot] = l->eyePosition.z;
            (&ctx->halfX.x)[slot] = l->halfVector.x;
            (&ctx->halfY.x)[slot] = l->halfVector.y;
            (&ctx->halfZ.x)[slot] = l->halfVector.z;
        }
    }
}

// FUNC 0x0024F218 pgl_update_light_colors
/* Light x material (face 0): ambientColor = emission + globalAmbient x
 * ambient + the lights' ambient x ambient (summed in ACC); per light
 * diffuseLit / specularLit, also into the VU slot.  MAT_SPECULAR follows
 * the bits of the material's specular r, g, b. */
void pgl_update_light_colors(GsContext *ctx)
{
    GsLight *l;
    u32 spec;
    Vec4 acc, amb, dif, spc;

    spec = pgl_f2u(ctx->material[0].specular.x) | pgl_f2u(ctx->material[0].specular.y)
         | pgl_f2u(ctx->material[0].specular.z);
    ctx->lightFlags &= ~PGL_LF_MATERIAL;
    if (spec != 0)
        ctx->lightFlags |= PGL_LF_MAT_SPECULAR;
    else
        ctx->lightFlags &= ~(PGL_LF_MATERIAL | PGL_LF_MAT_SPECULAR);
    amb = vec4_of(ctx->material[0].ambient);
    dif = vec4_of(ctx->material[0].diffuse);
    spc = vec4_of(ctx->material[0].specular);
    /* ACC = emission * 1 + globalAmbient * ambient */
    acc = pgl::vu::add(vec4_of(ctx->material[0].emission),
                       pgl::vu::mul(vec4_of(ctx->globalAmbient), amb));
    for (l = ctx->lightList; l != NULL; l = l->next) {
        Vec4 d = pgl::vu::mul(vec4_of(l->diffuse), dif);
        Vec4 s = pgl::vu::mul(vec4_of(l->specular), spc);

        acc = pgl::vu::add(acc, pgl::vu::mul(vec4_of(l->ambient), amb));
        if (l->vuIndex >= 0) {
            vec4_store(ctx->lightDiffuse[l->vuIndex], d);
            vec4_store(ctx->lightSpecular[l->vuIndex], s);
        }
        vec4_store(l->diffuseLit, d);
        vec4_store(l->specularLit, s);
    }
    vec4_store(ctx->ambientColor, acc);
}

// FUNC 0x0024F2D0 pgl_send_mode5
/* Lighting mode 5: constVector, (255, 255, 255, alpha scale), constColor;
 * VU1L_CONST. */
void pgl_send_mode5(GsContext *ctx)
{
    block_put(0, vec4_of(ctx->constVector));
    block_put(1, lim_vec(ctx));
    block_put(2, vec4_of(ctx->constColor));
    g_pglLightBlock.count = 3;
    ctx->vu1LightProg = VU1L_CONST;
}

// FUNC 0x0024F348 pgl_send_mode2
/* Lighting mode 2: envMtx[0..3], qB00 x qAF0, qB10 x qAF0; VU1L_ENVMTX. */
void pgl_send_mode2(GsContext *ctx)
{
    Vec4 af0 = vec4_of(ctx->qAF0);

    for (int r = 0; r < 4; r++)
        block_put(r, vec4_of(ctx->envMtx[r]));
    block_put(4, pgl::vu::mul(vec4_of(ctx->qB00), af0));
    block_put(5, pgl::vu::mul(vec4_of(ctx->qB10), af0));
    g_pglLightBlock.count = 6;
    ctx->vu1LightProg = VU1L_ENVMTX;
}

// FUNC 0x0024F3C0 pgl_build_env_matrix
/* Lighting mode 2's matrix: rows = the modelview's 3x3 (premultiplied by
 * normalMtx rows 0..2 with normalMtxEnable), row 3 = R^T (-t) (divided by
 * the scale with scaleEnable), its w whatever it was (the asm never sets
 * it: here the modelview inverse's). */
void pgl_build_env_matrix(GsContext *ctx)
{
    Vec4 mv[4];
    Vec4 t, r0, r1, r2, row3;

    pgl_modelview(mv);
    /* vf1..vf3 = transpose(vf20..vf22) (w 0), vf4.xyz = R^T (-t) */
    t = Vec4{0.0f - mv[3].x, 0.0f - mv[3].y, 0.0f - mv[3].z, mv[3].w};
    r0 = Vec4{mv[0].x, mv[1].x, mv[2].x, 0.0f};
    r1 = Vec4{mv[0].y, mv[1].y, mv[2].y, 0.0f};
    r2 = Vec4{mv[0].z, mv[1].z, mv[2].z, 0.0f};
    row3 = t;
    for (int k = 0; k < 3; k++) {
        float a = r0[k] * t.x;
        a = a + r1[k] * t.y;
        row3[k] = a + r2[k] * t.z;
    }
    /* vf23 = 0 * vf1..vf3 + vf4 * 1 */
    if (ctx->scaleEnable != 0) {
        row3.x = row3.x * pgl::vu::div(1.0f, ctx->scale.x);
        row3.y = row3.y * pgl::vu::div(1.0f, ctx->scale.y);
        row3.z = row3.z * pgl::vu::div(1.0f, ctx->scale.z);
    }
    if (pgl::g_worldScale.w != 0.0f) {
        /* PC: under the course scale the eye from the affine inverse, the
         * normals' rows normalised (pgl::g_worldScale) */
        Vec4 inv[4] = {mv[0], mv[1], mv[2], mv[3]};
        pgl::invertAffine(inv);
        row3.x = inv[3].x;
        row3.y = inv[3].y;
        row3.z = inv[3].z;
        pgl::normalizeModelviewRows(mv);
    }
    if (ctx->normalMtxEnable != 0) {
        Vec4 n[3] = {vec4_of(ctx->normalMtx[0]), vec4_of(ctx->normalMtx[1]), vec4_of(ctx->normalMtx[2])};
        for (int r = 0; r < 3; r++)
            mv[r] = pgl::vu::xform3(n, mv[r]);
    }
    vec4_store(ctx->envMtx[0], mv[0]);
    vec4_store(ctx->envMtx[1], mv[1]);
    vec4_store(ctx->envMtx[2], mv[2]);
    vec4_store(ctx->envMtx[3], row3);
}

// FUNC 0x0024F4E0 pgl_set_vec_param
void pgl_set_vec_param(u32 kind, const f32 *v)
{
    GsContext *ctx = GS_SPR_CTX;

    switch (kind) {
    case PGL_PARAM_AF0:
        ctx->qAF0.x = v[0]; ctx->qAF0.y = v[1]; ctx->qAF0.z = v[2]; ctx->qAF0.w = v[3];
        return;
    case PGL_PARAM_B00:
        ctx->qB00.x = v[0]; ctx->qB00.y = v[1]; ctx->qB00.z = v[2]; ctx->qB00.w = v[3];
        return;
    case PGL_PARAM_B10:
        ctx->qB10.x = v[0]; ctx->qB10.y = v[1]; ctx->qB10.z = v[2]; ctx->qB10.w = v[3];
        return;
    }
}

// FUNC 0x0024F580 pgl_set_scalar_param
/* kind 0: qAF0 = (v, v, v, 1), 2: qB00 = (v, v, v, 1), 3: qB10 = (v, v, v, 0). */
void pgl_set_scalar_param(u32 kind, f32 v)
{
    GsContext *ctx = GS_SPR_CTX;

    switch (kind) {
    case PGL_PARAM_AF0:
        ctx->qAF0.x = v; ctx->qAF0.y = v; ctx->qAF0.z = v; ctx->qAF0.w = 1.0f;
        return;
    case PGL_PARAM_B00:
        ctx->qB00.x = v; ctx->qB00.y = v; ctx->qB00.z = v; ctx->qB00.w = 1.0f;
        return;
    case PGL_PARAM_B10:
        ctx->qB10.x = v; ctx->qB10.y = v; ctx->qB10.z = v; ctx->qB10.w = 0.0f;
        return;
    }
}

/* ---- The environment map (lighting mode 3) ---------------------------- */

// FUNC 0x0024F600 pgl_send_mode3
/* Lighting mode 3: the env-map matrix, qB00, qB10; VU1L_ENVMAP. */
void pgl_send_mode3(GsContext *ctx)
{
    for (int r = 0; r < 4; r++)
        block_put(r, pgl::g_envMap.texMtx[r]);
    block_put(4, vec4_of(ctx->qB00));
    block_put(5, vec4_of(ctx->qB10));
    g_pglLightBlock.count = 6;
    ctx->vu1LightProg = VU1L_ENVMAP;
}

// FUNC 0x0024F670 pgl_build_envmap_matrix
void pgl_build_envmap_matrix(GsContext *ctx)
{
    Vec4 mv[4];

    (void)ctx;
    pgl_modelview(mv);
    pgl::envmapBuildMatrix(pgl::g_envMap, mv);
}

// FUNC 0x0024F7A8 pgl_set_envmap_params
void pgl_set_envmap_params(f32 a, f32 b)
{
    pgl::g_envMap.paramA = a;
    pgl::g_envMap.paramB = b;
}

// FUNC 0x0024F7B8 pgl_envmap_rotate
void pgl_envmap_rotate(f32 x, f32 y, f32 z)
{
    pgl::envmapRotate(VU0, pgl::g_envMap, x, y, z);
}

// FUNC 0x0024F900 pgl_envmap_point_at
void pgl_envmap_point_at(f32 x, f32 y, f32 z)
{
    pgl::envmapPointAt(VU0, pgl::g_envMap, x, y, z);
}

// FUNC 0x0024F928 pgl_envmap_set_basis
void pgl_envmap_set_basis(f32 ax, f32 ay, f32 az, f32 bx, f32 by, f32 bz)
{
    Vec4 mv[4];

    pgl_modelview(mv);
    pgl::envmapSetBasis(VU0, pgl::g_envMap, mv, ax, ay, az, bx, by, bz);
}

/* PC (no original): the course scale's world scale (G001_flare.h).  Its
 * mirroring changing, the culling program is chosen again (gs_select_vu1_prog
 * turns the front face while mirrored). */
void pgl_set_world_scale(int on, f32 x, f32 y, f32 z)
{
    const int was = pgl_world_scale_mirrored();
    pgl::g_worldScale = on != 0 ? Vec4{x, y, z, 1.0f} : Vec4{1.0f, 1.0f, 1.0f, 0.0f};
    if (pgl_world_scale_mirrored() != was)
        gs_select_vu1_prog(GS_SPR_CTX);
}

int pgl_world_scale_on(void)
{
    return pgl::g_worldScale.w != 0.0f ? 1 : 0;
}

int pgl_world_scale_mirrored(void)
{
    const Vec4 &w = pgl::g_worldScale;
    return w.w != 0.0f && w.x * w.y * w.z < 0.0f ? 1 : 0;
}

/* ======================================================================== */
/* gs_lights.c                                                              */
/* ======================================================================== */

// FUNC 0x0025ABD0 gs_store_material_vec
/* A vector into material[face]: 0 ambient, 1 diffuse, 2 both, 3 emission,
 * 4 specular; lightFlags |= MATERIAL. */
void gs_store_material_vec(GsContext *ctx, int face, u32 kind, GsVec4Arg v)
{
    switch (kind) {
    case PGL_MAT_AMBIENT:
        ctx->material[face].ambient = v;
        break;
    case PGL_MAT_DIFFUSE:
        ctx->material[face].diffuse = v;
        break;
    case PGL_MAT_AMBIENT_AND_DIFFUSE:
        ctx->material[face].ambient = v;
        ctx->material[face].diffuse = v;
        break;
    case PGL_MAT_SPECULAR:
        ctx->material[face].specular = v;
        break;
    case PGL_MAT_EMISSION:
        ctx->material[face].emission = v;
        break;
    }
    ctx->lightFlags |= PGL_LF_MATERIAL;
}

// FUNC 0x0025AC70 gs_set_material_param
/* glMaterial-like: kind 11 the shininess (clamped 0..128, encoded, the pow
 * table cleared); others the RGBA bytes / 255.  Quirk kept: the face-1 call
 * passes face 0 too. */
void gs_set_material_param(int faceMask, int kind, u32 value)
{
    GsContext *ctx = GS_SPR_CTX;
    GsVec4Arg v;

    if (kind == PGL_MAT_SHININESS) {
        f32 raw = (f32)value;
        f32 shininess = ee_minf(ee_maxf(raw, 0.0f), PGL_EXPONENT_MAX);
        u32 enc = float_to_gs_param_encode(shininess);

        if (faceMask & PGL_FACE_FRONT) {
            ctx->material[0].shininess = shininess;
            ctx->material[0].x44.powTable = NULL;
            ctx->material[0].shininessEnc = enc;
        }
        if (faceMask & PGL_FACE_BACK) {
            ctx->material[1].shininess = shininess;
            ctx->material[1].shininessEnc = enc;
            ctx->material[1].x44.powTable = NULL;
        }
        ctx->lightFlags |= PGL_LF_MATERIAL;
        return;
    }
    v.x = (f32)(s32)(value & 0xFF) * U118_F_1_255;
    v.y = (f32)(s32)((value >> 8) & 0xFF) * U118_F_1_255;
    v.z = (f32)(s32)((value >> 16) & 0xFF) * U118_F_1_255;
    v.w = (f32)(s32)((value >> 24) & 0xFF) * U118_F_1_255;
    if (faceMask & PGL_FACE_FRONT)
        gs_store_material_vec(ctx, 0, (u32)kind, v);
    if (faceMask & PGL_FACE_BACK)
        gs_store_material_vec(ctx, 0, (u32)kind, v);
}

// FUNC 0x0025ADB0 gs_set_light_param_f
void gs_set_light_param_f(GsContext *ctx, GsLight *l, int pname, f32 v)
{
    (void)ctx;
    switch (pname) {
    case PGL_LIGHT_SPOT_EXPONENT: {
        f32 exponent = ee_minf(ee_maxf(v, 0.0f), PGL_EXPONENT_MAX);

        l->spotTable = NULL;
        l->spotExponent = exponent;
        l->spotExpEnc = float_to_gs_param_encode(exponent);
        break;
    }
    case PGL_LIGHT_SPOT_CUTOFF:
        l->spotCutoff = v;
        if (v != PGL_SPOT_CUTOFF_NONE)
            l->spotCosCutoff = pgl_cos_deg(v);
        break;
    case PGL_LIGHT_CONST_ATTEN:
        l->constAtten = v;
        break;
    case PGL_LIGHT_LINEAR_ATTEN:
        l->linearAtten = v;
        break;
    case PGL_LIGHT_QUAD_ATTEN:
        l->quadAtten = v;
        break;
    }
}

// FUNC 0x0025AE60 gs_set_light_param_v
/* glLightfv: colours stored as given; POSITION normalised when w == 0, both
 * POSITION and SPOT_DIRECTION (normalised) times the modelview; then
 * lightFlags |= LIGHTS if the light is on. */
void gs_set_light_param_v(GsContext *ctx, GsLight *l, u32 pname, GsVec4Arg v)
{
    Vec4 mv[4];

    switch (pname) {
    case PGL_LIGHT_AMBIENT:
        l->ambient = v;
        break;
    case PGL_LIGHT_DIFFUSE:
        l->diffuse = v;
        break;
    case PGL_LIGHT_SPECULAR:
        l->specular = v;
        break;
    case PGL_LIGHT_POSITION:
        pgl_modelview(mv);
        vec4_store(l->position, pgl::lightVectorToEye(mv, vec4_of(v), v.w == 0.0f));
        break;
    case PGL_LIGHT_SPOT_DIRECTION:
        pgl_modelview(mv);
        vec4_store(l->spotDir, pgl::lightVectorToEye(mv, vec4_of(v), true));
        break;
    }
    if (l->enable)
        ctx->lightFlags |= PGL_LF_LIGHTS;
}

// FUNC 0x0025AFE8 mat44_copy
/* A 4x4 copy (pgl_set_normal_matrix's; the retail copy garbles unaligned
 * matrices, the port's callers pass aligned ones: a plain copy). */
extern "C" void mat44_copy(void *dst, const void *src)
{
    memcpy(dst, src, 64);
}
