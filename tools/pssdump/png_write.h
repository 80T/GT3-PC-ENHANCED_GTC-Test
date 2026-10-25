/* png_write.h - a minimal PNG writer for the media test tools: RGB, 8 bits,
 * stored (uncompressed) deflate blocks.  No third-party code. */
#ifndef PSSDUMP_PNG_WRITE_H
#define PSSDUMP_PNG_WRITE_H

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace pngw {

inline uint32_t crc32(const uint8_t *p, size_t n, uint32_t crc = 0)
{
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; i++)
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

inline void put32be(std::vector<uint8_t> &v, uint32_t x)
{
    v.push_back((uint8_t)(x >> 24));
    v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));
    v.push_back((uint8_t)x);
}

inline void chunk(std::FILE *f, const char *type, const std::vector<uint8_t> &data)
{
    std::vector<uint8_t> c;
    put32be(c, (uint32_t)data.size());
    c.insert(c.end(), type, type + 4);
    c.insert(c.end(), data.begin(), data.end());
    put32be(c, crc32(c.data() + 4, c.size() - 4));
    std::fwrite(c.data(), 1, c.size(), f);
}

/* RGBA8 (alpha ignored) -> RGB PNG. */
inline bool write(const std::string &path, const uint8_t *rgba, int w, int h, int stride)
{
    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (!f)
        return false;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr;
    put32be(ihdr, (uint32_t)w);
    put32be(ihdr, (uint32_t)h);
    ihdr.push_back(8);
    ihdr.push_back(2);
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    chunk(f, "IHDR", ihdr);
    std::vector<uint8_t> raw;
    raw.reserve((size_t)(w * 3 + 1) * h);
    for (int y = 0; y < h; y++) {
        raw.push_back(0);
        const uint8_t *s = rgba + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            raw.push_back(s[x * 4]);
            raw.push_back(s[x * 4 + 1]);
            raw.push_back(s[x * 4 + 2]);
        }
    }
    std::vector<uint8_t> z;
    z.push_back(0x78);
    z.push_back(0x01);
    size_t pos = 0;
    do {
        size_t n = std::min<size_t>(65535, raw.size() - pos);
        z.push_back(pos + n == raw.size() ? 1 : 0);
        z.push_back((uint8_t)n);
        z.push_back((uint8_t)(n >> 8));
        z.push_back((uint8_t)~n);
        z.push_back((uint8_t)(~n >> 8));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
    } while (pos < raw.size());
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    put32be(z, (b << 16) | a);
    chunk(f, "IDAT", z);
    chunk(f, "IEND", std::vector<uint8_t>());
    std::fclose(f);
    return true;
}

} /* namespace pngw */

#endif
