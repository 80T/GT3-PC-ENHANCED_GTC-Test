/* pglmath - tests of the native pgl math (src/engine/pgl/native/pgl_math.*)
 * and of the GS state / primitive translation (pgl_state_gs.*,
 * pgl_state_prim.*): hand-computed values, the PS2's approximations against
 * the exact functions, and the fixed-function blend against the GS formula
 * for every ALPHA selector combination.
 *
 *   pglmath            run every test, print failures and a summary
 *   pglmath -v         also print the blend table and the error bounds
 */
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "engine/pgl/native/pgl_math.h"
#include "engine/pgl/native/pgl_state_gs.h"
#include "engine/pgl/native/pgl_state_prim.h"

using namespace pgl;

static int g_checks = 0, g_failures = 0;
static bool g_verbose = false;

#define CHECK(cond, ...) do {                                               \
        g_checks++;                                                         \
        if (!(cond)) {                                                      \
            g_failures++;                                                   \
            std::printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);     \
            std::printf(__VA_ARGS__);                                       \
            std::printf("\n");                                              \
        }                                                                   \
    } while (0)

static bool near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }
static const double PI = 3.14159265358979323846;

/* pgl_frustum's matrix (decomp pgl.c 0x002555A8), row-vector layout. */
static void frustumRows(float l, float r, float b, float t, float n, float f, float focal, float *m)
{
    float invWidth = 1.0f / (r - l), invHeight = 1.0f / (t - b), invDepth = 1.0f / (f - n);
    std::memset(m, 0, 16 * sizeof(float));
    m[0] = 2.0f * focal * invWidth;
    m[5] = 2.0f * focal * invHeight;
    m[8] = (r + l) * invWidth;
    m[9] = (t + b) * invHeight;
    m[10] = -(f + n) * invDepth;
    m[11] = -1.0f;
    m[14] = f * (-2.0f * n) * invDepth;
}

static void setupVu(Vu0 &vu)
{
    vu0Reset(vu);
    vu0InitMatrices(vu);
}

/* ======================================================================== */
/* sin / cos / atan / rand                                                  */
/* ======================================================================== */

static void testSinCos()
{
    Vu0 vu;
    setupVu(vu);
    /* hand values */
    float s, c;
    pglSinCosDeg(vu, 0.0f, &s, &c);
    CHECK(near(s, 0.0, 1e-6) && near(c, 1.0, 2e-6), "sincos 0 = %.9g %.9g", s, c);
    pglSinCosDeg(vu, 30.0f, &s, &c);
    CHECK(near(s, 0.5, 2e-6) && near(c, 0.8660254, 2e-6), "sincos 30 = %.9g %.9g", s, c);
    pglSinCosDeg(vu, 90.0f, &s, &c);
    CHECK(near(s, 1.0, 2e-6) && near(c, 0.0, 2e-6), "sincos 90 = %.9g %.9g", s, c);
    pglSinCosDeg(vu, 180.0f, &s, &c);
    CHECK(near(s, 0.0, 2e-6) && near(c, -1.0, 2e-6), "sincos 180 = %.9g %.9g", s, c);
    pglSinCosDeg(vu, -90.0f, &s, &c);
    CHECK(near(s, -1.0, 2e-6) && near(c, 0.0, 2e-6), "sincos -90 = %.9g %.9g", s, c);
    pglSinCosDeg(vu, -135.0f, &s, &c);
    CHECK(near(s, -0.70710678, 2e-6) && near(c, -0.70710678, 2e-6), "sincos -135 = %.9g %.9g", s, c);
    CHECK(near(pglCosDeg(vu, 60.0f), 0.5, 2e-6), "cos 60");
    CHECK(near(pglSinDeg(vu, 270.0f), -1.0, 2e-6), "sin 270");

    /* the polynomial at the end of its range, against its own value in double */
    double x = 1.5707963267948966;
    double expect = (((x * x * x) * (double)-0.166666567f + std::pow(x, 5) * (double)0.0083330255f) +
                     std::pow(x, 7) * (double)-0.000198074136f + x) + std::pow(x, 9) * (double)2.60188699e-06f;
    CHECK(near(vu0SinPoly((float)x), expect, 4e-7), "sin poly(pi/2) = %.9g, expected %.9g", vu0SinPoly((float)x), expect);

    /* the whole circle, several turns: the error of the x^9 polynomial */
    double maxErr = 0.0;
    for (int i = -2880; i <= 2880; i++) {
        float deg = i * 0.25f;
        pglSinCosDeg(vu, deg, &s, &c);
        double r = deg * PI / 180.0;
        maxErr = std::fmax(maxErr, std::fabs(s - std::sin(r)));
        maxErr = std::fmax(maxErr, std::fabs(c - std::cos(r)));
    }
    CHECK(maxErr < 6e-6, "sincos max error %.3g", maxErr);
    if (g_verbose)
        std::printf("  vu0 sincos (-720..720 deg): max error %.3g\n", maxErr);

    /* vf1 holds (sin, cos) after the call, as the game's asm reads it */
    vu0SinCosDeg(vu, 45.0f);
    CHECK(near(vu.vf[1].x, 0.70710678, 2e-6) && near(vu.vf[1].y, 0.70710678, 2e-6), "vf1 after vu0_sincos_deg");
}

