/* ipu_csc.cpp - the IPU's YCbCr -> RGBA conversion.  See ipu_csc.h.
 *
 * Table driven: the luma term and the four chroma terms are looked up per
 * sample, the sums clamped through a table; one row pair shares a chroma
 * row. */
#include "engine/media/native/ipu_csc.h"

#include <cstring>

#if !defined(GT3MEDIA_NO_SSE2) && (defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2) || defined(__SSE2__))
#define GT3MEDIA_SSE2 1
#include <emmintrin.h>
#endif

namespace gt3media {

namespace {

struct CscTables {
    int16_t lum[256];       /* (0x95 * max(Y - 16, 0)) >> 6 */
    int16_t rcr[256];       /* (0xCC * (Cr - 128)) >> 6 */
    int16_t gcr[256];       /* (-0x68 * (Cr - 128)) >> 6 */
    int16_t gcb[256];       /* (-0x32 * (Cb - 128)) >> 6 */
    int16_t bcb[256];       /* (0x102 * (Cb - 128)) >> 6 */
    /* clamp((v + 1) >> 1) for v in [-1024, 2048) (lum + bcb reaches 1067) */
    uint8_t clamp[3072];

    CscTables()
    {
        for (int i = 0; i < 256; i++) {
            int yy = i - 16 < 0 ? 0 : i - 16;
            int c = i - 128;
            lum[i] = (int16_t)((0x95 * yy) >> 6);
            /* >> of a negative value: arithmetic (floor), as the IPU */
            rcr[i] = (int16_t)((0xCC * c) >> 6);
            gcr[i] = (int16_t)((-0x68 * c) >> 6);
            gcb[i] = (int16_t)((-0x32 * c) >> 6);
            bcb[i] = (int16_t)((0x102 * c) >> 6);
        }
        for (int v = -1024; v < 2048; v++) {
            int r = (v + 1) >> 1;
            clamp[v + 1024] = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
        }
    }
};

const CscTables &tables()
{
    static const CscTables t;
    return t;
}

} /* namespace */

void ipu_csc_pixel(int y, int cb, int cr, uint8_t rgb[3])
{
    const CscTables &t = tables();
    int l = t.lum[y];
    rgb[0] = t.clamp[1024 + l + t.rcr[cr]];
    rgb[1] = t.clamp[1024 + l + t.gcr[cr] + t.gcb[cb]];
    rgb[2] = t.clamp[1024 + l + t.bcb[cb]];
}

#if defined(GT3MEDIA_SSE2)
/* 16 pixels of one row: the same integer arithmetic in 16-bit lanes
 * (0x102 * c >> 6 is computed as 0x81 * c >> 5, which is equal and fits
 * in 16 bits). */
inline void csc16_sse2(const uint8_t *yrow, __m128i rc, __m128i gc, __m128i bc, __m128i a, uint8_t *d)
{
    const __m128i z = _mm_setzero_si128(), bias = _mm_set1_epi8(16), one = _mm_set1_epi16(1);
    const __m128i ky = _mm_set1_epi16(0x95);
    __m128i yv = _mm_subs_epu8(_mm_loadu_si128((const __m128i *)yrow), bias);
    __m128i rgba[4];
    for (int half = 0; half < 2; half++) {
        __m128i y16 = half ? _mm_unpackhi_epi8(yv, z) : _mm_unpacklo_epi8(yv, z);
        __m128i l = _mm_srli_epi16(_mm_mullo_epi16(y16, ky), 6);
        /* chroma terms of these 8 pixels: 4 values, each twice */
        __m128i r = half ? _mm_unpackhi_epi16(rc, rc) : _mm_unpacklo_epi16(rc, rc);
        __m128i g = half ? _mm_unpackhi_epi16(gc, gc) : _mm_unpacklo_epi16(gc, gc);
        __m128i b = half ? _mm_unpackhi_epi16(bc, bc) : _mm_unpacklo_epi16(bc, bc);
        r = _mm_srai_epi16(_mm_add_epi16(_mm_add_epi16(l, r), one), 1);
        g = _mm_srai_epi16(_mm_add_epi16(_mm_add_epi16(l, g), one), 1);
        b = _mm_srai_epi16(_mm_add_epi16(_mm_add_epi16(l, b), one), 1);
        __m128i r8 = _mm_packus_epi16(r, r), g8 = _mm_packus_epi16(g, g), b8 = _mm_packus_epi16(b, b);
        __m128i rg = _mm_unpacklo_epi8(r8, g8);
        __m128i ba = _mm_unpacklo_epi8(b8, a);
        rgba[half * 2] = _mm_unpacklo_epi16(rg, ba);
        rgba[half * 2 + 1] = _mm_unpackhi_epi16(rg, ba);
    }
    for (int i = 0; i < 4; i++)
        _mm_storeu_si128((__m128i *)(d + i * 16), rgba[i]);
}
#endif

void ipu_csc_rgba(const uint8_t *y, int yStride, const uint8_t *cb, const uint8_t *cr, int cStride,
                  int w, int h, uint8_t *dst, int dstStride, uint8_t alpha)
{
    const CscTables &t = tables();
    const uint8_t *clampT = t.clamp + 1024;
    const uint32_t a = (uint32_t)alpha << 24;
    for (int row = 0; row < h; row += 2) {
        const uint8_t *y0 = y + (size_t)row * yStride;
        const uint8_t *y1 = y0 + yStride;
        const uint8_t *u = cb + (size_t)(row >> 1) * cStride;
        const uint8_t *v = cr + (size_t)(row >> 1) * cStride;
        uint32_t *d0 = (uint32_t *)(dst + (size_t)row * dstStride);
        uint32_t *d1 = (uint32_t *)(dst + (size_t)(row + 1) * dstStride);
        bool second = row + 1 < h;
        int x = 0;
#if defined(GT3MEDIA_SSE2)
        const __m128i z = _mm_setzero_si128(), c128 = _mm_set1_epi16(128);
        const __m128i av = _mm_set1_epi8((char)alpha);
        for (; x + 16 <= w; x += 16) {
            __m128i cbv = _mm_sub_epi16(_mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(u + (x >> 1))), z), c128);
            __m128i crv = _mm_sub_epi16(_mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(v + (x >> 1))), z), c128);
            __m128i rc = _mm_srai_epi16(_mm_mullo_epi16(crv, _mm_set1_epi16(0xCC)), 6);
            __m128i gc = _mm_add_epi16(_mm_srai_epi16(_mm_mullo_epi16(crv, _mm_set1_epi16(-0x68)), 6),
                                       _mm_srai_epi16(_mm_mullo_epi16(cbv, _mm_set1_epi16(-0x32)), 6));
            __m128i bc = _mm_srai_epi16(_mm_mullo_epi16(cbv, _mm_set1_epi16(0x81)), 5);
            csc16_sse2(y0 + x, rc, gc, bc, av, (uint8_t *)(d0 + x));
            if (second)
                csc16_sse2(y1 + x, rc, gc, bc, av, (uint8_t *)(d1 + x));
        }
