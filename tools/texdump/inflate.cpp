/* inflate.cpp - deflate (RFC 1951) and gzip (RFC 1952) decoding, written for
 * tools/texdump: canonical Huffman codes decoded bit by bit (small and
 * plain; speed does not matter here). */
#include "inflate.h"

#include <cstring>

namespace {

struct Bits {
    const uint8_t *p;
    size_t size;
    size_t pos = 0;     /* byte */
    uint32_t bit = 0;   /* bit in the byte */
    bool over = false;

    uint32_t get(uint32_t n)
    {
        uint32_t v = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (pos >= size) {
                over = true;
                return 0;
            }
            v |= (uint32_t)((p[pos] >> bit) & 1) << i;
            if (++bit == 8) {
                bit = 0;
                pos++;
            }
        }
        return v;
    }
    void align()
    {
        if (bit != 0) {
            bit = 0;
            pos++;
        }
    }
};

/* A canonical Huffman code: count of codes per length, symbols in code
 * order. */
struct Huffman {
    uint16_t count[16];
    uint16_t symbol[320];

    bool build(const uint8_t *lengths, int n)
    {
        std::memset(count, 0, sizeof(count));
        for (int i = 0; i < n; i++)
            count[lengths[i]]++;
        count[0] = 0;
        uint16_t offs[16];
        offs[1] = 0;
        for (int len = 1; len < 15; len++)
            offs[len + 1] = (uint16_t)(offs[len] + count[len]);
        for (int i = 0; i < n; i++)
            if (lengths[i] != 0)
                symbol[offs[lengths[i]]++] = (uint16_t)i;
        return true;
    }

    int decode(Bits &b) const
    {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; len++) {
            code |= (int)b.get(1);
            if (b.over)
                return -1;
            int c = count[len];
            if (code - first < c)
                return symbol[index + (code - first)];
            index += c;
            first = (first + c) << 1;
            code <<= 1;
        }
        return -1;
    }
};

const uint16_t kLenBase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
const uint8_t kLenExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
const uint16_t kDistBase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                 8193, 12289, 16385, 24577 };
const uint8_t kDistExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

bool codes(Bits &b, std::vector<uint8_t> &out, const Huffman &lit, const Huffman &dist)
{
    for (;;) {
        int s = lit.decode(b);
        if (s < 0)
            return false;
        if (s < 256) {
            out.push_back((uint8_t)s);
        } else if (s == 256) {
            return true;
        } else {
            s -= 257;
            if (s >= 29)
                return false;
            uint32_t len = kLenBase[s] + b.get(kLenExtra[s]);
            int d = dist.decode(b);
            if (d < 0 || d >= 30)
                return false;
            uint32_t back = kDistBase[d] + b.get(kDistExtra[d]);
            if (b.over || back > out.size())
                return false;
            size_t from = out.size() - back;
            for (uint32_t i = 0; i < len; i++)
                out.push_back(out[from + i]);
        }
    }
}

} /* namespace */

bool inflate_raw(const uint8_t *src, size_t size, std::vector<uint8_t> &out, size_t *consumed)
{
    Bits b{ src, size };
    uint32_t last;
    do {
        last = b.get(1);
        uint32_t type = b.get(2);
        if (b.over)
            return false;
        if (type == 0) {
            b.align();
            if (b.pos + 4 > size)
                return false;
            uint32_t len = src[b.pos] | (src[b.pos + 1] << 8);
            b.pos += 4;
            if (b.pos + len > size)
                return false;
            out.insert(out.end(), src + b.pos, src + b.pos + len);
            b.pos += len;
        } else if (type == 1) {
            uint8_t l[288], d[30];
            int i = 0;
            for (; i < 144; i++) l[i] = 8;
            for (; i < 256; i++) l[i] = 9;
            for (; i < 280; i++) l[i] = 7;
            for (; i < 288; i++) l[i] = 8;
            for (i = 0; i < 30; i++) d[i] = 5;
            Huffman lit, dist;
            lit.build(l, 288);
            dist.build(d, 30);
            if (!codes(b, out, lit, dist))
                return false;
        } else if (type == 2) {
            uint32_t nlen = b.get(5) + 257, ndist = b.get(5) + 1, ncode = b.get(4) + 4;
            static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
            uint8_t cl[19] = {};
            for (uint32_t i = 0; i < ncode; i++)
                cl[order[i]] = (uint8_t)b.get(3);
            Huffman clh;
            clh.build(cl, 19);
            uint8_t lens[320] = {};
            uint32_t n = 0;
            while (n < nlen + ndist) {
                int s = clh.decode(b);
                if (s < 0)
                    return false;
                if (s < 16) {
                    lens[n++] = (uint8_t)s;
                } else {
                    uint8_t v = 0;
                    uint32_t rep;
                    if (s == 16) {
                        if (n == 0)
                            return false;
                        v = lens[n - 1];
                        rep = 3 + b.get(2);
                    } else if (s == 17) {
                        rep = 3 + b.get(3);
                    } else {
                        rep = 11 + b.get(7);
                    }
                    if (n + rep > nlen + ndist)
                        return false;
                    while (rep--)
                        lens[n++] = v;
                }
            }
            Huffman lit, dist;
            lit.build(lens, (int)nlen);
            dist.build(lens + nlen, (int)ndist);
            if (!codes(b, out, lit, dist))
                return false;
        } else {
            return false;
        }
    } while (!last);
    b.align();
    if (consumed)
        *consumed = b.pos;
    return true;
}

bool gunzip(const uint8_t *src, size_t size, std::vector<uint8_t> &out, size_t *consumed)
{
    if (size < 18 || src[0] != 0x1F || src[1] != 0x8B || src[2] != 8)
        return false;
    uint8_t flg = src[3];
    size_t p = 10;
    if (flg & 4) {                              /* FEXTRA */
        if (p + 2 > size) return false;
        p += 2 + (src[p] | (src[p + 1] << 8));
    }
    if (flg & 8)                                /* FNAME */
        while (p < size && src[p++] != 0) {}
    if (flg & 16)                               /* FCOMMENT */
        while (p < size && src[p++] != 0) {}
    if (flg & 2)                                /* FHCRC */
        p += 2;
    if (p >= size)
        return false;
    size_t used = 0;
    if (!inflate_raw(src + p, size - p, out, &used))
        return false;
    if (consumed)
        *consumed = p + used + 8;
    return true;
}
