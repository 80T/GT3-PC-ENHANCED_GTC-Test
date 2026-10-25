/* pgl_tex1.cpp - the 'Tex1' texture sets and texture register blocks of
 * pgl: the decompiled tex1.cpp (0x002487B8..0x00249B28) and texregs.c
 * (unit u114, 0x0024B930..0x0024BF10), ported.
 *
 * Relocation, VRAM linking, CLUT variations and animations, the override
 * switches and the GsTexRegs builders are the decomp's.  PC: the uploads
 * are records of the renderer's VRAM registry (pglr_tex1_upload): the set
 * keeps its pixels in RAM and the renderer decodes the textures it is
 * asked for (docs/graphics.md 5). */
#include "engine/pgl/pgl_port.h"

/* EE COP1 abs.s / max.s (clut_anim_step). */
static inline f32 ee_absf(f32 a) { return fabsf(a); }
#define EE_MAXF(a, b) ((a) > (b) ? (a) : (b))

#define REBASE(p, delta)  ((__typeof__(p))((u8 *)(p) + (delta)))
#define RELOC(p, d)      do { (p) = REBASE(p, d); } while (0)
#define TEX1_RELOC(p, d) do { if ((p) != NULL) (p) = REBASE(p, d); } while (0)

/* GsTexRegs as bit fields (tex1_set_vram_base). */
typedef struct Tex1GsTex0 {
    u64 TBP0:14, TBW:6, PSM:6, TW:4, TH:4, TCC:1, TFX:2, CBP:14, CPSM:4, CSM:1, CSA:5, CLD:3;
} Tex1GsTex0;
typedef struct Tex1GsTex1 {
    u64 LCM:1, pad01:1, MXL:3, MMAG:1, MMIN:3, MTBA:1, pad10:9, L:2, pad21:11, K:12, pad44:20;
} Tex1GsTex1;
typedef struct Tex1GsMiptbp {
    u64 TBP1:14, TBW1:6, TBP2:14, TBW2:6, TBP3:14, TBW3:6, pad60:4;
} Tex1GsMiptbp;
typedef struct Tex1TexRegBits {
    Tex1GsTex0 tex0; Tex1GsTex1 tex1; Tex1GsMiptbp miptbp1; Tex1GsMiptbp miptbp2; u64 clamp;
} Tex1TexRegBits;
static_assert(sizeof(Tex1TexRegBits) == sizeof(GsTexRegs), "Tex1TexRegBits");

// FUNC 0x002487B8 tex1_set_vram_base
/* Re-links the set for a new VRAM base (vramWordAddr >> 6 blocks): TBP0,
 * CBP, the MIPTBPs of mipmapped textures and every transfer's DBP get the
 * delta (14 / 16 bits wrapping). */
Tex1Image *tex1_set_vram_base(Tex1Image *img, u32 vramWordAddr)
{
    u32 base = vramWordAddr >> 6;
    s32 delta = (s32)(base - img->vramBase);
    int i;

    if (delta == 0)
        return img;

    img->vramBase = (u16)base;
    for (i = 0; i < img->nTex; i++) {
        Tex1TexRegBits *t = &((Tex1TexRegBits *)img->tex)[i];
        int mxl;

        t->tex0.TBP0 = (int)t->tex0.TBP0 + delta;
        t->tex0.CBP = (int)t->tex0.CBP + delta;
        mxl = t->tex1.MXL;
        if (mxl > 0) {
            t->miptbp1.TBP1 = (int)t->miptbp1.TBP1 + delta;
            t->miptbp1.TBP2 = (int)t->miptbp1.TBP2 + delta;
            t->miptbp1.TBP3 = (int)t->miptbp1.TBP3 + delta;
            if (mxl >= 4) {
                t->miptbp2.TBP1 = (int)t->miptbp2.TBP1 + delta;
                t->miptbp2.TBP2 = (int)t->miptbp2.TBP2 + delta;
                t->miptbp2.TBP3 = (int)t->miptbp2.TBP3 + delta;
            }
        }
    }
    if (img->xfer != NULL) {
        Tex1Xfer *x = img->xfer;

        for (i = 0; i < img->nXfer; i++, x++)
            x->dbp = (u16)(x->dbp + delta);
    }
    return img;
}

