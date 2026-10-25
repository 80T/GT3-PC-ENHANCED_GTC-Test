/* ads_stream.cpp - the streamed BGM player (see ads_stream.h). */
#include "engine/sound/native/ads_stream.h"

#include <chrono>
#include <cstring>

namespace spu2 {

static inline uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool ads_parse_header(const uint8_t *head, uint32_t size, AdsHeader *out)
{
    if (size < ADS_HEAD_BYTES)
        return false;
    if (std::memcmp(head, "SShd", 4) != 0 || std::memcmp(head + 0x20, "SSbd", 4) != 0)
        return false;
    if (rd32(head + 4) != 0x18)
        return false;
    AdsHeader h;
    h.type = rd32(head + 0x08);
    h.rate = rd32(head + 0x0C);
    h.channels = rd32(head + 0x10);
    h.interleave = rd32(head + 0x14);
    h.loopStart = (int32_t)rd32(head + 0x18);
    h.loopEnd = (int32_t)rd32(head + 0x1C);
    h.bodySize = rd32(head + 0x24);
    if (h.channels < 1 || h.channels > 2 || h.interleave == 0 || (h.interleave & 15) || h.rate == 0)
        return false;
    *out = h;
    return true;
}

AdsStream::AdsStream()
{
    for (auto &c : voiceChannel_)
        for (int &v : c)
            v = -1;
}

AdsStream::~AdsStream()
{
    close();
}

bool AdsStream::open(AdsReadFn read, void *user, bool loop, bool threaded, uint32_t fifoBytes)
{
    close();

    uint8_t head[ADS_HEAD_BYTES];
    if (read == nullptr || read(user, 0, head, ADS_HEAD_BYTES) != (int)ADS_HEAD_BYTES)
        return false;
    AdsHeader h;
    if (!ads_parse_header(head, ADS_HEAD_BYTES, &h) || h.type != ADS_TYPE_ADPCM)
        return false;

    std::lock_guard<std::mutex> g(lock_);
    setup_locked(h, fifoBytes);
    read_ = read;
    user_ = user;
    loop_ = loop;
    pushMode_ = false;
    headerReady_ = true;
    open_ = true;
    quit_ = false;
    if (threaded)
        feeder_ = std::thread(&AdsStream::feeder_main, this);
    return true;
}

/* The stream's parameters, FIFOs and clock from its head. */
void AdsStream::setup_locked(const AdsHeader &h, uint32_t fifoBytes)
{
    hdr_ = h;
    srcPos_ = 0;
    eof_ = false;
    paused_ = false;
    consumed_ = 0;
    finalCounted_ = false;
    underruns_ = 0;
    /* FIFO: a power of two, at least a few chunks */
    uint32_t cap = 0x4000;
    while (cap < fifoBytes)
        cap <<= 1;
    while (cap < 4 * h.interleave)
        cap <<= 1;
    for (uint32_t c = 0; c < 2; c++) {
        fifo_[c].buf.assign(c < h.channels ? cap : 0, 0);
        fifo_[c].rd = fifo_[c].wr = fifo_[c].fill = 0;
    }
    /* pdispu2 0x0005A0: pitch = rate * 4096 / 48000 (the IOP's reciprocal
     * multiply rounds down the same way); its clock rate = pitch * 48000 /
     * 4096 */
    pitch_ = (uint16_t)(((uint64_t)h.rate << 12) / OUTPUT_RATE);
    clockRate_ = (uint32_t)(((uint64_t)pitch_ * OUTPUT_RATE) >> 12);
    blocksPerChunk_ = h.interleave / ADPCM_BLOCK_BYTES;
    samplesPerChunk_ = blocksPerChunk_ * ADPCM_BLOCK_SAMPLES;
    clock_ = AdsClock();
    /* pdistr 0x0001F0: the length = body / (16 * channels) * 28 samples */
    clock_.total = (uint32_t)((uint64_t)h.bodySize / (ADPCM_BLOCK_BYTES * h.channels) * ADPCM_BLOCK_SAMPLES);
}

/* A chunk of the body into the channel FIFOs: a whole chunk is `interleave'
 * bytes per channel; a short last chunk is split evenly.  The caller made
 * sure there is room. */
void AdsStream::deinterleave_locked(const uint8_t *src, uint32_t n)
{
    uint32_t channels = hdr_.channels, il = hdr_.interleave;
    uint32_t per = (n == il * channels) ? il : ((n / channels) & ~15u);
    for (uint32_t c = 0; c < channels; c++) {
        Fifo &f = fifo_[c];
        const uint8_t *p = src + c * per;
        uint32_t cap = (uint32_t)f.buf.size();
        for (uint32_t i = 0; i < per; i++) {
            f.buf[f.wr] = p[i];
            f.wr = (f.wr + 1) & (cap - 1);
        }
        f.fill += per;
    }
}

void AdsStream::open_push(uint32_t fifoBytes)
{
    close();
    std::lock_guard<std::mutex> g(lock_);
    pushMode_ = true;
    headerReady_ = false;
    badStream_ = false;
    pushFifoBytes_ = fifoBytes;
    pushHead_.clear();
    pushChunk_.clear();
    read_ = nullptr;
    user_ = nullptr;
    loop_ = false;
    eof_ = false;
    paused_ = false;
    clock_ = AdsClock();
    open_ = true;
    quit_ = false;
}

int AdsStream::push(const void *data, uint32_t size)
{
    std::lock_guard<std::mutex> g(lock_);
    if (!open_ || !pushMode_ || badStream_)
        return -1;
    const uint8_t *p = (const uint8_t *)data;
    uint32_t taken = 0;
    if (!headerReady_) {
        while (taken < size && pushHead_.size() < ADS_HEAD_BYTES)
            pushHead_.push_back(p[taken++]);
        if (pushHead_.size() < ADS_HEAD_BYTES)
            return (int)taken;
        AdsHeader h;
        if (!ads_parse_header(pushHead_.data(), ADS_HEAD_BYTES, &h) || h.type != ADS_TYPE_ADPCM) {
            badStream_ = true;
            return -1;
        }
        setup_locked(h, pushFifoBytes_);
        headerReady_ = true;
    }
    uint32_t chunk = hdr_.interleave * hdr_.channels;
    while (taken < size && srcPos_ < hdr_.bodySize) {
        uint32_t want = chunk;
        if (hdr_.bodySize - srcPos_ + (uint32_t)pushChunk_.size() < want)
            want = hdr_.bodySize - srcPos_ + (uint32_t)pushChunk_.size();
        if (pushChunk_.empty()) {
            /* only start a chunk there is room for */
            if ((uint32_t)fifo_[0].buf.size() - fifo_[0].fill < hdr_.interleave)
                break;
        }
        while (taken < size && pushChunk_.size() < want)
            pushChunk_.push_back(p[taken++]);
        if (pushChunk_.size() < want)
            break;
        deinterleave_locked(pushChunk_.data(), (uint32_t)pushChunk_.size());
        srcPos_ += (uint32_t)pushChunk_.size();
        pushChunk_.clear();
    }
    if (srcPos_ >= hdr_.bodySize)
        eof_ = true;
    return (int)taken;
}

void AdsStream::push_end()
{
    std::lock_guard<std::mutex> g(lock_);
    if (!open_ || !pushMode_)
        return;
    if (headerReady_ && !pushChunk_.empty())
        deinterleave_locked(pushChunk_.data(), (uint32_t)pushChunk_.size());
    pushChunk_.clear();
    eof_ = true;
}

bool AdsStream::header_ready() const
{
    std::lock_guard<std::mutex> g(lock_);
    return headerReady_;
}

uint32_t AdsStream::buffered() const
{
    std::lock_guard<std::mutex> g(lock_);
    return fifo_[0].fill;
}

void AdsStream::close()
{
    {
        std::lock_guard<std::mutex> g(lock_);
        quit_ = true;
    }
    wake_.notify_all();
    if (feeder_.joinable())
        feeder_.join();
    std::lock_guard<std::mutex> g(lock_);
    open_ = false;
    read_ = nullptr;
    user_ = nullptr;
    pushMode_ = false;
    pushHead_.clear();
    pushChunk_.clear();
    for (auto &f : fifo_) {
        f.buf.clear();
        f.rd = f.wr = f.fill = 0;
    }
    for (auto &c : voiceChannel_)
        for (int &v : c)
            v = -1;
}

void AdsStream::bind_voice(int core, int voice, int channel)
{
    std::lock_guard<std::mutex> g(lock_);
    voiceChannel_[core & 1][voice % VOICES_PER_CORE] = channel;
}

void AdsStream::set_paused(bool paused)
{
    std::lock_guard<std::mutex> g(lock_);
    paused_ = paused;
}

AdsClock AdsStream::clock() const
{
    std::lock_guard<std::mutex> g(lock_);
    return clock_;
}

uint64_t AdsStream::played_samples() const
{
    std::lock_guard<std::mutex> g(lock_);
    return consumed_ * ADPCM_BLOCK_SAMPLES;
}

uint32_t AdsStream::pump()
{
    std::lock_guard<std::mutex> pg(pumpLock_);
    uint32_t total = 0;
    std::vector<uint8_t> tmp;

    for (;;) {
        uint32_t n, at, chunk, il;
        AdsReadFn read;
        void *user;
        {
            std::lock_guard<std::mutex> g(lock_);
            if (!open_ || eof_ || quit_ || pushMode_)
                break;
            il = hdr_.interleave;
            chunk = il * hdr_.channels;
            uint32_t room = (uint32_t)fifo_[0].buf.size() - fifo_[0].fill;
            if (room < il)
                break;
            if (srcPos_ >= hdr_.bodySize) {
                if (loop_ && hdr_.bodySize > 0) {
                    srcPos_ = 0;        /* pdistr 0x00031C: seek back to the body */
                } else {
                    eof_ = true;
                    break;
                }
            }
            n = hdr_.bodySize - srcPos_;
            if (n > chunk)
                n = chunk;
            at = ADS_HEAD_BYTES + srcPos_;
            read = read_;
            user = user_;
        }

        tmp.resize(n);
        int got = read(user, at, tmp.data(), n);        /* outside the lock: file I/O */

        std::lock_guard<std::mutex> g(lock_);
        if (!open_ || quit_)
            break;
        if (got != (int)n) {
            eof_ = true;                /* a read error ends the song */
            break;
        }
        deinterleave_locked(tmp.data(), n);
        srcPos_ += n;
        total += n;
    }
    return total;
}

void AdsStream::feeder_main()
{
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(lock_);
            wake_.wait_for(lk, std::chrono::milliseconds(20), [&] {
                if (quit_)
                    return true;
                if (!open_ || eof_ || fifo_[0].buf.empty())
                    return false;
                return fifo_[0].buf.size() - fifo_[0].fill >= hdr_.interleave;
            });
            if (quit_)
                return;
        }
        pump();
    }
}

