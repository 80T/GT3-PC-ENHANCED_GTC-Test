/* mpeg2_video.h - the native MPEG-2 video decoder of the PSS movies.
 *
 * Replaces the PS2's IPU-assisted player (G002: mpeg_player.c,
 * 0x0025F390..0x00263368, with ipu.c's BDEC / VDEC / FDEC wrappers): the
 * same job - an MPEG-2 video elementary stream in, decoded pictures out in
 * display order - done entirely in software.
 *
 * Supported: MPEG-2 main profile, 4:2:0, frame pictures (I / P / B), frame
 * and field motion compensation in frame pictures, frame and field DCT,
 * both VLC tables, both scans, linear / non-linear quantiser scale, 8..11
 * bit intra DC, loaded and default matrices, concealment motion vectors.
 * Not supported (reported in the stats, the picture concealed): field
 * pictures, dual-prime prediction, MPEG-1 video, 4:2:2 / 4:4:4.  GT3's
 * 51 movies are all progressive frame pictures with frame prediction (see
 * docs/movies.md); the PS2 player itself only handles that much (its motion
 * compensation uses the first vector of each direction, frame prediction).
 *
 * Usage: push() elementary-stream bytes (with the PES PTS where the packet
 * had one), call next() until it returns false (it needs more data), push
 * more; setEndOfStream() at the end so the last reference picture comes
 * out.  A picture returned by next() stays valid until the next call to
 * next(), push() or reset().
 */
#ifndef GT3_MEDIA_NATIVE_MPEG2_VIDEO_H
#define GT3_MEDIA_NATIVE_MPEG2_VIDEO_H

#include <cstddef>
#include <cstdint>
#include <memory>

namespace gt3media {

/* sequence_header + sequence_extension. */
struct SequenceInfo {
    bool valid = false;
    bool mpeg2 = false;             /* a sequence_extension was seen */
    int width = 0;                  /* horizontal_size */
    int height = 0;                 /* vertical_size */
    int codedWidth = 0;             /* macroblock-aligned */
    int codedHeight = 0;
    int aspectRatioCode = 0;
    int frameRateCode = 0;
    double frameRate = 0;           /* pictures per second */
    int bitRate = 0;                /* bits per second */
    int profileLevel = 0;
    bool progressiveSequence = false;
    int chromaFormat = 0;           /* 1 = 4:2:0 */
    bool loadIntraMatrix = false;
    bool loadNonIntraMatrix = false;
};

/* A decoded picture, in display order.  8-bit planes, 4:2:0. */
struct VideoPicture {
    const uint8_t *y = nullptr;
    const uint8_t *cb = nullptr;
    const uint8_t *cr = nullptr;
    int yStride = 0;
    int cStride = 0;
    int width = 0;                  /* display size: horizontal_size x vertical_size */
    int height = 0;
    int codedWidth = 0;             /* plane size (macroblock-aligned) */
    int codedHeight = 0;
    int type = 0;                   /* picture_coding_type: 1 I, 2 P, 3 B */
    int temporalReference = 0;
    int64_t pts = -1;               /* 90 kHz, -1 when its PES packet had none */
    bool topFieldFirst = false;
    bool repeatFirstField = false;
    bool progressiveFrame = true;
    int concealedMbs = 0;           /* macroblocks not decoded (errors) */
    int64_t decodeIndex = 0;        /* pictures decoded before this one */
    int64_t sequenceEnds = 0;       /* sequence_end_codes before this picture (a looping movie: its pass) */
};

/* Counters over everything decoded (what the movies actually use). */
struct VideoStats {
    int64_t sequences = 0;
    int64_t sequenceEnds = 0;
    int64_t gops = 0;
    int64_t pictures[4] = {0, 0, 0, 0};     /* by picture_coding_type */
    int64_t output = 0;
    int64_t slices = 0;
    int64_t mbIntra = 0;
    int64_t mbInter = 0;
    int64_t mbSkipped = 0;
    int64_t mbFieldDct = 0;
    int64_t mbFrameMotion = 0;
    int64_t mbFieldMotion = 0;
    int64_t mbDualPrime = 0;
    int64_t mbQuant = 0;
    int64_t blocks = 0;
    int64_t blocksDcOnly = 0;
    int64_t mvOutside = 0;          /* predictions reaching outside the reference */
    int64_t sliceLongTails = 0;     /* slices followed by 8+ zero bits before the next start code */
    int64_t sliceEndsMidRow = 0;    /* slices not ending at the end of a macroblock row */
    int64_t errors = 0;             /* slice / VLC errors */
    int64_t concealedMbs = 0;
    int64_t unsupported = 0;        /* field pictures, dual prime, MPEG-1... */
    int64_t skippedPictures = 0;    /* B / P pictures without their references */
    /* picture coding extension values seen (bit masks of the values) */
    unsigned pictureStructures = 0; /* 1 << picture_structure */
    unsigned intraDcPrecisions = 0; /* 1 << intra_dc_precision */
    unsigned qScaleTypes = 0;
    unsigned intraVlcFormats = 0;
    unsigned alternateScans = 0;
    unsigned framePredFrameDct = 0;
    unsigned concealmentVectors = 0;
    unsigned progressiveFrames = 0;
    unsigned repeatFirstFields = 0;
    unsigned topFieldFirsts = 0;
    char lastError[128] = {0};
};

class Mpeg2VideoDecoder {
public:
    Mpeg2VideoDecoder();
    ~Mpeg2VideoDecoder();
    Mpeg2VideoDecoder(const Mpeg2VideoDecoder &) = delete;
    Mpeg2VideoDecoder &operator=(const Mpeg2VideoDecoder &) = delete;

    /* Forget everything (data, references, sequence); stats are kept. */
    void reset();

    /* Elementary-stream bytes.  pts (90 kHz) applies to the first picture
     * whose start code begins in these bytes (ISO 13818-1 2.4.3.7). */
    void push(const uint8_t *data, size_t size, int64_t pts = -1);

    /* No more data: next() then returns the last reference picture. */
    void setEndOfStream();

    /* The next picture in display order; false when more data is needed
     * (or, after setEndOfStream, when everything has been returned). */
    bool next(VideoPicture &out);

    /* Everything decoded and returned after setEndOfStream(). */
    bool finished() const;

    /* Parses the headers in front of the first picture (no picture is
     * decoded): true once the next unit is a picture and a sequence header
     * has been read; false when more data is needed. */
    bool readHeaders();

    const SequenceInfo &sequence() const;
    const VideoStats &stats() const;
    void clearStats();

    /* Bytes pushed but not decoded yet. */
    size_t buffered() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} /* namespace gt3media */

#endif