// FUNC 0x00248A30 gs_tex_override_enable
/* GL-style switches of the library state's head (GS_OVR_*). */
void gs_tex_override_enable(u32 what)
{
    GsLibState *st = &g_002F72C0;

    switch (what) {
    case GS_OVR_ENABLE:     st->enable = 1; return;
    case GS_OVR_MAT_SCALE:  st->matScaleOn = 1; return;
    case GS_OVR_PRIM_IIP:   st->primMask = st->primMask | 0x08; return;
    case GS_OVR_PRIM_TME:   st->primMask = st->primMask | 0x10; return;
    case GS_OVR_PRIM_FGE:   st->primMask = st->primMask | 0x20; return;
    case GS_OVR_PRIM_ABE:   st->primMask = st->primMask | 0x40; return;
    case GS_OVR_PRIM_AA1:   st->primMask = st->primMask | 0x80; return;
    case GS_OVR_TEX0_TFX:   st->tex0Mask = st->tex0Mask | 0x0000001800000000ull; return;
    case GS_OVR_TEX1_MMAG:  st->tex1Mask = st->tex1Mask | 0x20; return;
    case GS_OVR_TEX1_MMIN:  st->tex1Mask = st->tex1Mask | 0x1C0; return;
    case GS_OVR_CLAMP_U:    st->clampMask = st->clampMask | 0x00FFFFF3; return;
    case GS_OVR_CLAMP_V:    st->clampMask = st->clampMask | 0x00000FFFFF00000Cull; return;
    }
}

// FUNC 0x00248BB8 gs_tex_override_disable
void gs_tex_override_disable(u32 what)
{
    GsLibState *st = &g_002F72C0;

    switch (what) {
    case GS_OVR_ENABLE:     st->enable = 0; return;
    case GS_OVR_MAT_SCALE:  st->matScaleOn = 0; return;
    case GS_OVR_PRIM_IIP:   st->primMask = st->primMask & 0xFFF7; return;
    case GS_OVR_PRIM_TME:   st->primMask = st->primMask & 0xFFEF; return;
    case GS_OVR_PRIM_FGE:   st->primMask = st->primMask & 0xFFDF; return;
    case GS_OVR_PRIM_ABE:   st->primMask = st->primMask & 0xFFBF; return;
    case GS_OVR_PRIM_AA1:   st->primMask = st->primMask & 0xFF7F; return;
    case GS_OVR_TEX0_TFX:   st->tex0Mask = st->tex0Mask & 0xFFFFFFE7FFFFFFFFull; return;
    case GS_OVR_TEX1_MMAG:  st->tex1Mask = st->tex1Mask & (u64)(s64)-0x21; return;
    case GS_OVR_TEX1_MMIN:  st->tex1Mask = st->tex1Mask & (u64)(s64)-0x1C1; return;
    case GS_OVR_CLAMP_U:    st->clampMask = st->clampMask & 0xFFFFFFFFFF00000Cull; return;
    case GS_OVR_CLAMP_V:    st->clampMask = st->clampMask & 0xFFFFF00000FFFFF3ull; return;
    }
}

// FUNC 0x00248D48 vif_model_relocate
/* Offsets -> pointers of a VifModel: ptr04 when non-NULL, every part's
 * data. */
VifModel *vif_model_relocate(VifModel *m)
{
    VifModel *old = m->self;
    s32 delta;
    int n;
    int i;

    m->self = m;
    delta = (s32)((u8 *)m - (u8 *)old);
    TEX1_RELOC(m->ptr04, delta);
    n = m->nParts;
    for (i = 0; i < n; i++)
        RELOC(m->part[i].data, delta);
    pglr_model_loaded(m);                   /* PC: a model loaded here, decoded again by the renderer (ahead) */
    return m;
}

// FUNC 0x00248DA8 tex1_relocate
/* The set's offsets -> pointers for its current address (nothing when it
 * is in place).  PC: and a fresh identity for the renderer's registry (a
 * set (re)loaded where an older one was is not confused with it). */
