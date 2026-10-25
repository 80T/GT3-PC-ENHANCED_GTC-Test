/* pgl_vu0.cpp - pgl's VU0 routines: the decompiled vu0_matrix_asm.c
 * (0x00257A44..0x00257EF4), vu0_current_matrix.c (0x00257EF4..0x00258150),
 * vu0_math.c (0x00245C10..0x00245E58), vecmath.c (0x0024B5B0..0x0024B930)
 * and the VU0 register loaders of flare.c (0x00248680..0x002487B8), ported.
 *
 * PC: the VU0 macro-mode code is pgl_math's C++ on pgl::g_vu0 (the same
 * registers, data memory, micro subroutines and order of operations), so
 * the game's rewritten VU0 code finds vf28..vf31 / vf24..vf27 / vf1 as on
 * the PS2. */
#include "engine/pgl/pgl_port.h"

/* ======================================================================== */
/* vu0_matrix_asm.c                                                         */
/* ======================================================================== */

// FUNC 0x00257A44 vu0_sincos_deg
/* vf1.x = sin(deg), vf1.y = cos(deg) (micro subroutine 0x1D0). */
void vu0_sincos_deg(float deg)
{
    pgl::vu0SinCosDeg(VU0, deg);
}

// FUNC 0x00257B30 vu0_load_identity_regs
void vu0_load_identity_regs(void)
{
    pgl::ctxLoadIdentity(*GS_SPR_CTX, VU0);
}

// FUNC 0x00257B5C vu0_reset_rotation
void vu0_reset_rotation(void)
{
    pgl::ctxResetRotation(*GS_SPR_CTX, VU0);
}

// FUNC 0x00257B84 vu0_reset_translation
void vu0_reset_translation(void)
{
    pgl::ctxResetTranslation(*GS_SPR_CTX, VU0);
}

// FUNC 0x00257B9C pgl_load_matrix
void pgl_load_matrix(const float *m)
{
    pgl::ctxLoadMatrix(*GS_SPR_CTX, VU0, m);
}

// FUNC 0x00257C34 pgl_get_matrix
void pgl_get_matrix(float *m)
{
    pgl::vu0GetMatrix(VU0, m);
}

// FUNC 0x00257CBC pgl_rotate
/* glRotate(deg, axis): the rotation built in vf1..vf4, then current = R x
 * current (micro subroutine 0x000). */
void pgl_rotate(float deg, float x, float y, float z)
{
    pgl::ctxRotate(*GS_SPR_CTX, VU0, deg, x, y, z);
}

// FUNC 0x00257D7C vu0_mult_matrix_regs
/* current = vf1..vf4 x current (micro subroutine 0x000), matrices changed. */
void vu0_mult_matrix_regs(void)
{
    pgl::vu0Micro000(VU0);
    GS_SPR_CTX->matrixDirty = 1;
}

// FUNC 0x00257D94 pgl_mult_matrix
/* glMultMatrix: current = m x current. */
void pgl_mult_matrix(const float *m)
{
    pgl::ctxMultMatrix(*GS_SPR_CTX, VU0, m);
}

// FUNC 0x00257E30 pgl_mult_matrix_90
/* current = current x m. */
void pgl_mult_matrix_90(const float *m)
{
    pgl::ctxMultMatrix90(*GS_SPR_CTX, VU0, m);
}

// FUNC 0x00257EB4 pgl_translate
void pgl_translate(float x, float y, float z)
{
    pgl::ctxTranslate(*GS_SPR_CTX, VU0, x, y, z);
}

// FUNC 0x00257EF0 F_00257EF0
/* Falls into vu0_scale_inv_y. */
void F_00257EF0(float s)
{
    vu0_scale_inv_y(s);
}

/* ======================================================================== */
/* vu0_current_matrix.c                                                     */
/* ======================================================================== */

// FUNC 0x00257EF4 vu0_scale_inv_y
/* The current matrix scaled by (1, 1 / s, 1). */
void vu0_scale_inv_y(f32 s)
{
    pgl::ctxScaleInvY(*GS_SPR_CTX, VU0, s);
}

// FUNC 0x00257F04 vu0_scale
void vu0_scale(f32 x, f32 y, f32 z)
{
    pgl::ctxScale(*GS_SPR_CTX, VU0, x, y, z);
}

