/* pss_demux.h - the PSS (MPEG-2 program stream) demultiplexer of the
 * native movie player.
 *
 * On the PS2 the IOP module pdimpeg (client T522: pdimpeg_open / _feed /
 * _close, 0x0025CEB8..0x0025D028) reads the .pss file, keeps the audio for
 * itself (played on the IOP side) and passes the video PES packets to the
 * EE, one per 4 KB unit; mpegif::feed (T529, 0x0025EF00) hands them to the
 * player, whose mpeg_pes_parse (0x00260748) takes the payload and the PTS.
 * Here one demultiplexer reads the whole program stream (ISO 13818-1
 * 2.5) and returns every PES packet with its payload and time stamps.
 *
 * GT3's .pss files: 16 KB packs (a 14-byte MPEG-2 pack header, then PES
 * packets: video 0xE0 of 4096 / 4082 bytes, audio in private stream 1
 * 0xBD), one system header (0xBB) in the first pack, padding (0xBE), and
 * the program end code (0xB9).  The audio PES payload starts with a 4-byte
 * Sony sub-header (FF A1 00 00 in every GT3 movie) which the demultiplexer
 * strips; the audio data itself is described in pss_audio.h.
 */
#ifndef GT3_MEDIA_NATIVE_PSS_DEMUX_H
#define GT3_MEDIA_NATIVE_PSS_DEMUX_H

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace gt3media {

/* Where the bytes come from: a read callback (returns the bytes read, 0 at
 * the end) and an optional rewind to the first byte (looping movies). */
struct ByteSource {
    void *user = nullptr;
    size_t (*read)(void *user, void *dst, size_t size) = nullptr;
    bool (*rewind)(void *user) = nullptr;
};

/* A ByteSource over memory (the struct must outlive the source). */
struct MemorySource {
    const uint8_t *data = nullptr;
    size_t size = 0;
    size_t pos = 0;
    ByteSource source();
};

/* A ByteSource over a stdio FILE (opened in binary mode by the caller). */
ByteSource file_source(std::FILE *f);

/* One PES packet. */
struct PesPacket {
    uint8_t streamId = 0;           /* 0xE0.. video, 0xBD private stream 1, 0xC0.. MPEG audio */
    uint8_t subStreamId = 0;        /* private stream 1: the first payload byte */
    const uint8_t *payload = nullptr;   /* valid until the next call */
    size_t size = 0;
    int64_t pts = -1;               /* 90 kHz; -1 absent */
    int64_t dts = -1;
    uint64_t offset = 0;            /* of the packet in the stream */
    const uint8_t *raw = nullptr;   /* the whole packet (start code, header, payload) */
    size_t rawSize = 0;
    uint8_t subHeader[4] = {0, 0, 0, 0};    /* private stream 1: the stripped sub-header */
};

struct DemuxStats {
    int64_t packs = 0;
    int64_t systemHeaders = 0;
    int64_t pes = 0;
    int64_t videoPackets = 0;
    int64_t audioPackets = 0;       /* private stream 1 */
    int64_t paddingPackets = 0;
    int64_t otherPackets = 0;
    int64_t programEnds = 0;
    int64_t resyncs = 0;            /* bytes skipped looking for a start code */
    int64_t errors = 0;
    uint64_t videoBytes = 0;
    uint64_t audioBytes = 0;
};

class PssDemuxer {
public:
    explicit PssDemuxer(const ByteSource &src = ByteSource());

    void setSource(const ByteSource &src);

    /* Next PES packet (system headers, packs and padding are consumed
     * silently); false at the program end code or the end of the data. */
    bool next(PesPacket &pkt);

    /* Back to the first byte (needs the source's rewind). */
    bool rewind();

    uint64_t position() const { return base_ + pos_; }
    const DemuxStats &stats() const { return stats_; }

private:
    bool ensure(size_t n);

    ByteSource src_;
    std::vector<uint8_t> buf_;
    size_t pos_ = 0;
    size_t len_ = 0;
    uint64_t base_ = 0;         /* stream offset of buf_[0] */
    bool eof_ = false;
    DemuxStats stats_;
};

} /* namespace gt3media */

#endif