Tex1Image *tex1_relocate(Tex1Image *img)
{
    s32 delta = (s32)((u8 *)img - (u8 *)img->self);

    if (delta == 0)
        return img;

    img->self = img;
    TEX1_RELOC(img->ptr08, delta);
    TEX1_RELOC(img->tex, delta);
    if (img->xfer != NULL) {
        int n;
        int i;

        RELOC(img->xfer, delta);
        n = img->nXfer;
        for (i = 0; i < n; i++)
            RELOC(img->xfer[i].data, delta);
    }
    if (img->ptrTable != NULL) {
        u32 *p;
        int n;

        RELOC(img->ptrTable, delta);
        p = img->ptrTable;
        n = (int)*p++;
        while (n--) {
            if (*p != 0)
                *p += (u32)delta;
            p++;
        }
    }
    if (img->clutAnim != NULL) {
        RELOC(img->clutAnim, delta);
        clut_anim_table_relocate(img->clutAnim, delta);
    }
    tex1_overflow(img, 0);                  /* PC: (a new load: no VRAM asked for yet) */
    pglr_tex1_loaded(img);
    return img;
}

/* PC (ROADMAP 18, no memory limits; G001_flare.h): the sets that found no
 * VRAM, until they get some or are loaded again (at most kOverflowMax, the
 * oldest dropped).  A set's entry is trusted while its header is still the
 * relocated set's (magic, self): the memory of one unloaded may hold
 * anything.  s_srcOn: the renderer was last told a set (pglr_texture_source
 * with NULL ends it). */
enum { kOverflowMax = 256 };
static Tex1Image *s_overflow[kOverflowMax];
static int s_nOverflow;
static int s_srcOn;

void tex1_overflow(Tex1Image *img, int on)
{
    int i;

    for (i = 0; i < s_nOverflow; i++)
        if (s_overflow[i] == img)
            break;
    if (on == 0) {
        if (i < s_nOverflow) {
            s_overflow[i] = s_overflow[--s_nOverflow];
        }
        return;
    }
    if (i < s_nOverflow)
        return;
    printf("PC: Tex1 set %p (%u blocks, %u textures) found no VRAM: drawn from the set\n", (void *)img,
           img->vramSize, img->nTex);
    if (s_nOverflow == kOverflowMax) {
        memmove(&s_overflow[0], &s_overflow[1], sizeof(s_overflow[0]) * (kOverflowMax - 1));
        s_nOverflow--;
    }
    s_overflow[s_nOverflow++] = img;
}

/* gfx_set_texture_regs: the texture t (TEX0 as it goes out) of a set that
 * found no VRAM is named to the renderer, which binds it from the set; the
 * next texture of another set ends that. */
void tex1_texture_source(const GsTexRegs *t, u64 tex0)
{
    const Tex1Image *src = NULL;
    int i;

    for (i = 0; i < s_nOverflow && src == NULL; i++) {
        const Tex1Image *img = s_overflow[i];
        if (img->magic == 0x31786554 && img->self == img && img->tex != NULL && t >= img->tex &&
            t < img->tex + img->nTex)
            src = img;
    }
    if (src != NULL) {
        pglr_texture_source(src, src->vramBase, tex0);
        s_srcOn = 1;
    } else if (s_srcOn) {
        pglr_texture_source(NULL, 0, 0);
        s_srcOn = 0;
    }
}

typedef union U113Tex0 {
    u64 raw;
    struct {
        u64 tbp0 : 14; u64 tbw : 6; u64 psm : 6; u64 tw : 4; u64 th : 4; u64 tcc : 1; u64 tfx : 2;
        u64 cbp : 14; u64 cpsm : 4; u64 csm : 1; u64 csa : 5; u64 cld : 3;
    } f;
} U113Tex0;

// FUNC 0x00248EC8 tex1_select_variation
/* Variation `index' of the set: each entry (texture << 23 | CPSM << 19 | CBP
 * offset << 5 | CSA) rewrites that texture's TEX0.CPSM, CBP (+ vramBase)
 * and CSA. */
