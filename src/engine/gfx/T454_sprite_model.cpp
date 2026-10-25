/* T454_sprite_model.cpp - SpriteModel (? name; unit u102).
 *
 * A model of camera-facing quads (? trees, spectators): each part is a list
 * of entries, either a sprite (a quad of the sprite's width and height
 * standing at x, y, z, turned to face the camera about the vertical) or,
 * with bit 15 of the index, a prebuilt packet.  One TEX1 texture.
 */
#include "engine/gfx/model_set_internal.h"
#include "vu0asm.h"
#include "engine/gfx/T454_sprite_model.h"

/* SpriteEntry.idx bit 15: a packet number, not a sprite. */
enum { SPRITE_ENTRY_PACKET = 0x8000 };

/* .bss (a file static): the camera's right axis in model space, xz only,
 * normalised (the quads of spriteset_draw_part are built along it). */
static GsVec4 g_0049D1A0;                   /* 0x0049D1A0 */

/* As modelset_relocate: statement macros (U102_RELOC_STMT, and
 * U102_RELOC_NZ for unk_08 and the material offsets), one index per loop,
 * the material offsets through a pointer view (not the union). */
// FUNC 0x00226AA0 spriteset_relocate
void spriteset_relocate(SpriteModel *m)
{
    int delta;

    if (m == NULL)
        return;
    delta = (u8 *)m - (u8 *)m->base;
    m->base = m;
    U102_RELOC_NZ(m->unk_08, delta);
    if (m->parts != NULL) {
        int i;
        U102_RELOC_STMT(m->parts, delta);
        for (i = 0; i < m->nParts; i++) {
            if (m->parts[i] != NULL)
                U102_RELOC_STMT(m->parts[i], delta);
        }
    }
    if (m->tex != NULL) {
        U102_RELOC_STMT(m->tex, delta);
        tex1_relocate(m->tex);
    }
    if (m->materials != NULL) {
        int i;
        U102_RELOC_STMT(m->materials, delta);
        for (i = 0; i < m->nMaterials; i++)
            U102_RELOC_NZ(*(void **)&m->materials[i].x48.offset, delta);
    }
    if (m->sprites != NULL) {
        int i;
        U102_RELOC_STMT(m->sprites, delta);
        for (i = 0; i < m->nSprites; i++) {
            if (m->sprites[i].uvs != NULL)
                U102_RELOC_STMT(m->sprites[i].uvs, delta);
            if (m->sprites[i].colors != NULL)
                U102_RELOC_STMT(m->sprites[i].colors, delta);
        }
    }
    if (m->packets != NULL) {
        int i;
        U102_RELOC_STMT(m->packets, delta);
        for (i = 0; i < m->nPackets; i++) {
            if (m->packets[i] != NULL) {
                U102_RELOC_STMT(m->packets[i], delta);
                vif_model_relocate(m->packets[i]);
            }
        }
    }
}

// FUNC 0x00226C80 spriteset_reset_clut_anims
void spriteset_reset_clut_anims(SpriteModel *m)
{
    if (m != NULL && m->tex != NULL)
        clut_anim_reset_all(m->tex);
}

// FUNC 0x00226CB0 spriteset_step_clut_anims
void spriteset_step_clut_anims(SpriteModel *m, f32 dt, int id)
{
    if (m != NULL && m->tex != NULL)
        clut_anim_step(m->tex, dt, id);
}

/* PC (no original): the CLUT animations at ofs from their state
 * (clut_anim_pose; the dynamic frame rate). */
void spriteset_pose_clut_anims(SpriteModel *m, int id, f32 ofs)
{
    if (m != NULL && m->tex != NULL)
        clut_anim_pose(m->tex, id, ofs);
}

// FUNC 0x00226CE0 spriteset_apply_clut_anims
void spriteset_apply_clut_anims(SpriteModel *m)
{
    if (m != NULL && m->tex != NULL)
        clut_anim_apply_all(m->tex);
}

// FUNC 0x00226D10 spriteset_release_clut_anims
void spriteset_release_clut_anims(SpriteModel *m, int id)
{
    if (m != NULL && m->tex != NULL)
        clut_anim_release(m->tex, id);
}

/* The render states (texture, fog, depth test, blending; alpha test EQUAL
 * 0x80, failing pixels written to the colour buffer only, not to Z), then
 * the billboard axis: with R = the
 * rotation of the modelview matrix (vu0_load_modelview: vf20..vf22) and t its
 * translation (vf23), vf23 = R^T * -t (? the eye in model space: unused
 * here), vf5 = R^T * (0, 0, 1), g_0049D1A0 = (0, 1, 0) x vf5, then
 * normalised in x and z (rsqrt.s).  Last, the texture is made resident and
 * the packets get m's texture and material tables with texmat_select as
 * their texture hook.
 * Under egcs the transpose is one asm whose five 128-bit temporaries are
 * outputs (the compiler picks retail's registers in this operand order),
 * the store of vf7 is an asm of its own (its address is formed after the
 * VU0 code), the rsqrt.s a volatile asm; the two table calls sit in a
 * statement block (retail aligns the return label after it). */
