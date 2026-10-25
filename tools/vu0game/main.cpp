/* vu0game - tests of the game's rewritten VU0 / EE / MMI assembly
 * (src/engine/pgl/native/vu0_game.h, include/pc_ee.h; docs/asm.md).
 *
 * Each rewritten block is run twice on the same random inputs: once by the
 * port's C++ (the vu0g helpers on pgl::g_vu0), once by a reference that
 * executes the original instruction sequence literally - one function per
 * VU0 / MMI instruction, lane by lane, with its own register file (VF, ACC,
 * Q, the clip flags, 128-bit GPRs, HI / LO).  The two must agree bit for
 * bit: the order of the operations is the same, so any difference is a
 * wrong register, lane, mask or operand order in the port.  The float to
 * unsigned conversion is checked against fp-bit's fptoui algorithm (decomp
 * src/runtime/libgcc/fp_bit.c), the icon morph also against the formula of
 * docs/pgl.md 9.
 *
 *   vu0game        run everything, print failures and a summary
 */
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "engine/pgl/native/vu0_game.h"
#include "pc_ee.h"

using pgl::Vec4;

static int g_checks = 0, g_failures = 0;

#define CHECK(cond, ...) do {                                               \
        g_checks++;                                                         \
        if (!(cond)) {                                                      \
            if (g_failures < 50) {                                          \
                std::printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
                std::printf(__VA_ARGS__);                                   \
                std::printf("\n");                                          \
            }                                                               \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

static uint32_t bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
static float fromBits(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }
static bool sameBits(const Vec4 &a, const float *b)
{
    return bits(a.x) == bits(b[0]) && bits(a.y) == bits(b[1]) && bits(a.z) == bits(b[2]) && bits(a.w) == bits(b[3]);
}
static bool sameBits(const Vec4 &a, const Vec4 &b)
{
    return bits(a.x) == bits(b.x) && bits(a.y) == bits(b.y) && bits(a.z) == bits(b.z) && bits(a.w) == bits(b.w);
}

/* A small LCG: reproducible inputs. */
static uint32_t g_seed = 12345;
static uint32_t rnd() { g_seed = g_seed * 1664525u + 1013904223u; return g_seed; }
static float rndf(float lo, float hi) { return lo + (hi - lo) * (float)(rnd() >> 8) * (1.0f / 16777216.0f); }

/* ======================================================================== */
/* The reference: VU0 macro mode and MMI, one instruction at a time         */
/* ======================================================================== */

static const float kMax = 3.40282347e+38f;
enum { X = 1, Y = 2, Z = 4, W = 8, XYZ = 7, XZ = 5, XYZW = 15 };

struct Ref {
    float    vf[32][4];
    float    acc[4];
    float    q;
    uint32_t clip;
    uint32_t gpr[32][4];            /* 128-bit GPRs as words */
    uint32_t lo[4], hi[4];
};

static void refReset(Ref &r)
{
    std::memset(&r, 0, sizeof r);
    r.vf[0][3] = 1.0f;
}
/* the reference VU state from / to the port's */
static void refFromVu(Ref &r)
{
    for (int i = 0; i < 32; i++)
        for (int k = 0; k < 4; k++)
            r.vf[i][k] = pgl::g_vu0.vf[i][k];
    r.clip = pgl::g_vu0.clip;
}
static void refLoad(Ref &r, int n, const void *p) { std::memcpy(r.vf[n], p, 16); }          /* lqc2 */
static void refStore(const Ref &r, int n, void *p) { std::memcpy(p, r.vf[n], 16); }        /* sqc2 */

/* vmulaBC.mask ACC, fs, ftBC */
static void vmulabc(Ref &r, int mask, int fs, int ft, int bc)
{
    for (int k = 0; k < 4; k++)
        if (mask & (1 << k)) r.acc[k] = r.vf[fs][k] * r.vf[ft][bc];
}
static void vmaddabc(Ref &r, int mask, int fs, int ft, int bc)
{
    for (int k = 0; k < 4; k++)
        if (mask & (1 << k)) r.acc[k] = r.acc[k] + r.vf[fs][k] * r.vf[ft][bc];
}
static void vmaddbc(Ref &r, int mask, int fd, int fs, int ft, int bc)
{
    float t[4];
    for (int k = 0; k < 4; k++)
        t[k] = r.acc[k] + r.vf[fs][k] * r.vf[ft][bc];
    for (int k = 0; k < 4; k++)
        if (mask & (1 << k)) r.vf[fd][k] = t[k];
}
static void vmul(Ref &r, int mask, int fd, int fs, int ft)
{
    float t[4];
    for (int k = 0; k < 4; k++)
        t[k] = r.vf[fs][k] * r.vf[ft][k];
    for (int k = 0; k < 4; k++)
        if (mask & (1 << k)) r.vf[fd][k] = t[k];
}
static void vmadd(Ref &r, int mask, int fd, int fs, int ft)
{
    float t[4];
    for (int k = 0; k < 4; k++)
        t[k] = r.acc[k] + r.vf[fs][k] * r.vf[ft][k];
    for (int k = 0; k < 4; k++)
        if (mask & (1 << k)) r.vf[fd][k] = t[k];
}
static void vsubbc(Ref &r, int mask, int fd, int fs, int ft, int bc)
{
    float t[4];
    for (int k = 0; k < 4; k++)
        t[k] = r.vf[fs][k] - r.vf[ft][bc];
    for (int k = 0; k < 4; k++)
        if (mask & (1 << k)) r.vf[fd][k] = t[k];
}
static void vsub(Ref &r, int mask, int fd, int fs, int ft)
{
    float t[4];
    for (int k = 0; k < 4; k++)
        t[k] = r.vf[fs][k] - r.vf[ft][k];
    for (int k = 0; k < 4; k++)
        if (mask & (1 << k)) r.vf[fd][k] = t[k];
}
static void vmove(Ref &r, int fd, int fs) { std::memcpy(r.vf[fd], r.vf[fs], 16); }
static void vmr32(Ref &r, int fd, int fs)
{
    float t[4] = {r.vf[fs][1], r.vf[fs][2], r.vf[fs][3], r.vf[fs][0]};
    std::memcpy(r.vf[fd], t, 16);
}
/* vdiv Q, fsF, ftF: +-max for a zero divisor */
static void vdiv(Ref &r, int fs, int fsf, int ft, int ftf)
{
    float n = r.vf[fs][fsf], d = r.vf[ft][ftf];
    if (d == 0.0f)
        r.q = ((bits(n) ^ bits(d)) >> 31) ? -kMax : kMax;
    else
        r.q = n / d;
}
static void vmini(Ref &r, int fd, int fs, int ft)
{
    for (int k = 0; k < 4; k++)
        r.vf[fd][k] = r.vf[fs][k] < r.vf[ft][k] ? r.vf[fs][k] : r.vf[ft][k];
}
static void vmulq(Ref &r, int fd, int fs)
{
    for (int k = 0; k < 4; k++)
        r.vf[fd][k] = r.vf[fs][k] * r.q;
}
/* vftoi4: x 16, truncated, saturated; the int's bits in the lane */
static void vftoi4(Ref &r, int fd, int fs)
{
    for (int k = 0; k < 4; k++) {
        double v = (double)r.vf[fs][k] * 16.0;
        int32_t i;
        if (v != v) i = 0;                  /* (no NaN on the VU; the port's convention) */
        else if (v >= 2147483647.0) i = 0x7FFFFFFF;
        else if (v <= -2147483648.0) i = (int32_t)0x80000000u;
        else i = (int32_t)std::trunc(v);
        r.vf[fd][k] = fromBits((uint32_t)i);
    }
}
/* vclipw.xyz fs, ftw: +x -x +y -y +z -z against |w| into bits 0..5, the
 * older judgements shifted up (24 bits kept) */
static void vclipw(Ref &r, int fs, int ft)
{
    float w = std::fabs(r.vf[ft][3]);
    uint32_t f = 0;
    for (int k = 0; k < 3; k++) {
        if (r.vf[fs][k] > w) f |= 1u << (2 * k);
        if (r.vf[fs][k] < -w) f |= 2u << (2 * k);
    }
    r.clip = ((r.clip << 6) | f) & 0xFFFFFF;
}
/* vopmula.xyz ACC, fs, ft / vopmsub.xyz fd, fs, ft */
static void vopmula(Ref &r, int fs, int ft)
{
    r.acc[0] = r.vf[fs][1] * r.vf[ft][2];
    r.acc[1] = r.vf[fs][2] * r.vf[ft][0];
    r.acc[2] = r.vf[fs][0] * r.vf[ft][1];
}
static void vopmsub(Ref &r, int fd, int fs, int ft)
{
    float t[3] = {r.acc[0] - r.vf[fs][1] * r.vf[ft][2],
                  r.acc[1] - r.vf[fs][2] * r.vf[ft][0],
                  r.acc[2] - r.vf[fs][0] * r.vf[ft][1]};
    r.vf[fd][0] = t[0];
    r.vf[fd][1] = t[1];
    r.vf[fd][2] = t[2];
}
/* qmfc2 / qmtc2 and the MMI word moves (EE Core Instruction Set) */
static void qmfc2(Ref &r, int rt, int vf) { std::memcpy(r.gpr[rt], r.vf[vf], 16); }
static void qmtc2(Ref &r, int rt, int vf) { std::memcpy(r.vf[vf], r.gpr[rt], 16); }
static void pextlw(Ref &r, int rd, int rs, int rt)
{
    uint32_t t[4] = {r.gpr[rt][0], r.gpr[rs][0], r.gpr[rt][1], r.gpr[rs][1]};
    std::memcpy(r.gpr[rd], t, 16);
}
static void pextuw(Ref &r, int rd, int rs, int rt)
{
    uint32_t t[4] = {r.gpr[rt][2], r.gpr[rs][2], r.gpr[rt][3], r.gpr[rs][3]};
    std::memcpy(r.gpr[rd], t, 16);
}
static void pcpyld(Ref &r, int rd, int rs, int rt)
{
    uint32_t t[4] = {r.gpr[rt][0], r.gpr[rt][1], r.gpr[rs][0], r.gpr[rs][1]};
    std::memcpy(r.gpr[rd], t, 16);
}
static void pcpyud(Ref &r, int rd, int rs, int rt)
{
    uint32_t t[4] = {r.gpr[rs][2], r.gpr[rs][3], r.gpr[rt][2], r.gpr[rt][3]};
    std::memcpy(r.gpr[rd], t, 16);
}
static int16_t half(const uint32_t *q, int i) { return (int16_t)(uint16_t)(q[i >> 1] >> (16 * (i & 1))); }
/* pmulth / pmaddh rd, rs, rt: the 8 halfword products into LO / HI as the
 * manual orders them; rd = (LO.w0, HI.w0, LO.w2, HI.w2) */
static void pmaddhImpl(Ref &r, int rd, int rs, int rt, bool add)
{
    uint32_t p[8];
    for (int i = 0; i < 8; i++)
        p[i] = (uint32_t)((int32_t)half(r.gpr[rs], i) * (int32_t)half(r.gpr[rt], i));
    uint32_t *dst[8] = {&r.lo[0], &r.lo[1], &r.hi[0], &r.hi[1], &r.lo[2], &r.lo[3], &r.hi[2], &r.hi[3]};
    for (int i = 0; i < 8; i++)
        *dst[i] = add ? *dst[i] + p[i] : p[i];
    uint32_t t[4] = {r.lo[0], r.hi[0], r.lo[2], r.hi[2]};
    std::memcpy(r.gpr[rd], t, 16);
}
static void pmulth(Ref &r, int rd, int rs, int rt) { pmaddhImpl(r, rd, rs, rt, false); }
static void pmaddh(Ref &r, int rd, int rs, int rt) { pmaddhImpl(r, rd, rs, rt, true); }
static void pmflo(Ref &r, int rd) { std::memcpy(r.gpr[rd], r.lo, 16); }
/* pcpyh rd, rt: halfword 0 into 0..3, halfword 4 into 4..7 */
static void pcpyh(Ref &r, int rd, int rt)
{
    uint32_t lo = r.gpr[rt][0] & 0xFFFF, hi = r.gpr[rt][2] & 0xFFFF;
    uint32_t t[4] = {lo | lo << 16, lo | lo << 16, hi | hi << 16, hi | hi << 16};
    std::memcpy(r.gpr[rd], t, 16);
}
static uint64_t gpr64(const Ref &r, int n) { return (uint64_t)r.gpr[n][0] | ((uint64_t)r.gpr[n][1] << 32); }
static void setGpr64(Ref &r, int n, uint64_t v, uint64_t upper)
{
    r.gpr[n][0] = (uint32_t)v;
    r.gpr[n][1] = (uint32_t)(v >> 32);
    r.gpr[n][2] = (uint32_t)upper;          /* the upper doubleword: whatever the register held */
    r.gpr[n][3] = (uint32_t)(upper >> 32);
}

/* ======================================================================== */
/* Inputs                                                                   */
/* ======================================================================== */

static Vec4 rndVec(float lo, float hi) { return Vec4{rndf(lo, hi), rndf(lo, hi), rndf(lo, hi), rndf(lo, hi)}; }

/* A random port VU0 state: every register random, vf0 = (0, 0, 0, 1),
 * vf21 the limits with some lanes FLT_MAX and some small (vmini must bite),
 * a random clip register. */
static void randomVu()
{
    pgl::vu0Reset(pgl::g_vu0);
    for (int i = 1; i < 32; i++)
        pgl::g_vu0.vf[i] = rndVec(-4.0f, 4.0f);
    Vec4 &lim = pgl::g_vu0.vf[21];
    for (int k = 0; k < 4; k++)
        lim[k] = (rnd() & 1) ? kMax : rndf(-2.0f, 3.0f);
    pgl::g_vu0.clip = rnd() & 0xFFFFFF;
}

/* a perspective-like combined matrix in vf24..vf27 (w from -z) so that the
 * projection divides by a sensible w, sometimes 0 */
static void perspectiveVf24(bool zeroW)
{
    for (int r = 0; r < 4; r++)
        pgl::g_vu0.vf[24 + r] = rndVec(-2.0f, 2.0f);
    pgl::g_vu0.vf[24].w = 0.0f;
    pgl::g_vu0.vf[25].w = 0.0f;
    pgl::g_vu0.vf[26].w = -1.0f;
    pgl::g_vu0.vf[27].w = zeroW ? 0.0f : rndf(0.5f, 3.0f);
}

/* ======================================================================== */
/* Tests                                                                    */
/* ======================================================================== */

/* race_car_model_internal.h vu0_project: lqc2 vf16; vmulax / vmadday /
 * vmaddaz / vmaddw vf0w through vf24..vf27; vdiv Q, vf0w, vf16w; vmini
 * vf21; vmulq.  RaceSmoke's vertex (T173): the same + vmulaw / vmadd vf22,
 * vf23 + vftoi4. */
static void testProjection()
{
    for (int n = 0; n < 2000; n++) {
        randomVu();
        perspectiveVf24(n % 50 == 0);
        if (n % 3 == 0)
            pgl::g_vu0.vf[21] = Vec4{kMax, kMax, kMax, kMax};
        Vec4 p = rndVec(-10.0f, 10.0f);
        p.z = -std::fabs(p.z) - 0.1f;

        Ref r;
        refReset(r);
        refFromVu(r);
        refLoad(r, 16, &p);
        vmulabc(r, XYZW, 24, 16, 0);
        vmaddabc(r, XYZW, 25, 16, 1);
        vmaddabc(r, XYZW, 26, 16, 2);
        vmaddbc(r, XYZW, 16, 27, 0, 3);
        vdiv(r, 0, 3, 16, 3);
        vmini(r, 16, 16, 21);
        vmulq(r, 16, 16);
        Vec4 got = vu0g::projectCombined(p);
        CHECK(sameBits(got, r.vf[16]), "vu0_project %d: (%g %g %g %g) vs (%g %g %g %g)", n,
              got.x, got.y, got.z, got.w, r.vf[16][0], r.vf[16][1], r.vf[16][2], r.vf[16][3]);

        /* RaceSmoke: the input w is 0 and not read (vmaddw takes vf0w) */
        Vec4 ps = p;
        ps.w = 0.0f;
        refReset(r);
        refFromVu(r);
        refLoad(r, 1, &ps);
        vmulabc(r, XYZW, 24, 1, 0);
        vmaddabc(r, XYZW, 25, 1, 1);
        vmaddabc(r, XYZW, 26, 1, 2);
        vmaddbc(r, XYZW, 1, 27, 0, 3);
        vdiv(r, 0, 3, 1, 3);
        vmini(r, 1, 1, 21);
        vmulq(r, 1, 1);
        vmulabc(r, XYZW, 22, 0, 3);
        vmadd(r, XYZW, 1, 1, 23);
        vftoi4(r, 1, 1);
        int32_t out[4];
        vu0g::projectScreenFixed(out, ps);
        CHECK(bits(r.vf[1][0]) == (uint32_t)out[0] && bits(r.vf[1][1]) == (uint32_t)out[1] &&
              bits(r.vf[1][2]) == (uint32_t)out[2] && bits(r.vf[1][3]) == (uint32_t)out[3],
              "smoke vertex %d: %d %d %d %d vs %d %d %d %d", n, out[0], out[1], out[2], out[3],
              (int)bits(r.vf[1][0]), (int)bits(r.vf[1][1]), (int)bits(r.vf[1][2]), (int)bits(r.vf[1][3]));
    }
    /* w = 0: Q = +max (the VU's), no infinity */
    randomVu();
    perspectiveVf24(true);
    pgl::g_vu0.vf[21] = Vec4{kMax, kMax, kMax, kMax};
    Vec4 got = vu0g::projectCombined(Vec4{0.0f, 0.0f, 0.0f, 1.0f});
    CHECK(pgl::g_vu0.q == kMax, "Q for w 0: %g", pgl::g_vu0.q);
    (void)got;
}

/* T447 TireModelBase::clipTest and T451 command 0x0A: vf1 = vf24..vf27 x
 * (p, 1); vf2 = vf1 * vf20; vclipw vf1 (T447: cfc2 here), vclipw vf2;
 * cfc2 vi18. */
static void testClip()
{
    for (int n = 0; n < 4000; n++) {
        randomVu();
        perspectiveVf24(false);
        pgl::g_vu0.vf[20] = Vec4{0.5f, 0.5f, 1.0f, 1.0f};
        float corner[3] = {rndf(-8, 8), rndf(-8, 8), rndf(-8, 8)};
        if (n % 7 == 0)
            pgl::g_vu0.vf[27].w = rndf(-3.0f, -0.5f);      /* behind the eye */

        /* the reference: the 12-byte corner through the GPRs (w = z's sign) */
        Ref r;
        refReset(r);
        refFromVu(r);
        uint32_t zb = bits(corner[2]);
        float in[4] = {corner[0], corner[1], corner[2], fromBits((uint32_t)((int32_t)zb >> 31))};
        refLoad(r, 1, in);
        vmulabc(r, XYZW, 24, 1, 0);
        vmaddabc(r, XYZW, 25, 1, 1);
        vmaddabc(r, XYZW, 26, 1, 2);
        vmaddbc(r, XYZW, 1, 27, 0, 3);
        vmul(r, XYZW, 2, 1, 20);
        vclipw(r, 1, 1);
        uint32_t refFirst = r.clip;
        vclipw(r, 2, 2);
        uint32_t refBoth = r.clip;

        /* the port (T447's form) */
        Vec4 vf1 = vu0g::xformW1(&vu0g::vf(24), vu0g::load3(corner));
        Vec4 vf2 = pgl::vu::mul(vf1, vu0g::vf(20));
        uint32_t first = vu0g::clipw(vf1);
        uint32_t both = vu0g::clipw(vf2);
        CHECK(sameBits(vf1, r.vf[1]) && sameBits(vf2, r.vf[2]), "clip transform %d", n);
        CHECK(first == refFirst && both == refBoth, "clip flags %d: %06X %06X vs %06X %06X", n,
              first, both, refFirst, refBoth);
        CHECK(vu0g::wBits(vf1) == (int32_t)bits(r.vf[1][3]), "clip w %d", n);
    }
}

/* T454 spriteset_begin_draw: the asm as written, MMI transpose included. */
static void testBillboard()
{
    for (int n = 0; n < 2000; n++) {
        randomVu();
        /* a rigid modelview (rotation from random Euler angles + translation) in vf20..vf23 */
        float a = rndf(-3, 3), b = rndf(-3, 3), c = rndf(-3, 3);
        float ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b), cc = std::cos(c), sc = std::sin(c);
        float R[3][3] = {{cb * cc, cb * sc, -sb},
                         {sa * sb * cc - ca * sc, sa * sb * sc + ca * cc, sa * cb},
                         {ca * sb * cc + sa * sc, ca * sb * sc - sa * cc, ca * cb}};
        for (int i = 0; i < 3; i++)
            pgl::g_vu0.vf[20 + i] = Vec4{R[i][0], R[i][1], R[i][2], (n & 1) ? 0.0f : rndf(-1, 1)};
        pgl::g_vu0.vf[23] = Vec4{rndf(-50, 50), rndf(-50, 50), rndf(-50, 50), 1.0f};

        /* the first block (before vu0_load_modelview, which touches only vf20..vf23) */
        Ref r;
        refReset(r);
        refFromVu(r);
        vmr32(r, 5, 0);
        {
            Vec4 vf0 = vu0g::vf(0);
            vu0g::vf(5) = Vec4{vf0.y, vf0.z, vf0.w, vf0.x};
        }
        /* the second block, literally */
        vmr32(r, 6, 5);
        vsub(r, XYZ, 23, 0, 23);
        qmfc2(r, 6, 20);
        qmfc2(r, 2, 21);
        qmfc2(r, 4, 22);
        pextlw(r, 3, 2, 6);
        pextuw(r, 5, 2, 6);
        pextlw(r, 2, 0, 4);
        pextuw(r, 4, 0, 4);
        pcpyld(r, 6, 2, 3);
        pcpyud(r, 2, 3, 2);
        pcpyld(r, 4, 4, 5);
        qmtc2(r, 6, 20);
        qmtc2(r, 2, 21);
        qmtc2(r, 4, 22);
        vmulabc(r, XYZ, 20, 23, 0);
        vmaddabc(r, XYZ, 21, 23, 1);
        vmaddbc(r, XYZ, 23, 22, 23, 2);
        vmulabc(r, XYZW, 20, 5, 0);
        vmaddabc(r, XYZW, 21, 5, 1);
        vmaddbc(r, XYZW, 5, 22, 5, 2);
        vopmula(r, 6, 5);
        vopmsub(r, 7, 5, 6);
        float refAxis[4];
        refStore(r, 7, refAxis);

        Vec4 axis = vu0g::billboardAxis();
        CHECK(sameBits(axis, refAxis), "billboard axis %d: (%g %g %g %g) vs (%g %g %g %g)", n,
              axis.x, axis.y, axis.z, axis.w, refAxis[0], refAxis[1], refAxis[2], refAxis[3]);
        bool regs = true;
        for (int i = 20; i <= 23; i++)
            regs = regs && sameBits(pgl::g_vu0.vf[i], r.vf[i]);
        CHECK(regs, "billboard vf20..vf23 (the rigid inverse) %d", n);
        CHECK(sameBits(pgl::g_vu0.vf[5], r.vf[5]) && sameBits(pgl::g_vu0.vf[6], r.vf[6]), "billboard vf5 / vf6 %d", n);
    }
    /* the rigid inverse is the inverse: M x M^-1 = I (within rounding) */
    randomVu();
    float ang = 0.7f;
    pgl::g_vu0.vf[20] = Vec4{std::cos(ang), 0.0f, -std::sin(ang), 0.0f};
    pgl::g_vu0.vf[21] = Vec4{0.0f, 1.0f, 0.0f, 0.0f};
    pgl::g_vu0.vf[22] = Vec4{std::sin(ang), 0.0f, std::cos(ang), 0.0f};
    pgl::g_vu0.vf[23] = Vec4{3.0f, -2.0f, 5.0f, 1.0f};
    Vec4 m[4] = {pgl::g_vu0.vf[20], pgl::g_vu0.vf[21], pgl::g_vu0.vf[22], pgl::g_vu0.vf[23]};
    pgl::invertRigid(m);
    Vec4 p{1.5f, -0.25f, 2.0f, 1.0f};
    Vec4 q = vu0g::xform(&pgl::g_vu0.vf[20], p);
    Vec4 back = vu0g::xform(m, q);
    CHECK(std::fabs(back.x - p.x) < 1e-5f && std::fabs(back.y - p.y) < 1e-5f && std::fabs(back.z - p.z) < 1e-5f,
          "rigid inverse round trip: %g %g %g", back.x, back.y, back.z);
}

