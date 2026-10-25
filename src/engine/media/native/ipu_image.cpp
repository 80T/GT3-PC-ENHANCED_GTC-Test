/* ipu_image.cpp - IDEC of an IPU still image.  See ipu_image.h.
 *
 * What the IPU does for ipu_decode_image's command (IDEC, QSC 1, DTD from
 * the flags, RGB32 output without dithering), in software. */
#include "engine/media/native/ipu_image.h"

#include "engine/media/native/mpeg2_bits.h"
#include "engine/media/native/mpeg2_block.h"
#include "engine/media/native/mpeg2_idct.h"
#include "engine/media/native/mpeg2_tables.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace gt3media {

namespace {

uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int rd16s(const uint8_t *p)
{
    return (int16_t)(p[0] | (p[1] << 8));
}

void set_error(IpuImageStats &st, const char *msg)
{
    st.errors++;
    if (st.error[0] == 0)
        snprintf(st.error, sizeof(st.error), "%s", msg);
}

} /* namespace */

bool ipu_image_info(const void *data, size_t size, IpuImageInfo *info)
{
    const uint8_t *p = (const uint8_t *)data;
    if (size < kIpuImageHeaderSize || std::memcmp(p, "ipum", 4) != 0)
        return false;
    info->size = rd32(p + 4);
    info->width = rd16s(p + 8);
    info->height = rd16s(p + 10);
    info->frames = rd32(p + 12);
    info->flags = p[16];
    return info->width > 0 && info->height > 0 && (info->width & 15) == 0 && (info->height & 15) == 0;
}