static void testAtan()
{
    Vu0 vu;
    setupVu(vu);
    CHECK(vu0Atan2(vu, 0.0f, 1.0f) == 0.0f, "atan2(0, 1)");
    CHECK(near(vu0Atan2(vu, 0.0f, -1.0f), 3.1415927, 1e-7), "atan2(0, -1)");
    CHECK(near(vu0Atan2(vu, 1.0f, 0.0f), 1.5707964, 1e-7), "atan2(1, 0)");
    CHECK(near(vu0Atan2(vu, -1.0f, 0.0f), -1.5707964, 1e-7), "atan2(-1, 0)");
    CHECK(near(vu0Atan2(vu, 1.0f, 1.0f), PI / 4, 1e-6), "atan2(1, 1) = %.9g", vu0Atan2(vu, 1.0f, 1.0f));
    CHECK(near(vu0Atan2(vu, -1.0f, -1.0f), -3 * PI / 4, 2e-6), "atan2(-1, -1)");
    /* the series at r = 0 (t = -1) gives pi/4 minus its coefficient sum: 3e-8 */
    CHECK(near(vu0AtanPoly(-1.0f), 0.0, 1e-7), "atan poly(-1) = %.9g", vu0AtanPoly(-1.0f));
    double maxErr = 0.0;
    for (int i = 0; i < 720; i++) {
        double a = i * PI / 360.0;
        for (double r : {0.001, 0.5, 1.0, 37.0}) {
            float y = (float)(std::sin(a) * r), xx = (float)(std::cos(a) * r);
            double e = std::fabs(vu0Atan2(vu, y, xx) - std::atan2((double)y, (double)xx));
            if (e > PI)
                e = std::fabs(e - 2 * PI);
            maxErr = std::fmax(maxErr, e);
        }
    }
    CHECK(maxErr < 2e-6, "vu0 atan2 max error %.3g", maxErr);
    if (g_verbose)
        std::printf("  vu0 atan2: max error %.3g\n", maxErr);
}

static void testFastmath()
{
    double eSin = 0, eCos = 0, eTan = 0, eAtan = 0, eExp = 0, eLog = 0;
    for (int i = -4000; i <= 4000; i++) {
        float x = i * 0.0031f;
        float s, c;
        fm::sincosf(x, &s, &c);
        eSin = std::fmax(eSin, std::fabs(fm::sinf(x) - std::sin((double)x)));
        eSin = std::fmax(eSin, std::fabs(s - std::sin((double)x)));
        eCos = std::fmax(eCos, std::fabs(fm::cosf(x) - std::cos((double)x)));
        eCos = std::fmax(eCos, std::fabs(c - std::cos((double)x)));
        if (std::fabs(std::cos((double)x)) > 0.05)
            eTan = std::fmax(eTan, std::fabs(fm::tanf(x) - std::tan((double)x)) / std::fmax(1.0, std::fabs(std::tan((double)x))));
        eAtan = std::fmax(eAtan, std::fabs(fm::atanf(x * 10.0f) - std::atan((double)x * 10.0)));
        if (x > -8.0f && x < 8.0f)
            eExp = std::fmax(eExp, std::fabs(fm::expf(x) - std::exp((double)x)) / std::exp((double)x));
        if (x > 0.0f)
            eLog = std::fmax(eLog, std::fabs(fm::logf(x) - std::log((double)x)));
    }
    CHECK(eSin < 5e-6 && eCos < 5e-6, "fastmath sin %.3g cos %.3g", eSin, eCos);
    CHECK(eTan < 1e-4, "fastmath tan %.3g", eTan);
    CHECK(eAtan < 1e-4, "fastmath atan %.3g", eAtan);
    CHECK(eExp < 1e-5, "fastmath exp (relative) %.3g", eExp);
    CHECK(eLog < 1e-5, "fastmath log %.3g", eLog);
    if (g_verbose)
        std::printf("  fastmath max errors: sin %.3g cos %.3g tan %.3g (rel) atan %.3g exp %.3g (rel) log %.3g\n",
                    eSin, eCos, eTan, eAtan, eExp, eLog);
    /* hand values */
    CHECK(fm::cosf(0.0f) == fm::cosf(-0.0f) || near(fm::cosf(-0.0f), 1.0, 2e-6), "cosf(-0)");
    CHECK(near(fm::cosf(0.0f), 1.0, 2e-6), "cosf(0) = %.9g", fm::cosf(0.0f));
    CHECK(near(fm::powf(2.0f, 10.0f), 1024.0, 0.02), "powf(2, 10) = %.9g", fm::powf(2.0f, 10.0f));
    CHECK(near(fm::atan2f(1.0f, 1.0f), PI / 4, 1e-4), "atan2f(1, 1)");
    /* the quirk: 0 whenever x + y == 0 */
    CHECK(fm::atan2f(1.0f, -1.0f) == 0.0f && fm::atan2f(-1.0f, 1.0f) == 0.0f, "atan2f quirk");
    CHECK(near(fm::atan2f(-1.0f, -2.0f), std::atan2(-1.0, -2.0), 1e-4), "atan2f(-1, -2)");
}

static void testRand()
{
    Vu0 vu;
    setupVu(vu);
    vu0RandSeed(vu, u2f(0x3F800001u));
    CHECK(vu.r == 0x3F800001u, "vrinit");
    const uint32_t expect[] = {0x3F800002u, 0x3F800004u, 0x3F800008u, 0x3F800010u, 0x3F800021u, 0x3F800042u};
    for (uint32_t e : expect) {
        float v = vu0Rand(vu);
        CHECK(vu.r == e, "rnext %08X, expected %08X", vu.r, e);
        CHECK(v == u2f(e) - 1.0f, "rand value");
    }
    vu0RandSeed(vu, 0.123f);
    for (int i = 0; i < 10000; i++) {
        float v = vu0Rand(vu);
        if (!(v >= 0.0f && v < 1.0f)) {
            CHECK(false, "rand out of range %.9g", v);
            break;
        }
    }
}

/* ======================================================================== */
/* The matrix library                                                       */
/* ======================================================================== */

static Vec4 apply(Vu0 &vu, float x, float y, float z, float w)
{
    float src[4] = {x, y, z, w}, dst[4];
    vu0ApplyMatrix(vu, dst, src);
    return Vec4{dst[0], dst[1], dst[2], dst[3]};
}

static bool nearV(const Vec4 &a, float x, float y, float z, float w, double eps)
{
    return near(a.x, x, eps) && near(a.y, y, eps) && near(a.z, z, eps) && near(a.w, w, eps);
}

