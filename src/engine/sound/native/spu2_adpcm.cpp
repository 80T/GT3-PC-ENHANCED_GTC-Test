/* spu2_adpcm.cpp - SPU2 ADPCM block decoding (see spu2_adpcm.h). */
#include "engine/sound/native/spu2_adpcm.h"

namespace spu2 {

/* The prediction filters (x / 64): the same five pairs as the PS1 SPU and the
 * CD-XA decoder.  Filter numbers 5..15 decode as filter 0 on the SPU2. */
static const int32_t kFilter[16][2] = {
    {   0,   0 },
    {  60,   0 },
    { 115, -52 },
    {  98, -55 },
    { 122, -60 },
};

void adpcm_decode_block(const uint8_t block[ADPCM_BLOCK_BYTES], int16_t out[ADPCM_BLOCK_SAMPLES],
                        AdpcmHistory &h)
{
    const int shift = (block[0] & 0x0F) + 16;   /* applied to the nibble placed in bits 28..31 */
    const int32_t f0 = kFilter[block[0] >> 4][0];
    const int32_t f1 = kFilter[block[0] >> 4][1];
    int32_t p1 = h.prev1;
    int32_t p2 = h.prev2;

    for (int i = 0; i < ADPCM_BLOCK_SAMPLES; i++) {
        uint32_t byte = block[2 + (i >> 1)];
        uint32_t nib = (i & 1) ? (byte >> 4) : (byte & 0x0F);
        /* sign-extended nibble in bits 28..31, arithmetic shift down:
         * shift 0 keeps it at bits 12..15 (shifts 13..15 keep fewer bits) */
        int32_t s = (int32_t)(nib << 28) >> shift;
        s += (p1 * f0 + p2 * f1 + 32) >> 6;
        if (s > 32767)
            s = 32767;
        else if (s < -32768)
            s = -32768;
        out[i] = (int16_t)s;
        p2 = p1;
        p1 = s;
    }
    h.prev1 = p1;
    h.prev2 = p2;
}

bool adpcm_sample_extent(const uint8_t *data, uint32_t start, uint32_t limit,
                         uint32_t *length, int32_t *loopStart, bool *looped)
{
    int32_t loop = -1;
    for (uint32_t o = start; o + ADPCM_BLOCK_BYTES <= limit; o += ADPCM_BLOCK_BYTES) {
        uint8_t fl = data[o + 1];
        if (fl & ADPCM_FLAG_LOOP_START)
            loop = (int32_t)(o - start);
        if (fl & ADPCM_FLAG_LOOP_END) {
            *length = o + ADPCM_BLOCK_BYTES - start;
            *loopStart = loop;
            *looped = (fl & ADPCM_FLAG_LOOP) != 0;
            return true;
        }
    }
    *length = limit > start ? limit - start : 0;
    *loopStart = loop;
    *looped = false;
    return false;
}

} /* namespace spu2 */
