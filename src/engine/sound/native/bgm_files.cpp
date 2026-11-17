/* bgm_files.cpp - the streamed music's files (see bgm_files.h). */
#include "engine/sound/native/bgm_files.h"
#include "platform/ui_text.h"                  /* PC (2026-11-16): N_ - the launcher's song manager shows the events */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace spu2 {

namespace {

uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

void put32(std::vector<uint8_t> &v, uint32_t x)
{
    for (int i = 0; i < 4; i++)
        v.push_back((uint8_t)(x >> (8 * i)));
}

void put16(std::vector<uint8_t> &v, uint16_t x)
{
    v.push_back((uint8_t)x);
    v.push_back((uint8_t)(x >> 8));
}

void put_tag(std::vector<uint8_t> &v, const char *tag)
{
    v.insert(v.end(), tag, tag + 4);
}

void set32(std::vector<uint8_t> &v, size_t at, uint32_t x)
{
    for (int i = 0; i < 4; i++)
        v[at + i] = (uint8_t)(x >> (8 * i));
}

/* The samples per channel of `bytes' of one channel's part of a chunk. */
uint32_t channel_frames(const AdsHeader &h, uint32_t bytes)
{
    return h.type == ADS_TYPE_PCM ? bytes / 2 : bytes / ADPCM_BLOCK_BYTES * ADPCM_BLOCK_SAMPLES;
}

std::string lower(std::string s)
{
    for (char &c : s)
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
    return s;
}

bool ends_with_ci(const std::string &s, const char *ext)
{
    const size_t n = std::strlen(ext);
    return s.size() >= n && lower(s.substr(s.size() - n)) == ext;
}

std::string trim(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t'))
        a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t'))
        b--;
    return s.substr(a, b - a);
}

} /* namespace */

/* ---- .ads ---------------------------------------------------------------- */

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

uint32_t ads_frames(const AdsHeader &h, uint32_t bytes)
{
    const uint32_t chunk = h.interleave * h.channels;
    return bytes / chunk * channel_frames(h, h.interleave) + channel_frames(h, bytes % chunk / h.channels);
}

void ads_decode(const AdsHeader &h, const uint8_t *body, uint32_t bytes, AdpcmHistory hist[2],
                std::vector<int16_t> &out)
{
    const uint32_t ch = h.channels, chunk = h.interleave * ch;
    for (uint32_t at = 0; at < bytes; at += chunk) {
        /* (a shorter last chunk: split evenly) */
        const uint32_t part = std::min(chunk, bytes - at) / ch;
        const uint32_t n = channel_frames(h, part);
        const size_t first = out.size();
        out.resize(first + (size_t)n * ch);
        int16_t *dst = out.data() + first;
        for (uint32_t c = 0; c < ch; c++) {
            const uint8_t *src = body + at + c * part;
            if (h.type == ADS_TYPE_PCM) {
                for (uint32_t i = 0; i < n; i++)
                    dst[i * ch + c] = (int16_t)rd16(src + 2 * i);
            } else {
                int16_t s[ADPCM_BLOCK_SAMPLES];
                for (uint32_t b = 0; b < part / ADPCM_BLOCK_BYTES; b++) {
                    adpcm_decode_block(src + b * ADPCM_BLOCK_BYTES, s, hist[c]);
                    for (int i = 0; i < ADPCM_BLOCK_SAMPLES; i++)
                        dst[(b * ADPCM_BLOCK_SAMPLES + i) * ch + c] = s[i];
                }
            }
        }
    }
}

/* ---- .wav ---------------------------------------------------------------- */

bool wav_probe(AdsReadFn read, void *user, uint32_t size, WavInfo *out)
{
    uint8_t h[12];
    if (read(user, 0, h, 12) != 12 || std::memcmp(h, "RIFF", 4) != 0 || std::memcmp(h + 8, "WAVE", 4) != 0)
        return false;
    WavInfo w;
    bool fmt = false, data = false;
    uint32_t at = 12;
    while (at + 8 <= size) {
        uint8_t c[8];
        if (read(user, at, c, 8) != 8)
            break;
        const uint32_t len = rd32(c + 4);
        const uint32_t body = at + 8;
        if (std::memcmp(c, "fmt ", 4) == 0 && len >= 16) {
            uint8_t f[40] = {};
            const uint32_t n = std::min<uint32_t>(len, sizeof(f));
            if (read(user, body, f, n) != (int)n)
                return false;
            uint16_t tag = rd16(f);
            if (tag == 0xFFFE && n >= 26)
                tag = rd16(f + 24);         /* EXTENSIBLE: the subformat GUID's first word */
            w.channels = rd16(f + 2);
            w.rate = rd32(f + 4);
            w.blockAlign = rd16(f + 12);
            w.bits = rd16(f + 14);
            w.isFloat = tag == 3;           /* WAVE_FORMAT_IEEE_FLOAT */
            const bool intOk = tag == 1 && (w.bits == 8 || w.bits == 16 || w.bits == 24 || w.bits == 32);
            const bool floatOk = w.isFloat && (w.bits == 32 || w.bits == 64);
            if ((!intOk && !floatOk) || w.channels < 1 || w.channels > 8 || w.rate == 0 ||
                w.blockAlign != w.channels * (w.bits / 8))
                return false;
            fmt = true;
        } else if (std::memcmp(c, "data", 4) == 0) {
            w.dataOffset = body;
            w.dataBytes = body <= size ? std::min(len, size - body) : 0;
            data = true;
        } else if (std::memcmp(c, "LIST", 4) == 0 && len >= 4 && len <= (1u << 16)) {
            std::vector<uint8_t> l(len);
            if (read(user, body, l.data(), len) == (int)len && std::memcmp(l.data(), "INFO", 4) == 0) {
                for (uint32_t i = 4; i + 8 <= len;) {
                    const uint32_t sl = rd32(&l[i + 4]);
                    if (i + 8 + sl > len)
                        break;
                    std::string v((const char *)&l[i + 8], sl);
                    v = v.substr(0, v.find('\0'));
                    if (std::memcmp(&l[i], "INAM", 4) == 0)
                        w.title = v;
                    else if (std::memcmp(&l[i], "IART", 4) == 0)
                        w.artist = v;
                    i += 8 + sl + (sl & 1);
                }
            }
        }
        if (len > UINT32_MAX - body - 1)
            break;
        at = body + len + (len & 1);
    }
    if (!fmt || !data)
        return false;
    w.dataBytes -= w.dataBytes % w.blockAlign;
    *out = w;
    return true;
}

