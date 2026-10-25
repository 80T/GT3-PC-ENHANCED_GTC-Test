/* pgl_particles.cpp - weather particles of pgl: the decompiled particles.c
 * (0x00245E58..0x002466F0), ported.
 *
 * A box of drops wrapped around a point in front of the eye, each drop
 * projected and drawn as a 6-vertex strip textured with the draw buffer
 * itself (a refraction look).  The GIF packets are written into the packet
 * buffer as the decomp writes them (gfx_open_direct: decoded by the
 * renderer when the frame executes); the VU0 arithmetic is pgl_math's on
 * the same registers. */
#include "engine/pgl/pgl_port.h"

using pgl::Vec4;

// FUNC 0x00245E58 weather_drop_init
void weather_drop_init(WeatherDrop *d, f32 spreadXZ, f32 spreadY, f32 velXZ, f32 velY, f32 velZ)
{
    f32 v;

    d->pos[0] = lcg_rand_range(-spreadXZ, spreadXZ);
    d->pos[1] = lcg_rand_range(-spreadY * 0.5f, spreadY * 0.5f);
    d->pos[2] = lcg_rand_range(-spreadXZ, spreadXZ);
    d->pos[3] = 1.0f;
    v = lcg_rand_range(-velXZ, velXZ);
    d->vel[1] = velY;
    d->vel[0] = v;
    d->vel[2] = lcg_rand_range(-velZ, velZ);
    d->vel[3] = 0.0f;
}

/* One XYZF2 / UV entry of the packet: vftoi4 of the vector (4 words). */
static inline void put_ftoi4(u32 *q, const Vec4 &v)
{
    q[0] = (u32)pgl::vu::ftoi4(v.x);
    q[1] = (u32)pgl::vu::ftoi4(v.y);
    q[2] = (u32)pgl::vu::ftoi4(v.z);
    q[3] = (u32)pgl::vu::ftoi4(v.w);
}

// FUNC 0x00245F30 weather_draw_quad
/* (x, y, z, 1) through vf24..vf27, clip-tested (any flag: 0).  Else a
 * 16-qword GIF packet around the screen point: TEXFLUSH, RGBAQ 0x80808080,
 * then PRIM 0x154 (tri strip, TME, ABE, FST) with 6 UV / XYZF2 loops: the
 * corners at +-(0.58 size, 0.5 size) and +-1.16 size on x (clamped on the
 * low side to the screen edge), the UVs at +-(0.58, 0.45) size and +-0.928
 * size around the same point in frame-buffer coordinates (clamped at 0).
 * Returns 1.  (The fog lanes of the PS2's pextlw operands are irrelevant:
 * PRIM has no FGE.) */