void tex1_select_variation(Tex1Image *img, int index)
{
    u32 *p = img->ptrTable;
    u64 vb;
    int n;

    if (p == NULL)
        return;
    if (index < 0)
        return;
    n = (int)*p++;
    if (!(index < n))
        return;
    vb = img->vramBase;
    p = (u32 *)p[index];
    n = (int)*p++;
    n &= 0xFFFF;
    while (n--) {
        u32 e = *p++;
        u32 t = e >> 23;
        U113Tex0 *tex0 = (U113Tex0 *)&img->tex[t].tex0;

        tex0->f.cpsm = e >> 19;
        tex0->f.cbp = ((e >> 5) & 0x3FFF) + vb;
        tex0->f.csa = e;
    }
}

// FUNC 0x00248FF8 clut_anim_channel_reset
void clut_anim_channel_reset(ClutAnimChannel *ch)
{
    ch->time = 0.0f;
    ch->dir = 1;
    ch->lerp = 1.0f;
    ch->cur = 0;
    ch->hold = 0;
    ch->next = (ch->nKeys != 0);
    if (ch->keys[0].time < ch->time)
        ch->hold = 1;
}

// FUNC 0x00249048 clut_anim_channel_apply
/* When dirty: dest[i] = per byte (cur[i] w + next[i] (0x1000 - w)) >> 12,
 * w = lerp * 4096 (the MMI halfword products). */
void clut_anim_channel_apply(ClutAnimChannel *ch)
{
    s32 w, wn;
    const u32 *src0, *src1;
    u32 *dst;
    u32 n;

    if (ch->dirty == 0)
        return;

    w  = (s32)(ch->lerp * 4096.0f);
    wn = 0x1000 - w;
    src0 = ch->keys[ch->cur].palette;
    src1 = ch->keys[ch->next].palette;
    dst  = ch->dest;
    for (n = ch->nColors; n != 0; n--) {
        u32 a = *src0++;
        u32 b = *src1++;
        u32 out = 0;
        int k;

        for (k = 0; k < 4; k++) {
            s32 ca = (s32)((a >> (8 * k)) & 0xFF);
            s32 cb = (s32)((b >> (8 * k)) & 0xFF);
            u32 sum = (u32)(ca * (s16)w) + (u32)(cb * (s16)wn);

            out |= ((sum >> 12) & 0xFF) << (8 * k);
        }
        *dst++ = out;
    }
    ch->dirty = 0;
}

static inline void clut_anim_table_apply(ClutAnimTable *tbl)
{
    int i;

    if (tbl->ch == NULL)
        return;
    for (i = 0; i < tbl->count; i++)
        clut_anim_channel_apply(&tbl->ch[i]);
}

// FUNC 0x00249110 clut_anim_apply_all
void clut_anim_apply_all(Tex1Image *img)
{
    if (img->clutAnim == NULL)
        return;
    clut_anim_table_apply(img->clutAnim);
}

