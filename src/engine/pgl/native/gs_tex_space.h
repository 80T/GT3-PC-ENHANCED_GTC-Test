/* gs_tex_space.h - a block range laid out the way the GS lays it out.
 *
 * TexSpace holds the bytes of a run of GS blocks (firstBlock ..
 * firstBlock + blockCount - 1, wrapping at 0x4000) for one upload group:
 * the uploads of a 'Tex1' set (tex1_upload_packet / tex1_upload_to_vram,
 * relative to its vramBase), or the uploads that cover one texture's
 * footprint.  upload() stores a host -> local transfer the way the GS
 * stores it (BITBLTBUF / TRXPOS / TRXREG, the format's page / block /
 * column order, gs_tex_layout.h), read() fetches a pixel in any format.  So
 * a texture uploaded in one format and sampled in another (the 'Tex1' sets'
 * 4-bit / 8-bit textures and CLUTs sent as PSMCT32 rectangles) reads back
 * as the GS would read it.
 *
 * This is the layout conversion those sets need, scoped to the set's own
 * data: it is not a model of the game's VRAM (there is no shared 4 MB
 * image; the renderer keeps VRAM addresses as handles, docs/graphics.md).
 */
#ifndef GT3PC_GS_TEX_SPACE_H
#define GT3PC_GS_TEX_SPACE_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "engine/pgl/native/gs_tex_format.h"

namespace gstex {

class TexSpace {
public:
    TexSpace() = default;
    TexSpace(uint32_t firstBlock, uint32_t blockCount) { reset(firstBlock, blockCount); }

    /* Empties it and makes it cover blockCount blocks from firstBlock (at
     * most the whole 0x4000). */
    void reset(uint32_t firstBlock, uint32_t blockCount);

    uint32_t firstBlock() const { return first_; }
    uint32_t blockCount() const { return count_; }
    bool contains(uint32_t block) const { return ((block - first_) & (GS_MEM_BLOCKS - 1)) < count_; }
    /* The block was written by an upload (or write()). */
    bool written(uint32_t block) const;

    /* A host -> local transfer of a w x h image of t.dpsm into the buffer
     * (dbp, dbw) at (dsax, dsay); data holds the pixels as the game sends
     * them (row-major, CT24 3 bytes, T4 / T4HL / T4HH two pixels a byte,
     * the first in the low nibble).  Pixels outside the space are dropped;
     * a short buffer stops the transfer where the data ends.  Returns the
     * pixels stored. */
    size_t upload(const Transfer &t, const void *data, size_t bytes);

    /* The raw value of pixel (x, y) of the buffer (bp, bw) read as psm
     * (CT24: the low 24 bits, T8H: the top byte, T4HL / T4HH: a nibble).
     * 0 outside the space. */
    uint32_t read(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) const;
    /* Writes the field of pixel (x, y) (the other bits of its word are
     * kept, as the GS keeps the alpha byte under a CT24 write). */
    void write(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t value);

    /* Raw access: the bytes of block b (256), nullptr outside. */
    const uint8_t *block(uint32_t b) const;

private:
    bool locate(uint32_t bitAddr, uint32_t *offset) const;

    uint32_t first_ = 0;
    uint32_t count_ = 0;
    std::vector<uint8_t> mem_;
    std::vector<uint8_t> written_;
};

} /* namespace gstex */

#endif /* GT3PC_GS_TEX_SPACE_H */