std::vector<uint8_t> wav_head(uint32_t rate, uint32_t channels, uint32_t frames, const std::string &title,
                              const std::string &artist)
{
    std::vector<uint8_t> v;
    put_tag(v, "RIFF");
    put32(v, 0);                            /* (set below) */
    put_tag(v, "WAVE");
    put_tag(v, "fmt ");
    put32(v, 16);
    put16(v, 1);                            /* WAVE_FORMAT_PCM */
    put16(v, (uint16_t)channels);
    put32(v, rate);
    put32(v, rate * channels * 2);
    put16(v, (uint16_t)(channels * 2));
    put16(v, 16);
    if (!title.empty() || !artist.empty()) {
        std::vector<uint8_t> info;
        put_tag(info, "INFO");
        auto add = [&info](const char *tag, const std::string &s) {
            if (s.empty())
                return;
            put_tag(info, tag);
            put32(info, (uint32_t)s.size() + 1);
            info.insert(info.end(), s.begin(), s.end());
            info.push_back(0);
            if ((s.size() + 1) & 1)
                info.push_back(0);
        };
        add("INAM", title);
        add("IART", artist);
        put_tag(v, "LIST");
        put32(v, (uint32_t)info.size());
        v.insert(v.end(), info.begin(), info.end());
    }
    const uint32_t dataBytes = frames * channels * 2;
    put_tag(v, "data");
    put32(v, dataBytes);
    set32(v, 4, (uint32_t)v.size() - 8 + dataBytes + (dataBytes & 1));
    return v;
}

/* ---- either ---------------------------------------------------------------- */

/* ---- .mp3 ---------------------------------------------------------------- */

namespace {

/* An MPEG audio frame header: its length, samples, rate, channels; false
 * when the 4 bytes are not one. */
struct Mp3Frame {
    uint32_t bytes = 0, samples = 0, rate = 0, channels = 0;
    int version = 0;                /* 1 MPEG-1, 2 MPEG-2, 25 MPEG-2.5 */
    int layer = 0;
};

bool mp3_frame(const uint8_t *h, Mp3Frame *f)
{
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0)
        return false;
    const int v = (h[1] >> 3) & 3, l = (h[1] >> 1) & 3, bi = h[2] >> 4, si = (h[2] >> 2) & 3;
    if (v == 1 || l == 0 || bi == 0 || bi == 15 || si == 3)
        return false;
    static const uint16_t kBitrate[2][3][15] = {
        {{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},     /* MPEG-1 layer I */
         {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},        /*        layer II */
         {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}},        /*        layer III */
        {{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},        /* MPEG-2 / 2.5 layer I */
         {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},             /*        layer II */
         {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}}};           /*        layer III */
    static const uint32_t kRate[3] = {44100, 48000, 32000};
    f->version = v == 3 ? 1 : v == 2 ? 2 : 25;
    f->layer = 4 - l;
    f->rate = kRate[si] >> (f->version == 1 ? 0 : f->version == 2 ? 1 : 2);
    const uint32_t br = kBitrate[f->version == 1 ? 0 : 1][f->layer - 1][bi] * 1000u;
    const uint32_t pad = (h[2] >> 1) & 1;
    if (f->layer == 1) {
        f->samples = 384;
        f->bytes = (12 * br / f->rate + pad) * 4;
    } else {
        f->samples = f->layer == 3 && f->version != 1 ? 576 : 1152;
        f->bytes = f->samples / 8 * br / f->rate + pad;
    }
    f->channels = (h[3] >> 6) == 3 ? 1 : 2;
    return f->bytes >= 4;
}

uint32_t syncsafe(const uint8_t *p)
{
    return (uint32_t)(p[0] & 0x7F) << 21 | (uint32_t)(p[1] & 0x7F) << 14 | (uint32_t)(p[2] & 0x7F) << 7 | (p[3] & 0x7F);
}

uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* A code point as Latin-1 ('?' beyond it) */
void put_latin1(std::string &s, uint32_t c)
{
    s.push_back(c < 0x100 ? (char)c : '?');
}

/* An ID3v2 text frame's text (its encoding byte first) as Latin-1. */
std::string id3_text(const uint8_t *p, size_t n)
{
    std::string s;
    if (n == 0)
        return s;
    const uint8_t enc = p[0];
    p++;
    n--;
    if (enc == 1 || enc == 2) {             /* UTF-16 (with a BOM) / UTF-16BE */
        bool be = enc == 2;
        size_t i = 0;
        if (enc == 1 && n >= 2) {
            be = p[0] == 0xFE && p[1] == 0xFF;
            i = 2;
        }
        for (; i + 1 < n; i += 2) {
            uint32_t c = be ? (uint32_t)(p[i] << 8 | p[i + 1]) : (uint32_t)(p[i] | p[i + 1] << 8);
            if (c == 0)
                break;
            if (c >= 0xD800 && c < 0xDC00 && i + 3 < n) {
                i += 2;                     /* (a surrogate pair: outside Latin-1 anyway) */
                c = 0x10000;
            }
            put_latin1(s, c);
        }
    } else if (enc == 3) {                  /* UTF-8 */
        for (size_t i = 0; i < n && p[i] != 0;) {
            uint32_t c = p[i];
            int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
            if (more)
                c &= 0x3F >> more;
            i++;
            for (; more > 0 && i < n; more--, i++)
                c = c << 6 | (p[i] & 0x3F);
            put_latin1(s, c);
        }
    } else {                                /* ISO-8859-1 */
        for (size_t i = 0; i < n && p[i] != 0; i++)
            s.push_back((char)p[i]);
    }
    return trim(s);
}

