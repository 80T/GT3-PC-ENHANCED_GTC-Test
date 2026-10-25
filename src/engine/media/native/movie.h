/* movie.h - a PSS movie: demultiplexer + MPEG-2 video + audio + IPU colour
 * conversion, the native replacement of the PS2's movie path.
 *
 * PS2 path (docs/movies.md): mpegif (T529) feeder thread -> pdimpeg IOP
 * stream (T522; audio played on the IOP) -> video PES packets ->
 * mpeg_player (G002) decoder thread with the IPU -> CSC to RGBA32 ->
 * 3-frame queue -> MenuBgMovie::update / upload (T253) once per frame.
 * Native path: Movie reads the .pss through a ByteSource, nextFrame()
 * gives the next picture in display order as RGBA8 (the IPU's colours)
 * and/or YCbCr planes, readAudio() the movie's sound as interleaved
 * 16-bit PCM.  No threads inside: the caller decides where it runs (the
 * port's decoder thread in place of the PS2's).
 *
 * Timing: frame n is shown at n / frameRate seconds after the first (the
 * PS2 shows one frame per two fields: 2 / 59.94 s, MenuBgMovie::update;
 * the movies are 29.97 or 30 fps); the audio starts with the first frame
 * (both streams' first PTS are equal in GT3's movies; info().audioDelay
 * has the difference).
 */
#ifndef GT3_MEDIA_NATIVE_MOVIE_H
#define GT3_MEDIA_NATIVE_MOVIE_H

#include <cstddef>
#include <cstdint>
#include <memory>

#include "engine/media/native/ipu_csc.h"
#include "engine/media/native/mpeg2_video.h"
#include "engine/media/native/pss_audio.h"
#include "engine/media/native/pss_demux.h"

namespace gt3media {

/* Raw audio: the private stream 1 payloads after the 4-byte sub-header,
 * in stream order (an .ads image: "SShd" header, then SPU2 ADPCM), for a
 * sound system that plays .ads streams itself (docs/sound.md: the port's
 * snd_native_bgm_push path, which takes exactly these bytes). */
typedef void (*MovieAudioSink)(void *user, const uint8_t *data, size_t size, int64_t pts);

struct MovieOptions {
    bool decodeAudio = true;        /* false: audio packets are skipped (mpegif's withSound 0) */
    MovieAudioSink audioSink = nullptr;     /* set: raw audio goes here instead of the decoder */
    void *audioSinkUser = nullptr;
    bool loop = false;              /* rewind at the end (the menu backgrounds, oneShot 0) */
    bool convertRgba = true;        /* nextFrame fills an RGBA8 picture */
    uint8_t alpha = kIpuAlpha;      /* alpha byte of the RGBA output (IPU: 0x80) */
    size_t maxAudioFrames = 44100 * 10;     /* decoded audio kept before the oldest is dropped */
};

struct MovieInfo {
    bool valid = false;
    int width = 0;                  /* display size */
    int height = 0;
    double frameRate = 0;
    int frameRateCode = 0;
    int aspectRatioCode = 0;
    bool hasAudio = false;
    PssAudioFormat audio;
    int64_t firstVideoPts = -1;     /* 90 kHz */
    int64_t firstAudioPts = -1;
    double audioDelay = 0;          /* seconds the audio starts after the first frame */
};

struct MovieFrame {
    int64_t index = 0;              /* display order, from 0, counting on across loops */
    double time = 0;                /* index / frameRate, seconds */
    int loop = 0;                   /* pass of a looping movie (0 first) */
    int width = 0;
    int height = 0;
    const uint8_t *rgba = nullptr;  /* RGBA8 (the caller's buffer or the movie's), null if not converted */
    int rgbaStride = 0;
    VideoPicture picture;           /* the YCbCr planes, type, PTS... */
};

struct MovieStats {
    DemuxStats demux;
    VideoStats video;
    AudioStats audio;
    int loops = 0;
};

class Movie {
public:
    Movie();
    ~Movie();
    Movie(const Movie &) = delete;
    Movie &operator=(const Movie &) = delete;

    /* Starts reading src (which must outlive the Movie or the next open):
     * reads up to the first picture to know the sequence (and the audio
     * format, when the movie has sound, within the first 1 MB).  False if
     * no MPEG-2 video sequence was found. */
    bool open(const ByteSource &src, const MovieOptions &opt = MovieOptions());
    void close();

    const MovieInfo &info() const;

    /* The next frame in display order.  With rgbaDst, the RGBA8 picture
     * goes there (rgbaStride bytes per row, 0 = width * 4); without it, to
     * an internal buffer (if options.convertRgba).  The planes and the
     * internal buffer stay valid until the next call.  False at the end. */
    bool nextFrame(MovieFrame &frame, uint8_t *rgbaDst = nullptr, int rgbaStride = 0);

    /* Up to `frames' sample frames of audio (interleaved 16-bit,
     * info().audio.channels per frame), reading on into the stream as
     * needed (the video is buffered meanwhile, up to 32 MB).  Fewer at the
     * end of the movie; 0 for a movie without sound. */
    size_t readAudio(int16_t *dst, size_t frames);

    /* Decoded audio sample frames waiting (without reading on). */
    size_t audioAvailable() const;

    /* The video has ended (no more frames). */
    bool ended() const;

    MovieStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} /* namespace gt3media */

#endif
