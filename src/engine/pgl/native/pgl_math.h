/* pgl_math.h - the VU0 side of the pgl graphics library as plain C++.
 *
 * On the PS2 pgl keeps its matrices in VU0 (decomp src/engine/pgl/
 * vu0_matrix_asm.c, vu0_current_matrix.c, pgl.c, vu1_lighting.c): the current
 * matrix of GsContext.matrixMode in vf28..vf31, the combined matrix in
 * vf24..vf27, the parked matrices of the other modes and the three matrix
 * stacks in VU0 data memory, and four micro subroutines of the VU0 microcode
 * (0x00349600) do the products, sin and atan.  Game code reaches into the
 * same registers with inline assembly (vf28..vf31 the current matrix,
 * vf24..vf27 the combined one, vf20..vf23 the viewport constants after
 * vu0_load_screen_regs, vf1 the sin / cos after vu0_sincos_deg).
 *
 * The port keeps that model: Vu0 below is the register file and data memory
 * (no instruction emulation, no microcode: the micro subroutines are written
 * out as the C++ they compute), g_vu0 the one instance the library and the
 * rewritten game asm share.  Every routine computes in the order the VU
 * did (ACC chains left to right, the same constants, the same polynomial),
 * so results match the PS2 within float rounding (the VU rounds toward zero
 * and has no denormals / Inf / NaN; x86 SSE rounds to nearest: at most one
 * ulp per operation).
 *
 * Matrices are rows (row-vector convention, as pgl: v' = v.x r0 + v.y r1 +
 * v.z r2 + v.w r3, translation in row 3), 16 floats in memory = OpenGL's
 * column-major layout of the same matrix.
 *
 * Library entry points that also touch GsContext (matrixDirty, matrixMode,
 * scale, the viewport vectors ...) are templates over the context type: the
 * port passes its GsContext (G001_flare.h member names), the test tool the
 * minimal mirror PglMathCtx.  Nothing here depends on Windows or the game.
 */
#ifndef PGL_NATIVE_PGL_MATH_H
#define PGL_NATIVE_PGL_MATH_H

#include <cstdint>
#include <cstring>

namespace pgl {

/* One VU0 / VU1 quadword (GsVec4). */
struct alignas(16) Vec4 {
    float x, y, z, w;

