/* png_write.h - a minimal PNG writer for tools/texdump (8-bit RGBA or RGB,
 * stored deflate blocks: no compression, no dependency). */
#ifndef TEXDUMP_PNG_WRITE_H
#define TEXDUMP_PNG_WRITE_H

#include <cstdint>
#include <string>

/* rgba: w * h pixels 0xAABBGGRR.  withAlpha false: RGB (alpha dropped). */
bool png_write(const std::string &path, const uint32_t *rgba, uint32_t w, uint32_t h,
               bool withAlpha = true);

#endif