/* A buffered sequential reader over AdsReadFn (the frame scan). */
struct Scan {
    AdsReadFn read;
    void *user;
    uint32_t size;
    std::vector<uint8_t> buf;
    uint32_t base = 0;                      /* the file offset of buf[0] */
    /* p points at n bytes from offset `at' (false past the end) */
    bool get(uint32_t at, uint32_t n, const uint8_t **p)
    {
        if (at < base || at + n > base + buf.size()) {
            buf.resize(0x10000);
            const int got = read(user, at, buf.data(), (uint32_t)buf.size());
            buf.resize(got > 0 ? (size_t)got : 0);
            base = at;
            if (buf.size() < n)
                return false;
        }
        *p = buf.data() + (at - base);
        return true;
    }
};

} /* namespace */

bool mp3_probe(AdsReadFn read, void *user, uint32_t size, Mp3Info *out, bool quick, bool estimate)
{
    Scan s{read, user, size};
    const uint8_t *p;
    Mp3Info m;
    uint32_t at = 0;
    /* ID3v2: its tags, then past it */
    if (s.get(0, 10, &p) && std::memcmp(p, "ID3", 3) == 0) {
        const int major = p[3];
        const bool footer = (p[5] & 0x10) != 0;
        const uint32_t tagSize = syncsafe(p + 6);
        std::vector<uint8_t> tag(quick ? 0 : tagSize);     /* (quick: past it; a cover picture can be large) */
        if (!quick && tagSize > 0 && tagSize < (16u << 20) && read(user, 10, tag.data(), tagSize) == (int)tagSize) {
            const bool v22 = major == 2;
            const uint32_t hd = v22 ? 6 : 10;
            for (uint32_t i = 0; i + hd <= tagSize;) {
                const uint8_t *f = tag.data() + i;
                if (f[0] == 0)
                    break;                  /* (the padding) */
                const uint32_t fs = v22 ? (uint32_t)(f[3] << 16 | f[4] << 8 | f[5])
                                        : major >= 4 ? syncsafe(f + 4) : be32(f + 4);
                if (fs > tagSize - i - hd)
                    break;
                const std::string id((const char *)f, v22 ? 3 : 4);
                if (id == "TIT2" || id == "TT2")
                    m.title = id3_text(f + hd, fs);
                else if (id == "TPE1" || id == "TP1")
                    m.artist = id3_text(f + hd, fs);
                i += hd + fs;
            }
        }
        at = 10 + tagSize + (footer ? 10 : 0);
    }
    /* the first frame: a header with another one right after it */
    Mp3Frame first;
    bool found = false;
    for (uint32_t limit = at + 0x10000; at + 4 <= limit && s.get(at, 4, &p); at++) {
        Mp3Frame f, g;
        const uint8_t *q;
        if (mp3_frame(p, &f) && s.get(at + f.bytes, 4, &q) && mp3_frame(q, &g) && g.rate == f.rate &&
            g.layer == f.layer) {
            first = f;
            found = true;
            break;
        }
    }
    if (!found)
        return false;
    m.rate = first.rate;
    m.channels = first.channels;
    /* Xing / Info (after the side information) or VBRI (at 36): the count */
    uint64_t counted = 0;
    if (s.get(at, std::min<uint32_t>(first.bytes, 200), &p)) {
        const uint32_t side = first.version == 1 ? (first.channels == 1 ? 17 : 32) : (first.channels == 1 ? 9 : 17);
        const uint32_t x = 4 + side;
        if (first.layer == 3 && first.bytes >= x + 12 &&
            (std::memcmp(p + x, "Xing", 4) == 0 || std::memcmp(p + x, "Info", 4) == 0) && (be32(p + x + 4) & 1))
            counted = (uint64_t)be32(p + x + 8) * first.samples;
        else if (first.bytes >= 36 + 18 && std::memcmp(p + 36, "VBRI", 4) == 0)
            counted = (uint64_t)be32(p + 36 + 14) * first.samples;
    }
    if (counted == 0 && estimate && size != 0xFFFFFFFFu && size > at)
        counted = (uint64_t)(size - at) / first.bytes * first.samples;
    if (counted == 0 && !quick) {
        /* every frame (a 64 KB window at a time) */
        for (uint32_t o = at; o + 4 <= size && s.get(o, 4, &p);) {
            Mp3Frame f;
            if (!mp3_frame(p, &f))
                break;                      /* (an ID3v1 tag, the end) */
            counted += f.samples;
            o += f.bytes;
        }
    }
    m.frames = counted;
    /* ID3v1: when v2 had none */
    if (!quick && (m.title.empty() || m.artist.empty()) && size >= 128 && size != 0xFFFFFFFFu &&
        s.get(size - 128, 128, &p) && std::memcmp(p, "TAG", 3) == 0) {
        auto field = [](const uint8_t *f, size_t n) {
            std::string t((const char *)f, n);
            return trim(t.substr(0, t.find('\0')));
        };
        if (m.title.empty())
            m.title = field(p + 3, 30);
        if (m.artist.empty())
            m.artist = field(p + 33, 30);
    }
    *out = m;
    return m.frames > 0 || quick;
}

