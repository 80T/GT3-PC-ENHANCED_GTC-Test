/* pgl_math.cpp - the VU0 side of the pgl graphics library as plain C++
 * (see pgl_math.h).  Each routine names the PS2 code it replaces; the VU0
 * instruction sequences are written out in their order of evaluation. */
#include "pgl_math.h"

#include <cmath>

namespace pgl {

/* In its power-on state from the start: vf0 is the VU's constant register
 * (0, 0, 0, 1), and the game's VU0 code takes w = 1 from it (an all-zero
 * vf0 made modelset_set_projection_scale 0: every LOD the lowest). */
static Vu0 powerOnVu0()
{
    Vu0 vu;
    vu0Reset(vu);
    return vu;
}
Vu0 g_vu0 = powerOnVu0();
EnvMap g_envMap;

namespace {

const float kFltMax = 3.40282347e+38f;      /* 0x7F7FFFFF: the VU's +-max */
const float kPi = 3.14159274f;              /* 0x40490FDB */

inline bool signBit(float f) { return (f2u(f) >> 31) != 0; }

inline Vec4 v4(float x, float y, float z, float w) { return Vec4{x, y, z, w}; }

void loadRows(Vec4 *dst, const float *m)
{
    for (int r = 0; r < 4; r++)
        dst[r] = v4(m[r * 4 + 0], m[r * 4 + 1], m[r * 4 + 2], m[r * 4 + 3]);
}

/* vmr32: (y, z, w, x) */
inline Vec4 mr32(const Vec4 &v) { return v4(v.y, v.z, v.w, v.x); }

}  /* namespace */

void vu0Reset(Vu0 &vu)
{
    std::memset(&vu, 0, sizeof(vu));
    vu.vf[0] = v4(0.0f, 0.0f, 0.0f, 1.0f);
    vu.r = 0x3F800000u | 0x2C1E41u;     /* arbitrary non-zero LFSR state; the game seeds it (RaceSmoke::reset) */
}

/* ======================================================================== */
/* VU float helpers                                                         */
/* ======================================================================== */
namespace vu {

float div(float num, float den)
{
    if (den == 0.0f)
        return (signBit(num) != signBit(den)) ? -kFltMax : kFltMax;
    return num / den;
}

float rsqrt(float num, float sq)
{
    float a = std::fabs(sq);            /* the VU / EE FPU take |sq| (and set the I flag) */
    if (a == 0.0f)
        return signBit(num) ? -kFltMax : kFltMax;
    return num / std::sqrt(a);
}

int32_t ftoi(float f, int fracBits)
{
    float v = f * (float)(1 << fracBits);   /* a power of two: exact */
    if (v != v)
        return 0;
    if (v >= 2147483648.0f)
        return 0x7FFFFFFF;
    if (v <= -2147483648.0f)
        return (int32_t)0x80000000u;
    return (int32_t)v;                      /* truncation toward zero */
}

uint32_t clipFlags(const Vec4 &v, float w)
{
    float a = std::fabs(w);
    uint32_t f = 0;
    if (v.x > a) f |= 0x01;
    if (v.x < -a) f |= 0x02;
    if (v.y > a) f |= 0x04;
    if (v.y < -a) f |= 0x08;
    if (v.z > a) f |= 0x10;
    if (v.z < -a) f |= 0x20;
    return f;
}

}  /* namespace vu */

/* ======================================================================== */
/* Micro subroutines                                                        */
/* ======================================================================== */

/* 0x000..0x088: ACC = vf28 m.x, + vf29 m.y, + vf30 m.z, row = ACC + vf31 m.w
 * for m = vf1..vf4; the lower slots move vf1..vf3 into vf28..vf30 while
 * vf31 is written by the last upper instruction. */
void vu0Micro000(Vu0 &vu)
{
    const Vec4 cur[4] = {vu.vf[28], vu.vf[29], vu.vf[30], vu.vf[31]};
    Vec4 row[4];
    for (int r = 0; r < 4; r++)
        row[r] = vu::xform(cur, vu.vf[1 + r]);
    vu.vf[1] = row[0];
    vu.vf[2] = row[1];
    vu.vf[3] = row[2];
    for (int r = 0; r < 4; r++)
        vu.vf[28 + r] = row[r];
}

/* 0x090..0x118: vf28+r = vf1 cur.x + vf2 cur.y + vf3 cur.z + vf4 cur.w. */
void vu0Micro090(Vu0 &vu)
{
    const Vec4 m[4] = {vu.vf[1], vu.vf[2], vu.vf[3], vu.vf[4]};
    for (int r = 0; r < 4; r++)
        vu.vf[28 + r] = vu::xform(m, vu.vf[28 + r]);
}

/* 0x120..0x1C8: lq vf24..vf27 = qwords 8..11, then rows as 0x000 with the
 * projection in place of the current matrix. */
void vu0Micro120(Vu0 &vu)
{
    Vec4 p[4];
    for (int r = 0; r < 4; r++)
        p[r] = vu.mem[VU0_PARKED_PROJECTION + r];
    Vec4 row[4];
    for (int r = 0; r < 4; r++)
        row[r] = vu::xform(p, vu.vf[28 + r]);
    vu.vf[1] = row[0];
    vu.vf[2] = row[1];
    vu.vf[3] = row[2];
    for (int r = 0; r < 4; r++)
        vu.vf[24 + r] = row[r];
}

/* The constants of 0x1D0 (the I register loads): the same as fastmath's. */
static const float kSinC3 = -0.166666567f;      /* 0xBE2AAAA4 */
static const float kSinC5 = 0.00833302550f;     /* 0x3C08873E */
static const float kSinC7 = -0.000198074136f;   /* 0xB94FB21F */
static const float kSinC9 = 2.60188699e-06f;    /* 0x362E9C14 */

float vu0SinPoly(float x)
{
    float x2 = x * x;                   /* 0x1D0 mul vf2 = vf1 vf1 */
    float x4 = x2 * x2;                 /* 0x1E0 */
    float x3 = x2 * x;                  /* 0x1E8 */
    float x5 = x4 * x;                  /* 0x1F0 */
    float x7 = x4 * x3;                 /* 0x1F8 */
    float x9 = x4 * x5;                 /* 0x208 */
    float acc = x3 * kSinC3;            /* 0x200 mulai */
    acc = acc + x5 * kSinC5;            /* 0x210 maddai */
    acc = acc + x7 * kSinC7;            /* 0x218 maddai */
    acc = acc + x * 1.0f;               /* 0x220 maddaw vf1, vf0w */
    return acc + x9 * kSinC9;           /* 0x228 maddi */
}

void vu0Micro1D0(Vu0 &vu)
{
    Vec4 x = vu.vf[1];
    Vec4 x2 = vu::mul(x, x), x4 = vu::mul(x2, x2), x3 = vu::mul(x2, x);
    Vec4 x5 = vu::mul(x4, x), x7 = vu::mul(x4, x3), x9 = vu::mul(x4, x5);
    vu.vf[2] = x2;
    vu.vf[3] = x3;
    vu.vf[4] = x9;
    vu.vf[5] = x5;
    vu.vf[6] = x7;
    for (int k = 0; k < 4; k++)
        vu.vf[1][k] = vu0SinPoly(x[k]);
}

/* The constants of 0x2D0 (Sony's VU arctangent series around t = 0, t =
 * (r - 1) / (r + 1), plus pi/4). */
static const float kAtanC1 = 0.999999344f;      /* 0x3F7FFFF5 */
static const float kAtanC3 = -0.333298564f;     /* 0xBEAAA61C */
static const float kAtanC5 = 0.199465364f;      /* 0x3E4C40A6 */
static const float kAtanC7 = -0.139085338f;     /* 0xBE0E6C63 */
static const float kAtanC9 = 0.0964200422f;     /* 0x3DC577DF */
static const float kAtanC11 = -0.0559098870f;   /* 0xBD6501C4 */
static const float kAtanC13 = 0.0218612291f;    /* 0x3CB31652 */
static const float kAtanC15 = -0.00405405788f;  /* 0xBB84D7E7 */
static const float kPi4 = 0.785398185f;         /* 0x3F490FDB */

/* One lane of 0x2F0..0x380 with vf1 = t and vf8 = k (1 in x..z, 2 in w). */
static float atanLane(float t, float k)
{
    float t2 = t * t;                   /* 0x300 vf7 */
    float acc = t * kAtanC1;            /* 0x308 mulai */
    float p = t2 * t;                   /* 0x310 vf5 = t^3 */
    float q = p * t2;                   /* 0x318 vf6 = t^5 */
    acc = acc + p * kAtanC3;            /* 0x320 */
    p = q * t2;                         /* 0x328 t^7 */
    acc = acc + q * kAtanC5;            /* 0x330 */
    q = p * t2;                         /* 0x338 t^9 */
    acc = acc + p * kAtanC7;            /* 0x340 */
    p = q * t2;                         /* 0x348 t^11 */
    acc = acc + q * kAtanC9;            /* 0x350 */
    q = p * t2;                         /* 0x358 t^13 */
    acc = acc + p * kAtanC11;           /* 0x360 */
    p = q * t2;                         /* 0x368 t^15 */
    acc = acc + q * kAtanC13;           /* 0x370 */
    acc = acc + k * kPi4;               /* 0x378 maddai vf8 */
    return acc + p * kAtanC15;          /* 0x380 maddi */
}

float vu0AtanPoly(float t) { return atanLane(t, 1.0f); }

static void atanTail(Vu0 &vu, float qv)
{
    vu.q = qv;
    vu.vf[8] = v4(1.0f, 1.0f, 1.0f, 2.0f);         /* 0x2F0 addi vf8, vf0, I (I = 1) */
    vu.vf[1] = v4(qv, qv, qv, 1.0f + qv);          /* 0x2F8 addq vf1, vf0, Q */
    for (int k = 0; k < 4; k++)
        vu.vf[4][k] = atanLane(vu.vf[1][k], vu.vf[8][k]);
    vu.vf[7] = vu::mul(vu.vf[1], vu.vf[1]);
}

void vu0Micro2D0(Vu0 &vu)
{
    vu.vf[5] = vu::add(vu.vf[2], vu.vf[1]);         /* 0x2D0 b + a */
    vu.vf[6] = vu::sub(vu.vf[2], vu.vf[1]);         /* 0x2D8 b - a */
    atanTail(vu, vu::div(vu.vf[6].x, vu.vf[5].x));  /* 0x2E0 div Q, vf6x, vf5x */
}

void vu0Micro2C0(Vu0 &vu)
{
    vu.vf[5] = vu::add(vu.vf[1], v4(1.0f, 1.0f, 1.0f, 1.0f));   /* addi vf5, vf1, I (I = 1) */
    vu.vf[6] = vu::sub(vu.vf[1], v4(1.0f, 1.0f, 1.0f, 1.0f));   /* subi vf6, vf1, I */
    atanTail(vu, vu::div(vu.vf[6].x, vu.vf[5].x));
}

/* ======================================================================== */
/* The matrix library                                                       */
/* ======================================================================== */

/* vu0_load_identity_regs: vsub / vmove / vmr32 / vaddw on vf0. */
void vu0LoadIdentity(Vu0 &vu)
{
    vu.vf[28] = v4(1.0f, 0.0f, 0.0f, 0.0f);
    vu.vf[29] = v4(0.0f, 1.0f, 0.0f, 0.0f);
    vu.vf[30] = v4(0.0f, 0.0f, 1.0f, 0.0f);
    vu.vf[31] = v4(0.0f, 0.0f, 0.0f, 1.0f);
}

/* vu0_reset_rotation: the xyz of rows 0..2 (w lanes and vf31 kept). */
void vu0ResetRotation(Vu0 &vu)
{
    vu.vf[28].x = 1.0f; vu.vf[28].y = 0.0f; vu.vf[28].z = 0.0f;
    vu.vf[29].x = 0.0f; vu.vf[29].y = 1.0f; vu.vf[29].z = 0.0f;
    vu.vf[30].x = 0.0f; vu.vf[30].y = 0.0f; vu.vf[30].z = 1.0f;
}

void vu0ResetTranslation(Vu0 &vu)
{
    vu.vf[31].x = 0.0f;
    vu.vf[31].y = 0.0f;
    vu.vf[31].z = 0.0f;
}

void vu0LoadMatrix(Vu0 &vu, const float *m) { loadRows(&vu.vf[28], m); }

void vu0GetMatrix(const Vu0 &vu, float *m)
{
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++)
            m[r * 4 + k] = vu.vf[28 + r][k];
}