#endif
        for (; x < w; x += 2) {
            int cbv = u[x >> 1], crv = v[x >> 1];
            int rc = t.rcr[crv];
            int gc = t.gcr[crv] + t.gcb[cbv];
            int bc = t.bcb[cbv];
            int l;
#define CSC_PIX(dst, ysrc)                                                    \
            l = t.lum[(ysrc)];                                                \
            (dst) = (uint32_t)clampT[l + rc] | ((uint32_t)clampT[l + gc] << 8) | \
                    ((uint32_t)clampT[l + bc] << 16) | a;
            CSC_PIX(d0[x], y0[x]);
            CSC_PIX(d0[x + 1], y0[x + 1]);
            if (second) {
                CSC_PIX(d1[x], y1[x]);
                CSC_PIX(d1[x + 1], y1[x + 1]);
            }
#undef CSC_PIX
        }
    }
}

void rgba_to_ps2_blocks(const uint8_t *src, int srcStride, int w, int h, uint8_t *dst, bool columnMajor)
{
    int bw = w / 16, bh = h / 16;
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            int n = columnMajor ? bx * bh + by : by * bw + bx;
            uint8_t *d = dst + (size_t)n * 0x400;
            for (int r = 0; r < 16; r++)
                std::memcpy(d + r * 64, src + (size_t)(by * 16 + r) * srcStride + bx * 64, 64);
        }
    }
}

void ps2_blocks_to_rgba(const uint8_t *src, int w, int h, uint8_t *dst, int dstStride, bool columnMajor)
{
    int bw = w / 16, bh = h / 16;
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            int n = columnMajor ? bx * bh + by : by * bw + bx;
            const uint8_t *s = src + (size_t)n * 0x400;
            for (int r = 0; r < 16; r++)
                std::memcpy(dst + (size_t)(by * 16 + r) * dstStride + bx * 64, s + r * 64, 64);
        }
    }
}

} /* namespace gt3media */