/* T167: vu0_xform_vf20 (w from the operand), rb_rotate (vmaddz through
 * vf20..vf22), vu0_transpose_vf20_to_vf24, rb_rotate_inverse (vf24..vf26). */
static void testRigidBody()
{
    for (int n = 0; n < 2000; n++) {
        randomVu();
        Ref r;
        refReset(r);
        refFromVu(r);
        Vec4 p = rndVec(-5, 5);
        refLoad(r, 16, &p);
        vmulabc(r, XYZW, 20, 16, 0);
        vmaddabc(r, XYZW, 21, 16, 1);
        vmaddabc(r, XYZW, 22, 16, 2);
        vmaddbc(r, XYZW, 16, 23, 16, 3);
        CHECK(sameBits(vu0g::xform(&vu0g::vf(20), p), r.vf[16]), "xform_vf20 %d", n);

        float v3[3] = {rndf(-5, 5), rndf(-5, 5), rndf(-5, 5)};
        Vec4 in = vu0g::load3(v3);
        refLoad(r, 16, &in);
        vmulabc(r, XYZW, 20, 16, 0);
        vmaddabc(r, XYZW, 21, 16, 1);
        vmaddbc(r, XYZW, 16, 22, 16, 2);
        Vec4 got = vu0g::xform3(&vu0g::vf(20), in);
        CHECK(bits(got.x) == bits(r.vf[16][0]) && bits(got.y) == bits(r.vf[16][1]) && bits(got.z) == bits(r.vf[16][2]),
              "rb_rotate %d", n);

        /* the transpose, then rb_rotate_inverse */
        vu0g::transposeVf20ToVf24();
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                r.vf[24 + i][j] = r.vf[20 + j][i];
        bool same = true;
        for (int i = 24; i < 28; i++)
            same = same && sameBits(pgl::g_vu0.vf[i], r.vf[i]);
        CHECK(same, "transpose vf20 -> vf24 %d", n);
        refLoad(r, 16, &in);
        vmulabc(r, XYZW, 24, 16, 0);
        vmaddabc(r, XYZW, 25, 16, 1);
        vmaddbc(r, XYZW, 16, 26, 16, 2);
        got = vu0g::xform3(&vu0g::vf(24), in);
        CHECK(bits(got.x) == bits(r.vf[16][0]) && bits(got.y) == bits(r.vf[16][1]) && bits(got.z) == bits(r.vf[16][2]),
              "rb_rotate_inverse %d", n);
    }
    /* the 12-byte operand's w lane: z's sign extension */
    float neg[3] = {1.0f, 2.0f, -3.0f}, pos[3] = {1.0f, 2.0f, 3.0f};
    CHECK(bits(vu0g::load3(neg).w) == 0xFFFFFFFFu && bits(vu0g::load3(pos).w) == 0u, "load3 w");
}