/* ---- either ---------------------------------------------------------------- */

void wav_to_float(const BgmFileInfo &in, const uint8_t *src, uint32_t frames, std::vector<float> &out)
{
    const uint32_t ch = in.channels, bytes = in.bits / 8;
    auto sample = [&](const uint8_t *p) -> float {
        if (in.isFloat) {
            if (bytes == 8) {
                double d;
                std::memcpy(&d, p, 8);
                return (float)d;
            }
            float f;
            std::memcpy(&f, p, 4);
            return f;
        }
        switch (bytes) {
        case 1: return ((int)p[0] - 128) * (1.0f / 128.0f);
        case 2: return (int16_t)rd16(p) * (1.0f / 32768.0f);
        case 3: return (float)((int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 8) *
                       (1.0f / 8388608.0f);
        default: return (float)(int32_t)rd32(p) * (1.0f / 2147483648.0f);
        }
    };
    const size_t first = out.size();
    out.resize(first + (size_t)frames * (ch == 1 ? 1 : 2));
    float *o = out.data() + first;
    for (uint32_t i = 0; i < frames; i++, src += in.blockAlign) {
        if (ch == 1) {
            *o++ = sample(src);
        } else if (ch == 2) {
            *o++ = sample(src);
            *o++ = sample(src + bytes);
        } else {
            /* the front pair with the centre (L R C ...) */
            const float c = 0.7071f * sample(src + 2 * bytes);
            *o++ = (sample(src) + c) * 0.6f;
            *o++ = (sample(src + bytes) + c) * 0.6f;
        }
    }
}

std::string bgm_describe(const BgmFileInfo &in)
{
    const uint32_t secs = in.rate ? in.frames / in.rate : 0;
    char b[96];
    const char *kind = in.kind == BgmKind::wav ? "WAV" : in.kind == BgmKind::mp3 ? "MP3" : "ADS";
    char depth[16] = "";
    if (in.kind == BgmKind::wav)
        std::snprintf(depth, sizeof(depth), " %u-bit%s", in.bits, in.isFloat ? " float" : "");
    std::snprintf(b, sizeof(b), "%u:%02u, %.1f kHz %s%s %s", secs / 60, secs % 60, in.rate / 1000.0,
                  in.channels == 1 ? "mono" : in.channels == 2 ? "stereo" : "multichannel", depth, kind);
    return b;
}

bool bgm_probe(AdsReadFn read, void *user, uint32_t size, BgmFileInfo *out, bool quick, bool estimate)
{
    BgmFileInfo b;
    WavInfo w;
    if (wav_probe(read, user, size, &w)) {
        b.kind = BgmKind::wav;
        b.rate = w.rate;
        b.channels = w.channels;
        b.dataOffset = w.dataOffset;
        b.dataBytes = w.dataBytes;
        b.bits = w.bits;
        b.isFloat = w.isFloat;
        b.blockAlign = w.blockAlign;
        b.frames = w.dataBytes / w.blockAlign;
        b.title = w.title;
        b.artist = w.artist;
        *out = b;
        return true;
    }
    uint8_t head[ADS_HEAD_BYTES];
    if (read(user, 0, head, ADS_HEAD_BYTES) != (int)ADS_HEAD_BYTES || !ads_parse_header(head, ADS_HEAD_BYTES, &b.ads) ||
        (b.ads.type != ADS_TYPE_PCM && b.ads.type != ADS_TYPE_ADPCM)) {
        Mp3Info m;
        if (!mp3_probe(read, user, size, &m, quick, estimate))
            return false;
        b.kind = BgmKind::mp3;
        b.rate = m.rate;
        b.channels = m.channels;
        b.frames = (uint32_t)std::min<uint64_t>(m.frames, 0xFFFFFFFFu);
        b.title = m.title;
        b.artist = m.artist;
        *out = b;
        return true;
    }
    b.rate = b.ads.rate;
    b.channels = b.ads.channels;
    b.dataOffset = ADS_HEAD_BYTES;
    b.dataBytes = size > ADS_HEAD_BYTES ? std::min(b.ads.bodySize, size - ADS_HEAD_BYTES) : b.ads.bodySize;
    b.frames = ads_frames(b.ads, b.dataBytes);
    *out = b;
    return true;
}

/* ---- bgm/ads.inf and bgm/ads.txt ------------------------------------------- */

const char *ads_group_event(int g)
{
    /* BGM group 0x12 + g: RaceOrganization::updateStartAndTimer (0x12, 0x1B),
     * the race displays' finish (0x13), the race songs (0x14), the race
     * menus' ShortMenuParams (0x15, 0x16), the prize / trophy / car / license
     * controls (0x17, 0x18, 0x19, 0x21), MenuSound::selectMusic
     * ("replay_theater": 0x1A), the license examination (0x1D..0x20; 0x20
     * also the time trial's disqualification), the demo (0x22); 0x1C has no
     * caller */
    /* (PC 2026-11-16: marked for the interface's languages; the song manager
     * shows them through tr) */
    static const char *const kEvents[ADS_GROUPS] = {
        N_("Race start (10-second countdown)"),
        N_("Race finish"),
        N_("Race songs"),
        N_("Pre-race menu"),
        N_("Race results"),
        N_("Prize fanfare"),
        N_("Trophy won"),
        N_("Car won"),
        N_("Replay Theater"),
        N_("Race start (4-second countdown)"),
        N_("Not used by the game"),
        N_("License gold"),
        N_("License silver"),
        N_("License bronze"),
        N_("License failed / disqualified"),
        N_("License earned"),
        N_("Demo race"),
    };
    return g >= 0 && g < ADS_GROUPS ? kEvents[g] : "";
}

bool bgm_system_path(const std::string &file)
{
    if (file.size() >= 2 && file[0] == '\\' && file[1] == '\\')
        return true;
    const char c = file.empty() ? '\0' : (char)(file[0] | 0x20);
    return file.size() >= 3 && c >= 'a' && c <= 'z' && file[1] == ':' && (file[2] == '\\' || file[2] == '/');
}

std::string bgm_base_name(const std::string &file)
{
    const size_t slash = file.find_last_of("/\\");
    std::string b = slash == std::string::npos ? file : file.substr(slash + 1);
    const size_t dot = b.find_last_of('.');
    return dot == std::string::npos ? b : b.substr(0, dot);
}

std::string bgm_other_name(const std::string &file)
{
    if (bgm_system_path(file))
        return std::string();
    if (ends_with_ci(file, ".ads"))
        return file.substr(0, file.size() - 4) + ".wav";
    if (ends_with_ci(file, ".wav"))
        return file.substr(0, file.size() - 4) + ".ads";
    return std::string();
}

bool bgm_music_name(const std::string &file)
{
    return ends_with_ci(file, ".wav") || ends_with_ci(file, ".mp3") || ends_with_ci(file, ".ads");
}

bool ads_inf_read(const uint8_t *data, size_t size, AdsList *out, std::string *error, bool legacyMarkers)
{
    constexpr size_t kHead = 0x10 + 8 * ADS_GROUPS;
    if (size < kHead || std::memcmp(data, "MADS", 4) != 0) {
        *error = "not an ads.inf (no \"MADS\" head)";
        return false;
    }
    const uint32_t base = rd32(data + 8);
    auto at = [&](uint32_t ptr, size_t need, size_t *o) {
        if (ptr < base || ptr - base > size || size - (ptr - base) < need)
            return false;
        *o = ptr - base;
        return true;
    };
    auto str = [&](uint32_t ptr, std::string *s) {
        size_t o;
        if (!at(ptr, 1, &o))
            return false;
        const void *end = std::memchr(data + o, 0, size - o);
        if (end == nullptr)
            return false;
        s->assign((const char *)data + o, (const char *)end);
        return true;
    };
    AdsList list;
    for (int g = 0; g < ADS_GROUPS; g++) {
        const uint32_t tracks = rd32(data + 0x10 + 8 * g), count = rd32(data + 0x14 + 8 * g);
        size_t o;
        if (count == 0)
            continue;
        if (count > 0x10000 || !at(tracks, (size_t)count * 0x14, &o)) {
            *error = "group " + std::to_string(g) + "'s tracks are outside the file";
            return false;
        }
        for (uint32_t t = 0; t < count; t++) {
            const uint8_t *r = data + o + (size_t)t * 0x14;
            AdsListTrack k;
            size_t mo;
            if (!str(rd32(r), &k.id) || !str(rd32(r + 4), &k.file) || !str(rd32(r + 8), &k.title) ||
                !str(rd32(r + 12), &k.artist) || !at(rd32(r + 16), 0x10C, &mo)) {
                *error = "group " + std::to_string(g) + " track " + std::to_string(t) + " points outside the file";
                return false;
            }
            k.hasId = k.hasTitle = k.hasArtist = true;
            const uint8_t *m = data + mo;
            k.rate = rd32(m + 4);
            k.length = rd32(m + 8);
            const double rate = k.rate != 0 ? k.rate : 44100.0;
            /* the times' addresses: of the block's build (its +0), so file
             * offset = address - built + where the block is */
            const uint32_t built = rd32(m);
            for (int ch = 0; ch < ADS_MARKER_CHANNELS; ch++) {
                const uint32_t n = rd32(m + 0x0C + 4 * ch);
                size_t to;
                if (n == 0)
                    continue;
                const uint32_t ptr = legacyMarkers ? rd32(m + 0x8C + 4 * ch)
                                                   : rd32(m + 0x8C + 4 * ch) - built + base + (uint32_t)mo;
                if (n > 0x100000 || !at(ptr, (size_t)n * 4, &to)) {
                    *error = k.file + "'s markers of channel " + std::to_string(ch) + " are outside the file";
                    return false;
                }
                for (uint32_t i = 0; i < n; i++)
                    k.markers[ch].push_back(rd32(data + to + 4 * i) / rate);
            }
            list.group[g].push_back(std::move(k));
        }
    }
    *out = std::move(list);
    return true;
}

namespace {

const char *const kTextHead =
    "# bgm/ads.txt - the streamed music.  GT3 PC reads this list instead of\n"
    "# bgm/ads.inf (the launcher wrote it from ads.inf, which stays as the\n"
    "# fallback: without this file the game reads ads.inf again).\n"
    "#\n"
    "# [group N]     ADS group N (0..16), its tracks following in order.  Group 2\n"
    "#               is the race songs; each other one plays for an event (its\n"
    "#               first track: the launcher's song manager calls it a slot):\n"
    "#                 0 race start (10-second countdown)  1 race finish\n"
    "#                 3 pre-race menu  4 race results  5 prize fanfare\n"
    "#                 6 trophy won  7 car won  8 Replay Theater\n"
    "#                 9 race start (4-second countdown)  10 not used\n"
    "#                 11 / 12 / 13 license gold / silver / bronze\n"
    "#                 14 license failed  15 license earned  16 demo race\n"
    "# track FILE    a track: its file in bgm\\, a .wav, an .mp3 or an .ads;\n"
    "#               when it is missing, the other of the same name (.wav and\n"
    "#               .ads).  A race song that starts with its intro plays\n"
    "#               i_FILE first.  Or the full path of a file anywhere on the\n"
    "#               machine, in UTF-8 (track D:\\Music\\song.mp3): it plays from\n"
    "#               there, with no intro, and a race skips it while it is\n"
    "#               not there.\n"
    "# Each line under a track is optional (left out: the value in brackets):\n"
    "#   title TEXT          [the .wav's title tag, else the file's name]\n"
    "#   artist TEXT         [the .wav's artist tag, else unknown]\n"
    "#   id TEXT             [the file's name without its extension]\n"
    "#   markers CH T T ...  beat markers of channel CH (0..31), in seconds from\n"
    "#                       the start [none]: the replay cameras cut on them.\n"
    "#                       The line can repeat for more of a channel.\n"
    "#   rate HZ             [the file's sample rate] the markers' time base\n"
    "#   length SAMPLES      [the file's length] where the markers start again\n"
    "#                       when the song loops\n"
    "#   bpm N               [none] the song's tempo, beats a minute: the song\n"
    "#                       manager made its markers from it (1 every beat, 2\n"
    "#                       every bar, 3 and 4 the camera cuts); the game\n"
    "#                       reads only the markers\n"
    "# A .wav or .mp3 put into bgm\\ that this list does not name is added to\n"
    "# group 2 (the race songs) by the launcher when it starts; a commented-out\n"
    "# track line keeps a file out: # track FILE.  The launcher's song manager\n"
    "# (Audio > Songs) edits this list and writes it anew (comments other than\n"
    "# these are not kept).\n"
    "# Lines starting with # are comments.  Text is kept byte for byte.\n";

std::string seconds_text(double s)
{
    char b[32];
    std::snprintf(b, sizeof(b), "%.6f", s);
    std::string t = b;
    while (!t.empty() && t.back() == '0')
        t.pop_back();
    if (!t.empty() && t.back() == '.')
        t.pop_back();
    return t;
}

/* A track's lines: what the file gives (probe) left out.  (The title and
 * artist are written even where the file's tags have them: the text is
 * where they are edited; an empty one is written empty.)  A full path's
 * file is not looked at, a list of a whole library written at each edit: all
 * the track has is written, so the game need not look at it either
 * (ads_list_image). */
std::string track_block(const AdsListTrack &k, const AdsListProbe &probe)
{
    BgmFileInfo info;
    std::string found;
    const bool have = !bgm_system_path(k.file) && probe && probe(k.file, &info, &found);
    const std::string file = have && !found.empty() ? found : k.file;
    std::string t = "track " + file + "\n";
    const std::string base = bgm_base_name(file);
    if (k.hasTitle)
        t += k.title.empty() ? std::string("  title\n") : "  title   " + k.title + "\n";
    if (k.hasArtist)
        t += k.artist.empty() ? std::string("  artist\n") : "  artist  " + k.artist + "\n";
    if (k.hasId && k.id != base)
        t += "  id      " + k.id + "\n";
    const uint32_t fileRate = have ? info.rate : 0;
    const uint32_t rate = k.rate != 0 ? k.rate : 44100;
    if (k.rate != 0 && k.rate != fileRate)
        t += "  rate    " + std::to_string(k.rate) + "\n";
    if (k.length >= 0) {
        const int64_t fileLength = have && info.rate ? (int64_t)info.frames * rate / info.rate : -1;
        if (k.length != fileLength)
            t += "  length  " + std::to_string(k.length) + "\n";
    }
    if (k.bpm > 0.0)
        t += "  bpm     " + seconds_text(k.bpm) + "\n";
    for (int ch = 0; ch < ADS_MARKER_CHANNELS; ch++) {
        const std::vector<double> &m = k.markers[ch];
        for (size_t i = 0; i < m.size(); i += 16) {
            t += "  markers " + std::to_string(ch);
            for (size_t j = i; j < m.size() && j < i + 16; j++)
                t += " " + seconds_text(m[j]);
            t += "\n";
        }
    }
    return t;
}

/* "[group N]" -> N, -1 for another line. */
int group_header(const std::string &trimmed)
{
    int g = -1;
    char tail = 0;
    if (std::sscanf(lower(trimmed).c_str(), "[group %d %c", &g, &tail) == 2 && tail == ']' && g >= 0 && g < ADS_GROUPS)
        return g;
    return -1;
}

/* The text's lines (a final line break or not; '\r' dropped). */
std::vector<std::string> text_lines(const std::string &text)
{
    std::vector<std::string> lines;
    size_t pos = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos)
            end = text.size();
        std::string l = text.substr(pos, end - pos);
        if (!l.empty() && l.back() == '\r')
            l.pop_back();
        lines.push_back(l);
        pos = end + 1;
    }
    return lines;
}

} /* namespace */