bool ipu_image_decode(const void *data, size_t size, uint8_t *dst, int dstStride, IpuLayout layout,
                      uint8_t alpha, IpuImageStats *statsOut, size_t dstSize)
{
    IpuImageStats st;
    IpuImageInfo info;
    if (!ipu_image_info(data, size, &info)) {
        set_error(st, "not an ipum picture");
        if (statsOut)
            *statsOut = st;
        return false;
    }
    const int w = info.width, h = info.height;
    if (dstStride == 0)
        dstStride = w * 4;
    /* what fits in dstSize: the rows (Linear) or the macroblocks */
    auto rowFits = [&](int y) { return (size_t)y * dstStride + (size_t)w * 4 <= dstSize; };
    if (layout == IpuLayout::Linear) {
        for (int y = 0; y < h && rowFits(y); y++)
            std::memset(dst + (size_t)y * dstStride, 0, (size_t)w * 4);
    } else {
        std::memset(dst, 0, std::min((size_t)w * h * 4, dstSize));
    }

    /* The bitstream (size - 1 bytes after the flags), copied with the
     * reader's zero padding. */
    size_t avail = size == (size_t)-1 ? (size_t)info.size + 0x10 : size;
    size_t bsLen = info.size > 0 ? info.size - 1 : 0;
    if (kIpuImageHeaderSize + bsLen > avail)
        bsLen = avail > kIpuImageHeaderSize ? avail - kIpuImageHeaderSize : 0;
    std::vector<uint8_t> bs(bsLen + kBitReaderPadding, 0);
    std::memcpy(bs.data(), (const uint8_t *)data + kIpuImageHeaderSize, bsLen);
    BitReader br(bs.data(), bsLen);

    /* IPU_CTRL from the flags; IDEC's own fields. */
    const int idp = info.flags & 3;
    const bool dtd = (info.flags & 0x04) != 0;
    const bool altScan = (info.flags & 0x10) != 0;
    const bool ivf = (info.flags & 0x20) != 0;
    const bool qst = (info.flags & 0x40) != 0;
    const bool mpeg1 = (info.flags & 0x80) != 0;

    const Mpeg2Tables &t = mpeg2_tables();
    alignas(16) uint16_t wq[64];
    CoefParams cp;
    cp.tabA = t.dctA[ivf && !mpeg1 ? 1 : 0];
    cp.tabB = t.dctB[ivf && !mpeg1 ? 1 : 0];
    cp.scan = t.scan[altScan ? 1 : 0];
    cp.wq = wq;
    cp.mpeg1 = mpeg1;
    int curQ = -1;
    auto setQ = [&](int code) {
        /* IDEC's initial QSC is a code like the macroblocks' (the PS2
         * passes 1; with the non-linear scale 1 either way) */
        int q = qst ? t.nonLinearQ[code & 31] : (code & 31) * 2;
        if (q == curQ)
            return;
        curQ = q;
        for (int i = 0; i < 64; i++)
            wq[i] = (uint16_t)(t.defaultIntra[i] * q);
    };
    setQ(1);

    int dcPred[3];
    const int dcReset = 1 << (7 + idp);
    dcPred[0] = dcPred[1] = dcPred[2] = dcReset;

    alignas(16) int16_t blk[64];
    std::memset(blk, 0, sizeof(blk));
    alignas(16) uint8_t mbY[256], mbCb[64], mbCr[64];
    alignas(16) uint8_t rgba[16 * 64];

    const int bw = w / 16, bh = h / 16;
    const int total = bw * bh;
    bool ok = true;
    for (int n = 0; n < total; n++) {
        /* macroblock_type of an I picture */
        bool quant;
        if (br.peek(1)) {
            br.skip(1);
            quant = false;
        } else if (br.peek(2) == 1) {
            br.skip(2);
            quant = true;
        } else {
            set_error(st, "bad macroblock_type");
            ok = false;
            break;
        }
        if (quant) {
            setQ((int)br.get(5));
            st.quantChanges++;
        }
        int dctType = dtd ? br.getBit() : 0;
        if (dctType)
            st.fieldDct++;

        bool mbOk = true;
        for (int b = 0; b < 6 && mbOk; b++) {
            int cc = b < 4 ? 0 : b - 3;
            bool vlcOk = true;
            int diff = read_dc_diff(br, t, cc, vlcOk);
            if (!vlcOk) {
                mbOk = false;
                break;
            }
            dcPred[cc] += diff;
            blk[0] = (int16_t)(dcPred[cc] << (3 - idp));
            unsigned rowMask;
            int r = decode_intra_ac(br, cp, blk, rowMask);
            if (r == BLOCK_ERROR || br.overrun()) {
                clear_block(blk, 0xFF);
                mbOk = false;
                break;
            }
            uint8_t *d;
            int stride;
            if (b < 4) {
                if (dctType) {
                    d = mbY + (b & 1) * 8 + (b >> 1) * 16;
                    stride = 32;
                } else {
                    d = mbY + (b & 1) * 8 + (b >> 1) * 128;
                    stride = 16;
                }
            } else {
                d = b == 4 ? mbCb : mbCr;
                stride = 8;
            }
            if (r == BLOCK_DC_ONLY)
                idct_put_dc(blk[0], d, stride);
            else
                idct_put(blk, rowMask, d, stride);
            clear_block(blk, rowMask);
        }
        if (!mbOk) {
            set_error(st, "block VLC error");
            ok = false;
            break;
        }

        /* CSC of the macroblock, then out in rows of blocks */
        int mx = n % bw, my = n / bw;
        bool fits = layout == IpuLayout::Linear
                        ? (size_t)(my * 16 + 15) * dstStride + (size_t)(mx + 1) * 64 <= dstSize
                        : (size_t)(n + 1) * 0x400 <= dstSize;
        if (!fits) {
            set_error(st, "picture bigger than its buffer");
            ok = false;
            break;
        }
        if (layout == IpuLayout::Linear) {
            ipu_csc_rgba(mbY, 16, mbCb, mbCr, 8, 16, 16,
                         dst + (size_t)my * 16 * dstStride + (size_t)mx * 64, dstStride, alpha);
        } else {
            ipu_csc_rgba(mbY, 16, mbCb, mbCr, 8, 16, 16, rgba, 64, alpha);
            std::memcpy(dst + (size_t)n * 0x400, rgba, 0x400);
        }
        st.macroblocks++;

        if (n == total - 1) {
            /* what follows the last macroblock: zero bits up to the start
             * code that ends the picture */
            size_t here = br.bitPos();
            BitReader probe = br;
            if (probe.nextStartCode()) {
                st.tailBits = (int)(probe.bitPos() - here);
                st.endCode = probe.peek32() & 0x1FF;
                st.tailZero = true;
                for (size_t p = here; p < probe.bitPos(); p += 16) {
                    int nb = (int)std::min<size_t>(16, probe.bitPos() - p);
                    BitReader z = br;
                    z.setBitPos(p);
                    if (z.peek(nb) != 0)
                        st.tailZero = false;
                }
            }
            break;
        }
        /* macroblock_address_increment before the next macroblock; a
         * start code (or anything invalid) ends the picture early */
        int inc = 0;
        for (;;) {
            const Vlc &e = t.mbaInc[br.peek(11)];
            if (e.len == 0) {
                inc = -1;
                break;
            }
            br.skip(e.len);
            if (e.value == MBA_ESCAPE) {
                inc += 33;
                continue;
            }
            if (e.value == MBA_STUFFING)
                continue;
            inc += e.value;
            break;
        }
        if (inc < 0) {
            set_error(st, "picture ends early");
            ok = false;
            break;
        }
        if (inc != 1) {
            /* as the IPU: the predictors restart, the output does not
             * skip */
            st.addressJumps++;
            dcPred[0] = dcPred[1] = dcPred[2] = dcReset;
        }
    }
    if (statsOut)
        *statsOut = st;
    return ok;
}

int ipic_archive_count(const void *data, size_t size)
{
    const uint8_t *p = (const uint8_t *)data;
    if (size < 8 || std::memcmp(p, "ipua", 4) != 0)
        return 0;
    int n = (int)rd32(p + 4);
    if (n < 0 || (size_t)n * 0x20 + 8 > size)
        return 0;
    return n;
}

bool ipic_archive_entry(const void *data, size_t size, int i, IpicEntryInfo *e)
{
    int n = ipic_archive_count(data, size);
    if (i < 0 || i >= n)
        return false;
    const uint8_t *p = (const uint8_t *)data + 8 + (size_t)i * 0x20;
    e->offset = rd32(p);
    std::memcpy(e->name, p + 4, 0x1C);
    e->name[0x1C] = 0;
    return e->offset < size;
}

} /* namespace gt3media */