/* T451 modelset_set_projection_scale: vf1 = (0, 0, -1, 1), vf2 = (-1, 0,
 * -1, 1) (vmove + vsubw), through vf20..vf23 with their own w, then the
 * viewport (vmulaw ACC, vf22, vf0w; vmadd vf23). */
static void testProjectionScale()
{
    for (int n = 0; n < 1000; n++) {
        randomVu();
        Ref r;
        refReset(r);
        refFromVu(r);
        vmove(r, 1, 0);
        vmove(r, 2, 0);
        vsubbc(r, Z, 1, 1, 0, 3);
        vsubbc(r, XZ, 2, 2, 0, 3);
        vmulabc(r, XYZW, 20, 1, 0);
        vmaddabc(r, XYZW, 21, 1, 1);
        vmaddabc(r, XYZW, 22, 1, 2);
        vmaddbc(r, XYZW, 1, 23, 1, 3);
        vmulabc(r, XYZW, 20, 2, 0);
        vmaddabc(r, XYZW, 21, 2, 1);
        vmaddabc(r, XYZW, 22, 2, 2);
        vmaddbc(r, XYZW, 2, 23, 2, 3);
        vmulabc(r, XYZW, 22, 0, 3);
        vmadd(r, XYZW, 1, 1, 23);
        vmulabc(r, XYZW, 22, 0, 3);
        vmadd(r, XYZW, 2, 2, 23);

        /* the port's statements (T451) */
        Vec4 vf0 = vu0g::vf(0);
        vu0g::vf(1) = vf0;
        vu0g::vf(2) = vf0;
        vu0g::vf(1).z = vf0.z - vf0.w;
        vu0g::vf(2).x = vf0.x - vf0.w;
        vu0g::vf(2).z = vf0.z - vf0.w;
        vu0g::vf(1) = vu0g::xform(&vu0g::vf(20), vu0g::vf(1));
        vu0g::vf(2) = vu0g::xform(&vu0g::vf(20), vu0g::vf(2));
        vu0g::vf(1) = vu0g::viewport(vu0g::vf(1));
        vu0g::vf(2) = vu0g::viewport(vu0g::vf(2));
        CHECK(sameBits(vu0g::vf(1), r.vf[1]) && sameBits(vu0g::vf(2), r.vf[2]), "projection scale %d", n);
    }
}