/* pdispu2 0x002438 (the stream clock): + one chunk; past the length it
 * wraps and counts the wrap. */
void AdsStream::clock_advance()
{
    uint32_t pos = clock_.pos + samplesPerChunk_;
    if (clock_.total < pos && clock_.total > 0) {
        pos -= clock_.total;
        clock_.loops++;
    }
    clock_.pos = pos;
    clock_.timeUs = (int64_t)((uint64_t)pos * 1000000u / (clockRate_ ? clockRate_ : 1));
    clock_.ended = pos >= clock_.total;
}

bool AdsStream::next_block(int core, int voice, uint8_t block[ADPCM_BLOCK_BYTES])
{
    bool wake = false;
    {
        std::lock_guard<std::mutex> g(lock_);
        int ch = voiceChannel_[core & 1][voice % VOICES_PER_CORE];
        if (!open_ || ch < 0 || ch >= (int)hdr_.channels)
            return false;
        if (paused_) {
            std::memset(block, 0, ADPCM_BLOCK_BYTES);
            return true;
        }
        Fifo &f = fifo_[ch];
        if (f.fill >= ADPCM_BLOCK_BYTES) {
            uint32_t cap = (uint32_t)f.buf.size();
            for (int i = 0; i < ADPCM_BLOCK_BYTES; i++) {
                block[i] = f.buf[f.rd];
                f.rd = (f.rd + 1) & (cap - 1);
            }
            f.fill -= ADPCM_BLOCK_BYTES;
            if (ch == 0) {
                /* a voice entering the next chunk: the IOP's IRQ at the
                 * start of a ring half advanced the clock by the chunk just
                 * played */
                if (consumed_ > 0 && consumed_ % blocksPerChunk_ == 0)
                    clock_advance();
                consumed_++;
            }
            wake = f.fill < (uint32_t)f.buf.size() / 2 && !eof_;
        } else if (eof_) {
            if (ch == 0 && !finalCounted_) {
                finalCounted_ = true;
                clock_advance();        /* the last chunk played out */
            }
            return false;
        } else {
            underruns_++;
            std::memset(block, 0, ADPCM_BLOCK_BYTES);
            wake = true;
        }
    }
    if (wake)
        wake_.notify_one();
    return true;
}

} /* namespace spu2 */
