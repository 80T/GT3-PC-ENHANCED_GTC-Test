/* gs_tex1.h - 'Tex1' texture sets: reading them and decoding their textures.
 *
 * A Tex1 set (G001_flare.h Tex1Image, 0x30-byte header; engine/pgl/tex1.cpp)
 * is the unit every textured thing of the game uploads: menu images (.img,
 * .imgs packs, .pmb layouts), fonts (.fnt), the race HUD (display.imgs,
 * meter_*.img), car / course / wheel / tyre models (one set per texture of a
 * GTM1 ModelSet), sprite models, the flare and smoke textures.  It holds
 *   - nTex register blocks (GsTexRegs: TEX0, TEX1, MIPTBP1, MIPTBP2, CLAMP),
 *     one per texture (or per palette: a font's faces use tex[palette]);
 *   - nXfer upload records (Tex1Xfer: data, BITBLTBUF DBP / DBW / DPSM, w, h);
 *   - optionally the variation table (ptrTable: per variation a list of
 *     (texture << 23 | CPSM << 19 | CBP offset << 5 | CSA) rewrites of TEX0,
 *     tex1_select_variation 0x00248EC8) and the CLUT animation table
 *     (ClutAnimTable: channels blending key palettes into a CLUT's data).
 * VRAM addresses (TBP0, CBP, MIPTBPn, DBP) are relative to vramBase until
 * tex1_set_vram_base (0x002487B8) relinks the set for its VRAM block; the
 * uploads cover [vramBase, vramBase + vramSize) blocks.
 *
 * The register blocks do not say which upload holds which texture: the
 * uploads are VRAM rectangles, often in another format than the textures
 * read from them (docs/graphics.md: most sets send their T4 / T8 textures
 * and CLUTs as PSMCT32 rectangles of buffer width 64).  So a set is decoded
 * by laying all its uploads out in a TexSpace of its blocks (tex1_layout)
 * and reading each texture from it in its own format.
 */
#ifndef GT3PC_GS_TEX1_H
#define GT3PC_GS_TEX1_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "engine/pgl/native/gs_tex_decode.h"
#include "engine/pgl/native/gs_tex_format.h"
#include "engine/pgl/native/gs_tex_space.h"

namespace gstex {

struct Tex1Upload {
    const uint8_t *data = nullptr;  /* the pixels (in the caller's buffer) */
    size_t bytes = 0;               /* image_bytes(dpsm, w, h), cut to the buffer */
    Transfer t;                     /* dbp / dbw / dpsm, w, h (dsax = dsay = 0) */
};

struct Tex1Key {
    float time = 0;                 /* duration; negative: hold */
    const uint8_t *palette = nullptr;   /* nColors RGBA32 */
};

struct Tex1AnimChannel {
    uint16_t id = 0, mode = 0, nColors = 0, nKeys = 0;
    const uint8_t *dest = nullptr;  /* the CLUT colours it writes (inside an upload's data) */
    std::vector<Tex1Key> keys;
};

struct Tex1Set {
    size_t   offset = 0;            /* the header's place in the parsed buffer */
    uint32_t size = 0;              /* bytes to the next image of a pack */
    uint32_t vramBase = 0;          /* blocks */
    uint32_t vramSize = 0;          /* blocks */
    std::vector<TexRegs>    tex;
    std::vector<Tex1Upload> uploads;
    /* variations: per variation its list of TEX0 rewrites */
    std::vector<std::vector<uint32_t>> variations;
    std::vector<Tex1AnimChannel> anims;
};

/* Parses the set whose header is at `offset' of a file image (the pointer
 * fields hold offsets from offset - self: 0 for a lone .img, the GTM1 /
 * font file start for the sets inside those).  The upload data and the
 * palettes point into `file'.  False (and *err) when the header does not
 * fit or its tables run out of the buffer. */
bool tex1_parse(const uint8_t *file, size_t size, size_t offset, Tex1Set &out,
                std::string *err = nullptr);

/* The same for a set the game has relocated in place (tex1_relocate): the
 * pointer fields are addresses.  32-bit builds only (the game's pointers
 * are 32-bit); returns false elsewhere. */
bool tex1_from_memory(const void *img, Tex1Set &out);

/* The same from a copy of such a set's header (0x30 bytes), register blocks
 * (nTex x 0x28) and upload records (nXfer x 0xC), taken while nothing
 * changed them: the game relinks a set in place for every VRAM block it gets
 * (tex1_set_vram_base) - while the renderer's executor thread reads what the
 * game recorded a frame before.  The texels, CLUT variations and animations
 * are read where the copy points (relinking leaves them). */
bool tex1_from_copy(const uint8_t *hdr, const uint8_t *tex, const uint8_t *xfer, Tex1Set &out);

/* Finds every set in a buffer (the 'Tex1' magic at a 4-aligned offset
 * whose header parses), in file order.  For loose .img files, packs and
 * model files alike. */
std::vector<Tex1Set> tex1_scan(const uint8_t *file, size_t size);

/* Lays every upload of the set out in a space of its blocks. */
void tex1_layout(const Tex1Set &set, TexSpace &space);

/* The uploads that hold texels of texture i (all its mip levels) or its
 * CLUT: which Tex1Xfer records a GsTexRegs block uses. */
std::vector<size_t> tex1_texture_sources(const Tex1Set &set, size_t texIndex);

/* Rewrites TEX0.CPSM / CBP / CSA of the textures listed by variation
 * `index' (tex1_select_variation's rule; CBP = offset + vramBase).  False if
 * there is no such variation. */
bool tex1_select_variation(Tex1Set &set, int index);

/* One decoded texture: the rectangle (x, y, w, h) of its 2^TW x 2^TH space
 * (decode_region: the clamp region, or all of it) as RGBA8 (GS alpha). */
struct DecodedTexture {
    Rect rect;
    uint32_t level = 0;
    std::vector<uint32_t> rgba;     /* rect.w * rect.h */
};

/* Decodes mip level `level' of texture i from the laid-out space, loading
 * its CLUT the way pgl does (pgl_clut_load).  full: the whole texture space
 * instead of the clamp region. */
bool tex1_decode(const Tex1Set &set, const TexSpace &space, size_t texIndex, uint32_t level,
                 const Texa &texa, DecodedTexture &out, bool full = false);

} /* namespace gstex */

#endif /* GT3PC_GS_TEX1_H */