/* modelset_sphere_depth's PS2 behaviour: vf1 is loaded before
 * vu0_sync_state; with the matrices dirty the sync (micro 0x120) leaves the
 * combined rows in vf1..vf3, so the depth is that of row 0, not of the
 * sphere.  The port keeps it by using g_vu0.vf[1] across the call. */
static void testSyncClobber()
{
    randomVu();
    pgl::vu0InitMatrices(pgl::g_vu0);
    for (int r = 0; r < 4; r++)
        pgl::g_vu0.vf[28 + r] = rndVec(-2, 2);
    Vec4 sphere{1.0f, 2.0f, 3.0f, 0.0f};
    pgl::g_vu0.vf[1] = sphere;
    pgl::vu0SyncMatrices(pgl::g_vu0, pgl::MTX_MODELVIEW, false, Vec4{1, 1, 1, 1});
    CHECK(sameBits(pgl::g_vu0.vf[1], pgl::g_vu0.vf[24]), "micro 0x120 leaves row 0 in vf1");
}

/* T077 icon_model_draw: pcpyh of the weights, pmulth / pmaddh over the
 * shapes, pmflo; x = (s16)(acc >> 12), y = (s16)(lo >> 44), z = (s16)((acc
 * >> 12) >> 32). */
static void testIconMorph()
{
    const int shapeCounts[4] = {2, 4, 6, 8};
    for (int n = 0; n < 20000; n++) {
        int shapes = shapeCounts[n & 3];
        uint64_t src[8], w[8];
        int32_t weights[8];
        for (int k = 0; k < shapes; k++) {
            uint64_t v = 0;
            for (int h = 0; h < 4; h++) {
                uint16_t e;
                switch (rnd() % 8) {
                case 0: e = 0x8000; break;                  /* -32768 */
                case 1: e = 0x7FFF; break;
                default: e = (uint16_t)rnd(); break;
                }
                v |= (uint64_t)e << (16 * h);
            }
            src[k] = v;
            switch (rnd() % 6) {
            case 0: weights[k] = 4096; break;               /* 1.0 */
            case 1: weights[k] = -32768; break;
            case 2: weights[k] = (int32_t)rnd(); break;     /* only the low halfword counts */
            default: weights[k] = (int32_t)(rnd() % 8193) - 4096; break;
            }
        }
        /* the reference: the instructions, with garbage in the upper halves */
        Ref r;
        refReset(r);
        for (int k = 0; k < shapes; k++) {
            setGpr64(r, 31, (uint64_t)(int64_t)weights[k], ((uint64_t)rnd() << 32) | rnd());
            pcpyh(r, 16 + k, 31);
            setGpr64(r, 2 + k, src[k], ((uint64_t)rnd() << 32) | rnd());
        }
        pmulth(r, 12, 2, 16);
        for (int k = 1; k < shapes; k++)
            pmaddh(r, 12, 2 + k, 16 + k);
        pmflo(r, 13);
        uint64_t refAcc = gpr64(r, 12), refLo = gpr64(r, 13);
        int16_t rx = (int16_t)(refAcc >> 12), ry = (int16_t)(refLo >> 44), rz = (int16_t)((refAcc >> 12) >> 32);

        /* the port */
        for (int k = 0; k < shapes; k++)
            w[k] = vu0g::pcpyh(weights[k]);
        CHECK(w[0] == gpr64(r, 16), "pcpyh %d: %016llX vs %016llX", n,
              (unsigned long long)w[0], (unsigned long long)gpr64(r, 16));
        uint64_t acc, lo;
        vu0g::pmaddhChain(src, w, shapes, &acc, &lo);
        int16_t x = (int16_t)(acc >> 12), y = (int16_t)(lo >> 44), z = (int16_t)((acc >> 12) >> 32);
        CHECK(x == rx && y == ry && z == rz, "icon morph %d (%d shapes): %d %d %d vs %d %d %d", n, shapes,
              x, y, z, rx, ry, rz);

        /* docs/pgl.md 9: (s16)((sum_k src_k[i] * w_k) >> 12), 32-bit wrapping sums */
        uint32_t sum[3] = {0, 0, 0};
        for (int k = 0; k < shapes; k++)
            for (int i = 0; i < 3; i++)
                sum[i] += (uint32_t)((int32_t)(int16_t)(src[k] >> (16 * i)) * (int32_t)(int16_t)weights[k]);
        CHECK(x == (int16_t)((int32_t)sum[0] >> 12) && y == (int16_t)((int32_t)sum[1] >> 12) &&
              z == (int16_t)((int32_t)sum[2] >> 12), "icon morph formula %d", n);
    }
}

