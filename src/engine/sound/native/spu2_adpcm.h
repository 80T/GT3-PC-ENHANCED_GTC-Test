/* spu2_adpcm.h - SPU2 ADPCM ("VAG" blocks): the sample format of every GT3
 * sound bank (.ins, .es, music.ins) and of the streamed BGM (.ads).
 *
 * A block is 16 bytes and holds 28 samples:
 *   byte 0   bits 0-3 shift, bits 4-7 prediction filter (0..4; the SPU2
 *            treats 5..15 as filter 0)
 *   byte 1   flags: bit 0 loop end (the voice sets its ENDX bit and jumps to
 *            its loop start address), bit 1 loop / repeat (without it the
 *            voice stops at a loop end), bit 2 loop start (the voice takes
 *            this block's address as its loop start)
 *   2..15    28 signed 4-bit samples, low nibble first
 * sample = (nibble << 12) >> shift + (prev1 * f0 + prev2 * f1 + 32) >> 6,
 * clamped to 16 bits; the filter history continues from block to block
 * (also across a loop jump).  Hardware behaviour as documented by nocash's
 * psx-spx and PCSX2's SPU2 (reference only); the code is this port's.
 *
 * Plain C++, no Windows or game dependency. */
#ifndef GT3PC_SPU2_ADPCM_H
#define GT3PC_SPU2_ADPCM_H

#include <cstdint>

namespace spu2 {

enum : uint8_t {
    ADPCM_FLAG_LOOP_END   = 0x01,
    ADPCM_FLAG_LOOP       = 0x02,
    ADPCM_FLAG_LOOP_START = 0x04,
};

constexpr int ADPCM_BLOCK_BYTES   = 16;
constexpr int ADPCM_BLOCK_SAMPLES = 28;

/* The decoder history of one channel: the last two decoded samples. */
struct AdpcmHistory {
    int32_t prev1 = 0;
    int32_t prev2 = 0;
};

/* Decodes one block into 28 samples and updates the history. */
void adpcm_decode_block(const uint8_t block[ADPCM_BLOCK_BYTES], int16_t out[ADPCM_BLOCK_SAMPLES],
                        AdpcmHistory &h);

/* Walks a sample stored in SPU RAM (or a file image) from its start: the
 * byte length up to and including the block with the loop-end flag, the
 * byte offset of its loop start (-1 if none: the last block with the
 * loop-start flag before the end), and whether it loops (end flag with the
 * repeat bit).  limit bounds the walk.  Returns false when no end block is
 * found before limit. */
bool adpcm_sample_extent(const uint8_t *data, uint32_t start, uint32_t limit,
                         uint32_t *length, int32_t *loopStart, bool *looped);

/* Diagnostics: the block's header is one GT3's encoder writes (filter 0..4,
 * shift 0..12, flag byte 0..7).  All 4.6 million blocks of the disc's 1,216
 * sound banks pass; other data (PCM, headers, code) fails it in most
 * blocks.  The hardware itself decodes any header. */
inline bool adpcm_header_plausible(const uint8_t block[ADPCM_BLOCK_BYTES])
{
    return (block[0] >> 4) <= 4 && (block[0] & 0x0F) <= 12 && block[1] <= 7;
}

} /* namespace spu2 */

#endif /* GT3PC_SPU2_ADPCM_H */
