/* inflate.cpp - gzip decompression for the installer (inflate.h).
 *
 * A plain RFC 1951 decoder: Huffman codes rebuilt from their code lengths
 * as canonical codes (3.2.2), with two things for speed, as the installer
 * unpacks 3.8 GB: a 64-bit bit buffer, and a lookup table that decodes every
 * code of up to 9 bits in one step (the longer, rarer codes walk the
 * canonical code bit by bit).  The output goes into a buffer sized from the
 * gzip trailer; writing past it is an error, so a damaged stream cannot run
 * away. */
#include "inflate.h"

#include "platform/ui_text.h"  /* PC (2026-11-16): the interface's languages */

#include <string.h>

namespace inflate {
namespace {

/* ---- bits ---------------------------------------------------------------- */

struct Bits {
    const uint8_t *p;
    size_t size;
    size_t pos = 0;         /* next byte to load */
    uint64_t buf = 0;       /* loaded bits not used yet, LSB first */
    int count = 0;
    size_t pad = 0;         /* zero bytes loaded past the end of the input */

    void fill()
    {
        while (count <= 56) {
            uint64_t byte = 0;
            if (pos < size)
                byte = p[pos++];
            else
                pad++;
            buf |= byte << count;
            count += 8;
        }
    }
    uint32_t need(int n)    /* n <= 32 */
    {
        if (count < n)
            fill();
        uint32_t v = (uint32_t)(buf & ((1ull << n) - 1));
        buf >>= n;
        count -= n;
        return v;
    }
    void drop(int n) { buf >>= n; count -= n; }
    void align() { drop(count & 7); }
    /* True once a bit past the input was used. */
    bool overrun() const { return pad * 8 > (size_t)count; }
};

/* The output, sized from the gzip trailer. */
struct Out {
    uint8_t *p;
    size_t n;
    size_t cap;
};

/* ---- Huffman tables -------------------------------------------------------- */

constexpr int kMaxBits = 15;
constexpr int kFastBits = 9;

struct Huffman {
    uint16_t count[kMaxBits + 1];   /* codes of each length */
    uint16_t symbol[320];           /* symbols in canonical order */
    uint16_t fast[1 << kFastBits];  /* next kFastBits input bits -> (length << 9) | symbol; 0: a longer code */
};

uint32_t reverse(uint32_t code, int len)
{
    uint32_t r = 0;
    for (int i = 0; i < len; i++, code >>= 1)
        r = (r << 1) | (code & 1);
    return r;
}

/* Builds h from code lengths; false for an over-subscribed set (an
 * incomplete set is allowed: a single distance code, for example). */
bool build(Huffman &h, const uint8_t *lengths, int n)
{
    memset(h.count, 0, sizeof(h.count));
    memset(h.fast, 0, sizeof(h.fast));
    for (int i = 0; i < n; i++)
        h.count[lengths[i]]++;
    if (h.count[0] == n)
        return true;
    int left = 1;
    for (int len = 1; len <= kMaxBits; len++) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0)
            return false;
    }
    uint16_t offs[kMaxBits + 1];
    offs[1] = 0;
    for (int len = 1; len < kMaxBits; len++)
        offs[len + 1] = (uint16_t)(offs[len] + h.count[len]);
    for (int i = 0; i < n; i++)
        if (lengths[i] != 0)
            h.symbol[offs[lengths[i]]++] = (uint16_t)i;

    /* The short codes: canonical code values, consecutive per length; the
     * stream holds a code's bits first bit first, so the table index is the
     * code reversed, with every value of the bits after it. */
    uint32_t code = 0;
    int index = 0;
    for (int len = 1; len <= kFastBits; len++) {
        for (int k = 0; k < h.count[len]; k++, index++, code++) {
            uint16_t entry = (uint16_t)((len << 9) | h.symbol[index]);
            for (uint32_t i = reverse(code, len); i < (1u << kFastBits); i += 1u << len)
                h.fast[i] = entry;
        }
        code <<= 1;
    }
    return true;
}