static inline void clut_anim_channel_step(ClutAnimChannel *ch, float dt)
{
    int k;
    f32 t;
    f32 prev;

    if (ch->hold > 0)
        return;
    prev = ch->time;
    t = prev + dt;
    k = ch->cur;

    while (ee_absf(ch->keys[ch->cur].time) <= t) {
        t -= ee_absf(ch->keys[ch->cur].time);
        ch->cur = ch->next;
        switch (ch->mode) {
        case 0:
            k = ch->cur;
            if (k != (int)ch->nKeys - 1)
                ch->next = (u16)(k + 1);
            else
                ch->next = 0;
            break;
        case 1:
            if (ch->cur == 0) {
                ch->dir = 1;
                k = ch->cur;
            } else {
                k = ch->cur;
                if (k == (int)ch->nKeys - 1)
                    ch->dir = -1;
            }
            ch->next = (u16)(ch->cur + ch->dir);
            break;
        case 2:
            k = ch->cur;
            if (k == (int)ch->nKeys - 1)
                goto done;
            ch->next = (u16)(k + 1);
            break;
        default:
            k = ch->cur;
            break;
        }
        if (ch->keys[ch->cur].time < 0.0f)
            ch->hold++;
        if (ch->hold > 0) {
            t = 0.0f;
            goto done;
        }
    }

    while (t < 0.0f) {
        if (ch->keys[ch->cur].time < 0.0f && 0.0f < prev)
            ch->hold++;
        if (ch->hold > 0) {
            t = 0.0f;
            goto done;
        }
        ch->next = ch->cur;
        switch (ch->mode) {
        case 0:
            ch->cur = (ch->cur != 0) ? ch->cur - 1 : ch->nKeys - 1;
            k = ch->cur;
            break;
        case 1:
            if (ch->cur == 0)
                ch->dir = -1;
            else if (k == (int)ch->nKeys - 1)
                ch->dir = 1;
            ch->cur = ch->cur - ch->dir;
            k = ch->cur;
            break;
        case 2:
            if (ch->cur == 0) {
                ch->next = 1;
                goto done;
            }
            ch->cur = ch->cur - 1;
            k = ch->cur;
            break;
        default:
            break;
        }
        t += ee_absf(ch->keys[ch->cur].time);
        prev = t;
    }

done:
    ch->time = t;
    t = EE_MAXF(t, 0.0f);
    ch->lerp = 1.0f - t / ee_absf(ch->keys[ch->cur].time);
    ch->dirty = 1;
}

static inline void clut_anim_table_step(ClutAnimTable *tbl, int id, float dt)
{
    int i;

    if (tbl->ch == NULL)
        return;
    for (i = 0; i < tbl->count; i++) {
        if (id >= 0 && tbl->ch[i].id != id)
            continue;
        clut_anim_channel_step(&tbl->ch[i], dt);
    }
}

// FUNC 0x00249180 clut_anim_step
void clut_anim_step(Tex1Image *img, float dt, int id)
{
    if (img->clutAnim == NULL)
        return;
    clut_anim_table_step(img->clutAnim, id, dt);
}

/* PC (no original; G001_flare.h): the channels of animation id (every one
 * when id < 0) written into their CLUTs as they stand ofs seconds from
 * their state: a copy of the channel stepped by ofs (the step walks back
 * over the keys too) and applied; the channel itself is not changed.  A
 * frozen channel (hold) is left alone, as the step leaves it (and so not
 * applied over another channel's colours).  ofs 0 writes the state's
 * colours. */
void clut_anim_pose(Tex1Image *img, int id, float ofs)
{
    ClutAnimTable *tbl = img->clutAnim;
    int i;

    if (tbl == NULL || tbl->ch == NULL)
        return;
    for (i = 0; i < tbl->count; i++) {
        const ClutAnimChannel *ch = &tbl->ch[i];
        if (id >= 0 && ch->id != id)
            continue;
        if (ch->hold > 0 || ch->nKeys == 0 || ch->keys == NULL)
            continue;
        ClutAnimChannel c = *ch;
        if (ofs != 0.0f) {
            float o = ofs;
            if (c.time + o < 0.0f && c.time + o > -1e-5f)
                o = -c.time;                /* a rounding, not into the key before */
            clut_anim_channel_step(&c, o);
        }
        c.dirty = 1;
        clut_anim_channel_apply(&c);
    }
}

static inline void clut_anim_table_release(ClutAnimTable *tbl, int id)
{
    int i;

    if (tbl->ch == NULL)
        return;
    for (i = 0; i < tbl->count; i++) {
        if (id >= 0 && tbl->ch[i].id != id)
            continue;
        tbl->ch[i].hold--;
    }
}

// FUNC 0x002494A0 clut_anim_release
void clut_anim_release(Tex1Image *img, int id)
{
    if (img->clutAnim == NULL)
        return;
    clut_anim_table_release(img->clutAnim, id);
}

