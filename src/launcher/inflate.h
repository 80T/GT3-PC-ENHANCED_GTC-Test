/* inflate.h - gzip decompression for the installer.
 *
 * GT3.VOL stores a third of its files gzip-packed (RoFS entry type 2); the
 * game's loose-file device wants every file plain, so the installer unpacks
 * them.  One gzip member (RFC 1952) holding raw deflate data (RFC 1951):
 * stored, fixed and dynamic Huffman blocks.  The member's CRC-32 and size
 * are checked, so a damaged image or a reader bug cannot pass unnoticed. */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace inflate {

/* The largest unpacked size accepted (GT3.VOL's largest file is 256 MB);
 * a damaged trailer then cannot ask for gigabytes. */
constexpr uint32_t kMaxSize = 1u << 30;

/* Unpacks the gzip member `in` (`size` bytes) into `out`.  False with a
 * message in `error` on a malformed stream or a CRC / size mismatch. */
bool gunzip(const uint8_t *in, size_t size, std::vector<uint8_t> *out, std::wstring *error);

/* Unpacks the raw deflate data `in` (`size` bytes, no wrapper) of a known
 * unpacked size `out_size` into `out` (CORE.GT3: install.cpp).  False with
 * a message in `error` on a malformed stream or another size. */
bool inflate_raw(const uint8_t *in, size_t size, size_t out_size, std::vector<uint8_t> *out, std::wstring *error);

/* CRC-32 (IEEE 802.3, as gzip uses it) of `size` bytes, continuing `crc`. */
uint32_t crc32(uint32_t crc, const uint8_t *p, size_t size);

} // namespace inflate
