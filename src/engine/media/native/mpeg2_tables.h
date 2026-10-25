/* mpeg2_tables.h - the MPEG-2 video tables of the native movie / IPU decoder.
 *
 * The variable-length codes of ISO/IEC 13818-2 Annex B as lookup tables,
 * the two scan orders, the default quantiser matrix and the non-linear
 * quantiser scale.  On the PS2 these tables live in the IPU: its VDEC
 * command decodes macroblock_address_increment, macroblock_type,
 * motion_code and dmvector (ipu_vdec_*, ipu.c 0x00264C70..0x00264E00), its
 * BDEC / IDEC commands the coded block pattern, the DC sizes and the DCT
 * coefficients (ipu_bdec 0x00263688, ipu_decode_image 0x00264E58).  The
 * native decoder reads the same codes in software.
 *
 * The codes are written below in the standard's form (bit strings) and the
 * lookup tables are built from them once, on first use; the builder checks
 * that each code table is prefix-free.
 */
#ifndef GT3_MEDIA_NATIVE_MPEG2_TABLES_H
#define GT3_MEDIA_NATIVE_MPEG2_TABLES_H

#include <cstdint>

namespace gt3media {

/* One entry of a VLC lookup table indexed by the next `bits' bits of the
 * stream: the decoded value and the code length (0: not a valid code). */
struct Vlc {
    int16_t value;
    uint8_t len;
    uint8_t pad;
};

/* One entry of a DCT coefficient table (B.14 / B.15).  run 64 = end of
 * block, run 65 = escape; len excludes the sign bit; len 0 = invalid. */
struct DctVlc {
    uint8_t run;
    uint8_t level;
    uint8_t len;
    uint8_t pad;
};

enum : uint8_t {
    DCT_EOB    = 64,
    DCT_ESCAPE = 65
};

/* macroblock_type flags (as the PS2 decoder's MB_* and the IPU's). */
enum : int {
    MBT_INTRA   = 0x01,
    MBT_PATTERN = 0x02,
    MBT_BWD     = 0x04,
    MBT_FWD     = 0x08,
    MBT_QUANT   = 0x10
};

/* Values of the macroblock_address_increment table beyond 1..33. */
enum : int {
    MBA_ESCAPE   = 34,  /* macroblock_escape: +33 */
    MBA_STUFFING = 35   /* macroblock_stuffing (MPEG-1): skip */
};

struct Mpeg2Tables {
    /* B.1 macroblock_address_increment, indexed by 11 bits. */
    Vlc mbaInc[2048];
    /* B.2 / B.3 / B.4 macroblock_type for I, P, B pictures (index 6 bits;
     * the I table only uses the top 2 of them). */
    Vlc mbType[4][64];          /* [picture_coding_type & 3]: 1 I, 2 P, 3 B */
    /* B.9 coded_block_pattern (4:2:0), 9 bits. */
    Vlc cbp[512];
    /* B.10 motion_code: |motion_code| by 10 bits; a sign bit follows a
     * nonzero code. */
    Vlc motionCode[1024];
    /* B.12 / B.13 dct_dc_size_luminance (9 bits) / _chrominance (10 bits). */
    Vlc dcLum[512];
    Vlc dcChrom[1024];
    /* B.14 (table zero) and B.15 (table one): A indexed by the next 9 bits
     * when the first 6 are not all zero, B by bits 6..15 when they are. */
    DctVlc dctA[2][512];
    DctVlc dctB[2][1024];
    /* scan[0] zigzag, scan[1] alternate: scan position -> natural index. */
    uint8_t scan[2][64];
    /* The default intra quantiser matrix, natural order. */
    uint8_t defaultIntra[64];
    /* quantiser_scale for q_scale_type 1, by quantiser_scale_code. */
    uint8_t nonLinearQ[32];
};

/* The tables (built on the first call; thread-safe). */
const Mpeg2Tables &mpeg2_tables();

/* Checks the built tables (prefix-free, complete where the standard is,
 * permutation scans); returns 0 and writes a message on failure. */
int mpeg2_tables_selftest(char *msg, int msgSize);

} /* namespace gt3media */

#endif