static inline void clut_anim_table_reset(ClutAnimTable *tbl, int id)
{
    int i;

    if (tbl->ch == NULL)
        return;
    for (i = 0; i < tbl->count; i++) {
        if (id >= 0 && tbl->ch[i].id != id)
            continue;
        clut_anim_channel_reset(&tbl->ch[i]);
        tbl->ch[i].dirty = 1;
        if (id < 0)
            clut_anim_channel_apply(&tbl->ch[i]);
    }
}

// FUNC 0x00249518 clut_anim_reset_all
/* Rewinds the animations (every channel when id < 0, then applied at once,
 * as GT3 does; the enhanced engine's id >= 0: those channels, applied by
 * the next clut_anim_apply_all). */
void clut_anim_reset_all(Tex1Image *img, int id)
{
    if (img->clutAnim == NULL)
        return;
    clut_anim_table_reset(img->clutAnim, id);
}

static inline void clut_anim_channel_relocate(ClutAnimChannel *ch, s32 delta)
{
    TEX1_RELOC(ch->dest, delta);
    if (ch->keys != NULL) {
        int n;
        int j;

        RELOC(ch->keys, delta);
        n = ch->nKeys;
        for (j = 0; j < n; j++) {
            if (ch->keys[j].palette != NULL)
                RELOC(ch->keys[j].palette, delta);
        }
    }
}

// FUNC 0x002495B8 clut_anim_table_relocate
void clut_anim_table_relocate(ClutAnimTable *tbl, s32 delta)
{
    int i;

    if (tbl->ch == NULL)
        return;
    RELOC(tbl->ch, delta);
    for (i = 0; i < (int)tbl->count; i++) {
        clut_anim_channel_relocate(&tbl->ch[i], delta);
        clut_anim_channel_reset(&tbl->ch[i]);
        tbl->ch[i].dirty = 0;
    }
}

// FUNC 0x002496B0 tex1_variation_count
u32 tex1_variation_count(Tex1Image *img)
{
    u32 *tbl = img->ptrTable;

    if (tbl == NULL)
        return 0;
    return tbl[0];
}

// FUNC 0x002496D0 tex1_upload_packet
/* Every transfer record of the set queued (in the frame, on the GIF list).
 * PC: recorded for the renderer's registry. */
void tex1_upload_packet(Tex1Image *img)
{
    pglr_tex1_upload(img, 0);
}

// FUNC 0x002497F0 tex1_upload_to_vram
/* The uploads sent at once (after gfx_sync_pipeline).  PC: registered at
 * once. */
void tex1_upload_to_vram(Tex1Image *img)
{
    gfx_sync_pipeline();
    pglr_tex1_upload(img, 1);
}