/* pgl_mult_matrix: vf1..vf4 = m, then micro 0x000. */
void vu0MultMatrix(Vu0 &vu, const float *m)
{
    loadRows(&vu.vf[1], m);
    vu0Micro000(vu);
}

/* pgl_mult_matrix_90: vf1..vf4 = m, then micro 0x090. */
void vu0MultMatrix90(Vu0 &vu, const float *m)
{
    loadRows(&vu.vf[1], m);
    vu0Micro090(vu);
}

/* pgl_rotate: the rotation about the normalised axis built in vf1..vf4
 * (the instruction order of 0x00257CBC), then micro 0x000. */
void vu0Rotate(Vu0 &vu, float deg, float x, float y, float z)
{
    vu0SinCosDeg(vu, deg);                          /* vf1 = (s, c) */
    Vec4 v = v4(x, y, z, 0.0f);                     /* vf5 (the stack word 3 stored 0) */
    vu.vf[5] = v;
    Vec4 sq = vu::mul(v, v);                        /* vf2 */
    Vec4 sc = vu.vf[1];                             /* vf6 = vf1 x 1 */
    Vec4 ax = vu::scale(v, v.x);                    /* vf7 */
    Vec4 ay = vu::scale(v, v.y);                    /* vf8 */
    Vec4 az = vu::scale(v, v.z);                    /* vf9 */
    sq.x = sq.x + sq.y;                             /* vaddy.x */
    sc.w = 1.0f - sc.y;                             /* vsuby.w: 1 - c */
    sq.x = sq.x + sq.z;                             /* vaddz.x: |v|^2 */
    float q = vu::rsqrt(1.0f, sq.x);                /* vrsqrt Q, vf0w, vf2x */
    Vec4 kx = v4(0.0f, v.z, -v.y, 0.0f);            /* vf10 */
    Vec4 ky = v4(-v.z, 0.0f, v.x, 0.0f);            /* vf11 */
    Vec4 kz = v4(v.y, -v.x, 0.0f, 0.0f);            /* vf12 */
    sc.x = sc.x * q;                                /* vmulq.xw */
    sc.w = sc.w * q;
    ax = vu::scale(ax, q);
    ay = vu::scale(ay, q);
    az = vu::scale(az, q);
    const Vec4 e[3] = {v4(1, 0, 0, 0), v4(0, 1, 0, 0), v4(0, 0, 1, 0)};
    const Vec4 *aa[3] = {&ax, &ay, &az};
    const Vec4 *kk[3] = {&kx, &ky, &kz};
    for (int r = 0; r < 3; r++) {
        Vec4 acc = vu::scale(e[r], sc.y);           /* vmulay ACC, e, c */
        acc = vu::add(acc, vu::scale(*aa[r], sc.w)); /* vmaddaw ACC, a, (1 - c) / |v| */
        Vec4 row = e[r];                            /* vmaddx.xyz: w of e kept (0) */
        row.x = acc.x + kk[r]->x * sc.x;
        row.y = acc.y + kk[r]->y * sc.x;
        row.z = acc.z + kk[r]->z * sc.x;
        vu.vf[1 + r] = row;
    }
    vu.vf[4] = v4(0.0f, 0.0f, 0.0f, 1.0f);
    vu.vf[6] = sc;
    vu.vf[7] = ax;
    vu.vf[8] = ay;
    vu.vf[9] = az;
    vu.vf[10] = kx;
    vu.vf[11] = ky;
    vu.vf[12] = kz;
    vu.q = q;
    vu0Micro000(vu);
}