int decode(Bits &b, const Huffman &h)
{
    if (b.count < kMaxBits)
        b.fill();
    uint32_t e = h.fast[b.buf & ((1u << kFastBits) - 1)];
    if (e != 0) {
        b.drop((int)(e >> 9));
        return (int)(e & 0x1FF);
    }
    /* a longer code (or none): the canonical walk over the buffered bits */
    uint64_t bits = b.buf;
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= kMaxBits; len++) {
        code |= (int)(bits & 1);
        bits >>= 1;
        int count = h.count[len];
        if (code - count < first) {
            b.drop(len);
            return h.symbol[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

/* ---- blocks ----------------------------------------------------------------- */

const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                               35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                               3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool codes(Bits &b, Out &o, const Huffman &lit, const Huffman &dist)
{
    for (;;) {
        int sym = decode(b, lit);
        if (sym < 0 || b.overrun())
            return false;
        if (sym < 256) {
            if (o.n >= o.cap)
                return false;
            o.p[o.n++] = (uint8_t)sym;
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29)
                return false;
            size_t len = kLenBase[sym] + b.need(kLenExtra[sym]);
            int ds = decode(b, dist);
            if (ds < 0 || ds >= 30)
                return false;
            size_t d = kDistBase[ds] + b.need(kDistExtra[ds]);
            if (d > o.n || len > o.cap - o.n || b.overrun())
                return false;
            uint8_t *dst = o.p + o.n;
            const uint8_t *src = dst - d;
            if (d >= len) {
                memcpy(dst, src, len);
            } else {
                for (size_t i = 0; i < len; i++)
                    dst[i] = src[i];   /* overlapping copies repeat */
            }
            o.n += len;
        }
    }
}

bool fixed_block(Bits &b, Out &o)
{
    static Huffman lit, dist;
    static bool made = false;
    if (!made) {
        uint8_t l[288];
        int i = 0;
        for (; i < 144; i++) l[i] = 8;
        for (; i < 256; i++) l[i] = 9;
        for (; i < 280; i++) l[i] = 7;
        for (; i < 288; i++) l[i] = 8;
        build(lit, l, 288);
        uint8_t d[30];
        for (i = 0; i < 30; i++) d[i] = 5;
        build(dist, d, 30);
        made = true;
    }
    return codes(b, o, lit, dist);
}

bool dynamic_block(Bits &b, Out &o)
{
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int nlen = (int)b.need(5) + 257, ndist = (int)b.need(5) + 1, ncode = (int)b.need(4) + 4;
    if (nlen > 286 || ndist > 30)
        return false;
    uint8_t lengths[320] = {};
    for (int i = 0; i < ncode; i++)
        lengths[order[i]] = (uint8_t)b.need(3);
    Huffman lencode;
    if (!build(lencode, lengths, 19))
        return false;
    int index = 0;
    while (index < nlen + ndist) {
        int sym = decode(b, lencode);
        if (sym < 0 || b.overrun())
            return false;
        if (sym < 16) {
            lengths[index++] = (uint8_t)sym;
            continue;
        }
        uint8_t len = 0;
        int repeat;
        if (sym == 16) {
            if (index == 0)
                return false;
            len = lengths[index - 1];
            repeat = 3 + (int)b.need(2);
        } else if (sym == 17) {
            repeat = 3 + (int)b.need(3);
        } else {
            repeat = 11 + (int)b.need(7);
        }
        if (index + repeat > nlen + ndist)
            return false;
        while (repeat--)
            lengths[index++] = len;
    }
    if (lengths[256] == 0)
        return false;
    Huffman lit, dist;
    if (!build(lit, lengths, nlen) || !build(dist, lengths + nlen, ndist))
        return false;
    return codes(b, o, lit, dist);
}

bool stored_block(Bits &b, Out &o)
{
    b.align();
    uint32_t len = b.need(16), nlen = b.need(16);
    if ((len ^ 0xFFFF) != nlen || len > o.cap - o.n)
        return false;
    /* the whole bytes still in the bit buffer come first, then the input */
    while (len != 0 && b.count >= 8) {
        o.p[o.n++] = (uint8_t)b.need(8);
        len--;
    }
    if (b.overrun() || b.pos + len > b.size)
        return false;
    memcpy(o.p + o.n, b.p + b.pos, len);
    o.n += len;
    b.pos += len;
    return true;
}

/* The deflate blocks of `in` into `out` (sized already); *n the bytes
 * written.  False on a malformed stream or output past `out`'s size. */
bool blocks(const uint8_t *in, size_t size, std::vector<uint8_t> *out, size_t *n)
{
    Bits b{in, size};
    Out o{out->data(), 0, out->size()};
    for (;;) {
        const uint32_t last = b.need(1);
        const uint32_t type = b.need(2);
        bool ok = type == 0 ? stored_block(b, o) : type == 1 ? fixed_block(b, o) : type == 2 ? dynamic_block(b, o) : false;
        if (!ok || b.overrun()) {
            *n = o.n;
            return false;
        }
        if (last)
            break;
    }
    *n = o.n;
    return true;
}

} // namespace

uint32_t crc32(uint32_t crc, const uint8_t *p, size_t size)
{
    /* slicing by 4: table[k][i] = the CRC of byte i followed by k zero bytes */
    static uint32_t table[4][256];
    static bool made = false;
    if (!made) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[0][i] = c;
        }
        for (uint32_t i = 0; i < 256; i++)
            for (int k = 1; k < 4; k++)
                table[k][i] = (table[k - 1][i] >> 8) ^ table[0][table[k - 1][i] & 0xFF];
        made = true;
    }
    crc = ~crc;
    while (size >= 4) {
        uint32_t w;
        memcpy(&w, p, 4);
        crc ^= w;
        crc = table[3][crc & 0xFF] ^ table[2][(crc >> 8) & 0xFF] ^ table[1][(crc >> 16) & 0xFF] ^ table[0][crc >> 24];
        p += 4;
        size -= 4;
    }
    while (size--)
        crc = table[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

bool gunzip(const uint8_t *in, size_t size, std::vector<uint8_t> *out, std::wstring *error)
{
    out->clear();
    /* the member header (RFC 1952 2.3) */
    if (size < 18 || in[0] != 0x1F || in[1] != 0x8B || in[2] != 8) {
        *error = tr(L"not a gzip member");
        return false;
    }
    const uint8_t flags = in[3];
    size_t pos = 10;
    if (flags & 4) {                        /* FEXTRA */
        if (pos + 2 > size) { *error = tr(L"truncated gzip header"); return false; }
        pos += 2 + (in[pos] | (in[pos + 1] << 8));
    }
    if (flags & 8)                          /* FNAME */
        while (pos < size && in[pos++] != 0) {}
    if (flags & 16)                         /* FCOMMENT */
        while (pos < size && in[pos++] != 0) {}
    if (flags & 2)                          /* FHCRC */
        pos += 2;
    if (pos + 8 > size) {
        *error = tr(L"truncated gzip header");
        return false;
    }
    const uint8_t *trailer = in + size - 8;
    const uint32_t wantCrc = trailer[0] | (trailer[1] << 8) | (trailer[2] << 16) | ((uint32_t)trailer[3] << 24);
    const uint32_t wantSize = trailer[4] | (trailer[5] << 8) | (trailer[6] << 16) | ((uint32_t)trailer[7] << 24);
    if (wantSize > kMaxSize) {
        *error = tr(L"the gzip member is too large");
        return false;
    }
    out->resize(wantSize);
    size_t n = 0;
    if (!blocks(in + pos, size - 8 - pos, out, &n)) {
        *error = tr(L"damaged deflate data");
        out->clear();
        return false;
    }
    if (n != wantSize || crc32(0, out->data(), out->size()) != wantCrc) {
        *error = tr(L"the unpacked data does not match its gzip check");
        out->clear();
        return false;
    }
    return true;
}

bool inflate_raw(const uint8_t *in, size_t size, size_t out_size, std::vector<uint8_t> *out, std::wstring *error)
{
    out->clear();
    if (out_size > kMaxSize) {
        *error = L"the unpacked size is too large";
        return false;
    }
    out->resize(out_size);
    size_t n = 0;
    const bool ok = blocks(in, size, out, &n);
    if (!ok || n != out_size) {
        *error = ok ? L"the unpacked data has another size" : L"damaged deflate data";
        out->clear();
        return false;
    }
    return true;
}

} // namespace inflate
