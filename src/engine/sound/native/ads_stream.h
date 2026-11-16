/* ads_stream.h - the streamed BGM (.ads) as GT3's IOP played it.
 *
 * The file: a 0x28-byte head, then the body.
 *   0x00 "SShd"  0x04 0x18 (head size)  0x08 type (0x10 SPU2 ADPCM, 0x01
 *   16-bit PCM)  0x0C rate (Hz)  0x10 channels  0x14 interleave (bytes per
 *   channel per chunk)  0x18 loop start  0x1C loop end (-1: none)
 *   0x20 "SSbd"  0x24 body size
 * The body interleaves the channels by `interleave' bytes (GT3: 0x400 of
 * channel 0, 0x400 of channel 1, ...); each channel is one continuous ADPCM
 * stream (its first block flagged loop start, its last loop end + repeat).
 * All 66 GT3 files: ADPCM, 44100 Hz, stereo, interleave 0x400, no loop
 * points; the body is a whole number of 0x800-byte chunks.
 *
 * On the PS2, pdistr.irx ('PBGM') read the body into an IOP FIFO, and
 * pdispu2.irx copied it chunk by chunk into two 0x800-byte rings in SPU RAM
 * (0x1D8000 left, 0x1F8000 right) played by core 1 voices 22 / 23 at pitch
 * rate * 4096 / 48000 (0xEB3 for 44.1 kHz), ADSR1 0x000F / ADSR2 0x0000,
 * left voice VOLL only, right voice VOLR only, dry, no reverb; it patched
 * the loop flags of each half so the voices circled the ring, and an SPU IRQ
 * at every quarter refilled the free half.  A "clock" counted the samples
 * played: +0x700 (one chunk per channel) each time a voice entered a new
 * half, wrapping at the song's length (the wrap count is the status "id");
 * with the loop flag the body was re-read from its start without a break.
 *
 * Natively the voices take their blocks straight from this player (a
 * VoiceSource), so the ring buffers and IRQs disappear; the clock keeps the
 * IOP's chunk granularity.  The file is read through a byte-source callback
 * (offset-based, so a loop re-reads from the body start) into per-channel
 * FIFOs, by a feeder thread of the player's own (or by pump() for offline
 * rendering).  Only ADPCM is supported (pdispu2 sent PCM through the core's
 * ADMA input instead of voices; the JP disc's movies carry PCM, which the
 * movie service plays through sound_native.h's PCM output instead).
 *
 * Plain C++, no Windows or game dependency. */
#ifndef GT3PC_ADS_STREAM_H
#define GT3PC_ADS_STREAM_H

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "engine/sound/native/bgm_files.h"      /* AdsHeader, ads_parse_header, AdsReadFn */
#include "engine/sound/native/spu2.h"

namespace spu2 {

/* The stream status the IOP answered 'PBGM' function 6 with. */
struct AdsClock {
    int64_t  timeUs = 0;        /* position in microseconds */
    uint32_t pos = 0;           /* samples played (chunk granularity), wraps at the song's length */
    uint32_t loops = 0;         /* wraps so far ("id") */
    uint32_t total = 0;         /* the song's length in samples */
    bool     ended = false;     /* pos reached the length */
};

class AdsStream : public VoiceSource {
public:
    AdsStream();
    ~AdsStream() override;

