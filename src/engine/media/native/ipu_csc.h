/* ipu_csc.h - YCbCr 4:2:0 to RGBA8, as the PS2's IPU converts it.
 *
 * On the PS2 the movie player hands every decoded picture to the IPU's CSC
 * command (ipu_csc_rgb32 0x00263938 / ipu_csc_chunk 0x002639F0: RGB32, no
 * dithering) and IDEC converts the still images the same way
 * (ipu_decode_image 0x00264E58, OFM 0, DTE 0).  The IPU's conversion is a
 * documented integer BT.601 matrix (the coefficients below, as PCSX2's
 * yuv2rgb.cpp documents them):
 *
 *   lum = (0x95 * max(Y - 16, 0)) >> 6
 *   R = clamp((lum + ((0xCC * (Cr - 128)) >> 6) + 1) >> 1)
 *   G = clamp((lum + ((-0x68 * (Cr - 128)) >> 6) + ((-0x32 * (Cb - 128)) >> 6) + 1) >> 1)
 *   B = clamp((lum + ((0x102 * (Cb - 128)) >> 6) + 1) >> 1)
 *
 * with the chroma of each 2 x 2 block of pixels taken as it is (no
 * interpolation), and the alpha the IPU writes: 0x80 for every pixel with
 * the thresholds the game sets (SETTH 0 / 0, ipu_init 0x002641F0), the
 * GS's "1.0".  The caller can ask for another alpha (0xFF for a plain
 * RGBA image).
 */
#ifndef GT3_MEDIA_NATIVE_IPU_CSC_H
#define GT3_MEDIA_NATIVE_IPU_CSC_H

#include <cstdint>

namespace gt3media {

/* The alpha value the IPU's CSC produces with the game's thresholds. */
const uint8_t kIpuAlpha = 0x80;

/* Converts a w x h picture (w, h even) of 4:2:0 planes to RGBA8 (bytes
 * R, G, B, A) at dst, dstStride bytes per row. */
void ipu_csc_rgba(const uint8_t *y, int yStride, const uint8_t *cb, const uint8_t *cr, int cStride,
                  int w, int h, uint8_t *dst, int dstStride, uint8_t alpha = kIpuAlpha);

/* One pixel (reference / tests). */
void ipu_csc_pixel(int y, int cb, int cr, uint8_t rgb[3]);

/* Rearranges a linear RGBA8 picture into the PS2's macroblock layout: 16 x
 * 16 blocks of 0x400 bytes, in rows of blocks (IDEC output, what
 * MpegImage::drawIpic T249 uploads) or in columns of blocks (the movie
 * player's frames, column-major frame stores: what
 * MenuBgMovie::uploadFrame / uploadField T253 upload).  w, h multiples of
 * 16. */
void rgba_to_ps2_blocks(const uint8_t *src, int srcStride, int w, int h, uint8_t *dst, bool columnMajor);

/* The inverse. */
void ps2_blocks_to_rgba(const uint8_t *src, int w, int h, uint8_t *dst, int dstStride, bool columnMajor);

} /* namespace gt3media */

#endif
