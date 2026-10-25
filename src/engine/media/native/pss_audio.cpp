/* pss_audio.cpp - the PSS movie audio decoder.  See pss_audio.h. */
#include "engine/media/native/pss_audio.h"

#include <cstring>

namespace gt3media {

namespace {

uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

const int kF0[5] = {0, 60, 115, 98, 122};
const int kF1[5] = {0, 0, -52, -55, -60};

} /* namespace */

bool spu2_adpcm_frame(const uint8_t *frame, int16_t *out, int outStep, int hist[2])
{
    int shift = frame[0] & 0x0F;
    int filter = (frame[0] >> 4) & 0x0F;
    bool ok = true;
    if (shift > 12) {
        shift = 9;
        ok = false;
    }
    if (filter > 4) {
        filter = 0;
        ok = false;
    }
    const int f0 = kF0[filter], f1 = kF1[filter];
    int s1 = hist[0], s2 = hist[1];
    for (int i = 0; i < 28; i++) {
        int nib = (frame[2 + (i >> 1)] >> ((i & 1) * 4)) & 0x0F;
        int s = (int16_t)(nib << 12) >> shift;
        s += (s1 * f0 + s2 * f1 + 32) >> 6;
        if (s > 32767)
            s = 32767;
        else if (s < -32768)
            s = -32768;
        out[i * outStep] = (int16_t)s;
        s2 = s1;
        s1 = s;
    }
    hist[0] = s1;
    hist[1] = s2;
    return ok;
}

void PssAudioDecoder::reset()
{
    fmt_ = PssAudioFormat();
    in_.clear();
    inPos_ = 0;
    pcm_.clear();
    pcmPos_ = 0;
    pcmChannels_ = 0;
    dataTaken_ = 0;
    std::memset(hist_, 0, sizeof(hist_));
    firstPts_ = -1;
    stats_ = AudioStats();
}

void PssAudioDecoder::push(const uint8_t *data, size_t size, int64_t pts)
{
    if (firstPts_ < 0 && pts >= 0)
        firstPts_ = pts;
    if (inPos_ > 0 && inPos_ >= in_.size() / 2) {
        in_.erase(in_.begin(), in_.begin() + (ptrdiff_t)inPos_);
        inPos_ = 0;
    }
    in_.insert(in_.end(), data, data + size);

    if (!fmt_.valid) {
        /* "SShd" <size> <size bytes> "SSbd" <data size> */
        size_t n = in_.size() - inPos_;
        const uint8_t *p = in_.data() + inPos_;
        if (n < 8)
            return;
        if (std::memcmp(p, "SShd", 4) != 0) {
            stats_.errors++;                        /* no header: drop the bytes */
            inPos_ = in_.size();
            return;
        }
        uint32_t hsize = rd32(p + 4);
        if (hsize < 0x18 || hsize > 0x100) {
            stats_.errors++;
            inPos_ = in_.size();
            return;
        }
        if (n < 8 + hsize + 8)
            return;
        const uint8_t *bd = p + 8 + hsize;
        if (std::memcmp(bd, "SSbd", 4) != 0) {
            stats_.errors++;
            inPos_ = in_.size();
            return;
        }
        fmt_.codec = rd32(p + 8);
        fmt_.sampleRate = (int)rd32(p + 12);
        fmt_.channels = (int)rd32(p + 16);
        fmt_.interleave = (int)rd32(p + 20);
        fmt_.loopStart = (int32_t)rd32(p + 24);
        fmt_.loopEnd = (int32_t)rd32(p + 28);
        fmt_.dataSize = rd32(bd + 4);
        bool ok = (fmt_.codec == PSS_AUDIO_ADPCM || fmt_.codec == PSS_AUDIO_PCM16) &&
                  fmt_.channels >= 1 && fmt_.channels <= 8 && fmt_.sampleRate > 0 &&
                  fmt_.interleave > 0 && (fmt_.interleave & 15) == 0;
        if (!ok || (pcmChannels_ != 0 && pcmChannels_ != fmt_.channels && available() != 0)) {
            stats_.errors++;
            fmt_ = PssAudioFormat();
            inPos_ = in_.size();
            return;
        }
        fmt_.valid = true;
        if (pcmChannels_ != fmt_.channels) {
            pcm_.clear();
            pcmPos_ = 0;
            pcmChannels_ = fmt_.channels;
        }
        inPos_ += 8 + hsize + 8;
    }
    decodeBlocks(false);
}

void PssAudioDecoder::flush()
{
    if (fmt_.valid)
        decodeBlocks(true);
}

void PssAudioDecoder::restartStream()
{
    flush();
    fmt_ = PssAudioFormat();
    in_.clear();
    inPos_ = 0;
    dataTaken_ = 0;
    std::memset(hist_, 0, sizeof(hist_));
}

/* Whole blocks (channels x interleave bytes); with `all' also a last
 * partial block, split evenly between the channels. */
void PssAudioDecoder::decodeBlocks(bool all)
{
    const int ch = fmt_.channels;
    const size_t block = (size_t)fmt_.interleave * ch;
    if (pcmPos_ > 0 && pcmPos_ >= pcm_.size() / 2) {
        pcm_.erase(pcm_.begin(), pcm_.begin() + (ptrdiff_t)pcmPos_);
        pcmPos_ = 0;
    }
    for (;;) {
        size_t n = in_.size() - inPos_;
        uint64_t left = fmt_.dataSize > dataTaken_ ? fmt_.dataSize - dataTaken_ : 0;
        if (left == 0) {
            inPos_ = in_.size();                    /* past SSbd: ignore */
            return;
        }
        if (n > left)
            n = (size_t)left;
        size_t per;                                 /* bytes per channel in this block */
        if (n >= block)
            per = fmt_.interleave;
        else if (all || n == left) {
            per = n / ch;
            per &= fmt_.codec == PSS_AUDIO_ADPCM ? ~(size_t)15 : ~(size_t)1;
            if (per == 0) {
                inPos_ += n;
                dataTaken_ += n;
                return;
            }
        } else {
            return;
        }
        const uint8_t *p = in_.data() + inPos_;
        size_t frames = fmt_.codec == PSS_AUDIO_ADPCM ? per / 16 * 28 : per / 2;
        size_t base = pcm_.size();
        pcm_.resize(base + frames * ch);
        int16_t *out = pcm_.data() + base;
        for (int c = 0; c < ch; c++) {
            const uint8_t *src = p + (size_t)c * per;
            if (fmt_.codec == PSS_AUDIO_ADPCM) {
                for (size_t f = 0; f < per / 16; f++) {
                    if (!spu2_adpcm_frame(src + f * 16, out + f * 28 * ch + c, ch, hist_[c]))
                        stats_.badFrames++;
                }
            } else {
                for (size_t s = 0; s < per / 2; s++)
                    out[s * ch + c] = (int16_t)(src[s * 2] | (src[s * 2 + 1] << 8));
            }
        }
        inPos_ += per * ch;
        dataTaken_ += per * ch;
        stats_.bytes += (int64_t)(per * ch);
        stats_.frames += (int64_t)frames;
    }
}

size_t PssAudioDecoder::read(int16_t *dst, size_t frames)
{
    size_t avail = available();
    if (frames > avail)
        frames = avail;
    size_t n = frames * pcmChannels_;
    if (n != 0)
        std::memcpy(dst, pcm_.data() + pcmPos_, n * sizeof(int16_t));
    pcmPos_ += n;
    return frames;
}

void PssAudioDecoder::drop(size_t frames)
{
    size_t avail = available();
    if (frames > avail)
        frames = avail;
    pcmPos_ += frames * pcmChannels_;
    stats_.dropped += (int64_t)frames;
}

} /* namespace gt3media */