    float &operator[](int i) { return (&x)[i]; }
    float operator[](int i) const { return (&x)[i]; }
};

/* Any struct with x, y, z, w members (GsVec4) as a Vec4. */
template <class V> inline Vec4 toVec4(const V &v) { return Vec4{v.x, v.y, v.z, v.w}; }
template <class V> inline void fromVec4(V &dst, const Vec4 &v) { dst.x = v.x; dst.y = v.y; dst.z = v.z; dst.w = v.w; }

inline uint32_t f2u(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
inline float u2f(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }

/* ======================================================================== */
/* The VU0 state                                                            */
/* ======================================================================== */

/* VU0 data memory layout of pgl (qword addresses). */
enum : int {
    VU0_PARKED_MODELVIEW  = 4,      /* matrix of mode m parked at 4 + 4m (pgl_matrix_mode) */
    VU0_PARKED_PROJECTION = 8,      /* also the projection of micro subroutine 0x120 */
    VU0_PARKED_TEXTURE    = 12,
    VU0_STACK_MODELVIEW   = 0x10,   /* vi11 at gfx_init: 0x10..0x4F, 16 matrices */
    VU0_STACK_PROJECTION  = 0x50,   /* vi12: 0x50..0x5F, 4 matrices */
    VU0_STACK_TEXTURE     = 0x60,   /* vi13: 0x60.., the rest of the 256 qwords */
    VU0_MEM_QWORDS        = 256     /* 4 KB; addresses wrap as on the VU */
};

/* pgl_matrix_mode modes (PGL_MTX_*). */
enum : int { MTX_MODELVIEW = 0, MTX_PROJECTION = 1, MTX_TEXTURE = 2 };

/* GsContext.lightFlags bit set by the matrix sync (PGL_LF_MODELVIEW). */
enum : uint8_t { LF_MODELVIEW = 0x02 };

struct Vu0 {
    Vec4     vf[32];                /* vf0 is (0, 0, 0, 1) */
    Vec4     acc;                   /* ACC */
    float    q;                     /* Q: div / sqrt / rsqrt */
    float    i;                     /* I: immediate */
    uint32_t r;                     /* R: 0x3F800000 | 23-bit LFSR state (vrnext / vrinit) */
    uint32_t clip;                  /* vi18: CLIP flags, the last 4 judgements x 6 bits */
    uint16_t vi[16];                /* vi10: vlqi / vsqi index, vi11..vi13: the matrix stacks */
    Vec4     mem[VU0_MEM_QWORDS];   /* data memory */
};

/* The VU0 of the port: shared by the library and the game's rewritten VU0
 * code, like the one VU0 of the PS2 (single-threaded use: the EE kernel did
 * not save COP2 either). */
extern Vu0 g_vu0;

/* Power-on state: everything 0 but vf0 = (0, 0, 0, 1) and R seeded (an
 * all-zero LFSR would only return 0; the VU's own reset value is unknown). */
void vu0Reset(Vu0 &vu);

/* ======================================================================== */
/* VU float helpers (macro-mode instructions the game asm uses)             */
/* ======================================================================== */
namespace vu {

/* v * M with the VU ACC chain: mulax r0, madday r1, maddaz r2, maddw r3. */
inline Vec4 xform(const Vec4 *m, const Vec4 &v)          /* w from v (vmaddw ..., vNw) */
{
    Vec4 o;
    for (int k = 0; k < 4; k++) {
        float a = m[0][k] * v.x;
        a = a + m[1][k] * v.y;
        a = a + m[2][k] * v.z;
        o[k] = a + m[3][k] * v.w;
    }
    return o;
}
inline Vec4 xformW1(const Vec4 *m, const Vec4 &v)        /* w = 1 (vmaddw ..., vf0w) */
{
    Vec4 o;
    for (int k = 0; k < 4; k++) {
        float a = m[0][k] * v.x;
        a = a + m[1][k] * v.y;
        a = a + m[2][k] * v.z;
        o[k] = a + m[3][k];
    }
    return o;
}
inline Vec4 xform3(const Vec4 *m, const Vec4 &v)         /* rows 0..2 only (vmaddz) */
{
    Vec4 o;
    for (int k = 0; k < 4; k++) {
        float a = m[0][k] * v.x;
        a = a + m[1][k] * v.y;
        o[k] = a + m[2][k] * v.z;
    }
    return o;
}
inline Vec4 add(const Vec4 &a, const Vec4 &b) { return Vec4{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
inline Vec4 sub(const Vec4 &a, const Vec4 &b) { return Vec4{a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w}; }
inline Vec4 mul(const Vec4 &a, const Vec4 &b) { return Vec4{a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w}; }
inline Vec4 scale(const Vec4 &a, float s) { return Vec4{a.x * s, a.y * s, a.z * s, a.w * s}; }
inline Vec4 mini(const Vec4 &a, const Vec4 &b)
{
    return Vec4{a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z, a.w < b.w ? a.w : b.w};
}
inline Vec4 maxi(const Vec4 &a, const Vec4 &b)
{
    return Vec4{a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z, a.w > b.w ? a.w : b.w};
}
/* vmulaw ACC, b, vf0w; vmadd d, a, c: d = b + a * c (the viewport mapping). */
inline Vec4 madd(const Vec4 &b, const Vec4 &a, const Vec4 &c)
{
    return Vec4{b.x + a.x * c.x, b.y + a.y * c.y, b.z + a.z * c.z, b.w + a.w * c.w};
}
/* vopmula / vopmsub: the cross product a x b in xyz (w of the result: 0). */
inline Vec4 cross(const Vec4 &a, const Vec4 &b)
{
    return Vec4{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x, 0.0f};
}

/* vdiv / vsqrt / vrsqrt with the VU's divide-by-zero result (+-max float,
 * the sign of the operands) instead of IEEE infinity. */
float div(float num, float den);
float rsqrt(float num, float sq);       /* num / sqrt(sq); also the EE FPU rsqrt.s */

/* vftoiN: float -> fixed with N fraction bits, truncated toward zero,
 * saturated to the int32 range (the VU never traps).  vitofN the inverse. */
int32_t ftoi(float f, int fracBits);
inline int32_t ftoi0(float f) { return ftoi(f, 0); }
inline int32_t ftoi4(float f) { return ftoi(f, 4); }
inline float itof0(int32_t i) { return (float)i; }
inline float itof4(int32_t i) { return (float)i * 0.0625f; }

/* vclipw.xyz v, w: the 6 judgement bits of one point (bit 0 x > |w|, 1 x <
 * -|w|, 2 y > |w|, 3 y < -|w|, 4 z > |w|, 5 z < -|w|). */
uint32_t clipFlags(const Vec4 &v, float w);
/* The same, shifted into vi18 as the instruction does (cfc2 vi18 reads the
 * last four judgements). */
inline void clipw(Vu0 &vu, const Vec4 &v, float w) { vu.clip = ((vu.clip << 6) | clipFlags(v, w)) & 0xFFFFFF; }

}  /* namespace vu */

/* ======================================================================== */
/* The VU0 micro subroutines (vcallms)                                     */
/* ======================================================================== */

/* 0x000: vf28..vf31 = vf1..vf4 x vf28..vf31 (current = m x current, m
 * applied first: glMultMatrix); vf1..vf3 = the new rows 0..2 as well. */
void vu0Micro000(Vu0 &vu);
/* 0x090: vf28..vf31 = vf28..vf31 x vf1..vf4 (current = current x m). */
void vu0Micro090(Vu0 &vu);
/* 0x120: vf24..vf27 = vf28..vf31 x VU0 qwords 8..11 (combined = modelview x
 * parked projection); vf1..vf3 = rows 0..2. */
void vu0Micro120(Vu0 &vu);
/* 0x1D0: vf1 = sin(vf1) per lane, x + c3 x^3 + c5 x^5 + c7 x^7 + c9 x^9
 * summed as the VU does ((((c3 x^3 + c5 x^5) + c7 x^7) + x) + c9 x^9);
 * arguments within [-pi/2, pi/2].  vf2..vf6 are left with the powers. */
void vu0Micro1D0(Vu0 &vu);
/* 0x2D0: vf4.x = atan(vf2.x / vf1.x) for 0 <= vf2.x <= vf1.x:
 * t = (b - a) / (b + a), atan = pi/4 + c1 t + c3 t^3 + ... + c15 t^15
 * (Sony's VU atan series).  Lanes y..w are computed alike. */
void vu0Micro2D0(Vu0 &vu);
/* 0x2C0 (no caller in GT3): vf4 = atan(vf1) for vf1 >= 0 through t = (x - 1)
 * / (x + 1). */
void vu0Micro2C0(Vu0 &vu);
/* The scalar polynomials the subroutines evaluate (lane by lane). */
float vu0SinPoly(float x);
float vu0AtanPoly(float t);

/* ======================================================================== */
/* The matrix library on the VU0 registers (the bodies of vu0_matrix_asm.c, */
/* vu0_current_matrix.c, pgl.c's matrix stack)                             */
/* ======================================================================== */

void vu0LoadIdentity(Vu0 &vu);                          /* vu0_load_identity_regs */
void vu0ResetRotation(Vu0 &vu);                         /* xyz of rows 0..2 = identity */
void vu0ResetTranslation(Vu0 &vu);                      /* vf31.xyz = 0 */
void vu0LoadMatrix(Vu0 &vu, const float *m);            /* pgl_load_matrix */
void vu0GetMatrix(const Vu0 &vu, float *m);             /* pgl_get_matrix */
void vu0MultMatrix(Vu0 &vu, const float *m);            /* pgl_mult_matrix: current = m x current */
void vu0MultMatrix90(Vu0 &vu, const float *m);          /* pgl_mult_matrix_90: current = current x m */
void vu0Rotate(Vu0 &vu, float deg, float x, float y, float z); /* pgl_rotate (glRotate) */
void vu0Translate(Vu0 &vu, float x, float y, float z);  /* pgl_translate: vf31 += x r0 + y r1 + z r2 */
void vu0Scale(Vu0 &vu, float x, float y, float z);      /* vu0_scale: rows 0..2 times x, y, z */
void vu0ScaleInvY(Vu0 &vu, float s);                    /* vu0_scale_inv_y / F_00257EF0: (1, 1/s, 1) */
void vu0RotateX(Vu0 &vu, float deg);                    /* M = Rx M */
void vu0RotateY(Vu0 &vu, float deg);
void vu0RotateZ(Vu0 &vu, float deg);
void vu0Transpose(Vu0 &vu);
void vu0InvertRigid(Vu0 &vu);                           /* R^T, t' = -t R^T (w of rows 0..2 -> 0) */
void vu0ApplyMatrix(Vu0 &vu, float *dst, const float *src); /* dst = src x current (vf1 = the result) */

/* pgl_matrix_mode's park / load and the matrix stacks (vi11..vi13). */
void vu0SwitchMatrixMode(Vu0 &vu, int oldMode, int newMode);
void vu0PushMatrix(Vu0 &vu, int mode);
void vu0PopMatrix(Vu0 &vu, int mode);
/* gfx_init_vu0_constants' VU0 part: identity in vf28..vf31 and parked for
 * all three modes, stack pointers 0x10 / 0x50 / 0x60. */
void vu0InitMatrices(Vu0 &vu);
/* pgl_sync_matrices' VU0 part: vf24..vf27 = modelview x projection by
 * mode (0: micro 0x120; 1: vf28..31 is the projection, times the parked
 * modelview; 2: parked projection times parked modelview), then the
 * scale of rows 0..2 when scaleEnable. */
void vu0SyncMatrices(Vu0 &vu, int matrixMode, bool scaleEnable, const Vec4 &scale);

/* The modelview / projection wherever they are (current or parked):
 * vu0_load_modelview / vu0_load_projection load them into vf20..vf23. */
void vu0GetModeMatrix(const Vu0 &vu, int matrixMode, int which, Vec4 out[4]);

/* ======================================================================== */
/* sin / cos / atan / random                                                */
/* ======================================================================== */

/* vu0_sincos_deg: vf1 = (sin deg, cos deg, 0, 0), reduced and evaluated as
 * the PS2 does (both lanes by micro 0x1D0).  Lanes z / w: the sign
 * extension words on the PS2 (garbage); 0 here. */
void vu0SinCosDeg(Vu0 &vu, float deg);
float pglSinDeg(Vu0 &vu, float deg);                    /* pgl_sin_deg (vf1.x) */
float pglCosDeg(Vu0 &vu, float deg);                    /* pgl_cos_deg (vf1.y) */
void pglSinCosDeg(Vu0 &vu, float deg, float *s, float *c);
/* vu0_atan2f: the axes answered directly, else micro 0x2D0 on (max, min)
 * of |x|, |y| and the octant tables. */
float vu0Atan2(Vu0 &vu, float y, float x);
/* vu0_rand: vrnext (the 23-bit LFSR, taps 22 and 4), vf1.x = the new R as
 * a float in [1, 2); returns it - 1. */
float vu0Rand(Vu0 &vu);
/* vu0_rand_seed's VU0 part: vrinit R, the mantissa bits of seed. */
void vu0RandSeed(Vu0 &vu, float seed);

/* pseudo_angle (pgl.c): octant-ratio angle in 45-degree units, -1 .. 7. */
float pseudoAngle(float a, float b);

/* ======================================================================== */
/* The game's own float library (engine/pdistd/fastmath.c)                 */
/* ======================================================================== */
/* GT3 links its own sinf / cosf / ... (no libm): polynomial and rational
 * approximations with their own range reductions.  These are the same
 * computations in the same order, under names that do not clash with the
 * CRT; the port routes the game's calls to them so that physics and camera
 * code keep the PS2 values (std::sin differs in the last bits and, for
 * atan2f, in its quirk: 0 whenever x + y == 0). */
namespace fm {
float sinf(float x);                    /* 0x0026A5F0 */
float cosf(float x);                    /* 0x0026A1A0 */
void  sincosf(float x, float *s, float *c);  /* 0x0026A4D0 */
float tanf(float x);                    /* 0x0026A6C0 */
float atanf(float x);                   /* 0x0026A088 */
float atan2f(float y, float x);         /* 0x00269FC8 (returns 0 when x + y == 0) */
float expf(float x);                    /* 0x0026A270 */
float logf(float x);                    /* 0x0026A388 */
float powf(float x, float y);           /* 0x0026A4A0 exp(y log x) */
}  /* namespace fm */

/* ======================================================================== */
/* Projection helpers of the immediate primitives                          */
/* ======================================================================== */

/* The viewport constants pgl keeps for VU1 (GsContext.viewScale, viewCenter,
 * limits, vBB0 = vf23, vf22, vf21, vf20 after vu0_load_screen_regs). */
struct Viewport {
    Vec4 scale;     /* (winW / 2, -winH / 2, (near - far) depthScale / 2, 255 - fog) */
    Vec4 center;    /* (x centre, y centre (12.4 / 16, with the draw offset), z centre, fog + 0.1) */
    Vec4 limits;    /* (FLT_MAX, FLT_MAX, FLT_MAX, fog start) */
    Vec4 bb0;       /* (0.5, 0.5, 1, 1) */
};
template <class Ctx> inline Viewport viewportOf(const Ctx &ctx)
{
    return Viewport{toVec4(ctx.viewScale), toVec4(ctx.viewCenter), toVec4(ctx.limits), toVec4(ctx.vBB0)};
}

/* A clip-space point through pgl's screen mapping (pgl_draw_line_3d, the
 * smoke particles): Q = 1 / w, p = min(p, limits) x Q, p = centre + p x
 * scale.  x, y: GS primitive coordinates in pixels (with the XYOFFSET base),
 * z: GS Z, w: the fog coefficient F = fog + 0.1 + (255 - fog) min(start, w) / w. */
Vec4 projectClip(const Vec4 &clip, const Viewport &vp);
/* The same with the GS's fixed-point conversion: vftoi4, x / y / z / f as
 * the PACKED XYZF2 register takes them (x, y 12.4 in 16 bits, z 24 bits, f 8
 * bits). */
struct GsXyzf { uint16_t x, y; uint32_t z; uint8_t f; bool adc; };
GsXyzf toXyzf2(const Vec4 &screen);

/* vu0_clip_code: the 6 judgement bits against |w|, swapped and inverted
 * for w < 0 (the signed planes x > w / x < -w). */
uint32_t clipCode(const Vec4 &v);
/* clip_line_homogeneous: Cohen-Sutherland clip of a - b in place; 0 =
 * (the clipped part) visible, else the OR of the codes (rejected). */
uint32_t clipLineHomogeneous(Vec4 &a, Vec4 &b);
/* vu0_clip_point: (x, y, z, 1) through the combined matrix vf24..vf27,
 * clipped against its own w (with the w < 0 remap); box_visible ANDs 8. */
uint32_t clipPointCombined(const Vu0 &vu, float x, float y, float z);
bool boxVisible(const Vu0 &vu, const float *lo, const float *hi);

/* ======================================================================== */
/* Environment map (lighting mode 3, vu1_lighting.c's VU0 math)            */
/* ======================================================================== */

struct EnvMap {
    Vec4  texMtx[4];        /* g_envmapTexMtx: sent by pgl_send_mode3 */
    Vec4  rot[3];           /* g_envmapRot: pgl_envmap_rotate's result */
    Vec4  basis[3];         /* g_envmapBasis: pgl_envmap_set_basis */
    Vec4  viewRot[3];       /* g_envmapViewRot */
    Vec4  eye;              /* g_envmapEye */
    float phase = 0.0f;     /* g_envmapPhase, -1 .. 0 turns */
    float paramA = 4.0f;    /* g_envmapParamA (pgl_set_envmap_params) */
    float paramB = 0.25f;   /* g_envmapParamB */
};
extern EnvMap g_envMap;

/* pgl_envmap_rotate: (as written) the basis' first row is used for all three
 * terms, v = row0 (x + y + z); phase = -(pseudo_angle(v.z, v.x) + 1) / 8. */
void envmapRotate(Vu0 &vu, EnvMap &em, float x, float y, float z);
void envmapPointAt(Vu0 &vu, EnvMap &em, float x, float y, float z);
void envmapSetBasis(Vu0 &vu, EnvMap &em, const Vec4 modelview[4],
                    float ax, float ay, float az, float bx, float by, float bz);
void envmapBuildMatrix(EnvMap &em, const Vec4 modelview[4]);    /* pgl_build_envmap_matrix */

/* The rigid inverse of vf20..vf23 (U115_VU_INVERT_VF20): rows 0..2
 * transposed (w = 0), row 3.xyz = -t R^T, row 3.w kept. */
void invertRigid(Vec4 m[4]);

/* ======================================================================== */
/* GsContext-level entry points (templates over the context type)          */
/* ======================================================================== */

/* The members these use, as G001_flare.h names them.  The test tool's
 * context; the port passes its GsContext. */
struct PglMathCtx {
    uint8_t  matrixDirty = 1;
    uint8_t  matrixMode = 0;
    uint8_t  vu1MatrixSent = 0;
    uint8_t  lightFlags = 0;
    uint8_t  scaleEnable = 0;
    uint8_t  halfHeight = 0;
    Vec4     scale{1.0f, 1.0f, 1.0f, 1.0f};
    Vec4     normalMtx[4]{};
    float    depthNear = 0.0f, depthFar = 1.0f, depthScale = 16776191.0f;
    float    depthOffset = 0.0f, depthBias = 0.0f;
    uint32_t spriteZF = 0;
    Vec4     viewScale{}, viewCenter{}, limits{}, vBB0{};
};

/* gfx_init_vu0_constants (0x002503E0), its VU0 and viewport part; the rest
 * of it is C the caller keeps (fogStart = 0, fogAmount = 1, g_pglAttribSp =
 * g_pglAttribStack). */
template <class Ctx> void ctxInitVu0Constants(Ctx &ctx, Vu0 &vu)
{
    vu0InitMatrices(vu);
    ctx.matrixDirty = 1;
    ctx.matrixMode = MTX_MODELVIEW;
    ctx.vu1MatrixSent = 0;
    const float fmax = u2f(0x7F7FFFFF);
    ctx.limits.x = fmax; ctx.limits.y = fmax; ctx.limits.z = fmax; ctx.limits.w = fmax;
    ctx.viewCenter.w = 255.0f;
    ctx.vBB0.x = 0.5f; ctx.vBB0.y = 0.5f; ctx.vBB0.z = 1.0f; ctx.vBB0.w = 1.0f;
}

/* pgl_matrix_mode (no matrixDirty: the combined matrix does not change). */
template <class Ctx> void ctxMatrixMode(Ctx &ctx, Vu0 &vu, int mode)
{
    if (ctx.matrixMode == mode)
        return;
    vu0SwitchMatrixMode(vu, ctx.matrixMode, mode);
    ctx.matrixMode = (uint8_t)mode;
}
template <class Ctx> void ctxPushMatrix(Ctx &ctx, Vu0 &vu) { vu0PushMatrix(vu, ctx.matrixMode); }
template <class Ctx> void ctxPopMatrix(Ctx &ctx, Vu0 &vu) { ctx.matrixDirty = 1; vu0PopMatrix(vu, ctx.matrixMode); }

/* The current-matrix routines: each marks the matrices changed. */
template <class Ctx> void ctxLoadIdentity(Ctx &ctx, Vu0 &vu) { vu0LoadIdentity(vu); ctx.matrixDirty = 1; }
template <class Ctx> void ctxResetRotation(Ctx &ctx, Vu0 &vu) { vu0ResetRotation(vu); ctx.matrixDirty = 1; }
template <class Ctx> void ctxResetTranslation(Ctx &ctx, Vu0 &vu) { vu0ResetTranslation(vu); ctx.matrixDirty = 1; }
template <class Ctx> void ctxLoadMatrix(Ctx &ctx, Vu0 &vu, const float *m) { ctx.matrixDirty = 1; vu0LoadMatrix(vu, m); }
template <class Ctx> void ctxMultMatrix(Ctx &ctx, Vu0 &vu, const float *m) { vu0MultMatrix(vu, m); ctx.matrixDirty = 1; }
template <class Ctx> void ctxMultMatrix90(Ctx &ctx, Vu0 &vu, const float *m) { vu0MultMatrix90(vu, m); ctx.matrixDirty = 1; }
template <class Ctx> void ctxRotate(Ctx &ctx, Vu0 &vu, float deg, float x, float y, float z)
{
    vu0Rotate(vu, deg, x, y, z);
    ctx.matrixDirty = 1;
}
template <class Ctx> void ctxTranslate(Ctx &ctx, Vu0 &vu, float x, float y, float z)
{
    ctx.matrixDirty = 1;
    vu0Translate(vu, x, y, z);
}
template <class Ctx> void ctxScale(Ctx &ctx, Vu0 &vu, float x, float y, float z) { ctx.matrixDirty = 1; vu0Scale(vu, x, y, z); }
template <class Ctx> void ctxScaleInvY(Ctx &ctx, Vu0 &vu, float s) { ctx.matrixDirty = 1; vu0ScaleInvY(vu, s); }
template <class Ctx> void ctxRotateX(Ctx &ctx, Vu0 &vu, float deg) { ctx.matrixDirty = 1; vu0RotateX(vu, deg); }
template <class Ctx> void ctxRotateY(Ctx &ctx, Vu0 &vu, float deg) { ctx.matrixDirty = 1; vu0RotateY(vu, deg); }
template <class Ctx> void ctxRotateZ(Ctx &ctx, Vu0 &vu, float deg) { ctx.matrixDirty = 1; vu0RotateZ(vu, deg); }
template <class Ctx> void ctxTranspose(Ctx &ctx, Vu0 &vu) { ctx.matrixDirty = 1; vu0Transpose(vu); }
template <class Ctx> void ctxInvertRigid(Ctx &ctx, Vu0 &vu) { ctx.matrixDirty = 1; vu0InvertRigid(vu); }

/* pgl_set_scale: scale.xyz (w kept), matrixDirty. */
template <class Ctx> void ctxSetScale(Ctx &ctx, float x, float y, float z)
{
    ctx.scale.x = x; ctx.scale.y = y; ctx.scale.z = z;
    ctx.matrixDirty = 1;
}
/* pgl_set_normal_matrix: normalMtx = m (16 floats).  The retail copy
 * (mat44_copy) garbles rows 1..3 of an unaligned source and one dword of
 * an unaligned destination; the port's callers pass aligned matrices, so
 * this is the plain copy. */
template <class Ctx> void ctxSetNormalMatrix(Ctx &ctx, const float *m)
{
    for (int r = 0; r < 4; r++) {
        ctx.normalMtx[r].x = m[r * 4 + 0];
        ctx.normalMtx[r].y = m[r * 4 + 1];
        ctx.normalMtx[r].z = m[r * 4 + 2];
        ctx.normalMtx[r].w = m[r * 4 + 3];
    }
}

/* pgl_sync_matrices: vf24..vf27 = the combined matrix; matrixDirty = 0, the
 * matrix due for VU1 again, the eye-space lights out of date. */
template <class Ctx> void ctxSyncMatrices(Ctx &ctx, Vu0 &vu)
{
    vu0SyncMatrices(vu, ctx.matrixMode, ctx.scaleEnable != 0, toVec4(ctx.scale));
    ctx.matrixDirty = 0;
    ctx.vu1MatrixSent = 0;
    ctx.lightFlags |= LF_MODELVIEW;
}
/* vu0_sync_state */
template <class Ctx> void ctxSyncState(Ctx &ctx, Vu0 &vu)
{
    if (ctx.matrixDirty)
        ctxSyncMatrices(ctx, vu);
}
/* vu0_load_screen_regs (sync, then vf20..vf23 = vBB0, limits, viewCenter,
 * viewScale) and its _nosync form. */
template <class Ctx> void ctxLoadScreenRegsNoSync(const Ctx &ctx, Vu0 &vu)
{
    vu.vf[23] = toVec4(ctx.viewScale);
    vu.vf[22] = toVec4(ctx.viewCenter);
    vu.vf[21] = toVec4(ctx.limits);
    vu.vf[20] = toVec4(ctx.vBB0);
}
template <class Ctx> void ctxLoadScreenRegs(Ctx &ctx, Vu0 &vu)
{
    ctxSyncState(ctx, vu);
    ctxLoadScreenRegsNoSync(ctx, vu);
}
/* vu0_load_modelview / vu0_load_projection: vf20..vf23 = that matrix. */
template <class Ctx> void ctxLoadModelview(const Ctx &ctx, Vu0 &vu) { vu0GetModeMatrix(vu, ctx.matrixMode, MTX_MODELVIEW, &vu.vf[20]); }
template <class Ctx> void ctxLoadProjection(const Ctx &ctx, Vu0 &vu) { vu0GetModeMatrix(vu, ctx.matrixMode, MTX_PROJECTION, &vu.vf[20]); }

/* pgl_set_sprite_depth's computation: the eye-space distance z through the
 * projection, divided, z clamped to [-1, 1], through the viewport, ftoi0;
 * returns spriteZF = F << 24 | Z (F = 0xFF when z <= 0).  vf1..vf3 and
 * vf20..vf23 are left as on the PS2. */
uint32_t spriteDepthZF(Vu0 &vu, const Vec4 projection[4], const Viewport &vp, float z);
template <class Ctx> void ctxSetSpriteDepth(Ctx &ctx, Vu0 &vu, float z)
{
    Vec4 p[4];
    vu0GetModeMatrix(vu, ctx.matrixMode, MTX_PROJECTION, p);
    ctx.spriteZF = spriteDepthZF(vu, p, viewportOf(ctx), z);
}

/* gs_set_depth_bias_at's computation: (x, y, z) and (x, y, z + dz) through
 * modelview and projection and divided by their own w; the bias is the
 * difference of the w lanes after the divide (as the binary does: ~0, the
 * z lanes were probably meant). */
float depthBiasAt(const Vec4 modelview[4], const Vec4 projection[4], float dz, float x, float y, float z);
/* gs_update_depth_range (plain C in the decomp, repeated for the template
 * users): the z lanes of the viewport. */
template <class Ctx> void ctxUpdateDepthRange(Ctx &ctx)
{
    float far = ctx.depthFar, near = ctx.depthNear;
    float mid = (near + far) * 0.5f;
    float offset = ctx.depthOffset + 1.0f;
    float scale = ctx.depthScale;
    ctx.vu1MatrixSent = 0;
    ctx.viewScale.z = (near - far) * scale * 0.5f;
    ctx.viewCenter.z = ((1.0f - mid) + ctx.depthBias) * scale + offset;
}
template <class Ctx> void ctxSetDepthBiasAt(Ctx &ctx, Vu0 &vu, float dz, float x, float y, float z)
{
    Vec4 mv[4], p[4];
    vu0GetModeMatrix(vu, ctx.matrixMode, MTX_MODELVIEW, mv);
    vu0GetModeMatrix(vu, ctx.matrixMode, MTX_PROJECTION, p);
    ctx.depthBias = depthBiasAt(mv, p, dz, x, y, z);
    ctxUpdateDepthRange(ctx);
}

/* gs_set_light_param_v's vectors (PGL_LIGHT_POSITION: normalised when w ==
 * 0; PGL_LIGHT_SPOT_DIRECTION: always): times the modelview, w included. */
Vec4 lightVectorToEye(const Vec4 modelview[4], const Vec4 &v, bool normalize);
/* pgl_xform_texcoords: (s, t, 0, 1) times the texture matrix, x and y kept. */
void xformTexcoords(const Vec4 texture[4], float *dstST, const float *srcST, int n);

}  /* namespace pgl */

#endif /* PGL_NATIVE_PGL_MATH_H */