/* The ring's point (1, 0, 0) and its transform. */
extern "C" const f32 g_00344AD0[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
static f32 g_004A6870[4];

// FUNC 0x002498C8 ring_draw
/* A ring of n vertex pairs from deg0 to deg1 (a full circle when deg0 ==
 * deg1, the last pair then repeating the first), the inner vertices scaled
 * by 1 - width; drawn by pgl_draw_strip.  (gt2k_menu::CircleGauge.) */
void ring_draw(u32 colorInner, u32 colorOuter, int n, f32 deg0, f32 deg1, f32 width)
{
    f32 *xyz;
    u32 *rgba;
    f32 *pair;
    f32 span, step;
    int last, count;
    int i;

    gs_flush_dirty_regs();
    xyz = (f32 *)gfx_alloc_tail((n * 6) / 4);
    rgba = (u32 *)gfx_alloc_tail(n / 2);
    for (i = 0; i < n; i++) {
        rgba[i * 2] = colorOuter;
        rgba[i * 2 + 1] = colorInner;
    }
    last = n - 1;
    count = n * 2;
    span = 360.0f;
    if (!(deg0 == deg1))
        span = deg1 - deg0;
    step = span / (f32)last;
    for (i = 0; i < n; i++) {
        pair = &xyz[i * 6];
        if (i == last && span == 360.0f) {
            pair[0] = xyz[0];
            pair[1] = xyz[1];
            pair[2] = xyz[2];
            pair[3] = xyz[3];
            pair[4] = xyz[4];
            pair[5] = xyz[5];
        } else {
            pgl_push_matrix();
            vu0_load_identity_regs();
            vu0_rotate_z(-deg0);
            vu0_apply_matrix(g_004A6870, g_00344AD0);
            pair[0] = g_004A6870[0];
            pair[1] = g_004A6870[1];
            pair[2] = g_004A6870[2];
            vu0_scale(1.0f - width, 1.0f - width, 1.0f - width);
            vu0_apply_matrix(g_004A6870, g_00344AD0);
            pair[3] = g_004A6870[0];
            pair[4] = g_004A6870[1];
            pair[5] = g_004A6870[2];
            pgl_pop_matrix();
        }
        deg0 = deg0 + step;
    }
    pgl_draw_strip(xyz, NULL, rgba, NULL, count);
}

// FUNC 0x00249B28 ring_draw_full
void ring_draw_full(u32 colorInner, u32 colorOuter, f32 width)
{
    ring_draw(colorInner, colorOuter, 0x20, 0.0f, 0.0f, width);
}

/* ======================================================================== */
/* texregs.c                                                                */
/* ======================================================================== */

// FUNC 0x0024B930 texregs_init
void texregs_init(GsTexRegs *t)
{
    t->miptbp1 = 0;
    t->miptbp2 = 0;
    t->clamp = 0;
    t->tex0 = 1ull << 34;
    t->tex1 = 0x60;
}

// FUNC 0x0024B958 texregs_set_buffer
void texregs_set_buffer(GsTexRegs *t, u32 level, u32 addr, s32 psm, s32 width)
{
    u32 tbp = addr >> 6;
    s32 tbw = width >> 6;

    switch (level) {
    case 0:
        t->tex0 = ((((((t->tex0 & ~0x3FFFull) | (tbp & 0x3FFF)) & 0xFFFFFFFFFFF03FFFull) |
                     ((u64)(tbw & 0x3F) << 14)) & 0xFFFFFFFFFC0FFFFFull) |
                   ((u64)(psm & 0x3F) << 20)) | (1ull << 34);
        break;
    case 1:
        t->miptbp1 = (((t->miptbp1 & ~0x3FFFull) | (tbp & 0x3FFF)) & 0xFFFFFFFFFFF03FFFull) |
                     ((u64)(tbw & 0x3F) << 14);
        break;
    case 2:
        t->miptbp1 = (((t->miptbp1 & 0xFFFFFFFC000FFFFFull) | ((u64)(tbp & 0x3FFF) << 20)) &
                      0xFFFFFF03FFFFFFFFull) | ((u64)(tbw & 0x3F) << 34);
        break;
    case 3:
        t->miptbp1 = (((t->miptbp1 & 0xFFC000FFFFFFFFFFull) | ((u64)(tbp & 0x3FFF) << 40)) &
                      0xF03FFFFFFFFFFFFFull) | ((u64)(tbw & 0x3F) << 54);
        break;
    case 4:
        t->miptbp2 = (((t->miptbp2 & ~0x3FFFull) | (tbp & 0x3FFF)) & 0xFFFFFFFFFFF03FFFull) |
                     ((u64)(tbw & 0x3F) << 14);
        break;
    case 5:
        t->miptbp2 = (((t->miptbp2 & 0xFFFFFFFC000FFFFFull) | ((u64)(tbp & 0x3FFF) << 20)) &
                      0xFFFFFF03FFFFFFFFull) | ((u64)(tbw & 0x3F) << 34);
        break;
    case 6:
        t->miptbp2 = (((t->miptbp2 & 0xFFC000FFFFFFFFFFull) | ((u64)(tbp & 0x3FFF) << 40)) &
                      0xF03FFFFFFFFFFFFFull) | ((u64)(tbw & 0x3F) << 54);
        break;
    }
}

// FUNC 0x0024BBA0 texregs_set_size
void texregs_set_size(u64 *tex0, s32 w, s32 h)
{
    s32 logW, logH;

    for (logW = 1; logW < 10 && (1 << logW) < w; logW++)
        ;
    for (logH = 1; logH < 10 && (1 << logH) < h; logH++)
        ;
    ((GsTex0 *)tex0)->TW = logW;
    ((GsTex0 *)tex0)->TH = logH;
}

// FUNC 0x0024BC68 texregs_set_mode
void texregs_set_mode(GsTexRegs *t, u32 what, s32 v)
{
    switch (what) {
    case PGL_TEXPARAM_TCC:
        t->tex0 = (t->tex0 & 0xFFFFFFFBFFFFFFFFull) | ((u64)(v & 1) << 34);
        break;
    case PGL_TEXPARAM_TFX:
        t->tex0 = (t->tex0 & 0xFFFFFFE7FFFFFFFFull) | ((u64)(v & 3) << 35);
        break;
    case PGL_TEXPARAM_MMIN:
        t->tex1 = (t->tex1 & ~0x1C0ull) | ((u64)(v & 7) << 6);
        break;
    case PGL_TEXPARAM_MMAG:
        t->tex1 = (t->tex1 & ~0x20ull) | ((u64)(v & 1) << 5);
        break;
    case PGL_TEXPARAM_WMS:
        t->clamp = (t->clamp & ~3ull) | (u64)(v & 3);
        break;
    case PGL_TEXPARAM_WMT:
        t->clamp = (t->clamp & ~0xCull) | (u64)((v & 3) << 2);
        break;
    case PGL_TEXPARAM_LCM:
        t->tex1 = (t->tex1 & ~1ull) | (u64)(v & 1);
        break;
    case PGL_TEXPARAM_MXL:
        t->tex1 = (t->tex1 & ~0x1Cull) | (u64)((v & 7) << 2);
        break;
    case PGL_TEXPARAM_K:
        t->tex1 = (t->tex1 & 0xFFFFF000FFFFFFFFull) | ((u64)(v & 0xFFF) << 32);
        break;
    case PGL_TEXPARAM_L:
        t->tex1 = (t->tex1 & 0xFFFFFFFFFFE7FFFFull) | ((u64)(v & 3) << 19);
        break;
    }
}

// FUNC 0x0024BDC8 texregs_set_clamp_region
void texregs_set_clamp_region(GsTexRegs *t, s32 axis, s32 lo, s32 hi)
{
    switch (axis) {
    case PGL_REGION_U:
        t->clamp = (((t->clamp & ~0x3FF0ull) | ((u64)(lo & 0x3FF) << 4)) & 0xFFFFFFFFFF003FFFull) |
                   ((u64)(hi & 0x3FF) << 14);
        break;
    case PGL_REGION_V:
        t->clamp = (((t->clamp & 0xFFFFFFFC00FFFFFFull) | ((u64)(lo & 0x3FF) << 24)) &
                    0xFFFFF003FFFFFFFFull) | ((u64)(hi & 0x3FF) << 34);
        break;
    }
}

// FUNC 0x0024BE68 tex_vram_size
/* VRAM words of a w x h texture of psm rounded up to whole pages,
 * *alignedW the rounded width; -1 for another psm. */
s32 tex_vram_size(u32 psm, s32 w, s32 h, s32 *alignedW)
{
    s32 pageW, pageH, shift, roundW, roundH;

    switch (psm) {
    case GS_PSMCT32: case GS_PSMCT24: case GS_PSMT8H: case GS_PSMT4HL: case GS_PSMT4HH:
    case GS_PSMZ32: case GS_PSMZ24:
        pageW = 0x40; pageH = 0x20; shift = 11;
        break;
    case GS_PSMCT16: case GS_PSMCT16S: case GS_PSMZ16: case GS_PSMZ16S:
        pageW = 0x40; pageH = 0x40; shift = 12;
        break;
    case GS_PSMT8:
        pageW = 0x80; pageH = 0x40; shift = 13;
        break;
    case GS_PSMT4:
        pageW = 0x80; pageH = 0x80; shift = 14;
        break;
    default:
        return -1;
    }
    roundW = (w + pageW - 1) & -pageW;
    roundH = (h + pageH - 1) & -pageH;
    if (alignedW != NULL)
        *alignedW = roundW;
    return (roundW * roundH << 11) >> shift;
}
