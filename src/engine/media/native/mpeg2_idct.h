/* mpeg2_idct.h - the 8x8 inverse DCT of the native decoder.
 *
 * On the PS2 the IPU does the IDCT inside BDEC / IDEC (ipu_bdec,
 * 0x00263688; the EE code then adds the result to the prediction:
 * mpeg_mb_put_block, 0x00260D48).  This is a separable floating-point
 * IDCT (even / odd decomposition) with the reference rounding
 * floor(x + 0.5): accurate to the IEEE 1180 reference, and fast enough
 * because most blocks are sparse (zero rows are skipped, DC-only blocks
 * take a shortcut).
 */
#ifndef GT3_MEDIA_NATIVE_MPEG2_IDCT_H
#define GT3_MEDIA_NATIVE_MPEG2_IDCT_H

#include <cstdint>

namespace gt3media {

/* block: 64 dequantised coefficients in natural (row-major) order.
 * rowMask: bit r set when row r has a nonzero coefficient (0xFF if
 * unknown).  The block is left as it was. */

/* dst = clamp(idct(block), 0, 255) (intra blocks). */
void idct_put(const int16_t *block, unsigned rowMask, uint8_t *dst, int stride);

/* dst = clamp(dst + idct(block), 0, 255) (non-intra blocks). */
void idct_add(const int16_t *block, unsigned rowMask, uint8_t *dst, int stride);

/* The same for a block whose only nonzero coefficient is the DC. */
void idct_put_dc(int dc, uint8_t *dst, int stride);
void idct_add_dc(int dc, uint8_t *dst, int stride);

/* The IDCT alone, rounded, into out[64] (tests). */
void idct_int(const int16_t *block, int *out);

/* The double-precision reference (tests). */
void idct_reference(const int16_t *block, int *out);

} /* namespace gt3media */

#endif
