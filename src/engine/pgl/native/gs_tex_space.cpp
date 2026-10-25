/* gs_tex_space.cpp - a block range laid out the way the GS lays it out (see
 * the header). */
#include "engine/pgl/native/gs_tex_space.h"
#include "engine/pgl/native/gs_tex_layout.h"

#include <algorithm>
#include <cstring>

namespace gstex {

void TexSpace::reset(uint32_t firstBlock, uint32_t blockCount)
{
    first_ = firstBlock & (GS_MEM_BLOCKS - 1);
    count_ = std::min<uint32_t>(blockCount, GS_MEM_BLOCKS);
    mem_.assign((size_t)count_ * GS_BLOCK_BYTES, 0);
    written_.assign(count_, 0);
}

bool TexSpace::written(uint32_t block) const
{
    uint32_t rel = (block - first_) & (GS_MEM_BLOCKS - 1);
    return rel < count_ && written_[rel] != 0;
}

const uint8_t *TexSpace::block(uint32_t b) const
{
    uint32_t rel = (b - first_) & (GS_MEM_BLOCKS - 1);
    if (rel >= count_)
        return nullptr;
    return &mem_[(size_t)rel * GS_BLOCK_BYTES];
}

/* bit address -> byte offset in mem_ (the shift inside the byte is
 * bitAddr & 7: 0 or 4 for the nibble formats, else 0). */
bool TexSpace::locate(uint32_t bitAddr, uint32_t *offset) const
{
    uint32_t blockNo = bitAddr >> 11;                   /* 2048 bits a block */
    uint32_t rel = (blockNo - first_) & (GS_MEM_BLOCKS - 1);
    if (rel >= count_)
        return false;
    *offset = rel * GS_BLOCK_BYTES + ((bitAddr >> 3) & (GS_BLOCK_BYTES - 1));
    return true;
}

uint32_t TexSpace::read(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) const
{
    uint32_t bit = pixel_bit_address(psm, bp, bw, x, y);
    uint32_t off;
    if (!locate(bit, &off))
        return 0;
    const uint8_t *p = &mem_[off];
    switch (psm_field_bits(psm)) {
    case 32:
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    case 24:
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    case 16:
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
    case 8:
        return p[0];
    case 4:
        return (p[0] >> (bit & 4)) & 0xF;
    default:
        return 0;
    }
}

void TexSpace::write(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    uint32_t bit = pixel_bit_address(psm, bp, bw, x, y);
    uint32_t off;
    if (!locate(bit, &off))
        return;
    uint8_t *p = &mem_[off];
    switch (psm_field_bits(psm)) {
    case 32:
        p[3] = (uint8_t)(value >> 24);
        /* fall through */
    case 24:
        p[2] = (uint8_t)(value >> 16);
        /* fall through */
    case 16:
        p[1] = (uint8_t)(value >> 8);
        /* fall through */
    case 8:
        p[0] = (uint8_t)value;
        break;
    case 4: {
        uint32_t sh = bit & 4;
        p[0] = (uint8_t)((p[0] & ~(0xF << sh)) | ((value & 0xF) << sh));
        break;
    }
    }
    written_[off / GS_BLOCK_BYTES] = 1;
}

size_t TexSpace::upload(const Transfer &t, const void *data, size_t bytes)
{
    const uint8_t *src = static_cast<const uint8_t *>(data);
    uint32_t bitsPer = psm_transfer_bits(t.dpsm);
    uint64_t totalBits = (uint64_t)bytes * 8;
    size_t stored = 0;
    uint64_t bitPos = 0;

    for (uint32_t y = 0; y < t.h; y++) {
        for (uint32_t x = 0; x < t.w; x++, bitPos += bitsPer) {
            if (bitPos + bitsPer > totalBits)
                return stored;
            const uint8_t *s = src + (bitPos >> 3);
            uint32_t v;
            switch (bitsPer) {
            case 32: v = (uint32_t)s[0] | ((uint32_t)s[1] << 8) | ((uint32_t)s[2] << 16) | ((uint32_t)s[3] << 24); break;
            case 24: v = (uint32_t)s[0] | ((uint32_t)s[1] << 8) | ((uint32_t)s[2] << 16); break;
            case 16: v = (uint32_t)s[0] | ((uint32_t)s[1] << 8); break;
            case 8:  v = s[0]; break;
            default: v = (s[0] >> (bitPos & 4)) & 0xF; break;   /* 4: low nibble first */
            }
            /* TRXPOS: the destination wraps at 2048 */
            write(t.dpsm, t.dbp, t.dbw, (t.dsax + x) & 0x7FF, (t.dsay + y) & 0x7FF, v);
            stored++;
        }
    }
    return stored;
}

} /* namespace gstex */