    /* Reads and checks the head, resets the FIFOs and the clock, reads the
     * first chunks.  threaded: start the feeder thread (else call pump()).
     * fifoBytes: per channel (the IOP had 0x40000 for both).  False when the
     * file is not an ADPCM .ads (nothing is kept). */
    bool open(AdsReadFn read, void *user, bool loop, bool threaded, uint32_t fifoBytes = 0x20000);
    /* Push mode, for the movies: the stream's bytes (head included) arrive
     * through push() instead of a read callback.  pdistr's 'MPG1' service
     * fed the audio of a PSS movie (MPEG private stream 1, substream 0xFF
     * 0xA1, a 4-byte prefix per packet, then exactly an .ads image) to the
     * same stream player. */
    void open_push(uint32_t fifoBytes = 0x40000);
    /* Appends stream bytes; returns how many were taken (less when the
     * FIFOs are full: push the rest later).  -1 once the head proved not
     * to be an ADPCM .ads. */
    int  push(const void *data, uint32_t size);
    /* No more bytes: the voices end after what is buffered. */
    void push_end();
    /* The head has been parsed (push mode: after enough bytes). */
    bool header_ready() const;
    /* Bytes buffered for channel 0 (push mode: start the voices once a
     * chunk or two is in). */
    uint32_t buffered() const;
    /* Stops the feeder, forgets the file (the clock is kept for status). */
    void close();
    bool is_open() const { return open_; }
    const AdsHeader &header() const { return hdr_; }

    /* The voice pitch the IOP used: rate * 4096 / 48000. */
    uint16_t voice_pitch() const { return pitch_; }

    /* Which channel a voice plays (the native driver binds core 1 voice 22
     * to channel 0 and 23 to channel 1). */
    void bind_voice(int core, int voice, int channel);

    /* Reads from the file until the FIFOs are full (or the end); returns the
     * bytes read.  Called by the feeder thread, or by offline renderers. */
    uint32_t pump();

    /* While paused the voices get silent blocks and nothing advances. */
    void set_paused(bool paused);

    AdsClock clock() const;
    /* Samples the voices have taken so far (block granularity, 28 samples;
     * not wrapped at the song's length): a finer clock than clock()'s
     * chunks, for the movies' audio / video sync. */
    uint64_t played_samples() const;
    /* The rate the voices really play at: pitch x 48000 / 4096 (44097 Hz
     * for a 44.1 kHz stream at pitch 0xEB3). */
    uint32_t play_rate() const { return clockRate_; }

    /* VoiceSource: the next block of the voice's channel. */
    bool next_block(int core, int voice, uint8_t block[ADPCM_BLOCK_BYTES]) override;

    /* Diagnostics */
    uint32_t underruns() const { return underruns_; }

private:
    struct Fifo {
        std::vector<uint8_t> buf;
        uint32_t rd = 0, wr = 0, fill = 0;
    };

    void feeder_main();
    void clock_advance();
    void setup_locked(const AdsHeader &h, uint32_t fifoBytes);
    void deinterleave_locked(const uint8_t *src, uint32_t n);

    mutable std::mutex lock_;
    std::mutex pumpLock_;           /* one pump() at a time (feeder or caller) */
    std::condition_variable wake_;
    std::thread feeder_;
    bool quit_ = false;

    bool open_ = false;
    AdsReadFn read_ = nullptr;
    void *user_ = nullptr;
    bool loop_ = false;
    AdsHeader hdr_;
    uint16_t pitch_ = 0x1000;
    uint32_t clockRate_ = 48000;
    uint32_t srcPos_ = 0;           /* body bytes read (from the body start) */
    bool eof_ = false;              /* the body was read to its end (no loop) */
    Fifo fifo_[2];
    int voiceChannel_[2][VOICES_PER_CORE];
    uint32_t blocksPerChunk_ = 64;
    uint32_t samplesPerChunk_ = 0x700;
    uint64_t consumed_ = 0;         /* blocks taken by channel 0 */
    bool finalCounted_ = false;
    bool paused_ = false;
    AdsClock clock_;
    uint32_t underruns_ = 0;
    /* push mode */
    bool pushMode_ = false;
    bool headerReady_ = false;
    bool badStream_ = false;
    uint32_t pushFifoBytes_ = 0;
    std::vector<uint8_t> pushHead_;     /* the head while it arrives */
    std::vector<uint8_t> pushChunk_;    /* a chunk while it arrives */
};

} /* namespace spu2 */

#endif /* GT3PC_ADS_STREAM_H */