/* pgl_translate: ACC = vf28 x, + vf29 y, + vf30 z; vf31 = ACC + vf31. */
void vu0Translate(Vu0 &vu, float x, float y, float z)
{
    vu.vf[1] = v4(x, 0.0f, 0.0f, 0.0f);
    vu.vf[2] = v4(y, 0.0f, 0.0f, 0.0f);
    vu.vf[3] = v4(z, 0.0f, 0.0f, 0.0f);
    Vec4 t;
    for (int k = 0; k < 4; k++) {
        float a = vu.vf[28][k] * x;
        a = a + vu.vf[29][k] * y;
        a = a + vu.vf[30][k] * z;
        t[k] = a + vu.vf[31][k] * 1.0f;
    }
    vu.vf[31] = t;
}

/* vu0_scale: rows 0..2 (all four lanes) times x, y, z. */
void vu0Scale(Vu0 &vu, float x, float y, float z)
{
    vu.vf[1] = v4(x, 0.0f, 0.0f, 0.0f);
    vu.vf[2] = v4(y, 0.0f, 0.0f, 0.0f);
    vu.vf[3] = v4(z, 0.0f, 0.0f, 0.0f);
    vu.vf[28] = vu::scale(vu.vf[28], x);
    vu.vf[29] = vu::scale(vu.vf[29], y);
    vu.vf[30] = vu::scale(vu.vf[30], z);
}

/* vu0_scale_inv_y (and F_00257EF0): div.s 1 / s, then vu0_scale(1, 1/s, 1). */
void vu0ScaleInvY(Vu0 &vu, float s) { vu0Scale(vu, 1.0f, vu::div(1.0f, s), 1.0f); }

/* vu0_rotate_x: row1' = c row1 + s row2, row2' = c row2 - s row1. */
void vu0RotateX(Vu0 &vu, float deg)
{
    vu0SinCosDeg(vu, deg);
    float s = vu.vf[1].x, c = vu.vf[1].y;
    vu.vf[2].x = 0.0f - s;
    Vec4 r1 = vu.vf[29], r2 = vu.vf[30], n1, n2;
    for (int k = 0; k < 4; k++) {
        n1[k] = r1[k] * c + r2[k] * s;
        n2[k] = r2[k] * c + r1[k] * vu.vf[2].x;
    }
    vu.vf[3] = n1;
    vu.vf[29] = n1;
    vu.vf[30] = n2;
}

