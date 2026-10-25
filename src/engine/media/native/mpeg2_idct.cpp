/* mpeg2_idct.cpp - the 8x8 inverse DCT.  See mpeg2_idct.h.
 *
 * f(x, y) = 1/4 sum_u sum_v C(u) C(v) F(u, v) cos((2x+1)u pi/16) cos((2y+1)v pi/16),
 * C(0) = 1/sqrt(2), C(k) = 1: rows then columns, each 1-D pass split into
 * the even part (F0, F2, F4, F6: a 4-point transform) and the odd part
 * (F1, F3, F5, F7: a 4 x 4 product).  The result is rounded as the
 * reference IDCT does, floor(x + 0.5).
 */
#include "engine/media/native/mpeg2_idct.h"

#include <cmath>

#if !defined(GT3MEDIA_NO_SSE2) && (defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2) || defined(__SSE2__))
#define GT3MEDIA_SSE2 1
#include <emmintrin.h>
#include <xmmintrin.h>
#endif

namespace gt3media {

namespace {

const float C1 = 0.980785280403230449f;    /* cos(1 pi / 16) */
const float C2 = 0.923879532511286756f;
const float C3 = 0.831469612302545237f;
const float C4 = 0.707106781186547524f;
const float C5 = 0.555570233019602225f;
const float C6 = 0.382683432365089772f;
const float C7 = 0.195090322016128268f;

/* One 8-point pass: in[k * is] -> out[n * os]. */
template <typename T>
inline void idct_1d(const T *in, int is, float *out, int os)
{
    float x0 = (float)in[0 * is], x1 = (float)in[1 * is];
    float x2 = (float)in[2 * is], x3 = (float)in[3 * is];
    float x4 = (float)in[4 * is], x5 = (float)in[5 * is];
    float x6 = (float)in[6 * is], x7 = (float)in[7 * is];

    float e0 = C4 * (x0 + x4);
    float e1 = C4 * (x0 - x4);
    float o0 = C2 * x2 + C6 * x6;
    float o1 = C6 * x2 - C2 * x6;
    float E0 = e0 + o0, E3 = e0 - o0;
    float E1 = e1 + o1, E2 = e1 - o1;

    float O0 = C1 * x1 + C3 * x3 + C5 * x5 + C7 * x7;
    float O1 = C3 * x1 - C7 * x3 - C1 * x5 - C5 * x7;
    float O2 = C5 * x1 - C1 * x3 + C7 * x5 + C3 * x7;
    float O3 = C7 * x1 - C5 * x3 + C3 * x5 - C1 * x7;

    out[0 * os] = E0 + O0;
    out[7 * os] = E0 - O0;
    out[1 * os] = E1 + O1;
    out[6 * os] = E1 - O1;
    out[2 * os] = E2 + O2;
    out[5 * os] = E2 - O2;
    out[3 * os] = E3 + O3;
    out[4 * os] = E3 - O3;
}

/* floor(v + 0.5) for |v| < 32768: truncation of a positive value. */
inline int round_ref(float v)
{
    return (int)(v + 32768.5f) - 32768;
}

/* The unrounded 2-D transform times 4 (the 1/4 is applied by the
 * callers' rounding step) into out[64]. */
void idct_float(const int16_t *block, unsigned rowMask, float *out)
{
    float tmp[64];
    for (int r = 0; r < 8; r++) {
        const int16_t *row = block + r * 8;
        float *t = tmp + r * 8;
        if (!(rowMask & (1u << r))) {
            for (int n = 0; n < 8; n++)
                t[n] = 0.0f;
        } else if ((row[1] | row[2] | row[3] | row[4] | row[5] | row[6] | row[7]) == 0) {
            float v = C4 * (float)row[0];
            for (int n = 0; n < 8; n++)
                t[n] = v;
        } else {
            idct_1d(row, 1, t, 1);
        }
    }
    for (int c = 0; c < 8; c++)
        idct_1d(tmp + c, 8, out + c, 8);
}

inline uint8_t clamp255(int v)
{
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

#if defined(GT3MEDIA_SSE2)
/* SSE: the same transform, four 1-D transforms at a time, each lane
 * computing exactly what idct_1d computes (same operations, same order:
 * the results are bit-identical to the scalar path).  m[r * 2 + h] holds
 * row r, columns 4h..4h+3. */
inline void transpose8(__m128 *m)
{
    __m128 a0 = m[0], a1 = m[2], a2 = m[4], a3 = m[6];
    __m128 b0 = m[1], b1 = m[3], b2 = m[5], b3 = m[7];
    __m128 c0 = m[8], c1 = m[10], c2 = m[12], c3 = m[14];
    __m128 d0 = m[9], d1 = m[11], d2 = m[13], d3 = m[15];
    _MM_TRANSPOSE4_PS(a0, a1, a2, a3);
    _MM_TRANSPOSE4_PS(b0, b1, b2, b3);
    _MM_TRANSPOSE4_PS(c0, c1, c2, c3);
    _MM_TRANSPOSE4_PS(d0, d1, d2, d3);
    m[0] = a0; m[2] = a1; m[4] = a2; m[6] = a3;
    m[1] = c0; m[3] = c1; m[5] = c2; m[7] = c3;
    m[8] = b0; m[10] = b1; m[12] = b2; m[14] = b3;
    m[9] = d0; m[11] = d1; m[13] = d2; m[15] = d3;
}

/* The 1-D transform down the eight rows of column half h. */
inline void idct_1d_ps(__m128 *m, int h)
{
    const __m128 c1 = _mm_set1_ps(C1), c2 = _mm_set1_ps(C2), c3 = _mm_set1_ps(C3), c4 = _mm_set1_ps(C4);
    const __m128 c5 = _mm_set1_ps(C5), c6 = _mm_set1_ps(C6), c7 = _mm_set1_ps(C7);
    __m128 x0 = m[0 + h], x1 = m[2 + h], x2 = m[4 + h], x3 = m[6 + h];
    __m128 x4 = m[8 + h], x5 = m[10 + h], x6 = m[12 + h], x7 = m[14 + h];

    __m128 e0 = _mm_mul_ps(c4, _mm_add_ps(x0, x4));
    __m128 e1 = _mm_mul_ps(c4, _mm_sub_ps(x0, x4));
    __m128 o0 = _mm_add_ps(_mm_mul_ps(c2, x2), _mm_mul_ps(c6, x6));
    __m128 o1 = _mm_sub_ps(_mm_mul_ps(c6, x2), _mm_mul_ps(c2, x6));
    __m128 E0 = _mm_add_ps(e0, o0), E3 = _mm_sub_ps(e0, o0);
    __m128 E1 = _mm_add_ps(e1, o1), E2 = _mm_sub_ps(e1, o1);

    __m128 O0 = _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(c1, x1), _mm_mul_ps(c3, x3)), _mm_mul_ps(c5, x5)),
                           _mm_mul_ps(c7, x7));
    __m128 O1 = _mm_sub_ps(_mm_sub_ps(_mm_sub_ps(_mm_mul_ps(c3, x1), _mm_mul_ps(c7, x3)), _mm_mul_ps(c1, x5)),
                           _mm_mul_ps(c5, x7));
    __m128 O2 = _mm_add_ps(_mm_add_ps(_mm_sub_ps(_mm_mul_ps(c5, x1), _mm_mul_ps(c1, x3)), _mm_mul_ps(c7, x5)),
                           _mm_mul_ps(c3, x7));
    __m128 O3 = _mm_sub_ps(_mm_add_ps(_mm_sub_ps(_mm_mul_ps(c7, x1), _mm_mul_ps(c5, x3)), _mm_mul_ps(c3, x5)),
                           _mm_mul_ps(c1, x7));