static void testMatrices()
{
    Vu0 vu;
    setupVu(vu);
    /* identity, translate */
    vu0LoadIdentity(vu);
    vu0Translate(vu, 1.0f, 2.0f, 3.0f);
    CHECK(nearV(vu.vf[31], 1, 2, 3, 1, 0), "translate row 3");
    CHECK(nearV(apply(vu, 0, 0, 0, 1), 1, 2, 3, 1, 0), "translate point");
    /* pgl_mult_matrix: current = m x current, m applied first */
    float sc[16] = {2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1};
    vu0MultMatrix(vu, sc);
    CHECK(nearV(apply(vu, 1, 1, 1, 1), 3, 4, 5, 1, 0), "mult (scale then translate)");
    /* pgl_mult_matrix_90: current = current x m, m applied last */
    vu0LoadIdentity(vu);
    vu0Translate(vu, 1.0f, 2.0f, 3.0f);
    vu0MultMatrix90(vu, sc);
    CHECK(nearV(apply(vu, 1, 1, 1, 1), 4, 6, 8, 1, 0), "mult90 (translate then scale)");
    /* pgl_rotate (glRotate): 90 degrees about z takes x to y */
    vu0LoadIdentity(vu);
    vu0Rotate(vu, 90.0f, 0.0f, 0.0f, 2.0f);         /* unnormalised axis */
    CHECK(nearV(apply(vu, 1, 0, 0, 1), 0, 1, 0, 1, 3e-6), "rotate z 90");
    CHECK(nearV(apply(vu, 0, 1, 0, 1), -1, 0, 0, 1, 3e-6), "rotate z 90 (y)");
    /* vu0_rotate_x / y / z = pgl_rotate about the axes (on the identity) */
    const float axes[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int a = 0; a < 3; a++) {
        Vu0 v1, v2;
        setupVu(v1);
        setupVu(v2);
        vu0Rotate(v1, 33.0f, axes[a][0], axes[a][1], axes[a][2]);
        if (a == 0) vu0RotateX(v2, 33.0f);
        if (a == 1) vu0RotateY(v2, 33.0f);
        if (a == 2) vu0RotateZ(v2, 33.0f);
        double e = 0;
        for (int r = 28; r < 32; r++)
            for (int k = 0; k < 4; k++)
                e = std::fmax(e, std::fabs(v1.vf[r][k] - v2.vf[r][k]));
        CHECK(e < 1e-6, "rotate axis %d vs vu0_rotate: %.3g", a, e);
    }
    /* rigid inverse: M^-1 M = I */
    vu0LoadIdentity(vu);
    vu0Translate(vu, 5.0f, -3.0f, 2.0f);
    vu0RotateY(vu, 40.0f);
    vu0RotateX(vu, -15.0f);
    float m[16];
    vu0GetMatrix(vu, m);
    vu0InvertRigid(vu);
    vu0MultMatrix(vu, m);                           /* current = M x M^-1 */
    double e = 0;
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++)
            e = std::fmax(e, std::fabs(vu.vf[28 + r][k] - (r == k ? 1.0f : 0.0f)));
    CHECK(e < 2e-6, "invert rigid: %.3g", e);
    /* transpose twice */
    vu0LoadMatrix(vu, m);
    vu0Transpose(vu);
    CHECK(vu.vf[28].y == m[4] && vu.vf[31].x == m[3] && vu.vf[29].w == m[13], "transpose");
    vu0Transpose(vu);
    float m2[16];
    vu0GetMatrix(vu, m2);
    CHECK(std::memcmp(m, m2, sizeof(m)) == 0, "transpose twice");
    /* scale_inv_y */
    vu0LoadIdentity(vu);
    vu0ScaleInvY(vu, 4.0f);
    CHECK(nearV(vu.vf[29], 0, 0.25f, 0, 0, 0), "scale_inv_y");
    /* reset rotation keeps w lanes and the translation */
    vu0LoadMatrix(vu, m);
    vu0ResetRotation(vu);
    CHECK(vu.vf[28].x == 1 && vu.vf[28].y == 0 && vu.vf[28].w == m[3] && vu.vf[31].x == m[12], "reset rotation");
}