/* fp-bit's fptoui (decomp src/runtime/libgcc/fp_bit.c 0x0027AEA0), from the
 * float's fields as unpack_f gives them (FRACBITS 23, NGARDS 7). */
static uint32_t fptoui(float f)
{
    uint32_t u = bits(f);
    uint32_t sign = u >> 31, exp = (u >> 23) & 0xFF, mant = u & 0x7FFFFF;
    if (exp == 0)                               /* zero (and denormals: tiny, 0 anyway) */
        return 0;
    if (exp == 0xFF)
        return mant ? 0 : (sign ? 0 : 0xFFFFFFFFu);  /* NaN 0; infinity: negative 0, else max */
    if (sign)
        return 0;
    int normalExp = (int)exp - 127;
    if (normalExp < 0)
        return 0;
    if (normalExp > 31)
        return 0xFFFFFFFFu;
    uint64_t fraction = (uint64_t)(mant | 0x800000) << 7;   /* NGARDS */
    if (normalExp > 30)
        return (uint32_t)(fraction << (normalExp - 30));
    return (uint32_t)(fraction >> (30 - normalExp));
}

static void testF2u()
{
    const float specials[] = {0.0f, -0.0f, 0.25f, 0.999f, 1.0f, 1.5f, -1.0f, -0.5f, -1e9f, 127.9f, 128.0f,
                              255.5f, 8191.5f, 2147483520.0f, 2147483648.0f, 3e9f, 4294967040.0f,
                              4294967296.0f, 1e20f, fromBits(0x7F800000u), fromBits(0xFF800000u),
                              fromBits(0x7FC00000u), fromBits(0x00000001u), 16777217.0f};
    for (float f : specials)
        CHECK(ee_f2u(f) == fptoui(f), "ee_f2u(%g): %u vs %u", f, ee_f2u(f), fptoui(f));
    for (int n = 0; n < 200000; n++) {
        float f = fromBits(rnd());
        CHECK(ee_f2u(f) == fptoui(f), "ee_f2u(%g) [%08X]: %u vs %u", f, bits(f), ee_f2u(f), fptoui(f));
    }
    /* what the fix is for: x86 wraps a negative value */
    CHECK(ee_f2u(-1.0f) == 0u, "negative");
}

/* rsqrt.s: num / sqrt(|sq|), +-max for 0 */
static void testRsqrt()
{
    CHECK(vu0g::eeRsqrt(1.0f, 0.0f) == kMax, "rsqrt 1/0");
    CHECK(vu0g::eeRsqrt(-2.0f, 0.0f) == -kMax, "rsqrt -2/0");
    for (int n = 0; n < 1000; n++) {
        float a = rndf(-10, 10), b = rndf(0.001f, 100);
        CHECK(bits(vu0g::eeRsqrt(a, b)) == bits(a / std::sqrt(b)), "rsqrt %g %g", a, b);
    }
}

int main()
{
    testProjection();
    testClip();
    testBillboard();
    testRigidBody();
    testProjectionScale();
    testSyncClobber();
    testIconMorph();
    testF2u();
    testRsqrt();
    std::printf("vu0game: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures != 0;
}
