/* ipu_image.h - the IPU still images ("ipum") of the native decoder.
 *
 * On the PS2 an IPU picture is decoded by one IDEC command over the whole
 * picture (ipu_decode_image, 0x00264E58, called by gt2k_menu::Ipic::decode
 * T250 for MpegImage::setIpic T249).  GT3's only IPU pictures are the 42
 * course stills of stream/course.ipic (gt2k_menu::IpicArchive: "ipua",
 * count, {offset, name[0x1C]} entries; read by ArcadeControl, T010).  The
 * launcher converts them to PNG files with this decoder (launcher/stills.h;
 * docs/movies.md "Stills"); the game reads the archive only for a still that
 * has none.
 *
 * The picture format (Sony's IPU stream, the IpuImageHeader record of
 * G002_mpeg_frameq.h):
 *   0x00 "ipum"
 *   0x04 u32 size     bytes from 0x10 to the end of the bitstream
 *   0x08 s16 width, 0x0A s16 height (multiples of 16)
 *   0x0C u32 frames   (1)
 *   0x10 u8  flags    bits 0-1 intra_dc_precision, 2 DTD (a dct_type bit
 *                     per macroblock), 4 alternate scan, 5 intra VLC
 *                     format (B.15), 6 q_scale_type, 7 MPEG-1; the PS2
 *                     loads them into IPU_CTRL (flags << 16, DTD cleared)
 *   0x11 the IDEC bitstream: per macroblock macroblock_type (I table: '1',
 *        or '01' + a 5-bit quantiser_scale_code), [dct_type], six intra
 *        blocks (DC differential + AC codes, 4:2:0), then a
 *        macroblock_address_increment ('1') before the next one; it ends
 *        with a start code (0x000001B0, 0x000001B1 in GT3's file).
 * IDEC starts with quantiser_scale_code 1 (the IDEC command's QSC), the
 * default intra matrix (ipu_init) and the DC predictors at
 * 128 << intra_dc_precision; each macroblock is colour-converted as the
 * CSC does (ipu_csc.h) and written out in order, rows of macroblocks.
 */
#ifndef GT3_MEDIA_NATIVE_IPU_IMAGE_H
#define GT3_MEDIA_NATIVE_IPU_IMAGE_H

#include <cstddef>
#include <cstdint>

#include "engine/media/native/ipu_csc.h"

namespace gt3media {

const size_t kIpuImageHeaderSize = 0x11;    /* header + flags byte */

struct IpuImageInfo {
    int width = 0;
    int height = 0;
    uint32_t size = 0;          /* header field: flags byte + bitstream */
    uint32_t frames = 0;
    uint8_t flags = 0;
};

enum class IpuLayout {
    Linear,                     /* rows of RGBA pixels, dstStride bytes apart */
    Macroblocks                 /* the IPU's output: 16 x 16 blocks of 0x400 bytes, rows of blocks */
};

struct IpuImageStats {
    int macroblocks = 0;        /* decoded */
    int quantChanges = 0;
    int fieldDct = 0;
    int addressJumps = 0;       /* increments other than 1 (DC predictors reset) */
    int tailBits = -1;          /* bits between the last macroblock and the end start code (-1: none found) */
    bool tailZero = false;      /* ...and they were all zero (the bitstream was consumed exactly) */
    uint32_t endCode = 0;       /* the start code that ends the picture (0x1B0) */
    int errors = 0;
    char error[96] = {0};
};

/* Reads the header; false if it is not an "ipum" picture. */
bool ipu_image_info(const void *data, size_t size, IpuImageInfo *info);

/* Decodes the picture at data (size bytes available from the header on;
 * SIZE_MAX trusts the header) into width * height RGBA8 pixels at dst.
 * dstStride: bytes per row for IpuLayout::Linear (0 = width * 4).
 * dstSize: the bytes at dst (SIZE_MAX: room for the whole picture); nothing
 * is written past them, a bigger picture is cut there (an error).
 * Returns false on an error (what was decoded stays; the rest is black). */
bool ipu_image_decode(const void *data, size_t size, uint8_t *dst, int dstStride,
                      IpuLayout layout = IpuLayout::Linear, uint8_t alpha = kIpuAlpha,
                      IpuImageStats *stats = nullptr, size_t dstSize = SIZE_MAX);

/* An IpicArchive file ("ipua"): count, then entries of {offset, name}. */
struct IpicEntryInfo {
    uint32_t offset;
    char name[0x1C + 1];
};

/* Number of entries (0 if not an archive); entry i into *e. */
int ipic_archive_count(const void *data, size_t size);
bool ipic_archive_entry(const void *data, size_t size, int i, IpicEntryInfo *e);

} /* namespace gt3media */

#endif