static void testStacksAndSync()
{
    Vu0 vu;
    vu0Reset(vu);
    PglMathCtx ctx;
    ctxInitVu0Constants(ctx, vu);
    CHECK(vu.vi[11] == 0x10 && vu.vi[12] == 0x50 && vu.vi[13] == 0x60, "stack pointers");
    float p[16];
    frustumRows(-1, 1, -1, 1, 1, 100, 1, p);
    ctxMatrixMode(ctx, vu, MTX_PROJECTION);
    ctxLoadIdentity(ctx, vu);
    ctxMultMatrix(ctx, vu, p);
    ctxMatrixMode(ctx, vu, MTX_MODELVIEW);
    CHECK(vu.mem[VU0_PARKED_PROJECTION + 3].z == p[14], "projection parked");
    ctxLoadIdentity(ctx, vu);
    ctxTranslate(ctx, vu, 0.0f, 0.0f, -10.0f);
    CHECK(ctx.matrixDirty == 1, "dirty");
    ctxSyncMatrices(ctx, vu);
    CHECK(ctx.matrixDirty == 0 && (ctx.lightFlags & LF_MODELVIEW), "sync flags");
    /* the origin: (0, 0, -10, 1) x P = (0, 0, 10 (f + n) / (f - n) - 2 f n / (f - n), 10) */
    double z = 10.0 * 101.0 / 99.0 - 200.0 / 99.0;
    CHECK(nearV(vu.vf[27], 0, 0, (float)z, 10, 2e-5), "combined row 3 (%.9g %.9g)", vu.vf[27].z, vu.vf[27].w);
    Vec4 c0[4] = {vu.vf[24], vu.vf[25], vu.vf[26], vu.vf[27]};
    /* the same combined matrix with the projection current, and the texture */
    ctxMatrixMode(ctx, vu, MTX_PROJECTION);
    ctxSyncMatrices(ctx, vu);
    bool same = true;
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++)
            same = same && near(vu.vf[24 + r][k], c0[r][k], 1e-6);
    CHECK(same, "sync in projection mode");
    ctxMatrixMode(ctx, vu, MTX_TEXTURE);
    ctxSyncMatrices(ctx, vu);
    same = true;
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++)
            same = same && near(vu.vf[24 + r][k], c0[r][k], 1e-6);
    CHECK(same, "sync in texture mode");
    ctxMatrixMode(ctx, vu, MTX_MODELVIEW);
    /* scale enable: rows 0..2 of the combined matrix */
    ctx.scaleEnable = 1;
    ctxSetScale(ctx, 2.0f, 3.0f, 4.0f);
    ctxSyncMatrices(ctx, vu);
    CHECK(vu.vf[24].x == c0[0].x * 2.0f && vu.vf[26].z == c0[2].z * 4.0f, "scale enable");
    ctx.scaleEnable = 0;
    /* push / translate / pop */
    ctxPushMatrix(ctx, vu);
    CHECK(vu.vi[11] == 0x14, "push moves vi11");
    ctxTranslate(ctx, vu, 1.0f, 1.0f, 1.0f);
    ctxPopMatrix(ctx, vu);
    CHECK(vu.vi[11] == 0x10 && nearV(vu.vf[31], 0, 0, -10, 1, 0), "pop restores");
    /* the parked modelview seen from the projection mode */
    ctxMatrixMode(ctx, vu, MTX_PROJECTION);
    Vec4 mv[4];
    vu0GetModeMatrix(vu, ctx.matrixMode, MTX_MODELVIEW, mv);
    CHECK(nearV(mv[3], 0, 0, -10, 1, 0), "parked modelview");
    ctxMatrixMode(ctx, vu, MTX_MODELVIEW);

    /* sprite depth: the GS Z of an eye distance through the viewport */
    ctx.depthScale = 16776191.0f;
    ctxUpdateDepthRange(ctx);
    ctx.viewScale.w = 255.0f - 64.0f;               /* gfx_set_fog_params(10, 64 / 255) */
    ctx.viewCenter.w = 64.0f + u2f(0x3DCCCCCD);     /* + 0.1 */
    ctx.limits.w = 10.0f;
    ctxSetSpriteDepth(ctx, vu, 20.0f);
    double ndc = (20.0 * 101.0 / 99.0 - 200.0 / 99.0) / 20.0;
    double zgs = (0.5 * 16776191.0 + 1.0) + (-0.5 * 16776191.0) * ndc;
    double fgs = 64.1 + 191.0 * (10.0 / 20.0);
    CHECK(near((double)(ctx.spriteZF & 0xFFFFFF), std::floor(zgs), 3.0), "sprite Z %u, expected %.1f",
          ctx.spriteZF & 0xFFFFFF, zgs);
    CHECK((ctx.spriteZF >> 24) == (uint32_t)fgs, "sprite F %u, expected %u", ctx.spriteZF >> 24, (uint32_t)fgs);
    ctxSetSpriteDepth(ctx, vu, -1.0f);
    CHECK((ctx.spriteZF >> 24) == 0xFF, "sprite F behind the eye");
    /* depth bias: ~0 (the w lanes, as the binary) */
    ctxSetDepthBiasAt(ctx, vu, 0.5f, 0.0f, 0.0f, -5.0f);
    CHECK(std::fabs(ctx.depthBias) < 1e-6, "depth bias %.3g", ctx.depthBias);
}

static void testClipAndProject()
{
    Vec4 a{0, 0, 0, 1}, b{2, 0, 0, 1};
    CHECK(clipCode(b) == 0x01, "clip +x");
    CHECK(clipCode(Vec4{0, -3, 0, 1}) == 0x08, "clip -y");
    /* w < 0: the signed planes, flags swapped and inverted: x = 0.5 is inside |w| but past -w..w? */
    CHECK(clipCode(Vec4{0, 0, 0, -1}) == 0x3F, "clip w < 0 at the origin: %02X", clipCode(Vec4{0, 0, 0, -1}));
    CHECK(clipLineHomogeneous(a, b) == 0 && near(b.x, 1.0, 1e-6) && a.x == 0.0f, "clip line to x = w");
    Vec4 c{2, 0, 0, 1}, d{3, 1, 0, 1};
    CHECK(clipLineHomogeneous(c, d) != 0, "reject line");

    Viewport vp;
    vp.scale = Vec4{320.0f, -224.0f, -8388095.5f, 0.0f};
    vp.center = Vec4{2048.0f, 2048.0f, 8388096.5f, 255.0f};
    vp.limits = Vec4{u2f(0x7F7FFFFF), u2f(0x7F7FFFFF), u2f(0x7F7FFFFF), u2f(0x7F7FFFFF)};
    vp.bb0 = Vec4{0.5f, 0.5f, 1.0f, 1.0f};
    GsXyzf g = toXyzf2(projectClip(Vec4{0, 0, 0, 1}, vp));
    CHECK(g.x == 0x8000 && g.y == 0x8000 && g.f == 255 && !g.adc, "centre %04X %04X f %u", g.x, g.y, g.f);
    g = toXyzf2(projectClip(Vec4{-2, 2, -2, 2}, vp));  /* top left, near */
    /* ndc z -1: 8388096.5 + 8388095.5 = depthScale + 1 */
    CHECK(g.x == (2048 - 320) * 16 && g.y == (2048 - 224) * 16 && g.z == 16776192, "corner %u %u z %u", g.x, g.y, g.z);
}