/* vu0_rotate_y: row0' = c row0 - s row2, row2' = s row0 + c row2. */
void vu0RotateY(Vu0 &vu, float deg)
{
    vu0SinCosDeg(vu, deg);
    float s = vu.vf[1].x, c = vu.vf[1].y;
    vu.vf[2].x = 0.0f - s;
    Vec4 r0 = vu.vf[28], r2 = vu.vf[30], n0, n2;
    for (int k = 0; k < 4; k++) {
        n2[k] = r0[k] * s + r2[k] * c;
        n0[k] = r0[k] * c + r2[k] * vu.vf[2].x;
    }
    vu.vf[3] = n2;
    vu.vf[28] = n0;
    vu.vf[30] = n2;
}

/* vu0_rotate_z: row0' = c row0 + s row1, row1' = c row1 - s row0. */
void vu0RotateZ(Vu0 &vu, float deg)
{
    vu0SinCosDeg(vu, deg);
    float s = vu.vf[1].x, c = vu.vf[1].y;
    vu.vf[2].x = 0.0f - s;
    Vec4 r0 = vu.vf[28], r1 = vu.vf[29], n0, n1;
    for (int k = 0; k < 4; k++) {
        n0[k] = r0[k] * c + r1[k] * s;
        n1[k] = r1[k] * c + r0[k] * vu.vf[2].x;
    }
    vu.vf[3] = n0;
    vu.vf[28] = n0;
    vu.vf[29] = n1;
}

/* vu0_transpose (pextlw / pextuw / pcpyld / pcpyud through the GPRs). */
void vu0Transpose(Vu0 &vu)
{
    Vec4 m[4] = {vu.vf[28], vu.vf[29], vu.vf[30], vu.vf[31]};
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++)
            vu.vf[28 + r][k] = m[k][r];
}

void invertRigid(Vec4 m[4])
{
    Vec4 t = m[3];
    t.x = 0.0f - t.x;                               /* vsub.xyz vf23, vf0, vf23 */
    t.y = 0.0f - t.y;
    t.z = 0.0f - t.z;
    Vec4 r0 = v4(m[0].x, m[1].x, m[2].x, 0.0f);     /* the 3x3 transposed, w = 0 */
    Vec4 r1 = v4(m[0].y, m[1].y, m[2].y, 0.0f);
    Vec4 r2 = v4(m[0].z, m[1].z, m[2].z, 0.0f);
    m[0] = r0;
    m[1] = r1;
    m[2] = r2;
    for (int k = 0; k < 3; k++) {                   /* vmulax.xyz / vmadday.xyz / vmaddz.xyz */
        float a = r0[k] * t.x;
        a = a + r1[k] * t.y;
        m[3][k] = a + r2[k] * t.z;
    }
    m[3].w = t.w;
}

/* vu0_invert_rigid on vf28..vf31. */
void vu0InvertRigid(Vu0 &vu) { invertRigid(&vu.vf[28]); }

/* vu0_apply_matrix: vf1 = src; vf1 = src x current (w from src); dst = vf1. */
void vu0ApplyMatrix(Vu0 &vu, float *dst, const float *src)
{
    vu.vf[1] = v4(src[0], src[1], src[2], src[3]);
    vu.vf[1] = vu::xform(&vu.vf[28], vu.vf[1]);
    dst[0] = vu.vf[1].x;
    dst[1] = vu.vf[1].y;
    dst[2] = vu.vf[1].z;
    dst[3] = vu.vf[1].w;
}

static int memIndex(int q) { return q & (VU0_MEM_QWORDS - 1); }

/* pgl_matrix_mode: park the old mode's matrix (vsqi), load the new one's
 * (vlqi); modes other than 0..2 neither park nor load. */
void vu0SwitchMatrixMode(Vu0 &vu, int oldMode, int newMode)
{
    if (oldMode >= 0 && oldMode <= 2) {
        int q = 4 + 4 * oldMode;
        for (int r = 0; r < 4; r++)
            vu.mem[memIndex(q + r)] = vu.vf[28 + r];
        vu.vi[10] = (uint16_t)(q + 4);
    }
    if ((unsigned)newMode <= 2) {
        int q = 4 + 4 * newMode;
        for (int r = 0; r < 4; r++)
            vu.vf[28 + r] = vu.mem[memIndex(q + r)];
        vu.vi[10] = (uint16_t)(q + 4);
    }
}

/* pgl_push_matrix: vsqi vf28..vf31, (vi11 / vi12 / vi13 ++). */
void vu0PushMatrix(Vu0 &vu, int mode)
{
    if (mode < 0 || mode > 2)
        return;
    uint16_t &sp = vu.vi[11 + mode];
    for (int r = 0; r < 4; r++) {
        vu.mem[memIndex(sp)] = vu.vf[28 + r];
        sp = (uint16_t)(sp + 1);
    }
}

/* pgl_pop_matrix: vlqd vf31..vf28, (--vi). */
void vu0PopMatrix(Vu0 &vu, int mode)
{
    if (mode < 0 || mode > 2)
        return;
    uint16_t &sp = vu.vi[11 + mode];
    for (int r = 3; r >= 0; r--) {
        sp = (uint16_t)(sp - 1);
        vu.vf[28 + r] = vu.mem[memIndex(sp)];
    }
}

void vu0InitMatrices(Vu0 &vu)
{
    vu0LoadIdentity(vu);
    vu.vi[11] = VU0_STACK_MODELVIEW;
    vu.vi[12] = VU0_STACK_PROJECTION;
    vu.vi[13] = VU0_STACK_TEXTURE;
    for (int q = VU0_PARKED_MODELVIEW; q < VU0_PARKED_TEXTURE + 4; q += 4)
        for (int r = 0; r < 4; r++)
            vu.mem[q + r] = vu.vf[28 + r];
    vu.vi[10] = VU0_PARKED_TEXTURE + 4;
}

/* U115_VU_MUL_VF24_BY_MEM4_ASM: vf24..vf27 = (qwords 4..7) x vf24..vf27. */
static void mulMem4ByVf24(Vu0 &vu)
{
    Vec4 p[4] = {vu.vf[24], vu.vf[25], vu.vf[26], vu.vf[27]};
    Vec4 row[4];
    for (int r = 0; r < 4; r++) {
        vu.vf[1 + r] = vu.mem[VU0_PARKED_MODELVIEW + r];
        row[r] = vu::xform(p, vu.vf[1 + r]);
    }
    for (int r = 0; r < 3; r++)
        vu.vf[1 + r] = row[r];
    for (int r = 0; r < 4; r++)
        vu.vf[24 + r] = row[r];
    vu.vi[10] = VU0_PARKED_MODELVIEW + 4;
}

