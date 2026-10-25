/* pss_demux.cpp - the PSS program-stream demultiplexer.  See pss_demux.h. */
#include "engine/media/native/pss_demux.h"

#include <cstring>

namespace gt3media {

namespace {

size_t mem_read(void *user, void *dst, size_t size)
{
    MemorySource *m = (MemorySource *)user;
    size_t n = m->size - m->pos;
    if (n > size)
        n = size;
    std::memcpy(dst, m->data + m->pos, n);
    m->pos += n;
    return n;
}

bool mem_rewind(void *user)
{
    ((MemorySource *)user)->pos = 0;
    return true;
}

size_t file_read(void *user, void *dst, size_t size)
{
    return std::fread(dst, 1, size, (std::FILE *)user);
}

bool file_rewind(void *user)
{
    return std::fseek((std::FILE *)user, 0, SEEK_SET) == 0;
}

/* The 33-bit time stamp at p ('xxxx' marker nibble, 3 + 15 + 15 bits). */
int64_t read_ts(const uint8_t *p)
{
    return ((int64_t)((p[0] >> 1) & 7) << 30) | ((int64_t)p[1] << 22) | ((int64_t)(p[2] >> 1) << 15) |
           ((int64_t)p[3] << 7) | (int64_t)(p[4] >> 1);
}

const size_t kBufSize = 0x20000;    /* > the largest PES packet (6 + 65535) */

} /* namespace */

ByteSource MemorySource::source()
{
    ByteSource s;
    s.user = this;
    s.read = mem_read;
    s.rewind = mem_rewind;
    return s;
}

ByteSource file_source(std::FILE *f)
{
    ByteSource s;
    s.user = f;
    s.read = file_read;
    s.rewind = file_rewind;
    return s;
}

PssDemuxer::PssDemuxer(const ByteSource &src) : src_(src)
{
    buf_.resize(kBufSize);
}

void PssDemuxer::setSource(const ByteSource &src)
{
    src_ = src;
    pos_ = len_ = 0;
    base_ = 0;
    eof_ = false;
}

bool PssDemuxer::rewind()
{
    if (src_.rewind == nullptr || !src_.rewind(src_.user))
        return false;
    pos_ = len_ = 0;
    base_ = 0;
    eof_ = false;
    return true;
}

/* At least n bytes at pos_ (false if the data ends first). */
bool PssDemuxer::ensure(size_t n)
{
    if (len_ - pos_ >= n)
        return true;
    if (pos_ > 0) {
        std::memmove(buf_.data(), buf_.data() + pos_, len_ - pos_);
        base_ += pos_;
        len_ -= pos_;
        pos_ = 0;
    }
    while (len_ < n && !eof_ && src_.read != nullptr) {
        size_t got = src_.read(src_.user, buf_.data() + len_, buf_.size() - len_);
        if (got == 0)
            eof_ = true;
        len_ += got;
    }
    return len_ - pos_ >= n;
}

bool PssDemuxer::next(PesPacket &pkt)
{
    for (;;) {
        if (!ensure(4))
            return false;
        const uint8_t *b = buf_.data() + pos_;
        if (b[0] != 0 || b[1] != 0 || b[2] != 1) {
            /* not at a start code: find the next prefix */
            pos_++;
            stats_.resyncs++;
            continue;
        }
        uint8_t id = b[3];
        if (id == 0xBA) {                               /* pack_header */
            if (!ensure(14))
                return false;
            b = buf_.data() + pos_;
            size_t len;
            if ((b[4] & 0xC0) == 0x40)
                len = 14 + (b[13] & 7);                 /* MPEG-2 */
            else if ((b[4] & 0xF0) == 0x20)
                len = 12;                               /* MPEG-1 */
            else {
                stats_.errors++;
                pos_ += 4;
                continue;
            }
            if (!ensure(len))
                return false;
            pos_ += len;
            stats_.packs++;
            continue;
        }
        if (id == 0xB9) {                               /* MPEG_program_end_code */
            pos_ += 4;
            stats_.programEnds++;
            return false;
        }
        if (id < 0xBB) {                                /* not a system-level code */
            stats_.errors++;
            pos_ += 4;
            continue;
        }
        if (!ensure(6))
            return false;
        b = buf_.data() + pos_;
        size_t total = 6 + (((size_t)b[4] << 8) | b[5]);
        if (!ensure(total)) {
            stats_.errors++;
            pos_ = len_;
            return false;
        }
        b = buf_.data() + pos_;
        uint64_t offset = position();
        pos_ += total;
        if (id == 0xBB) {
            stats_.systemHeaders++;
            continue;
        }
        if (id == 0xBE) {
            stats_.paddingPackets++;
            continue;
        }
        if (id == 0xBC || id == 0xBF || id >= 0xF0) {   /* stream map, private 2, others */
            stats_.otherPackets++;
            continue;
        }

        /* A PES packet with the PES header syntax. */
        pkt = PesPacket();
        pkt.streamId = id;
        pkt.offset = offset;
        size_t hp = 6;
        if ((b[6] & 0xC0) == 0x80) {                    /* MPEG-2 PES header */
            if (total < 9) {
                stats_.errors++;
                continue;
            }
            unsigned flags = b[7] >> 6;
            size_t hlen = b[8];
            if (9 + hlen > total) {
                stats_.errors++;
                continue;
            }
            if ((flags & 2) && hlen >= 5)
                pkt.pts = read_ts(b + 9);
            if (flags == 3 && hlen >= 10)
                pkt.dts = read_ts(b + 14);
            hp = 9 + hlen;
        } else {                                        /* MPEG-1 packet header */
            while (hp < total && b[hp] == 0xFF)
                hp++;
            if (hp < total && (b[hp] & 0xC0) == 0x40)
                hp += 2;
            if (hp < total && (b[hp] & 0xF0) == 0x20) {
                if (hp + 5 <= total)
                    pkt.pts = read_ts(b + hp);
                hp += 5;
            } else if (hp < total && (b[hp] & 0xF0) == 0x30) {
                if (hp + 10 <= total) {
                    pkt.pts = read_ts(b + hp);
                    pkt.dts = read_ts(b + hp + 5);
                }
                hp += 10;
            } else {
                hp++;                                   /* 0000 1111 */
            }
            if (hp > total) {
                stats_.errors++;
                continue;
            }
        }
        pkt.payload = b + hp;
        pkt.size = total - hp;
        pkt.raw = b;
        pkt.rawSize = total;
        stats_.pes++;
        if (id == 0xBD) {
            /* private stream 1: Sony's 4-byte sub-header (first byte
             * 0xFF) before the audio data */
            if (pkt.size >= 1)
                pkt.subStreamId = pkt.payload[0];
            if (pkt.size >= 4 && pkt.subStreamId == 0xFF) {
                std::memcpy(pkt.subHeader, pkt.payload, 4);
                pkt.payload += 4;
                pkt.size -= 4;
            }
            stats_.audioPackets++;
            stats_.audioBytes += pkt.size;
        } else if ((id & 0xF0) == 0xE0) {
            stats_.videoPackets++;
            stats_.videoBytes += pkt.size;
        } else {
            stats_.otherPackets++;
        }
        return true;
    }
}

} /* namespace gt3media */