static void testEnvMap()
{
    Vu0 vu;
    setupVu(vu);
    EnvMap em;
    Vec4 mv[4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    envmapSetBasis(vu, em, mv, 0, 1, 0, 1, 0, 0);
    CHECK(nearV(em.basis[0], 1, 0, 0, 0, 0) && nearV(em.basis[1], 0, 1, 0, 0, 0) && nearV(em.basis[2], 0, 0, 1, 0, 0),
          "envmap basis");
    /* view direction (0, 0, -1): v = row0 (x + y + z) = (-1, 0, 0), pseudo angle 4, phase -0.625, 225 degrees */
    CHECK(near(em.phase, -0.625, 1e-7), "envmap phase %.9g", em.phase);
    const double c = -0.70710678, s = -0.70710678;
    CHECK(nearV(em.rot[0], (float)c, 0, (float)-s, 0, 2e-6) && nearV(em.rot[2], (float)s, 0, (float)c, 0, 2e-6),
          "envmap rotation");
    envmapBuildMatrix(em, mv);
    CHECK(em.texMtx[0].w == -0.125f && near(em.texMtx[1].w, 0.25, 1e-7) && near(em.texMtx[2].w, -4.0 / 6.0, 1e-6) &&
          em.texMtx[3].w == 0.75f, "envmap matrix w column");
    CHECK(near(pseudoAngle(0, 1), 0, 0) && near(pseudoAngle(1, 0), 2, 0) && near(pseudoAngle(0, -1), 4, 0) &&
          near(pseudoAngle(-1, 0), 6, 0), "pseudo angle");
}

/* ======================================================================== */
/* GS state                                                                 */
/* ======================================================================== */

static gs::Regs defaultRegs()
{
    gs::Regs r;
    std::memset(&r, 0, sizeof(r));
    r.gen[gs::GEN_COLCLAMP] = 1;                    /* gfx_context_init */
    r.gen[gs::GEN_PRMODECONT] = 1;
    r.reg[gs::REG_FRAME] = (uint64_t)gs::PSMCT32 << 24;
    r.reg[gs::REG_ZBUF] = (uint64_t)1 << 24;        /* PSMZ24 */
    r.reg[gs::REG_TEST] = (1u << 16) | (2u << 17);  /* ZTE, ZTST GEQUAL */
    return r;
}

static uint64_t testReg(bool ate, int atst, int aref, int afail, bool date, bool datm, bool zte, int ztst)
{
    return (uint64_t)(ate ? 1 : 0) | ((uint64_t)atst << 1) | ((uint64_t)aref << 4) | ((uint64_t)afail << 12) |
           ((uint64_t)(date ? 1 : 0) << 14) | ((uint64_t)(datm ? 1 : 0) << 15) | ((uint64_t)(zte ? 1 : 0) << 16) |
           ((uint64_t)ztst << 17);
}

static void testBlendAll()
{
    static const int vals[] = {0, 1, 37, 64, 100, 127, 128, 200, 255};
    static const int cvals[] = {0, 1, 32, 64, 100, 127, 128};
    int exactCount = 0, inexactCount = 0;
    for (int sel = 0; sel < 256; sel++) {
        for (int fix : {0, 64, 128}) {
            gs::Alpha al = gs::decodeAlpha((uint64_t)sel | ((uint64_t)fix << 32));
            gs::Blend b = gs::translateBlend(al, true);
            double maxErr = 0;
            for (int cs : vals)
                for (int cd : vals)
                    for (int as : cvals)
                        for (int ad : cvals) {
                            int g = gs::gsBlendChannel(al, cs, cd, as, ad, true);
                            double n = std::floor(gs::nativeBlendChannel(b, cs, cd, as, ad) + 0.5);
                            maxErr = std::fmax(maxErr, std::fabs(n - g));
                        }
            /* (C = Ad as two DEST_ALPHA passes rounds twice where the GS
             * truncates once: within two units) */
            const double tol = b.ad == gs::AdMode::TwoPass ? 2.0 : 1.0;
            if (b.exact)
                CHECK(maxErr <= tol, "ALPHA %02X FIX %d marked exact, error %.0f", sel, fix, maxErr);
            if (fix == 64) {
                const bool reserved = al.a == 3 || al.b == 3 || al.c == 3 || al.d == 3;
                if (b.exact)
                    exactCount++;
                else
                    inexactCount++;
                if (g_verbose && !b.exact && !reserved)
                    std::printf("  ALPHA %02X (A%d B%d C%d D%d): inexact, max error %.0f\n", sel, al.a, al.b, al.c,
                                al.d, maxErr);
            }
        }
    }
    if (g_verbose)
        std::printf("  ALPHA selectors: %d exact, %d inexact (of 256, reserved 3s included)\n", exactCount,
                    inexactCount);
}

static void expectBlend(int sel, int fix, bool enable, gs::BlendFactor src, gs::BlendFactor dst, gs::BlendOp op,
                        gs::ColorPremul premul, bool exact)
{
    gs::Blend b = gs::translateBlend(gs::decodeAlpha((uint64_t)sel | ((uint64_t)fix << 32)), true);
    bool ok = b.enable == enable && (!enable || (b.src == src && b.dst == dst && b.op == op && b.premul == premul)) &&
              b.exact == exact;
    CHECK(ok, "ALPHA %02X: enable %d src %d dst %d op %d premul %d exact %d", sel, b.enable, (int)b.src, (int)b.dst,
          (int)b.op, (int)b.premul, b.exact);
}

static void testBlendGT3()
{
    using F = gs::BlendFactor;
    using O = gs::BlendOp;
    using P = gs::ColorPremul;
    expectBlend(0x44, 0, true, F::SrcC, F::InvSrcC, O::Add, P::None, true);         /* (Cs - Cd) As + Cd */
    expectBlend(0x48, 0, true, F::One, F::One, O::Add, P::TimesC, true);            /* Cs As + Cd: (Cs As) >> 7 in the shader */
    expectBlend(0x64, 64, true, F::SrcC, F::InvSrcC, O::Add, P::None, true);        /* (Cs - Cd) FIX + Cd */
    expectBlend(0x68, 64, true, F::One, F::One, O::Add, P::TimesC, true);           /* Cs FIX + Cd */
    expectBlend(0x88, 0, true, F::One, F::Zero, O::Add, P::TimesC, true);           /* Cs As (the flare results: As 2.0) */
    expectBlend(0x42, 0, true, F::SrcC, F::One, O::RevSubtract, P::None, true);     /* Cd - Cs As */
    expectBlend(0x84, 0, true, F::SrcC, F::SrcC, O::Subtract, P::None, true);       /* (Cs - Cd) As */
    expectBlend(0xA4, 64, true, F::SrcC, F::SrcC, O::Subtract, P::None, true);      /* (Cs - Cd) FIX */
    expectBlend(0x62, 64, true, F::SrcC, F::One, O::RevSubtract, P::None, true);    /* Cd - Cs FIX */
    expectBlend(0xA8, 64, true, F::One, F::Zero, O::Add, P::TimesC, true);          /* Cs FIX */
    expectBlend(0xA9, 64, true, F::Zero, F::SrcC, O::Add, P::None, true);           /* Cd FIX */
    expectBlend(0x58, 0, true, F::DstAlpha, F::One, O::Add, P::None, true);         /* Cs Ad + Cd: 2 passes */
    CHECK(gs::translateBlend(gs::decodeAlpha(0x58), true).ad == gs::AdMode::TwoPass, "0x58 two passes");
    expectBlend(0x54, 0, false, F::One, F::Zero, O::Add, P::None, true);            /* (Cs - Cd) Ad + Cd: shader */
    CHECK(gs::translateBlend(gs::decodeAlpha(0x54), true).ad == gs::AdMode::Shader, "0x54 in the shader");
    {
        /* a 24-bit frame: Ad = 0x80, i.e. FIX 0x80 */
        gs::Blend b24 = gs::translateBlend(gs::decodeAlpha(0x54), true, true);
        CHECK(b24.ad == gs::AdMode::None && b24.enable && b24.c == gs::CSource::Fix && b24.fix == 0x80 &&
              b24.src == F::SrcC && b24.dst == F::InvSrcC && b24.exact, "0x54 into CT24: FIX 0x80");
    }
    expectBlend(0x64, 255, true, F::SrcC, F::InvSrcC, O::Add, P::None, false);      /* FIX > 0x80 */
    expectBlend(0x00, 0, false, F::One, F::Zero, O::Add, P::None, true);            /* (Cs - Cs) As + Cs = Cs */
    expectBlend(0x40, 0, true, F::Zero, F::One, O::Add, P::None, true);             /* = Cd */
    expectBlend(0x49, 0, true, F::DstColor, F::One, O::Add, P::CAsColor, true);     /* (Cd - 0) As + Cd */
    expectBlend(0x09, 0, true, F::One, F::SrcC, O::Add, P::None, true);             /* (Cd - 0) As + Cs */
    expectBlend(0x08, 0, true, F::One, F::Zero, O::Add, P::OnePlusC, true);         /* (Cs - 0) As + Cs */
    expectBlend(0x46, 0, true, F::Zero, F::InvSrcC, O::Add, P::None, true);         /* (0 - Cd) As + Cd */
    expectBlend(0x41, 0, true, F::SrcC, F::One, O::RevSubtract, P::None, false);    /* (Cd - Cs) As + Cd */
    expectBlend(0x04, 0, true, F::One, F::SrcC, O::Subtract, P::OnePlusC, false);   /* (Cs - Cd) As + Cs */
    /* PRIM.ABE off: no blending whatever ALPHA says */
    CHECK(!gs::translateBlend(gs::decodeAlpha(0x44), false).enable, "ABE off");
}

static void testTests()
{
    /* gs_clear_buffers, depth only: ATE, ATST NEVER, AFAIL ZB_ONLY, ZTE, ZTST ALWAYS */
    gs::Regs r = defaultRegs();
    r.reg[gs::REG_TEST] = 0x32001;
    gs::DrawState s = gs::translate(r, 6);
    CHECK(s.passCount == 1 && s.pass[0].writeMask == 0 && s.pass[0].depthWrite && s.pass[0].gate == gs::AlphaGate::None &&
          s.depthFunc == gs::Compare::Always, "depth-only clear");
    r.reg[gs::REG_ZBUF] |= 1ull << 32;              /* zMask: the clear writes nothing */
    s = gs::translate(r, 6);
    CHECK(s.passCount == 0, "depth-only clear under ZMSK");
    /* colour-only clear: TEST 0 = no depth access */
    r = defaultRegs();
    r.reg[gs::REG_TEST] = 0;
    s = gs::translate(r, 6);
    CHECK(!s.depthTest && !s.depthWrite && s.passCount == 1 && s.pass[0].writeMask == gs::WRITE_RGBA, "colour clear");
    /* model set default: ATE GREATER 0x20, AFAIL KEEP; depth LEQUAL -> GEQUAL */
    r = defaultRegs();
    r.reg[gs::REG_TEST] = testReg(true, 6, 0x20, 0, false, false, true, 2);
    s = gs::translate(r, 0x14);
    CHECK(s.passCount == 1 && s.pass[0].gate == gs::AlphaGate::Pass && s.alphaFunc == gs::Compare::Greater &&
          s.aref == 0x20 && s.depthFunc == gs::Compare::GreaterEqual && s.depthWrite, "alpha test KEEP");
    /* FB_ONLY with depth writes: two passes */
    r.reg[gs::REG_TEST] = testReg(true, 6, 0x40, 1, false, false, true, 2);
    s = gs::translate(r, 0x14);
    CHECK(s.passCount == 2 && s.pass[1].gate == gs::AlphaGate::Fail && s.pass[1].writeMask == gs::WRITE_RGBA &&
          !s.pass[1].depthWrite && s.pass[0].depthWrite, "FB_ONLY");
    /* FB_ONLY without depth writes: one pass, no test */
    r.reg[gs::REG_ZBUF] |= 1ull << 32;
    s = gs::translate(r, 0x14);
    CHECK(s.passCount == 1 && s.pass[0].gate == gs::AlphaGate::None, "FB_ONLY under ZMSK");
    /* RGB_ONLY */
    r = defaultRegs();
    r.reg[gs::REG_TEST] = testReg(true, 5, 0x80, 3, false, false, true, 3);
    s = gs::translate(r, 0x14);
    CHECK(s.passCount == 2 && s.pass[1].writeMask == gs::WRITE_RGB && s.depthFunc == gs::Compare::Greater, "RGB_ONLY");
    /* DATE / DATM (CT32) */
    r = defaultRegs();
    r.reg[gs::REG_TEST] = testReg(false, 1, 0, 0, true, true, true, 1);
    s = gs::translate(r, 6);
    CHECK(s.date && s.datm, "DATE");
    /* write masks: gfx_set_frame_write_mask(0xFF000000) stores FBMSK 0x00FFFFFF */
    r = defaultRegs();
    r.reg[gs::REG_FRAME] |= (uint64_t)0x00FFFFFFu << 32;
    s = gs::translate(r, 6);
    CHECK(s.frameWriteMask == gs::WRITE_A && s.inexact == 0, "alpha-only mask");
    r.reg[gs::REG_FRAME] = ((uint64_t)gs::PSMCT24 << 24);
    s = gs::translate(r, 6);
    CHECK(s.frameWriteMask == gs::WRITE_RGB, "CT24 has no alpha");
    r.reg[gs::REG_FRAME] = (uint64_t)0x0000F000u << 32;
    s = gs::translate(r, 6);
    CHECK(s.inexact & gs::INEXACT_FBMSK, "partial FBMSK");
    /* fields */
    gs::Scissor sc = gs::decodeScissor(0 | (639ull << 16) | (0ull << 32) | (447ull << 48));
    CHECK(sc.x0 == 0 && sc.x1 == 639 && sc.y0 == 0 && sc.y1 == 447, "scissor");
    gs::FogCol fc = gs::decodeFogCol(0x00332211);
    CHECK(fc.r == 0x11 && fc.g == 0x22 && fc.b == 0x33, "fogcol");
    gs::Tex1 t1 = gs::decodeTex1(0x60 | (0xFF0ull << 32));
    CHECK(t1.mmag && t1.mmin == 1 && t1.k == -16, "tex1 (K %d)", t1.k);
    /* texture function: 0x80 = 1.0 */
    CHECK(gs::texFuncColor(0, 200, 0x80, 0x80) == 200 && gs::texFuncColor(0, 200, 0xFF, 0x80) == 255 &&
          gs::texFuncAlpha(0, true, 0x80, 0x80) == 0x80 && gs::texFuncAlpha(0, false, 0x10, 0x80) == 0x80,
          "texture function");
    CHECK(gs::fogChannel(200, 255, 0) == 200 && gs::fogChannel(200, 0, 50) == 50, "fog");
    /* keys differ where the state does */
    gs::Blend b1 = gs::translateBlend(gs::decodeAlpha(0x44), true), b2 = gs::translateBlend(gs::decodeAlpha(0x48), true);
    CHECK(gs::packBlendKey(b1, 15) != gs::packBlendKey(b2, 15) && gs::packBlendKey(b1, 15) != gs::packBlendKey(b1, 7),
          "blend keys");
}

/* ======================================================================== */
/* Immediate primitives                                                     */
/* ======================================================================== */

static prim::PrimCtx ctx640x448()
{
    prim::PrimCtx c;
    c.xoff = (uint16_t)((0x800 - 320) << 4);
    c.yoff = (uint16_t)((0x800 - 224) << 4);
    c.winX = 0;
    c.winY = 0;
    c.halfHeight = false;
    c.color = 0x80FF0000u;
    c.colorTex = 0x80808080u;
    c.spriteZF = 0xFF000000u | 0xFFFBFFu;
    c.primAttr = prim::PRIM_IIP | prim::PRIM_ABE;
    c.vp.scale = Vec4{320.0f, -224.0f, -8388095.5f, 0.0f};
    c.vp.center = Vec4{2048.0f, 2048.0f, 8388096.5f, 255.0f};
    float fm = u2f(0x7F7FFFFF);
    c.vp.limits = Vec4{fm, fm, fm, fm};
    c.vp.bb0 = Vec4{0.5f, 0.5f, 1.0f, 1.0f};
    return c;
}

static void testPrims()
{
    prim::PrimCtx c = ctx640x448();
    const uint16_t ofx = c.xoff, ofy = c.yoff;
    prim::GsVertex v[64];
    prim::Batch b = prim::flatSprite(c, 0, 0, 640, 448, v);
    CHECK(b.n == 2 && v[0].x == 27648 && v[0].y == 29184 && v[1].x == 37888 && v[1].y == 36352, "flat sprite XY");
    CHECK(b.prim == (prim::PRIM_SPRITE | prim::PRIM_IIP | prim::PRIM_ABE) && v[1].kick && !v[0].kick, "flat sprite prim");
    CHECK(prim::windowX(v[1].x, ofx) == 640.0f && prim::windowY(v[1].y, ofy) == 448.0f, "window coordinates");
    CHECK(v[0].z == 0xFFFBFF && v[0].f == 0xFF && v[0].rgba == c.color, "sprite Z / F / colour");
    CHECK(prim::toNative(0.0f, 2.0f) == 1.0f, "native pixel centre");
    /* field rendering: y in 12.3 */
    c.halfHeight = true;
    prim::flatSprite(c, 0, 0, 640, 448, v);
    CHECK(v[1].y == 29184 + 448 * 8, "half height");
    c.halfHeight = false;
    /* textured sprite: half-texel UVs, window origin added */
    c.winX = 10;
    c.winY = 20;
    b = prim::texturedSprite(c, 0, 0, 64, 32, 0, 0, 64, 32, v);
    CHECK(v[0].u == 8 && v[0].v == 8 && v[1].u == 64 * 16 + 8 && v[1].v == 32 * 16 + 8, "sprite UV");
    CHECK(prim::windowX(v[0].x, ofx) == 10.0f && prim::windowY(v[0].y, ofy) == 20.0f, "sprite window origin");
    CHECK(b.prim == (prim::PRIM_SPRITE | prim::PRIM_TME | prim::PRIM_FST | prim::PRIM_IIP | prim::PRIM_ABE) &&
          v[0].rgba == c.colorTex, "textured sprite prim / colour");
    b = prim::texturedSpriteF(c, 0.5f, 0.0f, 64.0f, 32.0f, 0.25f, 0.0f, 64.0f, 32.0f, v);
    CHECK(prim::windowX(v[0].x, ofx) == 10.5f && v[0].u == 4 + 8, "sprite_f");
    c.winX = 0;
    c.winY = 0;
    /* strips: 32-pixel boundaries, u interpolated */
    int n = prim::texturedSpriteStripCount(c, 0.0f, 100.0f);
    b = prim::texturedSpriteStripsF(c, 0.0f, 0.0f, 100.0f, 10.0f, 0.0f, 0.0f, 100.0f, 10.0f, v);
    CHECK(n == 4 && b.n == 8, "strip count %d / %d", n, b.n);
    CHECK(prim::windowX(v[1].x, ofx) == 32.0f && v[1].u == 8 + 32 * 16 && prim::windowX(v[7].x, ofx) == 100.0f &&
          v[7].u == 100 * 16 + 8, "strips");
    /* flat strips: 64-pixel boundaries */
    n = prim::flatSpriteStripCount(0, 640);
    b = prim::flatSpriteStrips(c, 0, 0, 640, 448, v);
    CHECK(n == 10 && b.n == 20 && prim::windowX(v[1].x, ofx) == 64.0f && prim::windowX(v[19].x, ofx) == 640.0f,
          "flat strips");
    /* lines */
    c.winX = 4;
    b = prim::line(c, 0, 0, 10, 10, v);
    CHECK(prim::windowX(v[0].x, ofx) == 4.0f && prim::windowX(v[1].x, ofx) == 14.0f && b.prim == (prim::PRIM_LINE | prim::PRIM_IIP | prim::PRIM_ABE),
          "line");
    b = prim::lineF(c, 0.5f, 0.0f, 10.0f, 10.25f, v);
    CHECK(prim::windowX(v[0].x, ofx) == 4.5f && prim::windowY(v[1].y, ofy) == 10.25f, "line_f");
    c.winX = 0;
    /* line strip through an identity combined matrix: NDC to pixels, ADC on a segment outside one plane */
    Vu0 vu;
    setupVu(vu);
    vu0SyncMatrices(vu, MTX_MODELVIEW, false, Vec4{1, 1, 1, 1});   /* identity x identity */
    const float pts[] = {0.0f, 0.0f, 2.0f, 0.0f, 3.0f, 0.5f, 0.0f, 0.5f};
    b = prim::lineStrip2d(c, vu, pts, 4, v);
    CHECK(b.n == 4 && b.prim == (prim::PRIM_LINE_STRIP | prim::PRIM_IIP | prim::PRIM_ABE), "line strip");
    CHECK(prim::windowX(v[0].x, ofx) == 320.0f && prim::windowY(v[0].y, ofy) == 224.0f, "strip centre");
    CHECK(v[1].kick && !v[2].kick && v[3].kick, "strip ADC (%d %d %d)", v[1].kick, v[2].kick, v[3].kick);
    /* 3D line: clipped, rejected */
    const float p0[3] = {0.0f, 0.0f, 0.0f}, p1[3] = {2.0f, 0.0f, 0.0f}, p2[3] = {3.0f, 0.0f, 0.0f};
    b = prim::line3d(c, vu, p0, p1, v);
    CHECK(b.n == 2 && prim::windowX(v[1].x, ofx) == 640.0f, "line 3d clipped (%g)", prim::windowX(v[1].x, ofx));
    b = prim::line3d(c, vu, p1, p2, v);
    CHECK(b.n == 0, "line 3d rejected");
    /* fan: XYZF3 for the first two vertices */
    const float fan[] = {0, 0, 10, 0, 10, 10, 0, 10};
    b = prim::polyFan2d(c, fan, nullptr, nullptr, 4, v);
    CHECK(b.n == 4 && !v[0].kick && !v[1].kick && v[2].kick && v[3].kick && v[0].rgba == c.color &&
          (b.prim & 7) == prim::PRIM_TRI_FAN, "fan");
    /* depth */
    CHECK(prim::depthOf(0xFFFFFF, 0x31) == 16777215.0f / 16777216.0f && prim::depthOf(0x2000000, 0x31) == 16777215.0f / 16777216.0f,
          "depth Z24");
}

/* -table: the 27 (A, B, D) forms with C = As (or FIX) and C = Ad as a
 * Markdown table (docs/pgl.md). */
static void printBlendTable()
{
    static const char *sel[3] = {"Cs", "Cd", "0"};
    static const char *fac[7] = {"ZERO", "ONE", "C", "1-C", "Ad'", "1-Ad'", "Cd"};
    static const char *op[3] = {"+", "src-dst", "dst-src"};
    static const char *pre[6] = {"", "Cs(1+C)", "Cs*255/128", "C", "0", "(Cs C)>>7"};
    std::printf("| sel | GS colour | C = As / FIX | C = Ad |\n|---|---|---|---|\n");
    for (int a = 0; a < 3; a++)
        for (int b = 0; b < 3; b++)
            for (int d = 0; d < 3; d++) {
                char form[64];
                std::snprintf(form, sizeof form, "(%s - %s) C + %s", sel[a], sel[b], sel[d]);
                std::printf("| %02X | %s |", a | b << 2 | d << 6, form);
                for (int c : {0, 1}) {
                    gs::Alpha al{(uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d, 0x40};
                    gs::Blend bl = gs::translateBlend(al, true);
                    std::string cell;
                    if (bl.ad == gs::AdMode::Shader) {
                        cell = "shader (Cd, Ad from a copy)";
                    } else if (bl.ad == gs::AdMode::TwoPass) {
                        cell = std::string("2 passes: Ad', ") + fac[(int)bl.dst] + "; Ad', ONE";
                    } else if (!bl.enable) {
                        cell = "off (Cs)";
                    } else {
                        if (bl.premul != gs::ColorPremul::None)
                            cell += std::string("o0 = ") + pre[(int)bl.premul] + "; ";
                        cell += std::string(fac[(int)bl.src]) + ", " + fac[(int)bl.dst];
                        if (bl.op != gs::BlendOp::Add)
                            cell += std::string(", ") + op[(int)bl.op];
                        if (!bl.exact)
                            cell += " **inexact**";
                    }
                    std::printf(" %s |", cell.c_str());
                }
                std::printf("\n");
            }
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "-v") == 0)
            g_verbose = true;
        if (std::strcmp(argv[i], "-table") == 0) {
            printBlendTable();
            return 0;
        }
    }
    testSinCos();
    testAtan();
    testFastmath();
    testRand();
    testMatrices();
    testStacksAndSync();
    testClipAndProject();
    testEnvMap();
    testBlendAll();
    testBlendGT3();
    testTests();
    testPrims();
    std::printf("pglmath: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