// FUNC 0x00257F40 pgl_set_scale
void pgl_set_scale(f32 x, f32 y, f32 z)
{
    pgl::ctxSetScale(*GS_SPR_CTX, x, y, z);
}

// FUNC 0x00257F60 vu0_rotate_x
void vu0_rotate_x(f32 deg)
{
    pgl::ctxRotateX(*GS_SPR_CTX, VU0, deg);
}

// FUNC 0x00257FA0 vu0_rotate_y
void vu0_rotate_y(f32 deg)
{
    pgl::ctxRotateY(*GS_SPR_CTX, VU0, deg);
}

// FUNC 0x00257FE0 vu0_rotate_z
void vu0_rotate_z(f32 deg)
{
    pgl::ctxRotateZ(*GS_SPR_CTX, VU0, deg);
}

// FUNC 0x00258020 vu0_transpose
void vu0_transpose(void)
{
    pgl::ctxTranspose(*GS_SPR_CTX, VU0);
}

// FUNC 0x00258078 vu0_invert_rigid
void vu0_invert_rigid(void)
{
    pgl::ctxInvertRigid(*GS_SPR_CTX, VU0);
}

// FUNC 0x002580EC vu0_apply_matrix
/* dst = src x current (w included); vf1 = dst. */
void vu0_apply_matrix(f32 *dst, const f32 *src)
{
    pgl::vu0ApplyMatrix(VU0, dst, src);
}

// FUNC 0x00258138 pgl_set_normal_matrix
void pgl_set_normal_matrix(const f32 *m)
{
    pgl::ctxSetNormalMatrix(*GS_SPR_CTX, m);
}

/* ======================================================================== */
/* vu0_math.c                                                               */
/* ======================================================================== */

extern "C" {
/* The atan2 octant tables (kept: game code may read them). */
const f32 g_00343CE8[8] = { 1.0f, -1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f };
const f32 g_00343D08[10] = {
    0.0f, 1.5707964f, 3.1415927f, 1.5707964f, 0.0f, -1.5707964f, -3.1415927f, -1.5707964f,
    0.0f, 0.0f
};
/* The LCG state of lcg_rand_seed / lcg_rand12 (0x003530AC). */
u32 g_003530AC = 0;
}

// FUNC 0x00245C10 vu0_atan2f
f32 vu0_atan2f(f32 y, f32 x)
{
    return pgl::vu0Atan2(VU0, y, x);
}

// FUNC 0x00245D5C vu0_rand
/* vrnext: [1, 2) - 1. */
f32 vu0_rand(void)
{
    return pgl::vu0Rand(VU0);
}

// FUNC 0x00245D78 vu0_rand_seed
/* vrinit with the bits of seed, then (the retail fall-through) the LCG
 * state = lcgSeed. */
void vu0_rand_seed(u32 lcgSeed, f32 seed)
{
    pgl::vu0RandSeed(VU0, seed);
    g_003530AC = lcgSeed;
}

// FUNC 0x00245D88 lcg_rand_seed
void lcg_rand_seed(u32 seed)
{
    g_003530AC = seed;
}

// FUNC 0x00245D90 lcg_rand12
/* state = state * 0x5D588B65 + 1; the float 1.0 with the state's low 23
 * bits as mantissa: [1, 2). */
f32 lcg_rand12(void)
{
    u32 state = g_003530AC * 0x5D588B65u + 1;

    g_003530AC = state;
    return pgl_u2f((state & 0x007FFFFF) | 0x3F800000);
}

// FUNC 0x00245DC8 lcg_rand_range
f32 lcg_rand_range(f32 lo, f32 hi)
{
    f32 r12 = lcg_rand12();

    return (r12 - 1.0f) * (hi - lo) + lo;
}

// FUNC 0x00245E10 lcg_rand_chance
int lcg_rand_chance(f32 p)
{
    f32 r12 = lcg_rand12();

    return r12 < p + 1.0f;
}

/* ======================================================================== */
/* vecmath.c                                                                */
/* ======================================================================== */

// FUNC 0x0024B5B0 vec3_normalize
/* out = v / |v| (rsqrt.s). */
void vec3_normalize(f32 *out, f32 x, f32 y, f32 z)
{
    f32 invLen = pgl::vu::rsqrt(1.0f, x * x + y * y + z * z);

    out[0] = x * invLen;
    out[1] = y * invLen;
    out[2] = z * invLen;
}

