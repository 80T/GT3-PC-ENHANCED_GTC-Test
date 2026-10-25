/* png_write.cpp - a minimal PNG writer (stored deflate blocks). */
#include "png_write.h"

#include <cstdio>
#include <vector>

namespace {

uint32_t crc_table[256];
bool crc_ready = false;

void crc_init()
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
    crc_ready = true;
}

uint32_t crc32(const uint8_t *p, size_t n, uint32_t c = 0xFFFFFFFFu)
{
    if (!crc_ready)
        crc_init();
    for (size_t i = 0; i < n; i++)
        c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}

void put32be(std::vector<uint8_t> &v, uint32_t x)
{
    v.push_back((uint8_t)(x >> 24));
    v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));
    v.push_back((uint8_t)x);
}

void chunk(std::vector<uint8_t> &out, const char *type, const std::vector<uint8_t> &data)
{
    put32be(out, (uint32_t)data.size());
    size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    put32be(out, crc32(&out[start], out.size() - start) ^ 0xFFFFFFFFu);
}

} /* namespace */

bool png_write(const std::string &path, const uint32_t *rgba, uint32_t w, uint32_t h, bool withAlpha)
{
    uint32_t bpp = withAlpha ? 4 : 3;
    std::vector<uint8_t> raw;
    raw.reserve((size_t)(w * bpp + 1) * h);
    for (uint32_t y = 0; y < h; y++) {
        raw.push_back(0);                       /* filter: none */
        for (uint32_t x = 0; x < w; x++) {
            uint32_t p = rgba[(size_t)y * w + x];
            raw.push_back((uint8_t)p);
            raw.push_back((uint8_t)(p >> 8));
            raw.push_back((uint8_t)(p >> 16));
            if (withAlpha)
                raw.push_back((uint8_t)(p >> 24));
        }
    }

    /* zlib stream of stored blocks */
    std::vector<uint8_t> z;
    z.push_back(0x78);
    z.push_back(0x01);
    size_t pos = 0;
    do {
        size_t n = raw.size() - pos;
        if (n > 65535)
            n = 65535;
        bool last = (pos + n == raw.size());
        z.push_back(last ? 1 : 0);
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

    std::vector<uint8_t> out = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    std::vector<uint8_t> ihdr;
    put32be(ihdr, w);
    put32be(ihdr, h);
    ihdr.push_back(8);                          /* bit depth */
    ihdr.push_back(withAlpha ? 6 : 2);          /* RGBA / RGB */
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});

    FILE *f = std::fopen(path.c_str(), "wb");
    if (f == nullptr)
        return false;
    bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}