// FUNC 0x00226D40 spriteset_begin_draw
void spriteset_begin_draw(SpriteModel *m)
{
    f32 axisX, axisZ, invLen;

    if (m == NULL)
        return;
    gfx_enable(PGL_CAP_VU1_PROG_33E);
    gfx_disable(PGL_CAP_CULL_FACE);
    gfx_enable(PGL_CAP_TEXTURE);
    gfx_enable(PGL_CAP_FOG);
    gfx_enable(PGL_CAP_DEPTH_TEST);
    gfx_enable(PGL_CAP_ALPHA_TEST);
    gfx_disable(PGL_CAP_DEST_ALPHA);
    gfx_enable(PGL_CAP_BLEND);
    gfx_depth_func(PGL_DEPTHFUNC_LEQUAL);
    gfx_alpha_func(PGL_ATEST_EQUAL, 1.0f);        /* AREF 0x80 */
    gfx_alpha_fail(PGL_AFAIL_FB_ONLY);
    gfx_blend_func(PGL_BLEND_ALPHA, 0.0f);
    /* PC: vnop; vmr32.xyzw vf5, vf0 (vf5 = (0, 0, 1, 0)) into the port's
     * vf5: it lives across vu0_load_modelview */
    {
        vu0g::Vec4 vf0 = vu0g::vf(0);
        vu0g::vf(5) = vu0g::Vec4{vf0.y, vf0.z, vf0.w, vf0.x};
    }
    vu0_load_modelview();
    /* PC: the second block (vmr32 vf6; the rigid inverse of vf20..vf23 with
     * the MMI transpose; vf5 through it; vf7.xyz = vf6 x vf5) on the port's
     * registers (vu0g::billboardAxis); sqc2 vf7 */
    vu0g::store(&g_0049D1A0, vu0g::billboardAxis());
    axisX = g_0049D1A0.x;
    axisZ = g_0049D1A0.z;
    invLen = u102_rsqrt(1.0f, axisX * axisX + axisZ * axisZ);
    g_0049D1A0.x = axisX * invLen;
    g_0049D1A0.z = axisZ * invLen;
    g_002F4250.loadTex1(m->tex);
    pgl_set_part_tex_func(texmat_select);
    do {
        texsel_set_table(m->tex->tex);
        matsel_set_table(m->materials);
    } while (0);
}

/* (m is unused) */
// FUNC 0x00226EB0 spriteset_end_draw
void spriteset_end_draw(SpriteModel *m)
{
    gfx_disable(PGL_CAP_TEXTURE);
    gfx_disable(PGL_CAP_FOG);
    gfx_disable(PGL_CAP_BLEND);
    gfx_alpha_fail(PGL_AFAIL_KEEP);
}

/* The quads of a part, batched (strip_batch_begin / strip_batch_end) while
 * the texture stays the same; a packet entry closes the batch and forgets
 * the texture.  A quad: one strip of 4 vertices (12 bytes each) in 3
 * qwords of the packet's tail: (x - ax w, y + h, z - az w), (x - ax w, y,
 * z - az w), (x + ax w, y + h, z + az w), (x + ax w, y, z + az w); ax, az =
 * g_0049D1A0, with the sprite's colours and uvs.  The coordinates are
 * written straight into the buffer (vertex 3, then 1, then 2 and 0 from
 * them): the float stores may alias g_0049D1A0 and the sprite, so retail
 * reloads w, h and the axis after each store.  curTex is a u16 like the
 * entry's tex (a promoted register, spilled as a word): with an int, egcs
 * reused the compared e->tex for texmat_select on the no-batch path, where
 * retail reloads it in the call's delay slot on both. */
// FUNC 0x00226EE8 spriteset_draw_part
void spriteset_draw_part(SpriteModel *m, int part)
{
    SpritePart *list;
    SpriteEntry *e;
    u16 curTex;                             /* as SpriteEntry.tex */
    int i, batchOpen;

    if (m == NULL || part >= m->nParts)
        return;
    list = m->parts[part];
    if (list == NULL)
        return;
    e = list->e;
    curTex = 0;
    batchOpen = 0;
    for (i = 0; i < list->count; i++, e++) {
        u32 idx = e->idx;
        if (idx & SPRITE_ENTRY_PACKET) {
            if (batchOpen != 0) {
                batchOpen = 0;
                strip_batch_end();
            }
            curTex = 0;
            idx &= ~SPRITE_ENTRY_PACKET;        /* and 0xFFFF7FFF */
            model_draw(m->packets[idx]);
        } else {
            Sprite *s = &m->sprites[idx];
            f32 *v;
            if (curTex != e->tex) {
                if (batchOpen != 0) {
                    batchOpen = 0;
                    strip_batch_end();
                }
                texmat_select(e->tex);
                curTex = e->tex;
            }
            v = (f32 *)gfx_alloc_tail(3);
            v[9]  = e->x + g_0049D1A0.x * s->w;     /* 0x24 vertex 3 */
            v[10] = e->y;                           /* 0x28 */
            v[11] = e->z + g_0049D1A0.z * s->w;     /* 0x2C */
            v[3]  = e->x - g_0049D1A0.x * s->w;     /* 0x0C vertex 1 */
            v[4]  = e->y;                           /* 0x10 */
            v[5]  = e->z - g_0049D1A0.z * s->w;     /* 0x14 */
            v[6]  = v[9];                           /* 0x18 vertex 2 */
            v[7]  = v[10] + s->h;                   /* 0x1C */
            v[8]  = v[11];                          /* 0x20 */
            v[0]  = v[3];                           /* 0x00 vertex 0 */
            v[1]  = v[4] + s->h;                    /* 0x04 */
            v[2]  = v[5];                           /* 0x08 */
            if (batchOpen == 0) {
                strip_batch_begin();
                batchOpen = 1;
            }
            strip_draw(v, NULL, s->colors, s->uvs, 4);
        }
    }
    if (batchOpen != 0)
        strip_batch_end();
}

// FUNC 0x00227100 spriteset_draw_all
void spriteset_draw_all(SpriteModel *m)
{
    int part;

    if (m == NULL)
        return;
    spriteset_begin_draw(m);
    for (part = 0; part < m->nParts; part++)
        spriteset_draw_part(m, part);
    spriteset_end_draw(m);
}