    m[0 + h] = _mm_add_ps(E0, O0);
    m[14 + h] = _mm_sub_ps(E0, O0);
    m[2 + h] = _mm_add_ps(E1, O1);
    m[12 + h] = _mm_sub_ps(E1, O1);
    m[4 + h] = _mm_add_ps(E2, O2);
    m[10 + h] = _mm_sub_ps(E2, O2);
    m[6 + h] = _mm_add_ps(E3, O3);
    m[8 + h] = _mm_sub_ps(E3, O3);
}

/* Rows then columns, as idct_float; the result rounded to 16-bit integers
 * (row r in out[r]). */
inline void idct_sse(const int16_t *block, __m128i *out)
{
    __m128 m[16];
    for (int r = 0; r < 8; r++) {
        __m128i v = _mm_loadu_si128((const __m128i *)(block + r * 8));
        m[r * 2] = _mm_cvtepi32_ps(_mm_srai_epi32(_mm_unpacklo_epi16(v, v), 16));
        m[r * 2 + 1] = _mm_cvtepi32_ps(_mm_srai_epi32(_mm_unpackhi_epi16(v, v), 16));
    }
    transpose8(m);                  /* rows become columns: the row pass */
    idct_1d_ps(m, 0);
    idct_1d_ps(m, 1);
    transpose8(m);
    idct_1d_ps(m, 0);               /* the column pass */
    idct_1d_ps(m, 1);
    const __m128 quarter = _mm_set1_ps(0.25f), bias = _mm_set1_ps(32768.5f);
    const __m128i off = _mm_set1_epi32(32768);
    for (int r = 0; r < 8; r++) {
        __m128i lo = _mm_sub_epi32(_mm_cvttps_epi32(_mm_add_ps(_mm_mul_ps(m[r * 2], quarter), bias)), off);
        __m128i hi = _mm_sub_epi32(_mm_cvttps_epi32(_mm_add_ps(_mm_mul_ps(m[r * 2 + 1], quarter), bias)), off);
        out[r] = _mm_packs_epi32(lo, hi);
    }
}
#endif

} /* namespace */