std::string ads_list_text(const AdsList &list, const AdsListProbe &probe)
{
    std::string t = kTextHead;
    for (int g = 0; g < ADS_GROUPS; g++) {
        t += "\n[group " + std::to_string(g) + "]\n";
        if (list.group[g].empty())
            t += "# (no tracks)\n";
        for (const AdsListTrack &k : list.group[g])
            t += track_block(k, probe);
        for (const std::string &f : list.keptOut[g])
            t += "# track " + f + "\n";
    }
    return t;
}

void ads_text_files(const std::string &text, std::vector<std::string> *listed, std::vector<std::string> *keptOut)
{
    bool inGroup = false;                   /* (the head's lines explain "track FILE": not files) */
    for (const std::string &line : text_lines(text)) {
        std::string l = trim(line);
        if (group_header(l) >= 0)
            inGroup = true;
        if (!inGroup)
            continue;
        bool comment = false;
        if (!l.empty() && l[0] == '#') {
            comment = true;
            l = trim(l.substr(1));
        }
        const size_t sp = l.find_first_of(" \t");
        if (sp == std::string::npos || lower(l.substr(0, sp)) != "track")
            continue;
        const std::string file = trim(l.substr(sp));
        if (!file.empty())
            (comment ? keptOut : listed)->push_back(file);
    }
}