int weather_draw_quad(f32 x, f32 y, f32 z, f32 size)
{
    GsContext *ctx = GS_SPR_CTX;
    Vec4 v1, v2, v3, v4, v5, v6;
    u32 flags, clipFlags;
    u32 *p;
    f32 hx, hy, uy;

    v1 = pgl::vu::xformW1(&VU0.vf[24], Vec4{x, y, z, 1.0f});
    clipFlags = pgl::vu::clipFlags(v1, v1.w);
    flags = clipFlags & 0x3F;
    if (v1.w < 0.0f)
        flags = (((clipFlags >> 1) | 0x2A) ^ ((clipFlags << 1) | 0x15)) & 0x3F;
    if (flags != 0)
        return 0;

    v3 = vec4_of(ctx->viewScale);
    v4 = vec4_of(ctx->viewCenter);
    v1 = pgl::vu::scale(pgl::vu::mini(v1, VU0.vf[21]), pgl::vu::div(1.0f, v1.w));
    v1 = pgl::vu::madd(VU0.vf[22], v1, VU0.vf[23]);

    p = gfx_open_direct(0x10);
    p[0x0] = 2;
    p[0x1] = 0x10000000;
    p[0x2] = 0xEE;
    p[0xC] = 0x8006;
    p[0xD] = 0x20AA4000;
    p[0xE] = 0x43;
    p[0x6] = 0x3F;
    p[0x8] = 0x80808080;
    p[0xA] = 1;
    p[0x3] = 0;
    p[0xF] = 0;
    p[0x4] = 1;
    p[0x5] = 0;
    p[0x7] = 0;
    p[0x9] = 0;
    p[0xB] = 0;

    /* XYZF2 of the 6 vertices */
    hx = size * 0.58f;
    hy = size * 0.5f;
    v2.x = v4.x - v3.x;
    v2.y = v4.y + v3.y;
    v5 = Vec4{hx, hy, 0.0f, 0.0f};
    v6 = pgl::vu::add(v1, v5);
    put_ftoi4(p + 0x14, v6);                            /* 0x50 */
    v6 = pgl::vu::sub(v1, v5);
    v6.x = v6.x > v2.x ? v6.x : v2.x;
    v6.y = v6.y > v2.y ? v6.y : v2.y;
    put_ftoi4(p + 0x3C, v6);                            /* 0xF0 */
    hy = -hy;
    v5 = Vec4{hx, hy, 0.0f, 0.0f};
    v6 = pgl::vu::add(v1, v5);
    v6.y = v6.y > v2.x ? v6.y : v2.x;                   /* (vmaxx.y: with the x edge, as the binary) */
    put_ftoi4(p + 0x34, v6);                            /* 0xD0 */
    v6 = pgl::vu::sub(v1, v5);
    v6.x = v6.x > v2.x ? v6.x : v2.x;
    put_ftoi4(p + 0x1C, v6);                            /* 0x70 */
    hy = hx + hx;
    v5 = Vec4{hy, 0.0f, 0.0f, 0.0f};
    v6 = pgl::vu::add(v1, v5);
    put_ftoi4(p + 0x24, v6);                            /* 0x90 */
    v6 = pgl::vu::sub(v1, v5);
    v6.x = v6.x > v2.x ? v6.x : v2.x;
    put_ftoi4(p + 0x2C, v6);                            /* 0xB0 */

    /* UV of the 6 vertices: the same point in frame-buffer coordinates */
    uy = size * 0.45f;
    v1 = pgl::vu::sub(v1, v4);
    v1.x = v1.x + v3.x;
    v1.y = v1.y - v3.y;
    v5 = Vec4{hx, uy, 0.0f, 0.0f};
    v6 = pgl::vu::add(v1, v5);
    put_ftoi4(p + 0x10, v6);                            /* 0x40 */
    v6 = pgl::vu::sub(v1, v5);
    v6.x = v6.x > 0.0f ? v6.x : 0.0f;
    v6.y = v6.y > 0.0f ? v6.y : 0.0f;
    put_ftoi4(p + 0x38, v6);                            /* 0xE0 */
    uy = -uy;
    v5 = Vec4{hx, uy, 0.0f, 0.0f};
    v6 = pgl::vu::add(v1, v5);
    v6.y = v6.y > 0.0f ? v6.y : 0.0f;
    put_ftoi4(p + 0x30, v6);                            /* 0xC0 */
    v6 = pgl::vu::sub(v1, v5);
    v6.x = v6.x > 0.0f ? v6.x : 0.0f;
    put_ftoi4(p + 0x18, v6);                            /* 0x60 */
    hx = hx * 1.6f;
    v5 = Vec4{hx, 0.0f, 0.0f, 0.0f};
    v6 = pgl::vu::add(v1, v5);
    put_ftoi4(p + 0x20, v6);                            /* 0x80 */
    v6 = pgl::vu::sub(v1, v5);
    v6.x = v6.x > 0.0f ? v6.x : 0.0f;
    put_ftoi4(p + 0x28, v6);                            /* 0xA0 */
    VU0.vf[1] = v1;
    VU0.vf[6] = v6;
    return 1;
}

// FUNC 0x002461D8 weather_init
/* Defaults (size 8, box 32 x 8, ahead 16, velocity +-0.5 / 0.01 / +-0.5),
 * a random start for every drop (last to first). */
void weather_init(Weather *w, WeatherDrop *drops, int count)
{
    int i;

    w->center[0] = 0.0f;
    w->center[1] = 0.0f;
    w->center[2] = 0.0f;
    w->center[3] = 0.0f;
    w->size = 8.0f;
    w->spreadXZ = 32.0f;
    w->spreadY = 8.0f;
    w->ahead = 16.0f;
    w->velXZ = 0.5f;
    w->velY = 0.01f;
    w->velZ = 0.5f;
    w->drops = drops;
    w->count = count;
    for (i = count - 1; i >= 0; i--)
        weather_drop_init(&w->drops[i], w->spreadXZ, w->spreadY, w->velXZ, w->velY, w->velZ);
}

// FUNC 0x00246290 weather_set
void weather_set(Weather *w, f32 size, f32 spreadXZ, f32 spreadY, f32 ahead, f32 velXZ, f32 velY)
{
    int i;

    w->center[0] = 0.0f;
    w->center[1] = 0.0f;
    w->center[2] = 0.0f;
    w->center[3] = 0.0f;
    w->size = size;
    w->spreadXZ = spreadXZ;
    w->spreadY = spreadY;
    w->ahead = ahead;
    w->velXZ = velXZ;
    w->velY = velY;
    w->velZ = velXZ;
    for (i = w->count - 1; i >= 0; i--)
        weather_drop_init(&w->drops[i], w->spreadXZ, w->spreadY, w->velXZ, w->velY, w->velZ);
}

