/* pss_audio.h - the audio stream of a PSS movie.
 *
 * On the PS2 the movie's sound never reaches the EE: the IOP module
 * pdimpeg (opened with mode 2 by mpegif::feed T529 when the movie has
 * sound; mode 3 drops it) plays it through the SPU2, held to the window
 * the game moves once per frame (MenuBgMovie::update T253 0x001CD1E0 ->
 * F_0025CFC0, the 'MPG2' RPC).  In GT3 only the opening and the two
 * ending movies have sound.
 *
 * The format (private stream 1, after the 4-byte sub-header the
 * demultiplexer strips): the same header as the game's .ads BGM files,
 *   "SShd", u32 0x18, u32 codec (0x10 SPU2 ADPCM, 0x01 16-bit PCM),
 *   u32 sample rate, u32 channels, u32 interleave, u32 loop start,
 *   u32 loop end;  "SSbd", u32 data bytes;
 * then the data: per channel `interleave' bytes in turn (L, R, L, R...).
 * GT3's movies: SPU2 ADPCM, 44100 Hz, stereo, interleave 0x400.
 *
 * SPU2 ADPCM: 16-byte frames of 28 samples, byte 0 = filter << 4 | shift,
 * byte 1 = loop flags (ignored for a stream), 14 bytes of 4-bit samples
 * (low nibble first); sample = (nibble << 12 >> shift) + (s1 * f0 + s2 *
 * f1 + 32) >> 6 with (f0, f1) = (0, 0), (60, 0), (115, -52), (98, -55),
 * (122, -60), clamped to 16 bits.
 */
#ifndef GT3_MEDIA_NATIVE_PSS_AUDIO_H
#define GT3_MEDIA_NATIVE_PSS_AUDIO_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gt3media {

enum : uint32_t {
    PSS_AUDIO_PCM16 = 0x01,
    PSS_AUDIO_ADPCM = 0x10
};

struct PssAudioFormat {
    bool valid = false;
    uint32_t codec = 0;
    int sampleRate = 0;
    int channels = 0;
    int interleave = 0;
    int32_t loopStart = -1;
    int32_t loopEnd = -1;
    uint32_t dataSize = 0;          /* SSbd */
};

struct AudioStats {
    int64_t bytes = 0;              /* data bytes taken */
    int64_t frames = 0;             /* sample frames decoded */
    int64_t dropped = 0;            /* sample frames dropped (buffer full) */
    int64_t badFrames = 0;          /* ADPCM frames with a reserved shift / filter */
    int64_t errors = 0;
};

/* Decodes one SPU2 ADPCM frame (16 bytes) into 28 samples; hist[0] =
 * previous sample, hist[1] = the one before.  Returns false for a frame
 * with a reserved shift (> 12) or filter (> 4) (decoded as shift 9 /
 * filter 0). */
bool spu2_adpcm_frame(const uint8_t *frame, int16_t *out, int outStep, int hist[2]);

class PssAudioDecoder {
public:
    void reset();

    /* Audio payload bytes (after the sub-header); pts of the packet. */
    void push(const uint8_t *data, size_t size, int64_t pts = -1);

    /* The stream ended: decode what is left of a partial block. */
    void flush();

    /* A new stream follows (a looping movie rewound): its header is read
     * again; decoded samples not read yet are kept. */
    void restartStream();

    const PssAudioFormat &format() const { return fmt_; }

    /* Decoded sample frames waiting. */
    size_t available() const { return pcmChannels_ > 0 ? (pcm_.size() - pcmPos_) / pcmChannels_ : 0; }

    /* Channels of the decoded samples (0 before the first header). */
    int channels() const { return pcmChannels_; }

    /* Up to `frames' sample frames (interleaved 16-bit) into dst. */
    size_t read(int16_t *dst, size_t frames);

    /* Throw away the oldest `frames' sample frames. */
    void drop(size_t frames);

    /* PTS of the first packet (90 kHz), -1 if none. */
    int64_t firstPts() const { return firstPts_; }

    /* Sample frames decoded since the start / reset. */
    int64_t decodedFrames() const { return stats_.frames; }

    const AudioStats &stats() const { return stats_; }

private:
    void decodeBlocks(bool all);

    PssAudioFormat fmt_;
    std::vector<uint8_t> in_;       /* undecoded bytes */
    size_t inPos_ = 0;
    std::vector<int16_t> pcm_;      /* decoded, interleaved */
    size_t pcmPos_ = 0;
    int pcmChannels_ = 0;
    uint64_t dataTaken_ = 0;        /* data bytes accepted (<= SSbd size) */
    int hist_[8][2] = {};
    int64_t firstPts_ = -1;
    AudioStats stats_;
};

} /* namespace gt3media */

#endif