std::string ads_text_add(const std::string &text, int group, const std::vector<AdsListTrack> &tracks)
{
    std::string block;
    for (const AdsListTrack &k : tracks)
        block += track_block(k, nullptr);
    std::vector<std::string> lines = text_lines(text);
    /* the group's first section: before the next [group N] (past its own
     * trailing blank lines), else at the end */
    size_t at = std::string::npos;
    for (size_t i = 0; i < lines.size(); i++) {
        if (group_header(trim(lines[i])) != group)
            continue;
        at = lines.size();
        for (size_t j = i + 1; j < lines.size(); j++)
            if (group_header(trim(lines[j])) >= 0 || trim(lines[j]).compare(0, 1, "[") == 0) {
                at = j;
                break;
            }
        while (at > i + 1 && trim(lines[at - 1]).empty())
            at--;
        /* (its "(no tracks)" note goes) */
        if (at == i + 2 && trim(lines[i + 1]) == "# (no tracks)") {
            lines.erase(lines.begin() + (ptrdiff_t)(i + 1));
            at--;
        }
        break;
    }
    std::string out;
    if (at == std::string::npos) {
        for (const std::string &l : lines)
            out += l + "\n";
        out += "\n[group " + std::to_string(group) + "]\n" + block;
        return out;
    }
    for (size_t i = 0; i < lines.size(); i++) {
        if (i == at)
            out += block;
        out += lines[i] + "\n";
    }
    if (at == lines.size())
        out += block;
    return out;
}

