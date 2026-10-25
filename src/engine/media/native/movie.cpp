/* movie.cpp - a PSS movie: demultiplexer, video, audio, colour conversion.
 * See movie.h. */
#include "engine/media/native/movie.h"

#include <vector>

namespace gt3media {

struct Movie::Impl {
    ByteSource src;
    MovieOptions opt;
    PssDemuxer demux;
    Mpeg2VideoDecoder video;
    PssAudioDecoder audio;
    MovieInfo info;
    bool demuxEnded = false;
    bool videoEos = false;
    bool videoEnded = false;
    int loops = 0;
    int64_t frameIndex = 0;
    int videoStreamId = -1;
    int64_t videoPacketsThisPass = 0;
    int64_t audioPackets = 0;
    bool sinkFormatKnown = false;
    std::vector<uint8_t> rgba;

    /* One PES packet into the decoders; false at the end of the data
     * (after a rewind when looping). */
    bool pump()
    {
        PesPacket p;
        if (!demux.next(p)) {
            if (opt.loop && videoPacketsThisPass > 0 && demux.rewind()) {
                loops++;
                videoPacketsThisPass = 0;
                if (opt.decodeAudio)
                    audio.restartStream();
                return true;
            }
            demuxEnded = true;
            return false;
        }
        if ((p.streamId & 0xF0) == 0xE0) {
            if (videoStreamId < 0)
                videoStreamId = p.streamId;
            if (p.streamId == videoStreamId) {
                if (info.firstVideoPts < 0 && p.pts >= 0)
                    info.firstVideoPts = p.pts;
                video.push(p.payload, p.size, p.pts);
                videoPacketsThisPass++;
            }
        } else if (p.streamId == 0xBD && p.subStreamId == 0xFF && opt.decodeAudio) {
            audioPackets++;
            if (opt.audioSink != nullptr) {
                opt.audioSink(opt.audioSinkUser, p.payload, p.size, p.pts);
                if (!sinkFormatKnown && !audio.format().valid)
                    audio.push(p.payload, p.size, p.pts);       /* the header, for info() */
                if (audio.format().valid) {
                    sinkFormatKnown = true;
                    audio.drop(audio.available());
                }
            } else {
                audio.push(p.payload, p.size, p.pts);
                size_t avail = audio.available();
                if (avail > opt.maxAudioFrames)
                    audio.drop(avail - opt.maxAudioFrames);
            }
        }
        return true;
    }
};

Movie::Movie() : impl_(new Impl) {}
Movie::~Movie() = default;

void Movie::close()
{
    impl_.reset(new Impl);
}

bool Movie::open(const ByteSource &src, const MovieOptions &opt)
{
    close();
    Impl &m = *impl_;
    m.src = src;
    m.opt = opt;
    m.demux.setSource(src);

    /* The sequence header (in front of the first picture). */
    while (!m.video.readHeaders()) {
        if (!m.pump() || m.demux.position() > (4u << 20))
            break;
    }
    const SequenceInfo &sq = m.video.sequence();
    if (!sq.valid || !sq.mpeg2)
        return false;

    /* The audio format: within the first 1 MB, if there is sound. */
    if (opt.decodeAudio) {
        while (!m.audio.format().valid && m.demux.position() < (1u << 20)) {
            if (!m.pump())
                break;
        }
    }

    MovieInfo &in = m.info;
    in.valid = true;
    in.width = sq.width;
    in.height = sq.height;
    in.frameRate = sq.frameRate > 0 ? sq.frameRate : 30000.0 / 1001;
    in.frameRateCode = sq.frameRateCode;
    in.aspectRatioCode = sq.aspectRatioCode;
    in.hasAudio = m.audio.format().valid;
    in.audio = m.audio.format();
    in.firstAudioPts = m.audio.firstPts();
    if (in.firstAudioPts >= 0 && in.firstVideoPts >= 0)
        in.audioDelay = (double)(in.firstAudioPts - in.firstVideoPts) / 90000.0;
    return true;
}

const MovieInfo &Movie::info() const
{
    return impl_->info;
}

bool Movie::nextFrame(MovieFrame &frame, uint8_t *rgbaDst, int rgbaStride)
{
    Impl &m = *impl_;
    if (!m.info.valid || m.videoEnded)
        return false;
    VideoPicture pic;
    for (;;) {
        if (m.video.next(pic))
            break;
        if (!m.pump()) {
            if (!m.videoEos) {
                m.video.setEndOfStream();
                m.videoEos = true;
                if (m.opt.decodeAudio)
                    m.audio.flush();
                continue;
            }
            m.videoEnded = true;
            return false;
        }
    }
    frame.index = m.frameIndex++;
    frame.time = (double)frame.index / m.info.frameRate;
    /* the pass this frame belongs to: GT3's movies end with one
     * sequence_end_code, so a looping movie's pass = the end codes decoded
     * before the picture (the demultiplexer's rewinds run ahead) */
    frame.loop = (int)pic.sequenceEnds;
    frame.width = pic.width;
    frame.height = pic.height;
    frame.picture = pic;
    frame.rgba = nullptr;
    frame.rgbaStride = 0;
    if (rgbaDst != nullptr) {
        if (rgbaStride == 0)
            rgbaStride = pic.width * 4;
        ipu_csc_rgba(pic.y, pic.yStride, pic.cb, pic.cr, pic.cStride, pic.width & ~1, pic.height & ~1,
                     rgbaDst, rgbaStride, m.opt.alpha);
        frame.rgba = rgbaDst;
        frame.rgbaStride = rgbaStride;
    } else if (m.opt.convertRgba) {
        m.rgba.resize((size_t)pic.width * pic.height * 4);
        ipu_csc_rgba(pic.y, pic.yStride, pic.cb, pic.cr, pic.cStride, pic.width & ~1, pic.height & ~1,
                     m.rgba.data(), pic.width * 4, m.opt.alpha);
        frame.rgba = m.rgba.data();
        frame.rgbaStride = pic.width * 4;
    }
    return true;
}

size_t Movie::readAudio(int16_t *dst, size_t frames)
{
    Impl &m = *impl_;
    if (!m.opt.decodeAudio || !m.info.hasAudio || m.opt.audioSink != nullptr)
        return 0;
    /* read on for the sound, buffering the video meanwhile (bounded: a
     * stretch without sound must not swallow the movie) */
    while (m.audio.available() < frames && !m.demuxEnded && m.video.buffered() < (32u << 20)) {
        if (!m.pump()) {
            m.audio.flush();
            break;
        }
    }
    return m.audio.read(dst, frames);
}

size_t Movie::audioAvailable() const
{
    return impl_->audio.available();
}

bool Movie::ended() const
{
    return impl_->videoEnded;
}

MovieStats Movie::stats() const
{
    MovieStats s;
    s.demux = impl_->demux.stats();
    s.video = impl_->video.stats();
    s.audio = impl_->audio.stats();
    s.loops = impl_->loops;
    return s;
}

} /* namespace gt3media */
