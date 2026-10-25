/* vu0_game.h - the VU0 macro-mode, EE FPU and MMI operations of the game's
 * inline assembly, as plain C++ (GT3 PC).
 *
 * Outside pgl, the game and two engine files reached into VU0 themselves
 * with macro-mode asm: the rigid bodies (T167) keep the body matrix in
 * vf20..vf27 between functions, the flares (T159), the smoke (T173), the
 * car and tyre models (T180, T447, race_car_model_internal.h,
 * tire_model_internal.h) and the model sets (T451, T454) transform with the
 * current matrix vf28..vf31 or the combined one vf24..vf27 and project with
 * the screen registers vf20..vf23 that vu0_load_screen_regs loads; the
 * memory card icons (T077) morph their shapes with MMI multiply-adds.  The
 * port computes the same values in the same order on pgl's VU0 register
 * file (pgl::g_vu0, pgl_math.h; docs/pgl.md section 9 lists the patterns,
 * docs/asm.md each rewritten block):
 *
 *   - registers that live beyond one asm block (vf20..vf31, and any other
 *     the code reads after a call or in a later block) are pgl::g_vu0's;
 *     scratch registers used inside one block are locals;
 *   - ACC chains are evaluated left to right as the VU did (pgl::vu::xform
 *     and friends), divisions and square roots with the VU / EE FPU results
 *     for 0 (+-max float, not infinity);
 *   - MMI arithmetic is exact integer arithmetic with the lanes' wrap-around.
 *
 * Plain C++ (no Windows, no game header): the test tool tools/vu0game uses
 * it alone.  Game files include it under pc_prelude.h's C library macros;
 * nothing here uses those names.
 */
#ifndef PGL_NATIVE_VU0_GAME_H
#define PGL_NATIVE_VU0_GAME_H

#include <cstdint>
#include <cstring>

#include "engine/pgl/native/pgl_math.h"

