/* inflate.h - gzip / raw deflate decoder for tools/texdump (the .pmb
 * layouts keep most textures gzipped; RFC 1951 / 1952). */
#ifndef TEXDUMP_INFLATE_H
#define TEXDUMP_INFLATE_H

#include <cstddef>
#include <cstdint>
#include <vector>

/* Inflates the gzip member at src (header 1F 8B 08); false on a bad
 * stream.  *consumed (optional): bytes of the member read. */
bool gunzip(const uint8_t *src, size_t size, std::vector<uint8_t> &out, size_t *consumed = nullptr);

/* A raw deflate stream. */
bool inflate_raw(const uint8_t *src, size_t size, std::vector<uint8_t> &out, size_t *consumed = nullptr);

#endif