void vu0SyncMatrices(Vu0 &vu, int matrixMode, bool scaleEnable, const Vec4 &scale)
{
    switch (matrixMode) {
    case MTX_MODELVIEW:
        vu0Micro120(vu);
        break;
    case MTX_PROJECTION:
        for (int r = 0; r < 4; r++)
            vu.vf[24 + r] = vu.vf[28 + r];
        mulMem4ByVf24(vu);
        break;
    case MTX_TEXTURE:
        for (int r = 0; r < 4; r++)
            vu.vf[24 + r] = vu.mem[VU0_PARKED_PROJECTION + r];
        mulMem4ByVf24(vu);
        break;
    default:
        break;
    }
    if (scaleEnable) {
        vu.vf[1] = scale;
        vu.vf[24] = vu::scale(vu.vf[24], scale.x);
        vu.vf[25] = vu::scale(vu.vf[25], scale.y);
        vu.vf[26] = vu::scale(vu.vf[26], scale.z);
    }
}

void vu0GetModeMatrix(const Vu0 &vu, int matrixMode, int which, Vec4 out[4])
{
    Vec4 m[4];
    if (matrixMode == which) {
        for (int r = 0; r < 4; r++)
            m[r] = vu.vf[28 + r];
    } else {
        for (int r = 0; r < 4; r++)
            m[r] = vu.mem[memIndex(4 + 4 * which + r)];
    }
    for (int r = 0; r < 4; r++)
        out[r] = m[r];
}

/* ======================================================================== */
/* sin / cos / atan / random                                                */
/* ======================================================================== */

/* vu0_sincos_deg (0x00257A44): per lane a (cos lane: deg, sin lane: deg -
 * 90): k = trunc(a / 180), n = -1 if a's sign bit is set; r = (90 - a)
 * pi/180 - (n ? pi : 0) + k pi, negated when (k + n) is odd; sin(r). */
static float sincosLane(float a)
{
    const float degToRad = u2f(0x3C8EFA35);     /* pi / 180 */
    const float inv180 = u2f(0x3BB60B61);       /* 1 / 180 */
    int32_t k = vu::ftoi0(a * inv180);          /* cvt.w.s: truncation */
    int32_t n = signBit(a) ? -1 : 0;
    float r = (90.0f - a) * degToRad;
    r = r - (n ? kPi : 0.0f);                   /* the pi bits masked by the sign */
    r = r + (float)k * kPi;
    float sign = ((k + n) & 1) ? -1.0f : 1.0f;
    return r * sign;
}

void vu0SinCosDeg(Vu0 &vu, float deg)
{
    vu.vf[1] = v4(sincosLane(deg - 90.0f), sincosLane(deg), 0.0f, 0.0f);
    vu0Micro1D0(vu);
}

float pglSinDeg(Vu0 &vu, float deg) { vu0SinCosDeg(vu, deg); return vu.vf[1].x; }
float pglCosDeg(Vu0 &vu, float deg) { vu0SinCosDeg(vu, deg); return vu.vf[1].y; }

void pglSinCosDeg(Vu0 &vu, float deg, float *s, float *c)
{
    vu0SinCosDeg(vu, deg);
    *s = vu.vf[1].x;
    *c = vu.vf[1].y;
}

/* vu0_atan2f (0x00245C10) and its octant tables g_00343CE8 / g_00343D08. */
float vu0Atan2(Vu0 &vu, float y, float x)
{
    static const float sgn[8] = {1.0f, -1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f};
    static const float base[8] = {0.0f, 1.5707964f, 3.1415927f, 1.5707964f,
                                  0.0f, -1.5707964f, -3.1415927f, -1.5707964f};
    if (y == 0.0f)
        return (0.0f <= x) ? 0.0f : 3.1415927f;
    if (x == 0.0f)
        return (0.0f <= y) ? 1.5707964f : -1.5707964f;
    float ax = x < 0.0f ? -x : x;
    float ay = y < 0.0f ? -y : y;
    if (ay <= ax) {
        vu.vf[1] = v4(ax, 0.0f, 0.0f, 0.0f);
        vu.vf[2] = v4(ay, 0.0f, 0.0f, 0.0f);
    } else {
        vu.vf[1] = v4(ay, 0.0f, 0.0f, 0.0f);
        vu.vf[2] = v4(ax, 0.0f, 0.0f, 0.0f);
    }
    vu0Micro2D0(vu);
    int octant = 0;
    if (y < 0.0f)
        octant = 4;
    if (x < 0.0f)
        octant |= 2;
    if (ax < ay)
        octant |= 1;
    float a = vu.vf[4].x;
    if (sgn[octant] < 0.0f)
        a = -a;
    return a + base[octant];
}

/* vrnext: the R LFSR (23 bits, feedback bit 22 xor bit 4 into bit 0). */
float vu0Rand(Vu0 &vu)
{
    uint32_t r = vu.r;
    uint32_t fb = ((r >> 4) ^ (r >> 22)) & 1;
    r = (((r << 1) | fb) & 0x007FFFFF) | 0x3F800000;
    vu.r = r;
    vu.vf[1].x = u2f(r);
    return vu.vf[1].x - 1.0f;
}

void vu0RandSeed(Vu0 &vu, float seed)
{
    vu.vf[1].x = seed;
    vu.r = 0x3F800000 | (f2u(seed) & 0x007FFFFF);
}

float pseudoAngle(float a, float b)
{
    if (a < b) {
        if (-a < b)
            return a / b;
        return 6.0f - b / a;
    }
    if (-a < b)
        return 2.0f - b / a;
    return a / b + 4.0f;
}