// FUNC 0x0024B5F0 vec3_cross
void vec3_cross(f32 *out, const f32 *a, const f32 *b)
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

// FUNC 0x0024B658 glu_look_at
void glu_look_at(f32 ex, f32 ey, f32 ez, f32 cx, f32 cy, f32 cz, f32 ux, f32 uy, f32 uz)
{
    alignas(16) f32 m[16];
    alignas(16) f32 forward[4];
    alignas(16) f32 up[4];
    alignas(16) f32 upOrtho[4];
    alignas(16) f32 side[4];

    vec3_normalize(forward, cx - ex, cy - ey, cz - ez);
    vec3_normalize(up, ux, uy, uz);
    vec3_cross(side, forward, up);
    vec3_cross(upOrtho, side, forward);
    m[0] = side[0];
    m[4] = side[1];
    m[8] = side[2];
    m[1] = upOrtho[0];
    m[5] = upOrtho[1];
    m[9] = upOrtho[2];
    m[2] = -forward[0];
    m[6] = -forward[1];
    m[10] = -forward[2];
    m[15] = 1.0f;
    m[12] = 0;
    m[13] = 0;
    m[14] = 0;
    m[3] = 0;
    m[7] = 0;
    m[11] = 0;
    pgl_mult_matrix(m);
    pgl_translate(-ex, -ey, -ez);
}

// FUNC 0x0024B7B0 mat_reflect
void mat_reflect(f32 px, f32 py, f32 pz, f32 nx, f32 ny, f32 nz)
{
    alignas(16) f32 m[16];
    f32 dist2 = px * nx + py * ny + pz * nz;
    f32 ax = nx * -2.0f;
    f32 ay = ny * -2.0f;
    f32 az = nz * -2.0f;

    dist2 = dist2 + dist2;
    m[0] = 1.0f - (nx + nx) * nx;
    m[1] = ax * ny;
    m[2] = ax * nz;
    m[3] = 0;
    m[4] = ay * nx;
    m[5] = 1.0f - (ny + ny) * ny;
    m[6] = ay * nz;
    m[7] = 0;
    m[8] = az * nx;
    m[9] = az * ny;
    m[10] = 1.0f - (nz + nz) * nz;
    m[11] = 0;
    m[12] = dist2 * nx;
    m[13] = dist2 * ny;
    m[14] = dist2 * nz;
    m[15] = 1.0f;
    pgl_mult_matrix(m);
}

// FUNC 0x0024B888 proj_shift_depth
void proj_shift_depth(f32 d)
{
    alignas(16) f32 m[16];
    f32 zNear, zFar;

    pgl_get_matrix(m);
    zFar = m[14] / (m[10] - 1.0f) + d;
    zNear = m[14] / (m[10] + 1.0f) + d;
    m[10] = -(zNear + zFar) / (zNear - zFar);
    m[14] = (zNear * -2.0f * zFar) / (zNear - zFar);
    pgl_load_matrix(m);
}

/* ======================================================================== */
/* flare.c's VU0 register loaders                                           */
/* ======================================================================== */

// FUNC 0x00248680 vu0_sync_state
void vu0_sync_state(void)
{
    pgl::ctxSyncState(*GS_SPR_CTX, VU0);
}

// FUNC 0x002486B8 vu0_load_screen_regs
/* Sync, then vf20..vf23 = vBB0, limits, viewCenter, viewScale. */
void vu0_load_screen_regs(void)
{
    GsContext *ctx = GS_SPR_CTX;

    if (ctx->matrixDirty)
        pgl_sync_matrices(ctx);
    pgl::ctxLoadScreenRegsNoSync(*ctx, VU0);
}

// FUNC 0x00248700 vu0_load_screen_regs_nosync
void vu0_load_screen_regs_nosync(void)
{
    pgl::ctxLoadScreenRegsNoSync(*GS_SPR_CTX, VU0);
}

// FUNC 0x00248720 vu0_load_modelview
void vu0_load_modelview(void)
{
    pgl::ctxLoadModelview(*GS_SPR_CTX, VU0);
}

// FUNC 0x00248768 vu0_load_projection
void vu0_load_projection(void)
{
    pgl::ctxLoadProjection(*GS_SPR_CTX, VU0);
}