void ads_list_parse(const std::string &text, AdsList *out, std::vector<std::string> *warnings)
{
    AdsList list;
    int group = -1;
    AdsListTrack *track = nullptr;
    size_t pos = 0;
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0)
        pos = 3;                            /* (a UTF-8 BOM) */
    auto warn = [warnings](int line, const std::string &w) {
        if (warnings)
            warnings->push_back("line " + std::to_string(line) + ": " + w);
    };
    for (int line = 1; pos <= text.size(); line++) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos)
            end = text.size();
        std::string l = text.substr(pos, end - pos);
        pos = end + 1;
        if (!l.empty() && l.back() == '\r')
            l.pop_back();
        l = trim(l);
        if (!l.empty() && l[0] == '#' && group >= 0) {
            /* a commented-out track line: kept out (written back as it is) */
            const std::string c = trim(l.substr(1));
            if (c.size() > 6 && lower(c.substr(0, 5)) == "track" && (c[5] == ' ' || c[5] == '\t'))
                list.keptOut[group].push_back(trim(c.substr(6)));
        }
        if (l.empty() || l[0] == '#')
            continue;
        if (l[0] == '[') {
            int g = -1;
            char tail = 0;
            if (std::sscanf(lower(l).c_str(), "[group %d %c", &g, &tail) == 2 && tail == ']' && g >= 0 &&
                g < ADS_GROUPS) {
                group = g;
                track = nullptr;
            } else {
                warn(line, "\"" + l + "\" is not [group N] with N 0.." + std::to_string(ADS_GROUPS - 1) +
                               ": its tracks are skipped");
                group = -1;
                track = nullptr;
            }
            continue;
        }
        size_t sp = l.find_first_of(" \t");
        const std::string key = lower(l.substr(0, sp));
        const std::string value = sp == std::string::npos ? std::string() : trim(l.substr(sp));
        if (key == "track") {
            if (group < 0) {
                warn(line, "a track outside a [group N]: skipped");
                track = nullptr;
            } else if (value.empty()) {
                warn(line, "a track without its file: skipped");
                track = nullptr;
            } else {
                list.group[group].emplace_back();
                track = &list.group[group].back();
                track->file = value;
                track->line = line;
            }
            continue;
        }
        if (track == nullptr) {
            if (group >= 0 || key == "title" || key == "artist" || key == "id" || key == "markers" || key == "rate" ||
                key == "length" || key == "bpm")
                warn(line, "\"" + key + "\" before any track: skipped");
            else
                warn(line, "\"" + l + "\" is not understood: skipped");
            continue;
        }
        if (key == "title") {
            track->title = value;
            track->hasTitle = true;
        } else if (key == "artist") {
            track->artist = value;
            track->hasArtist = true;
        } else if (key == "id") {
            track->id = value;
            track->hasId = true;
        } else if (key == "rate" || key == "length") {
            char *e = nullptr;
            const long long v = std::strtoll(value.c_str(), &e, 10);
            if (value.empty() || *e != 0 || v < (key == "rate" ? 1 : 0) || v > 0xFFFFFFFFll)
                warn(line, key + " \"" + value + "\" is not a whole number" + (key == "rate" ? " above 0" : "") +
                               ": the file's is used");
            else if (key == "rate")
                track->rate = (uint32_t)v;
            else
                track->length = v;
        } else if (key == "bpm") {
            char *e = nullptr;
            const double v = std::strtod(value.c_str(), &e);
            if (value.empty() || *e != 0 || !(v > 0.0) || v > 1000.0)
                warn(line, "bpm \"" + value + "\" is not a tempo: skipped");
            else
                track->bpm = v;
        } else if (key == "markers") {
            const char *p = value.c_str();
            char *e = nullptr;
            const long ch = std::strtol(p, &e, 10);
            if (e == p || ch < 0 || ch >= ADS_MARKER_CHANNELS) {
                warn(line, "markers without a channel 0..31 first: skipped");
                continue;
            }
            p = e;
            for (;;) {
                while (*p == ' ' || *p == '\t')
                    p++;
                if (*p == 0)
                    break;
                const double s = std::strtod(p, &e);
                const char *next = e;
                if (e == p || (*e != 0 && *e != ' ' && *e != '\t') || !(s >= 0.0) || s > 1e6) {
                    while (*next != 0 && *next != ' ' && *next != '\t')
                        next++;
                    warn(line, "the marker \"" + std::string(p, next) + "\" is not a time in seconds: skipped");
                } else {
                    track->markers[ch].push_back(s);
                }
                p = next;
            }
        } else {
            warn(line, "\"" + key + "\" is not a track's field (title, artist, id, markers, rate, length, bpm): skipped");
        }
    }
    *out = std::move(list);
}