/* ======================================================================== */
/* fastmath (engine/pdistd/fastmath.c, its C)                              */
/* ======================================================================== */
namespace fm {

static const float M_PI_2_F = 1.57079638f;      /* 0x3FC90FDB */
static const float M_PI_F = 3.14159275f;        /* 0x40490FDB */
static const float M_PI_4_F = 0.785398186f;     /* 0x3F490FDB */
static const float M_1_PI_F = 0.318309874f;     /* 0x3EA2F983 */
static const float M_2_PI_F = 0.636619747f;     /* 0x3F22F983 */
static const float M_LOG2E_F = 1.44269503f;     /* 0x3FB8AA3B */
static const float M_LN2_F = 0.693147183f;      /* 0x3F317218 */

/* sign * sin(t) for t in [-pi/2, pi/2] (the order of the R5900 FPU
 * accumulator: ACC = st, += c3 st^3, += c5 st^5, += c7 st^7, + c9 st^9). */
static inline float sinPoly(float t, float sign)
{
    float t2 = t * t;
    float st = t * sign;
    float t4 = t2 * t2;
    float st3 = t2 * st;
    float st5 = t4 * st;
    float st7 = t4 * st3;
    float st9 = st5 * t4;
    float acc = st + 0.0f;
    acc = acc + -0.166666568f * st3;            /* 0xBE2AAAA4 */
    acc = acc + 0.00833302551f * st5;           /* 0x3C08873E */
    acc = acc + -0.000198074137f * st7;         /* 0xB94FB21F */
    return acc + 2.601887e-06f * st9;           /* 0x362E9C14 */
}

float atanf(float x)
{
    int range = 0;
    float a = std::fabs(x);
    float inv = 1.0f / a;
    float mob = (1.0f - a) / (1.0f + a);
    if (2.41421366f < a) {                      /* tan(3pi/8) */
        a = inv;
        range = 1;
    } else if (0.414213568f < a) {              /* tan(pi/8) */
        a = mob;
        range = -1;
    }
    float z = a * a;
    float acc = 1.0f + 0.0f;
    float z2 = z * z;
    float a3 = z * a;
    acc = acc + 1.11111117f * z;                /* 0x3F8E38E4 */
    float den = acc + 0.238095239f * z2;        /* 0x3E73CF3D */
    acc = a + 0.0f;
    float a5 = a3 * z;
    float rden = 1.0f / den;
    acc = acc + 0.777777792f * a3;              /* 0x3F471C72 */
    float num = acc + 0.0677248687f * a5;       /* 0x3D8AB356 */
    float r = num * rden;
    if (range < 0)
        r = M_PI_4_F - r;
    else if (range > 0)
        r = M_PI_2_F - r;
    if (signBit(x))
        r = -r;
    return r;
}

float atan2f(float y, float x)
{
    float r = 0.0f;
    int q = 0;
    float c = x + y;
    if (c == 0.0f)
        return r;                               /* the quirk: also (1, -1), (-1, 1) */
    if (y < 0.0f) {
        y = -y;
        q = 4;
    }
    if (x < 0.0f) {
        x = -x;
        q |= 2;
    }
    if (y < x) {
        r = y / x;
    } else {
        q |= 1;
        r = x / y;
    }
    r = atanf(r);
    if (q & 1)
        r = M_PI_2_F - r;
    if (q & 2)
        r = M_PI_F - r;
    if (q & 4)
        r = -r;
    return r;
}

float cosf(float x)
{
    int n = vu::ftoi0(x * M_1_PI_F);
    int neg = signBit(x) ? 1 : 0;
    float pi = M_PI_2_F + M_PI_2_F;
    float k = (float)(n - neg);
    float sign = u2f(((uint32_t)(neg ^ (~n & 1)) << 31) | 0x3F800000);
    float t = (x - M_PI_2_F) - k * pi;
    return sinPoly(t, sign);
}

float sinf(float x)
{
    float u = x + M_PI_2_F;
    int n = vu::ftoi0(u * M_1_PI_F);
    int neg = signBit(u) ? 1 : 0;
    float pi = M_PI_2_F + M_PI_2_F;
    float k = (float)(n - neg);
    float sign = u2f(((uint32_t)(neg ^ (n & 1)) << 31) | 0x3F800000);
    float t = (x + 0.0f) - k * pi;
    return sinPoly(t, sign);
}

void sincosf(float x, float *s, float *c)
{
    int32_t mask = signBit(x) ? -1 : 0;
    float q = x * M_1_PI_F;
    float xa = x + u2f(0x40490FDBu & (uint32_t)mask);
    int n = vu::ftoi0(q);
    float k = (float)n;
    float sign = u2f(((uint32_t)(mask ^ n) << 31) | 0x3F800000);
    float u = (xa + 0.0f) - k * M_PI_F;
    float v = (M_PI_F - xa) + k * M_PI_F;
    float w = (xa - M_PI_2_F) - k * M_PI_F;
    u = (u < v) ? u : v;                        /* min.s */
    *c = sinPoly(w, -sign);
    *s = sinPoly(u, sign);
}

float tanf(float x)
{
    const float zero = 0.0f;
    int odd = 0;
    if (M_PI_4_F < std::fabs(x)) {
        float half = signBit(x) ? -0.5f : 0.5f;
        int n = vu::ftoi0(x * M_2_PI_F + half);
        odd = n & 1;
        x = x - (float)n * M_PI_2_F;
    }
    float z = x * x;
    float z2 = z * z;
    float x3 = z * x;
    float acc = 1.0f + zero;
    acc = acc + -0.444444448f * z;              /* 0xBEE38E39 */
    float x5 = z2 * x;
    float den = acc + 0.0158730168f * z2;       /* 0x3C820821 */
    acc = x + zero;
    acc = acc + -0.111111112f * x3;             /* 0xBDE38E39 */
    float num = acc + 0.00105820108f * x5;      /* 0x3A8AB356 */
    if (odd)
        return -den / num;
    return num / den;
}

float expf(float x)
{
    uint32_t bits = f2u(x);
    float a = std::fabs(x);
    int n = vu::ftoi0(M_LOG2E_F * a);
    float r = a - (float)n * M_LN2_F;
    float r2 = r * r;
    float r3 = r2 * r;
    float r4 = r2 * r2;
    float r5 = r3 * r2;
    float r7 = r4 * r3;
    float r6 = r3 * r3;
    float acc = 0.5f * r;
    acc = acc + 0.016025642f * r3;              /* 0x3C834835 */
    acc = acc + 8.74125909e-05f * r5;           /* 0x38B75147 */
    acc = acc + 5.78125566e-08f * r7;           /* 0x33784D94 */
    float p = acc;
    acc = acc + 0.115384616f * r2;              /* 0x3DEC4EC5 */
    acc = acc + 0.00145687641f * r4;            /* 0x3ABEF4A9 */
    acc = acc + 3.23750328e-06f * r6;           /* 0x365943E2 */
    p = p + p;
    float s = acc + 1.0f;
    float d = s - p;
    s = s * u2f((uint32_t)(n + 0x7F) << 23);    /* 2^n, no overflow check (as the PS2) */
    if ((int32_t)bits < 0)
        return d / s;
    return s / d;
}

float logf(float x)
{
    uint32_t bits = f2u(x);
    float t = u2f((bits | 0xFF800000u) - 0xC0000000u) - 1.0f;
    float e = (float)(int32_t)((bits >> 23) - 0x7F);
    float den = 1.0f + 2.5f * t;
    float t2 = t * t;
    float num = t + 2.0f * t2;
    den = den + 2.22222233f * t2;               /* 0x400E38E4 */
    float t3 = t2 * t;
    num = num + 1.30555559f * t3;               /* 0x3FA71C72 */
    den = den + 0.833333314f * t3;              /* 0x3F555555 */
    float t4 = t3 * t;
    num = num + 0.305555553f * t4;              /* 0x3E9C71C7 */
    den = den + 0.11904762f * t4;               /* 0x3DF3CF3D */
    float t5 = t4 * t;
    e = e * M_LN2_F;
    num = num + 0.0181216933f * t5;             /* 0x3C9473F2 */
    den = den + 0.0039682542f * t5;             /* 0x3B820821 */
    return e + num / den;
}

float powf(float x, float y) { return expf(y * logf(x)); }

}  /* namespace fm */

