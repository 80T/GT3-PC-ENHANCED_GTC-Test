/* gs_tex_layout.h - where the GS keeps a pixel in its local memory.
 *
 * The GS stores every format in pages (8 KB), each page cut into 32 blocks
 * of 256 bytes, each block into 4 columns of 64 bytes, with a block order
 * and a column order of its own per format (GS User's Manual, "Local Memory
 * Format").  The same bytes therefore hold different pixels when they are
 * written in one format and read in another, which the game relies on: most
 * 'Tex1' sets upload their 4-bit and 8-bit textures and their CLUTs as
 * PSMCT32 rectangles (see docs/graphics.md, "Upload / sample PSM
 * mismatches").  These functions give the address of a pixel so that such a
 * set can be laid out and read back in its texture format (TexSpace,
 * gs_tex_space.h); they do not model the GS itself.
 *
 * Addresses are bit addresses in the 4 MB space, not wrapped (block numbers
 * may pass 0x3FFF; TexSpace wraps them): bit = byte * 8 + the field's
 * lowest bit.  A field never crosses a byte boundary except the 16-, 24- and
 * 32-bit ones, which start on their own boundary.
 */
#ifndef GT3PC_GS_TEX_LAYOUT_H
#define GT3PC_GS_TEX_LAYOUT_H

#include <cstdint>

namespace gstex {

/* Block number (bp + page * 32 + the format's block table entry) of pixel
 * (x, y) of a buffer at block bp, bw * 64 pixels wide, in format psm.  Not
 * wrapped.  T8 / T4 buffers use bw / 2 pages per row (their pages are 128
 * pixels wide; their bw is even). */
uint32_t block_number(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y);

/* Bit address of pixel (x, y): block_number * 2048 + the position inside
 * the block (the column tables), + 24 for T8H / T4HL, + 28 for T4HH. */
uint32_t pixel_bit_address(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y);

/* Position of pixel (x, y) inside its block, in units of the format's
 * element: word (32-bit formats), halfword (16-bit), byte (T8), nibble (T4).
 * x, y are taken modulo the block size. */
uint32_t column_index32(uint32_t x, uint32_t y);    /* 0..63,  block  8 x 8  */
uint32_t column_index16(uint32_t x, uint32_t y);    /* 0..127, block 16 x 8  */
uint32_t column_index8(uint32_t x, uint32_t y);     /* 0..255, block 16 x 16 */
uint32_t column_index4(uint32_t x, uint32_t y);     /* 0..511, block 32 x 16 */

/* CSM1 CLUT order: the GS keeps a 256-entry CLUT as a 16 x 16 image whose
 * entry i sits at position clut_csm1_position(i) (row-major: x = p & 15,
 * y = p >> 4): bits 3 and 4 of the index swapped.  A 16-entry CLUT is an
 * 8 x 2 image in index order. */
inline uint32_t clut_csm1_position(uint32_t i)
{
    return (i & 0xE7u) | ((i & 0x08u) << 1) | ((i & 0x10u) >> 1);
}

/* Self test of the tables (block tables against their bit rules, column
 * tables are permutations); returns 0 when consistent, else a failure code.
 * Used by `texdump selftest'. */
int layout_self_test();

} /* namespace gstex */

#endif /* GT3PC_GS_TEX_LAYOUT_H */