void idct_put(const int16_t *block, unsigned rowMask, uint8_t *dst, int stride)
{
#if defined(GT3MEDIA_SSE2)
    (void)rowMask;
    __m128i v[8];
    idct_sse(block, v);
    for (int y = 0; y < 8; y++)
        _mm_storel_epi64((__m128i *)(dst + y * stride), _mm_packus_epi16(v[y], v[y]));
#else
    float f[64];
    idct_float(block, rowMask, f);
    for (int y = 0; y < 8; y++) {
        uint8_t *d = dst + y * stride;
        const float *s = f + y * 8;
        for (int x = 0; x < 8; x++)
            d[x] = clamp255(round_ref(s[x] * 0.25f));
    }
#endif
}

void idct_add(const int16_t *block, unsigned rowMask, uint8_t *dst, int stride)
{
#if defined(GT3MEDIA_SSE2)
    (void)rowMask;
    __m128i v[8];
    idct_sse(block, v);
    const __m128i z = _mm_setzero_si128();
    for (int y = 0; y < 8; y++) {
        uint8_t *d = dst + y * stride;
        __m128i p = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)d), z);
        _mm_storel_epi64((__m128i *)d, _mm_packus_epi16(_mm_add_epi16(p, v[y]), z));
    }
#else
    float f[64];
    idct_float(block, rowMask, f);
    for (int y = 0; y < 8; y++) {
        uint8_t *d = dst + y * stride;
        const float *s = f + y * 8;
        for (int x = 0; x < 8; x++)
            d[x] = clamp255(d[x] + round_ref(s[x] * 0.25f));
    }
#endif
}

/* DC only: f = F(0, 0) / 8 everywhere, rounded as floor(x + 0.5). */
void idct_put_dc(int dc, uint8_t *dst, int stride)
{
    uint8_t v = clamp255((dc + 4) >> 3);
    for (int y = 0; y < 8; y++) {
        uint8_t *d = dst + y * stride;
        for (int x = 0; x < 8; x++)
            d[x] = v;
    }
}

void idct_add_dc(int dc, uint8_t *dst, int stride)
{
    int v = (dc + 4) >> 3;
    if (v == 0)
        return;
    for (int y = 0; y < 8; y++) {
        uint8_t *d = dst + y * stride;
        for (int x = 0; x < 8; x++)
            d[x] = clamp255(d[x] + v);
    }
}

void idct_int(const int16_t *block, int *out)
{
#if defined(GT3MEDIA_SSE2)
    __m128i v[8];
    idct_sse(block, v);
    for (int r = 0; r < 8; r++) {
        alignas(16) int16_t row[8];
        _mm_store_si128((__m128i *)row, v[r]);
        for (int c = 0; c < 8; c++)
            out[r * 8 + c] = row[c];
    }
#else
    float f[64];
    idct_float(block, 0xFF, f);
    for (int i = 0; i < 64; i++)
        out[i] = round_ref(f[i] * 0.25f);
#endif
}

void idct_reference(const int16_t *block, int *out)
{
    static double c[8][8];
    static bool init = false;
    if (!init) {
        const double pi = 3.14159265358979323846;
        for (int k = 0; k < 8; k++)
            for (int n = 0; n < 8; n++)
                c[k][n] = (k == 0 ? std::sqrt(0.125) : 0.5) * std::cos((2 * n + 1) * k * pi / 16.0);
        init = true;
    }
    double tmp[64];
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            double s = 0;
            for (int u = 0; u < 8; u++)
                s += c[u][x] * block[y * 8 + u];
            tmp[y * 8 + x] = s;
        }
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++) {
            double s = 0;
            for (int v = 0; v < 8; v++)
                s += c[v][y] * tmp[v * 8 + x];
            out[y * 8 + x] = (int)std::floor(s + 0.5);
        }
}

} /* namespace gt3media */