/* ======================================================================== */
/* Projection helpers                                                       */
/* ======================================================================== */

/* vdiv Q, vf0w, vNw; vmini vN, vN, vf21; vwaitq; vmulq vN, vN, Q;
 * vmulaw ACC, vf22, vf0w; vmadd vN, vN, vf23. */
Vec4 projectClip(const Vec4 &clip, const Viewport &vp)
{
    float q = vu::div(1.0f, clip.w);
    Vec4 p = vu::mini(clip, vp.limits);
    p = vu::scale(p, q);
    return vu::madd(vp.center, p, vp.scale);
}

GsXyzf toXyzf2(const Vec4 &screen)
{
    GsXyzf g;
    int32_t x = vu::ftoi4(screen.x), y = vu::ftoi4(screen.y);
    int32_t z = vu::ftoi4(screen.z), f = vu::ftoi4(screen.w);
    g.x = (uint16_t)(x & 0xFFFF);
    g.y = (uint16_t)(y & 0xFFFF);
    g.z = ((uint32_t)z >> 4) & 0xFFFFFF;        /* PACKED XYZF2: Z in bits 4..27 of word 2 */
    g.f = (uint8_t)(((uint32_t)f >> 4) & 0xFF); /* F in bits 4..11 of word 3 */
    g.adc = (((uint32_t)f >> 15) & 1) != 0;     /* ADC: bit 15 of word 3 */
    return g;
}

uint32_t clipCode(const Vec4 &v)
{
    uint32_t code = vu::clipFlags(v, v.w);
    if (signBit(v.w))
        code = ((code >> 1) | 0x2A) ^ ((code << 1) | 0x15);
    return code & 0x3F;
}

uint32_t clipLineHomogeneous(Vec4 &a, Vec4 &b)
{
    for (int plane = 0; plane < 6; plane++) {
        uint32_t codeA = clipCode(a);
        uint32_t codeB = clipCode(b);
        if (codeA & codeB)
            return codeA | codeB;
        codeB |= codeA;
        if (codeB == 0)
            return 0;
        if ((codeB >> plane) & 1) {
            int axis = plane >> 1;
            float distA, distB;
            if (plane & 1) {
                distA = a[axis] + a.w;
                distB = b[axis] + b.w;
            } else {
                distA = a[axis] - a.w;
                distB = b[axis] - b.w;
            }
            float invDiff = 1.0f / (distA - distB);
            if ((codeA >> plane) & 1) {
                for (int j = 0; j < 4; j++)
                    a[j] = (b[j] * distA - a[j] * distB) * invDiff;
            } else {
                for (int j = 0; j < 4; j++)
                    b[j] = (b[j] * distA - a[j] * distB) * invDiff;
            }
        }
    }
    return 0;
}

uint32_t clipPointCombined(const Vu0 &vu, float x, float y, float z)
{
    Vec4 p = vu::xformW1(&vu.vf[24], v4(x, y, z, 1.0f));
    uint32_t clip = vu::clipFlags(p, p.w);
    if (signBit(p.w))
        clip = ((clip >> 1) | 0x2A) ^ ((clip << 1) | 0x15);
    return clip & 0x3F;
}

bool boxVisible(const Vu0 &vu, const float *lo, const float *hi)
{
    const float c[8][3] = {
        {lo[0], lo[1], lo[2]}, {hi[0], hi[1], hi[2]}, {lo[0], lo[1], hi[2]}, {hi[0], hi[1], lo[2]},
        {lo[0], hi[1], lo[2]}, {hi[0], lo[1], hi[2]}, {lo[0], hi[1], hi[2]}, {hi[0], lo[1], lo[2]},
    };
    uint32_t outside = 0x3F;
    for (int i = 0; i < 8; i++) {
        outside &= clipPointCombined(vu, c[i][0], c[i][1], c[i][2]);
        if (outside == 0)
            return true;
    }
    return false;
}