namespace vu0g {

using pgl::Vec4;

/* ======================================================================== */
/* Registers and quadword moves                                             */
/* ======================================================================== */

/* vfN of the port's VU0. */
inline Vec4 &vf(int n) { return pgl::g_vu0.vf[n]; }

/* lqc2 / sqc2, and the original's unaligned moves through the 128-bit GPRs
 * (ldl / ldr / lwl / lwr + pcpyld + qmtc2.ni in, qmfc2.ni + pcpyud + sdl /
 * sdr out): 16 bytes, any alignment, bit patterns kept. */
inline Vec4 load(const void *p)
{
    Vec4 v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
inline void store(void *p, const Vec4 &v) { std::memcpy(p, &v, sizeof v); }

/* A 12-byte operand moved as the original did (ldl / ldr x, y; lwl / lwr z
 * sign-extended into 64 bits; pcpyld): w is the sign extension of z, 0 or
 * 0xFFFFFFFF (never used as a number by the game's code). */
inline Vec4 load3(const float *p)
{
    Vec4 v;
    std::memcpy(&v, p, 12);
    uint32_t z;
    std::memcpy(&z, &v.z, 4);
    uint32_t w = (uint32_t)((int32_t)z >> 31);
    std::memcpy(&v.w, &w, 4);
    return v;
}

/* ======================================================================== */
/* Transforms and projection                                               */
/* ======================================================================== */

/* vmulax.xyzw ACC, M0, vx; vmadday ACC, M1, vy; vmaddaz ACC, M2, vz;
 * vmaddw d, M3, vw (w from v) / vf0w (w = 1); vmaddz d, M2, vz (rows 0..2). */
inline Vec4 xform(const Vec4 *m, const Vec4 &v) { return pgl::vu::xform(m, v); }
inline Vec4 xformW1(const Vec4 *m, const Vec4 &v) { return pgl::vu::xformW1(m, v); }
inline Vec4 xform3(const Vec4 *m, const Vec4 &v) { return pgl::vu::xform3(m, v); }

/* The perspective half of the screen projection (race_car_model's
 * vu0_project, RaceSmoke's vertices): vf24..vf27 x (p.xyz, 1); Q = 1 / w
 * (vdiv Q, vf0w, vNw: the w of the product, before the clamp); min(v, vf21)
 * (the limits of vu0_load_screen_regs); v x Q. */
inline Vec4 projectCombined(const Vec4 &p)
{
    Vec4 v = pgl::vu::xformW1(&pgl::g_vu0.vf[24], p);
    float q = pgl::vu::div(1.0f, v.w);
    pgl::g_vu0.q = q;
    v = pgl::vu::mini(v, pgl::g_vu0.vf[21]);
    return pgl::vu::scale(v, q);
}

/* The viewport half: vmulaw.xyzw ACC, vf22, vf0w; vmadd.xyzw d, v, vf23
 * (viewCenter + v x viewScale). */
inline Vec4 viewport(const Vec4 &v) { return pgl::vu::madd(pgl::g_vu0.vf[22], v, pgl::g_vu0.vf[23]); }

/* RaceSmoke's vertex (u058_project_vertex): both halves, then vftoi4.xyzw:
 * x, y in 12.4 GS coordinates, z and the fog value with 4 fraction bits. */
inline void projectScreenFixed(int32_t out[4], const Vec4 &p)
{
    Vec4 v = viewport(projectCombined(p));
    out[0] = pgl::vu::ftoi4(v.x);
    out[1] = pgl::vu::ftoi4(v.y);
    out[2] = pgl::vu::ftoi4(v.z);
    out[3] = pgl::vu::ftoi4(v.w);
}

/* ======================================================================== */
/* Clipping                                                                 */
/* ======================================================================== */

/* vclipw.xyz v, v.w (the .word 0x4BE109FF / 0x4BE211FF forms too), then
 * cfc2 r, vi18: the clip flag register with this judgement in bits 0..5 and
 * the three before it above. */
inline uint32_t clipw(const Vec4 &v)
{
    pgl::vu::clipw(pgl::g_vu0, v, v.w);
    return pgl::g_vu0.clip;
}

/* The w lane as the game tests it: the float's bits as an s32 (w < 0, -0.0
 * included). */
inline int32_t wBits(const Vec4 &v)
{
    int32_t i;
    std::memcpy(&i, &v.w, 4);
    return i;
}

/* ======================================================================== */
/* Matrices on the register file                                            */
/* ======================================================================== */

/* T167's vu0_transpose_vf20_to_vf24: vf24..vf27 = the transpose of
 * vf20..vf23 (w lanes included). */
inline void transposeVf20ToVf24()
{
    pgl::Vu0 &vu = pgl::g_vu0;
    Vec4 t[4];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            t[i][j] = vu.vf[20 + j][i];
    for (int i = 0; i < 4; i++)
        vu.vf[24 + i] = t[i];
}

/* SpriteModel's billboard axis (spriteset_begin_draw, after
 * vu0_load_modelview put the modelview into vf20..vf23):
 *   vf5 = (0, 0, 1, 0) (vmr32 of vf0, set before the load), vf6 = (0, 1, 0, 0)
 *   (vmr32 of vf5); vf20..vf23 = the rigid inverse (vsub.xyz vf23 = -t, the
 *   3x3 transposed through pextlw / pextuw / pcpyld / pcpyud with w = 0,
 *   vf23.xyz = -t R^T by vmulax / vmadday / vmaddz.xyz); vf5 = vf5 through
 *   vf20..vf22 (vmaddz.xyzw: the camera's z axis in model space); vf7.xyz =
 *   vf6 x vf5 (vopmula / vopmsub; vf7.w kept).  Returns vf7, which the
 *   caller stores whole (its w is whatever vf7 held). */
inline Vec4 billboardAxis()
{
    pgl::Vu0 &vu = pgl::g_vu0;
    Vec4 vf5 = vu.vf[5];
    Vec4 vf6 = {vf5.y, vf5.z, vf5.w, vf5.x};            /* vmr32.xyzw vf6, vf5 */
    pgl::invertRigid(&vu.vf[20]);
    vf5 = pgl::vu::xform3(&vu.vf[20], vf5);             /* vmulax / vmadday / vmaddz.xyzw */
    vu.vf[5] = vf5;
    vu.vf[6] = vf6;
    Vec4 c = pgl::vu::cross(vf6, vf5);                  /* vopmula.xyz ACC, vf6, vf5; vopmsub.xyz vf7, vf5, vf6 */
    vu.vf[7].x = c.x;
    vu.vf[7].y = c.y;
    vu.vf[7].z = c.z;
    return vu.vf[7];
}

/* ======================================================================== */
/* EE FPU                                                                   */
/* ======================================================================== */

/* rsqrt.s fd, fs, ft: fs / sqrt(|ft|), +-max float for ft = 0 (the EE FPU
 * has no infinity). */
inline float eeRsqrt(float num, float sq) { return pgl::vu::rsqrt(num, sq); }

/* ======================================================================== */
/* MMI (T077, the memory card icons)                                        */
/* ======================================================================== */

/* pcpyh d, x: the low halfword of x in the four halfwords of the low
 * doubleword (and of the high one, which the game never reads). */
inline uint64_t pcpyh(int32_t x) { return 0x0001000100010001ull * (uint16_t)x; }

/* pmulth d, a0, b0; pmaddh d, a1, b1; ...; pmflo lo over the low doublewords
 * of n operand pairs (halfwords 0..3: x, y, z, w).  Per halfword lane i the
 * signed 16 x 16 products are summed in 32 bits (wrapping) in LO.w0 (lane
 * 0), LO.w1 (1), HI.w0 (2), HI.w1 (3); d's low doubleword is (LO.w0,
 * HI.w0), pmflo's (LO.w0, LO.w1).  Lanes 4..7 (the high doublewords) are
 * not read by the game and not computed. */
inline void pmaddhChain(const uint64_t *a, const uint64_t *b, int n, uint64_t *d, uint64_t *lo)
{
    uint32_t sum[4] = {0, 0, 0, 0};
    for (int k = 0; k < n; k++) {
        for (int lane = 0; lane < 4; lane++) {
            int32_t x = (int16_t)(uint16_t)(a[k] >> (16 * lane));
            int32_t y = (int16_t)(uint16_t)(b[k] >> (16 * lane));
            sum[lane] += (uint32_t)(x * y);             /* |x y| <= 2^30: no int overflow */
        }
    }
    *d = (uint64_t)sum[0] | ((uint64_t)sum[2] << 32);
    *lo = (uint64_t)sum[0] | ((uint64_t)sum[1] << 32);
}

}  /* namespace vu0g */

#endif /* PGL_NATIVE_VU0_GAME_H */
