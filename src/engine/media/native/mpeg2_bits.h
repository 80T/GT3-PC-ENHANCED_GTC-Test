/* mpeg2_bits.h - the bit reader of the native MPEG-2 / IPU decoder.
 *
 * On the PS2 the IPU reads the bitstream: FDEC peeks and skips bits
 * (ipu_showbits / ipu_flushbits / ipu_getbits, ipu.c 0x002640B0..), the
 * VDEC / BDEC / IDEC commands consume codes.  Here a reader over a byte
 * buffer does the same: a bit position and big-endian 32-bit loads.
 *
 * The buffer must be readable for kBitReaderPadding bytes past its end (the
 * owners pad it with zeros): the decoders check overrun() once per block,
 * and one block reads at most 64 escape codes (24 bits each) past it.
 */
#ifndef GT3_MEDIA_NATIVE_MPEG2_BITS_H
#define GT3_MEDIA_NATIVE_MPEG2_BITS_H

#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(_MSC_VER)
#include <stdlib.h>
#define GT3MEDIA_BSWAP32(x) _byteswap_ulong(x)
#else
#define GT3MEDIA_BSWAP32(x) __builtin_bswap32(x)
#endif

namespace gt3media {

/* Zero bytes the owner of a bitstream buffer keeps after its end. */
const size_t kBitReaderPadding = 256;

class BitReader {
public:
    BitReader() : data_(nullptr), sizeBits_(0), pos_(0) {}
    BitReader(const uint8_t *data, size_t size) { init(data, size); }

    void init(const uint8_t *data, size_t size)
    {
        data_ = data;
        sizeBits_ = size * 8;
        pos_ = 0;
    }

    /* Next n bits (1..25) without consuming them. */
    uint32_t peek(int n) const
    {
        uint32_t w;
        std::memcpy(&w, data_ + (pos_ >> 3), 4);
        w = GT3MEDIA_BSWAP32(w) << (pos_ & 7);
        return w >> (32 - n);
    }

    /* Next 32 bits. */
    uint32_t peek32() const
    {
        return (peek(16) << 16) | peekAt(16, 16);
    }

    /* n bits (1..25) starting `skip' bits ahead. */
    uint32_t peekAt(int skip, int n) const
    {
        size_t p = pos_ + (size_t)skip;
        uint32_t w;
        std::memcpy(&w, data_ + (p >> 3), 4);
        w = GT3MEDIA_BSWAP32(w) << (p & 7);
        return w >> (32 - n);
    }

    /* Signed: the next n bits as a two's complement value. */
    int32_t peekSigned(int n) const
    {
        return (int32_t)(peek(n) << (32 - n)) >> (32 - n);
    }

    void skip(int n) { pos_ += (size_t)n; }

    uint32_t get(int n)
    {
        uint32_t v = peek(n);
        pos_ += (size_t)n;
        return v;
    }

    uint32_t get32()
    {
        uint32_t v = peek32();
        pos_ += 32;
        return v;
    }

    int getBit() { return (int)get(1); }

    void alignByte() { pos_ = (pos_ + 7) & ~(size_t)7; }

    size_t bitPos() const { return pos_; }
    void setBitPos(size_t p) { pos_ = p; }
    size_t bytePos() const { return pos_ >> 3; }
    size_t bitsLeft() const { return pos_ >= sizeBits_ ? 0 : sizeBits_ - pos_; }
    bool overrun() const { return pos_ > sizeBits_; }
    const uint8_t *data() const { return data_; }
    size_t size() const { return sizeBits_ >> 3; }

    /* At a start code prefix (0x000001) at the next byte boundary? */
    bool atStartCode() const
    {
        size_t p = (pos_ + 7) >> 3;
        return p + 3 <= (sizeBits_ >> 3) && data_[p] == 0 && data_[p + 1] == 0 && data_[p + 2] == 1;
    }

    /* Moves to the next start code prefix at or after the current byte
     * boundary; returns false (at the end) if there is none. */
    bool nextStartCode()
    {
        size_t p = (pos_ + 7) >> 3;
        size_t end = sizeBits_ >> 3;
        while (p + 3 <= end) {
            if (data_[p + 2] > 1)
                p += 3;
            else if (data_[p] == 0 && data_[p + 1] == 0 && data_[p + 2] == 1) {
                pos_ = p * 8;
                return true;
            } else
                p++;
        }
        pos_ = end * 8;
        return false;
    }

private:
    const uint8_t *data_;
    size_t sizeBits_;
    size_t pos_;
};

} /* namespace gt3media */

#endif
