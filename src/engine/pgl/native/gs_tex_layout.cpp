/* gs_tex_layout.cpp - GS local memory layout of the pixel formats.
 *
 * The block tables give the block number of each block-sized cell of a
 * page (indexed [block row][block column]); the column functions give the
 * position of a pixel inside its block.  The tables are the GS's (GS User's
 * Manual, "Local Memory Format"); written for this port from their bit
 * rules (see layout_self_test).
 */
#include "engine/pgl/native/gs_tex_layout.h"
#include "engine/pgl/native/gs_tex_format.h"

namespace gstex {

namespace {

/* PSMCT32 / CT24 / T8H / T4HL / T4HH, and PSMT8: a page is 8 x 4 blocks. */
const uint8_t kBlock32[4][8] = {
    {  0,  1,  4,  5, 16, 17, 20, 21 },
    {  2,  3,  6,  7, 18, 19, 22, 23 },
    {  8,  9, 12, 13, 24, 25, 28, 29 },
    { 10, 11, 14, 15, 26, 27, 30, 31 },
};
/* PSMZ32 / Z24: the CT32 table with bits 3 and 4 flipped. */
const uint8_t kBlock32Z[4][8] = {
    { 24, 25, 28, 29,  8,  9, 12, 13 },
    { 26, 27, 30, 31, 10, 11, 14, 15 },
    { 16, 17, 20, 21,  0,  1,  4,  5 },
    { 18, 19, 22, 23,  2,  3,  6,  7 },
};
/* PSMCT16, and PSMT4: a page is 4 x 8 blocks. */
const uint8_t kBlock16[8][4] = {
    {  0,  2,  8, 10 },
    {  1,  3,  9, 11 },
    {  4,  6, 12, 14 },
    {  5,  7, 13, 15 },
    { 16, 18, 24, 26 },
    { 17, 19, 25, 27 },
    { 20, 22, 28, 30 },
    { 21, 23, 29, 31 },
};
/* PSMCT16S */
const uint8_t kBlock16S[8][4] = {
    {  0,  2, 16, 18 },
    {  1,  3, 17, 19 },
    {  8, 10, 24, 26 },
    {  9, 11, 25, 27 },
    {  4,  6, 20, 22 },
    {  5,  7, 21, 23 },
    { 12, 14, 28, 30 },
    { 13, 15, 29, 31 },
};
/* PSMZ16: the CT16 table with bits 3 and 4 flipped. */
const uint8_t kBlock16Z[8][4] = {
    { 24, 26, 16, 18 },
    { 25, 27, 17, 19 },
    { 28, 30, 20, 22 },
    { 29, 31, 21, 23 },
    {  8, 10,  0,  2 },
    {  9, 11,  1,  3 },
    { 12, 14,  4,  6 },
    { 13, 15,  5,  7 },
};
/* PSMZ16S: the CT16S table with bits 3 and 4 flipped. */
const uint8_t kBlock16SZ[8][4] = {
    { 24, 26,  8, 10 },
    { 25, 27,  9, 11 },
    { 16, 18,  0,  2 },
    { 17, 19,  1,  3 },
    { 28, 30, 12, 14 },
    { 29, 31, 13, 15 },
    { 20, 22,  4,  6 },
    { 21, 23,  5,  7 },
};

/* The column layout of T8 / T4: a 16 x 4 (T8) or 32 x 4 (T4) column is
 * stored in the 8 x 2 words of a 32-bit column.  Pixel rows 0 / 2 of the
 * column go to word row 0, rows 1 / 3 to word row 1; the word column is
 * x mod 8, turned by 4 words for rows 2, 3 of even columns and rows 0, 1 of
 * odd columns; the byte (nibble) inside the word is (y >> 1) & 1, plus 2 x
 * (x >> 3).  So each word holds the pixels x, x + 8 (+ 16, + 24 for T4) of
 * rows y, y + 2. */
inline uint32_t swizzle_word_x(uint32_t x, uint32_t y)
{
    uint32_t swap = ((y + 2) >> 2) & 1;     /* rows 2..5 of every 8 */
    return (x + swap * 4) & 7;
}

inline uint32_t swizzle_word_y(uint32_t y)
{
    return ((y >> 2) << 1) | (y & 1);       /* column pair (y / 4), row parity */
}

} /* namespace */

uint32_t column_index32(uint32_t x, uint32_t y)
{
    x &= 7;
    y &= 7;
    return (y >> 1) * 16 + (x >> 1) * 4 + (y & 1) * 2 + (x & 1);
}

uint32_t column_index16(uint32_t x, uint32_t y)
{
    x &= 15;
    y &= 7;
    /* two pixels in a word: x and x + 8 */
    return column_index32(x & 7, y) * 2 + (x >> 3);
}

uint32_t column_index8(uint32_t x, uint32_t y)
{
    x &= 15;
    y &= 15;
    uint32_t w = column_index32(swizzle_word_x(x, y), swizzle_word_y(y));
    return w * 4 + ((y >> 1) & 1) + ((x >> 3) & 1) * 2;
}

uint32_t column_index4(uint32_t x, uint32_t y)
{
    x &= 31;
    y &= 15;
    uint32_t w = column_index32(swizzle_word_x(x, y), swizzle_word_y(y));
    return w * 8 + ((y >> 1) & 1) + ((x >> 3) & 3) * 2;
}

uint32_t block_number(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
{
    switch (psm) {
    case PSMCT32: case PSMCT24: case PSMT8H: case PSMT4HL: case PSMT4HH:
        return bp + ((y >> 5) * bw + (x >> 6)) * 32 + kBlock32[(y >> 3) & 3][(x >> 3) & 7];
    case PSMZ32: case PSMZ24:
        return bp + ((y >> 5) * bw + (x >> 6)) * 32 + kBlock32Z[(y >> 3) & 3][(x >> 3) & 7];
    case PSMCT16:
        return bp + ((y >> 6) * bw + (x >> 6)) * 32 + kBlock16[(y >> 3) & 7][(x >> 4) & 3];
    case PSMCT16S:
        return bp + ((y >> 6) * bw + (x >> 6)) * 32 + kBlock16S[(y >> 3) & 7][(x >> 4) & 3];
    case PSMZ16:
        return bp + ((y >> 6) * bw + (x >> 6)) * 32 + kBlock16Z[(y >> 3) & 7][(x >> 4) & 3];
    case PSMZ16S:
        return bp + ((y >> 6) * bw + (x >> 6)) * 32 + kBlock16SZ[(y >> 3) & 7][(x >> 4) & 3];
    case PSMT8:
        return bp + ((y >> 6) * (bw >> 1) + (x >> 7)) * 32 + kBlock32[(y >> 4) & 3][(x >> 4) & 7];
    case PSMT4:
        return bp + ((y >> 7) * (bw >> 1) + (x >> 7)) * 32 + kBlock16[(y >> 4) & 7][(x >> 5) & 3];
    default:
        return bp;
    }
}

uint32_t pixel_bit_address(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
{
    uint32_t block = block_number(psm, bp, bw, x, y) * (GS_BLOCK_BYTES * 8);

    switch (psm) {
    case PSMCT32: case PSMCT24: case PSMZ32: case PSMZ24:
        return block + column_index32(x, y) * 32;
    case PSMT8H:
    case PSMT4HL:
        return block + column_index32(x, y) * 32 + 24;
    case PSMT4HH:
        return block + column_index32(x, y) * 32 + 28;
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S:
        return block + column_index16(x, y) * 16;
    case PSMT8:
        return block + column_index8(x, y) * 8;
    case PSMT4:
        return block + column_index4(x, y) * 4;
    default:
        return block;
    }
}

int layout_self_test()
{
    /* the block tables against their bit rules */
    for (uint32_t by = 0; by < 4; by++)
        for (uint32_t bx = 0; bx < 8; bx++) {
            uint32_t b = (bx & 1) | ((by & 1) << 1) | (((bx >> 1) & 1) << 2) |
                         (((by >> 1) & 1) << 3) | (((bx >> 2) & 1) << 4);
            if (kBlock32[by][bx] != b) return 1;
            if (kBlock32Z[by][bx] != (b ^ 0x18)) return 2;
        }
    for (uint32_t by = 0; by < 8; by++)
        for (uint32_t bx = 0; bx < 4; bx++) {
            uint32_t b = (by & 1) | ((bx & 1) << 1) | (((by >> 1) & 1) << 2) |
                         (((bx >> 1) & 1) << 3) | (((by >> 2) & 1) << 4);
            uint32_t s = (by & 1) | ((bx & 1) << 1) | (((by >> 2) & 1) << 2) |
                         (((by >> 1) & 1) << 3) | (((bx >> 1) & 1) << 4);
            if (kBlock16[by][bx] != b) return 3;
            if (kBlock16Z[by][bx] != (b ^ 0x18)) return 4;
            if (kBlock16S[by][bx] != s) return 5;
            if (kBlock16SZ[by][bx] != (s ^ 0x18)) return 6;
        }
    /* the column functions are permutations of their block */
    {
        uint8_t seen[512] = {};
        for (uint32_t y = 0; y < 8; y++)
            for (uint32_t x = 0; x < 8; x++)
                if (seen[column_index32(x, y)]++) return 7;
    }
    {
        uint8_t seen[512] = {};
        for (uint32_t y = 0; y < 8; y++)
            for (uint32_t x = 0; x < 16; x++)
                if (seen[column_index16(x, y)]++) return 8;
    }
    {
        uint8_t seen[512] = {};
        for (uint32_t y = 0; y < 16; y++)
            for (uint32_t x = 0; x < 16; x++)
                if (seen[column_index8(x, y)]++) return 9;
    }
    {
        uint8_t seen[512] = {};
        for (uint32_t y = 0; y < 16; y++)
            for (uint32_t x = 0; x < 32; x++)
                if (seen[column_index4(x, y)]++) return 10;
    }
    /* reference points of the T8 / T4 column layout (GS manual figures) */
    if (column_index8(0, 0) != 0 || column_index8(1, 0) != 4 || column_index8(8, 0) != 2 ||
        column_index8(0, 1) != 8 || column_index8(0, 2) != 33 || column_index8(4, 2) != 1 ||
        column_index8(0, 4) != 96 || column_index8(4, 4) != 64)
        return 11;
    if (column_index4(0, 0) != 0 || column_index4(1, 0) != 8 || column_index4(8, 0) != 2 ||
        column_index4(16, 0) != 4 || column_index4(0, 2) != 65 || column_index4(4, 2) != 1 ||
        column_index4(0, 4) != 192 || column_index4(4, 4) != 128)
        return 12;
    /* a page of each format covers its 32 blocks exactly once */
    const uint32_t psms[] = { PSMCT32, PSMZ32, PSMCT16, PSMCT16S, PSMZ16, PSMZ16S, PSMT8, PSMT4 };
    for (uint32_t psm : psms) {
        PsmGeom g = psm_geom(psm);
        uint32_t count[32] = {};
        for (uint32_t y = 0; y < g.pageH; y += g.blockH)
            for (uint32_t x = 0; x < g.pageW; x += g.blockW)
                count[block_number(psm, 0, psm == PSMT8 || psm == PSMT4 ? 2 : 1, x, y)]++;
        for (uint32_t b = 0; b < 32; b++)
            if (count[b] != 1) return 13;
    }
    return 0;
}

} /* namespace gstex */