std::vector<uint8_t> ads_list_image(const AdsList &list, const AdsListProbe &probe,
                                    std::vector<std::string> *warnings)
{
    struct Built {
        std::string file, id, title, artist;
        uint32_t rate = 44100, length = 0;
        std::vector<uint32_t> markers[ADS_MARKER_CHANNELS];
    };
    std::vector<Built> tracks;
    uint32_t count[ADS_GROUPS] = {};
    for (int g = 0; g < ADS_GROUPS; g++) {
        for (const AdsListTrack &k : list.group[g]) {
            BgmFileInfo info;
            std::string found;
            /* a full path's file with all the list needs of it is left alone:
             * a library on another disk, or a network folder not answering,
             * costs the game's start nothing (ads_play looks when it plays;
             * a race skips a song not there: MusicPlayList::nextThere) */
            const bool system = bgm_system_path(k.file);
            bool noMarkers = true;
            for (int ch = 0; ch < ADS_MARKER_CHANNELS; ch++)
                noMarkers = noMarkers && k.markers[ch].empty();
            const bool skip = system && k.hasTitle && k.hasArtist && (noMarkers || (k.rate != 0 && k.length >= 0));
            const bool have = !skip && probe && probe(k.file, &info, &found);
            if (!have && !skip && warnings)
                warnings->push_back("line " + std::to_string(k.line) + ": " +
                                    (system ? k.file + " is not there"
                                            : "bgm/" + k.file + " is not there (nor " +
                                                  (bgm_other_name(k.file).empty() ? std::string("another format")
                                                                                  : bgm_other_name(k.file)) +
                                                  ")") +
                                    ": the track plays nothing");
            Built b;
            b.file = have && !found.empty() ? found : k.file;
            const std::string base = bgm_base_name(b.file);
            b.rate = k.rate != 0 ? k.rate : (have && info.rate != 0 ? info.rate : 44100);
            if (k.length >= 0)
                b.length = (uint32_t)std::min<int64_t>(k.length, 0xFFFFFFFFll);
            else if (have && info.rate != 0)
                b.length = (uint32_t)std::min<uint64_t>((uint64_t)info.frames * b.rate / info.rate, 0xFFFFFFFFull);
            b.title = k.hasTitle ? k.title : (have && !info.title.empty() ? info.title : base);
            b.artist = k.hasArtist ? k.artist : (have && !info.artist.empty() ? info.artist : std::string("unknown"));
            b.id = k.hasId ? k.id : base;
            for (int ch = 0; ch < ADS_MARKER_CHANNELS; ch++)
                for (double s : k.markers[ch])
                    b.markers[ch].push_back((uint32_t)std::min(std::llround(s * b.rate), 0xFFFFFFFFll));
            tracks.push_back(std::move(b));
            count[g]++;
        }
    }

    /* the layout: head, groups, tracks (all groups' in order: ads_get_tracks
     * -1 reads them as one run), markers, marker times, strings */
    constexpr uint32_t kGroups = 0x10, kTracks = kGroups + 8 * ADS_GROUPS, kMarkers = 0x10C;
    const uint32_t markersAt = kTracks + 0x14 * (uint32_t)tracks.size();
    uint32_t timesAt = markersAt + kMarkers * (uint32_t)tracks.size();
    std::vector<uint8_t> img(timesAt, 0);
    std::memcpy(img.data(), "MADS", 4);
    set32(img, 4, 1);
    set32(img, 8, 0);                       /* base: built at address 0 */
    set32(img, 12, (uint32_t)tracks.size());
    uint32_t first = 0;
    for (int g = 0; g < ADS_GROUPS; g++) {
        set32(img, kGroups + 8 * g, kTracks + 0x14 * first);
        set32(img, kGroups + 8 * g + 4, count[g]);
        first += count[g];
    }
    for (size_t t = 0; t < tracks.size(); t++) {
        const uint32_t m = markersAt + kMarkers * (uint32_t)t;
        set32(img, m, m);                   /* base: where it was built (bgm_markers_relocate) */
        set32(img, m + 4, tracks[t].rate);
        set32(img, m + 8, tracks[t].length);
        for (int ch = 0; ch < ADS_MARKER_CHANNELS; ch++) {
            const std::vector<uint32_t> &v = tracks[t].markers[ch];
            set32(img, m + 0x0C + 4 * ch, (uint32_t)v.size());
            set32(img, m + 0x8C + 4 * ch, v.empty() ? 0 : (uint32_t)img.size());
            for (uint32_t x : v)
                put32(img, x);
        }
        set32(img, kTracks + 0x14 * (uint32_t)t + 16, m);
    }
    std::map<std::string, uint32_t> strings;
    auto add = [&](const std::string &s) {
        auto it = strings.find(s);
        if (it != strings.end())
            return it->second;
        const uint32_t at = (uint32_t)img.size();
        img.insert(img.end(), s.begin(), s.end());
        img.push_back(0);
        strings[s] = at;
        return at;
    };
    for (size_t t = 0; t < tracks.size(); t++) {
        const uint32_t r = kTracks + 0x14 * (uint32_t)t;
        set32(img, r, add(tracks[t].id));
        set32(img, r + 4, add(tracks[t].file));
        set32(img, r + 8, add(tracks[t].title));
        set32(img, r + 12, add(tracks[t].artist));
    }
    while (img.size() & 15)
        img.push_back(0);
    return img;
}

} /* namespace spu2 */