// FUNC 0x00246320 weather_update
void weather_update(Weather *w)
{
    int i;

    for (i = w->count - 1; i >= 0; i--) {
        w->drops[i].pos[0] += w->drops[i].vel[0];
        w->drops[i].pos[1] += w->drops[i].vel[1];
        w->drops[i].pos[2] += w->drops[i].vel[2];
    }
}

// FUNC 0x00246380 weather_draw
/* centre = inverse(current) (0, 0, -ahead, 1); no depth writes; texture =
 * the draw buffer (region-clamped to its size); period / drop size from
 * the projection's x scale; a 50 % blend; RGBAQ; every drop, last first;
 * depth writes back on, the texture state reset.  addr is not read. */
void weather_draw(Weather *w, u32 addr)
{
    alignas(16) f32 eye[4];
    GsContext *ctx = GS_SPR_CTX;
    f32 period, size, m00;
    pgl::Vec4 proj[4];
    u32 *p;
    int i;

    (void)addr;
    eye[3] = 1.0f;
    eye[0] = 0.0f;
    eye[1] = 0.0f;
    eye[2] = -w->ahead;
    pgl_push_matrix();
    vu0_invert_rigid();
    vu0_apply_matrix(w->center, eye);
    pgl_pop_matrix();
    gfx_depth_mask(0);
    gfx_reset_texture_state_spr();
    gfx_set_texture_buffer(0, (u32)ctx->tgtA.base << 6, ctx->tgtA.psm, ctx->tgtA.width);
    gfx_set_texture_size(ctx->tgtA.width, ctx->tgtA.height);
    gfx_tex_param(PGL_TEXPARAM_MXL, 0);
    gfx_tex_param(PGL_TEXPARAM_WMS, PGL_WRAP_REGION_CLAMP);
    gfx_tex_param(PGL_TEXPARAM_WMT, PGL_WRAP_REGION_CLAMP);
    gfx_set_texture_region(0, 0, ctx->tgtA.width);
    gfx_set_texture_region(1, 0, ctx->tgtA.height);
    gs_flush_dirty_regs();

    size = w->size;
    pgl::vu0GetModeMatrix(VU0, ctx->matrixMode, pgl::MTX_PROJECTION, proj);
    for (int r = 0; r < 4; r++)
        VU0.vf[24 + r] = proj[r];
    m00 = VU0.vf[24].x;
    period = (1.0f / m00) * (w->spreadXZ + w->spreadXZ);
    size = size * sqrtf(m00);
    gfx_blend_func(PGL_BLEND_FIX, 0.5f);
    gs_flush_dirty_regs();
    p = gfx_open_direct(2);
    p[0] = 0x8001;
    p[1] = 0x10000000;
    p[2] = 0xE;
    p[3] = 0;
    p += 4;
    p[0] = 0x80808080;
    p[1] = 1;
    p[2] = 1;
    p[3] = 0;
    vu0_load_screen_regs();
    for (i = w->count - 1; i >= 0; i--)
        weather_draw_drop(&w->drops[i], w, period, w->spreadY, size);
    gfx_depth_mask(1);
    gfx_reset_texture_state_spr();
}

/* VU0 wrap of d into [0, period): d - (int)(d / period) * period, + period
 * when d < 0. */
static inline f32 weather_wrap(f32 d, f32 period)
{
    f32 q = pgl::vu::div(1.0f, period);
    f32 t = (f32)pgl::vu::ftoi0(d * q) * period;
    f32 r = d - t;

    if (d < 0.0f)
        r = r + period;
    return r;
}

// FUNC 0x002465B8 weather_draw_drop
/* The drop's position wrapped into the box around w->center (x, z modulo
 * period, y modulo min(periodY, period), each centred), then
 * weather_draw_quad. */
void weather_draw_drop(WeatherDrop *d, Weather *w, f32 period, f32 periodY, f32 size)
{
    f32 x, y, z;
    f32 half;
    f32 py;

    x = w->center[0] + weather_wrap(d->pos[0] - w->center[0], period);
    half = period * 0.5f;
    x = x - half;
    z = w->center[2] + weather_wrap(d->pos[2] - w->center[2], period);
    z = z - half;
    py = period;
    if (periodY < period)
        py = periodY;
    y = w->center[1] + weather_wrap(d->pos[1] - w->center[1], py) - py * 0.5f;
    weather_draw_quad(x, y, z, size);
}