/* pgl_set_sprite_depth (0x00256B80). */
uint32_t spriteDepthZF(Vu0 &vu, const Vec4 projection[4], const Viewport &vp, float z)
{
    for (int r = 0; r < 4; r++)
        vu.vf[20 + r] = projection[r];
    vu.vf[2] = vu.vf[0];                                /* (0, 0, 0, 1) */
    vu.vf[3] = v4(-1.0f, -1.0f, -1.0f, 0.0f);           /* vsubw vf3, vf0, vf0w */
    vu.vf[1] = v4(z, 0.0f, 0.0f, 0.0f);
    vu.vf[2].z = vu.vf[2].z - z;                        /* (0, 0, -z, 1) */
    Vec4 p = vu::xform(&vu.vf[20], vu.vf[2]);
    vu.vf[23] = vp.scale;
    vu.vf[22] = vp.center;
    vu.vf[21] = vp.limits;
    vu.vf[20] = vp.bb0;
    float q = vu::div(1.0f, p.w);
    p = vu::mini(p, vp.limits);
    p = vu::scale(p, q);
    if (p.z > 1.0f) p.z = 1.0f;                         /* vminiw.z vf1, vf1, vf0w */
    if (p.z < -1.0f) p.z = -1.0f;                       /* vmaxx.z vf1, vf1, vf3x */
    p = vu::madd(vp.center, p, vp.scale);
    vu.q = q;
    int32_t zi = vu::ftoi0(p.z), fi = vu::ftoi0(p.w);
    vu.vf[1] = p;
    uint32_t fog = (z <= 0.0f) ? 0xFFu : (uint32_t)fi;
    return (fog << 24) | (uint32_t)zi;
}

/* gs_set_depth_bias_at (0x00252E08). */
float depthBiasAt(const Vec4 modelview[4], const Vec4 projection[4], float dz, float x, float y, float z)
{
    Vec4 a = vu::xformW1(modelview, v4(x, y, z, 0.0f));    /* vmaddw vf2, vf23, vf0w */
    Vec4 b = a;
    b.z = a.z + dz;                                         /* vaddw.z vf3, vf2, vf1w */
    a = vu::xform(projection, a);
    b = vu::xform(projection, b);
    a = vu::scale(a, vu::div(1.0f, a.w));
    b = vu::scale(b, vu::div(1.0f, b.w));
    return b.w - a.w;
}

Vec4 lightVectorToEye(const Vec4 modelview[4], const Vec4 &v, bool normalize)
{
    Vec4 p = v;
    if (normalize) {
        float l = p.x * p.x + p.y * p.y;                    /* vmul, vaddy.x */
        l = l + p.z * p.z;                                  /* vaddz.x */
        p = vu::scale(p, vu::rsqrt(1.0f, l));
    }
    return vu::xform(modelview, p);
}

void xformTexcoords(const Vec4 texture[4], float *dstST, const float *srcST, int n)
{
    for (int i = 0; i < n; i++) {
        Vec4 r = vu::xform(texture, v4(srcST[i * 2], srcST[i * 2 + 1], 0.0f, 1.0f));
        dstST[i * 2] = r.x;
        dstST[i * 2 + 1] = r.y;
    }
}

/* ======================================================================== */
/* Environment map                                                          */
/* ======================================================================== */

void envmapRotate(Vu0 &vu, EnvMap &em, float x, float y, float z)
{
    const Vec4 &b0 = em.basis[0];
    Vec4 v;                                     /* (sic) the first basis row three times */
    for (int k = 0; k < 4; k++) {
        float a = b0[k] * x;
        a = a + b0[k] * y;
        v[k] = a + b0[k] * z;
    }
    float phase = -((pseudoAngle(v.z, v.x) + 1.0f) * 0.125f);
    em.phase = phase;
    vu0SinCosDeg(vu, phase * -360.0f);
    float s = vu.vf[1].x, c = vu.vf[1].y;
    const Vec4 ry[3] = {v4(0.0f + c, 0.0f, 0.0f - s, 0.0f), v4(0.0f, 0.0f + 1.0f, 0.0f, 0.0f),
                        v4(0.0f + s, 0.0f, 0.0f + c, 0.0f)};
    Vec4 nb[3];
    for (int r = 0; r < 3; r++)
        nb[r] = vu::xform3(ry, em.basis[r]);    /* basis_r x Ry */
    for (int r = 0; r < 3; r++)
        em.rot[r] = vu::xform3(nb, em.viewRot[r]);   /* viewRot_r x (basis x Ry) */
}

void envmapPointAt(Vu0 &vu, EnvMap &em, float x, float y, float z)
{
    envmapRotate(vu, em, x - em.eye.x, y - em.eye.y, z - em.eye.z);
}

void envmapSetBasis(Vu0 &vu, EnvMap &em, const Vec4 modelview[4],
                    float ax, float ay, float az, float bx, float by, float bz)
{
    Vec4 a = v4(ax, ay, az, 0.0f);
    Vec4 b = v4(bx, by, bz, 0.0f);
    Vec4 c = vu::cross(b, a);                   /* vopmula / vopmsub: b x a */
    float la = a.x * a.x + a.y * a.y;
    la = la + a.z * a.z;
    a = vu::scale(a, vu::rsqrt(1.0f, la));
    float lc = c.x * c.x + c.y * c.y;
    lc = lc + c.z * c.z;
    c = vu::scale(c, vu::rsqrt(1.0f, lc));
    Vec4 s = vu::cross(a, c);                   /* a x c */
    em.basis[0] = s;
    em.basis[1] = a;
    em.basis[2] = c;
    Vec4 inv[4] = {modelview[0], modelview[1], modelview[2], modelview[3]};
    invertRigid(inv);
    for (int r = 0; r < 3; r++)
        em.viewRot[r] = inv[r];
    em.eye = inv[3];                            /* (0, 0, 0, 1) through the inverse */
    Vec4 dir = vu::xform3(inv, v4(0.0f, 0.0f, -1.0f, 0.0f));
    envmapRotate(vu, em, dir.x, dir.y, dir.z);
}

void envmapBuildMatrix(EnvMap &em, const Vec4 modelview[4])
{
    for (int r = 0; r < 3; r++)
        em.texMtx[r] = vu::xform3(em.rot, modelview[r]);
    Vec4 inv[4] = {modelview[0], modelview[1], modelview[2], modelview[3]};
    invertRigid(inv);
    em.texMtx[3] = inv[3];
    em.texMtx[1].w = em.phase + 0.875f;
    em.texMtx[0].w = -0.125f;
    em.texMtx[2].w = em.paramA * u2f(0xBE2AAAABu);  /* -1/6 */
    em.texMtx[3].w = em.paramB + 0.5f;
}

}  /* namespace pgl */
