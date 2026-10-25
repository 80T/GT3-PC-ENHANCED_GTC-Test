/* mpg_service.cpp - the pdimpeg IOP module, native (media_services.h).
 *
 * On the PS2 the EE's mpegif (T529) asked the IOP module behind 'MPG1' /
 * 'MPG2' (client T522) for a movie: open {mode, lsn, sectors, host name},
 * then feed requests that filled a 32 KB EE buffer with eight 4 KB units,
 * each one video PES packet at its start (mpegif hands each unit to
 * mpeg_player_put_pes, whose mpeg_pes_parse reads it there), and close.
 * The module kept the sound (private stream 1) and played it through the
 * BGM stream player and voices (docs/sound.md); 'MPG2' function 3 gave it
 * the presentation window from MenuBgMovie::update.  The module itself is
 * not in the EE image; what it did is inferred from what the EE expects:
 *   - the units: GT3's files have 4096 / 4082-byte video packets behind
 *     14-byte pack headers, so the module re-aligned them to 4 KB;
 *   - the end: a unit without a video packet makes put_pes answer
 *     PUT_PES_EMPTY, which ends a one-shot movie (opening, endings: mode 2,
 *     with sound) and is skipped by a looping one (the menu backgrounds:
 *     mode 3, read from the disc).  So after the last packet comes one
 *     empty unit; with mode 3 the stream then starts again (the menu
 *     backgrounds loop on the PS2 when played from the disc, not only from
 *     the preloaded copy); modes 1 (preload) and 2 give empty units from
 *     there on;
 *   - the sound: the private stream 1 payloads after their 4-byte prefix
 *     form an .ads image, which sound_native.h's push functions play with
 *     the BGM voices.  The window [lo, hi] (us of the movie) holds the
 *     sound to the pictures: it starts with the first window (the second
 *     frame shown) and pauses while it has played past hi; (0, MAX) lets
 *     it run.
 * The callers are game threads (the feeder, the render thread for the
 * window): one runs at a time, so this state needs no lock; the sound
 * functions lock themselves.  Reads are positional (platform/disc.h,
 * host_file.h), done on the calling thread as T519 / T523 do. */
#include "engine/media/native/media_services.h"

#include "engine/media/native/mp4_frames.h"
#include "engine/media/native/mp4_movie.h"
#include "engine/media/native/mp4_token.h"
#include "engine/media/native/pss_demux.h"
#include "engine/sound/native/sound_native.h"
#include "platform/disc.h"
#include "platform/host_file.h"
#include "platform/log.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace gt3media;

namespace {

const int kUnit = 0x1000;           /* one PES packet per 4 KB of the feed buffer */

struct MpgService {
    bool open = false;
    int mode = 0;
    /* the source: gt3.vol from byte `base', or a host file */
    bool host = false;
    int hostFd = -1;
    uint64_t base = 0;
    uint64_t size = 0;
    uint64_t pos = 0;
    bool readError = false;
    PssDemuxer demux;
    bool fileEnded = false;         /* every packet of this pass handed out (marker sent) */
    int pass = 0;
    /* a video packet longer than a unit, still to be handed out */
    std::vector<uint8_t> splitRest;
    int64_t splitPts = -1;
    int64_t units = 0;
    /* the sound (mode 2) */
    bool sound = false;
    bool pushOpen = false;
    bool soundEnded = false;        /* the stream's last audio bytes are pending */
    bool pushEnded = false;
    bool started = false;           /* the first presentation window has come */
    bool paused = false;            /* paused by the window */
    std::vector<uint8_t> pending;   /* audio not taken by the BGM FIFOs yet */
    size_t pendingPos = 0;
    uint64_t audioBytes = 0;
    /* PC: an MP4 movie in its place (docs/movies.md "MP4 movies"): the frame
     * tokens, the sound decoded to PCM (sound_native.h snd_native_pcm_*) */
    bool mp4 = false;
    std::string mp4Path;            /* the Windows path */
    int mp4Frames = 0, mp4Next = 0;
    bool mp4Ended = false;          /* every token of this pass handed out (marker sent) */
    std::unique_ptr<Mp4AudioReader> audio;
    std::vector<int16_t> pcm;       /* decoded, not taken yet */
    size_t pcmPos = 0;
    bool pcmEnded = false;
    uint64_t pcmFrames = 0;
};

MpgService g_mpg;

/* Media Foundation and COM on the calling game thread (the feeder, the
 * render thread for the window), once each. */
bool mf_here()
{
    static thread_local MfScope t_mf;
    return t_mf.ok();
}

bool ends_with(const std::string &s, const char *ext)
{
    const size_t n = std::strlen(ext);
    if (s.size() < n)
        return false;
    for (size_t i = 0; i < n; i++)
        if (std::tolower((unsigned char)s[s.size() - n + i]) != std::tolower((unsigned char)ext[i]))
            return false;
    return true;
}

/* The MP4 movie to play for a host file: the file itself when it is one,
 * else the .mp4 of the same name beside the .pss (a mod's movie in place of
 * the game's); "" when there is none. */
std::string mp4_for(const char *hostName)
{
    std::string h = hostName;
    if (!ends_with(h, ".mp4")) {
        if (!ends_with(h, ".pss"))
            return std::string();
        h = h.substr(0, h.size() - 4) + ".mp4";
        if (!host_file_present(h.c_str()))
            return std::string();
    }
    char full[1024];
    if (host_file_full_path(h.c_str(), full, sizeof(full)) != 0)
        return std::string();
    return full;
}

/* An MP4's next frame token as a video PES unit (its PTS the frame's time
 * at 90 kHz). */
void mp4_unit(MpgService &s, uint8_t *unit)
{
    Mp4Token t = {};
    std::memcpy(t.magic, kMp4TokenMagic, sizeof(t.magic));
    t.index = (uint32_t)s.mp4Next;
    t.frames = (uint32_t)s.mp4Frames;
    t.flags = s.mode == 2 ? 0 : MP4_TOKEN_LOOP;      /* (modes 1, 3 loop: the preloaded copy, the backgrounds) */
    std::snprintf(t.path, sizeof(t.path), "%s", s.mp4Path.c_str());
    const int64_t pts = mp4_frame_time(s.mp4Next) * 9 / 1000;   /* 100 ns -> 90 kHz */
    const int hdr = 14, len = 3 + 5 + (int)sizeof(t);
    unit[0] = 0; unit[1] = 0; unit[2] = 1; unit[3] = 0xE0;
    unit[4] = (uint8_t)(len >> 8);
    unit[5] = (uint8_t)len;
    unit[6] = 0x80;
    unit[7] = 0x80;
    unit[8] = 5;
    unit[9] = (uint8_t)(0x21 | ((pts >> 29) & 0x0E));
    unit[10] = (uint8_t)(pts >> 22);
    unit[11] = (uint8_t)(0x01 | ((pts >> 14) & 0xFE));
    unit[12] = (uint8_t)(pts >> 7);
    unit[13] = (uint8_t)(0x01 | ((pts << 1) & 0xFE));
    std::memcpy(unit + hdr, &t, sizeof(t));
}

/* An MP4's unit: the next frame token, the end marker (zeros), then (mode
 * 3) the frames again or zeros.  True for a token. */
bool mp4_fill_unit(MpgService &s, uint8_t *unit)
{
    std::memset(unit, 0, kUnit);
    if (s.mp4Ended) {
        if (s.mode != 3 || s.mp4Frames <= 0)
            return false;
        s.mp4Ended = false;                 /* the menu backgrounds start again */
        s.mp4Next = 0;
        s.pass++;
    }
    if (s.mp4Next >= s.mp4Frames) {
        s.mp4Ended = true;                  /* this unit (zeros) is the end marker */
        return false;
    }
    mp4_unit(s, unit);
    s.mp4Next++;
    s.units++;
    return true;
}

/* An MP4's sound: decoded about a second ahead of what the output holds,
 * pushed as it takes it; its end once all is in. */
void mp4_pump_audio(MpgService &s)
{
    if (!s.audio || !mf_here())
        return;
    const int ahead = s.audio->rate();
    const int ch = s.audio->channels();
    for (int guard = 0; guard < 64; guard++) {
        if (s.pcmPos < s.pcm.size()) {
            const int frames = (int)((s.pcm.size() - s.pcmPos) / (size_t)ch);
            const int took = snd_native_pcm_push(s.pcm.data() + s.pcmPos, frames);
            s.pcmPos += (size_t)took * ch;
            s.pcmFrames += (uint64_t)took;
            if (took < frames)
                break;                      /* (the output's 2 s are full) */
        }
        if (s.pcmPos >= s.pcm.size()) {
            s.pcm.clear();
            s.pcmPos = 0;
        }
        if (s.pcmEnded || snd_native_pcm_buffered() >= ahead)
            break;
        if (!s.audio->next(s.pcm)) {
            s.pcmEnded = true;
            if (s.pcmPos >= s.pcm.size())
                snd_native_pcm_end();
            break;
        }
    }
    if (s.pcmEnded && s.pcmPos >= s.pcm.size())
        snd_native_pcm_end();
}

size_t source_read(void *user, void *dst, size_t n)
{
    MpgService &s = *(MpgService *)user;
    if (s.pos >= s.size)
        return 0;
    if (n > s.size - s.pos)
        n = (size_t)(s.size - s.pos);
    bool ok;
    if (s.host)
        ok = host_file_read_at(s.hostFd, s.base + s.pos, dst, (uint32_t)n) == (int)n;
    else
        ok = disc::read_bytes(s.base + s.pos, dst, (uint32_t)n);
    if (!ok) {
        if (!s.readError)
            pclog::write("movie: read error at byte %llu of the stream", (unsigned long long)s.pos);
        s.readError = true;
        return 0;
    }
    s.pos += n;
    return n;
}

bool source_rewind(void *user)
{
    MpgService &s = *(MpgService *)user;
    s.pos = 0;
    return !s.readError;
}

/* Audio into the BGM FIFOs: what they take now, the rest later. */
void push_pending(MpgService &s)
{
    if (!s.pushOpen || !s.started)
        return;
    while (s.pendingPos < s.pending.size()) {
        int n = snd_native_bgm_push(s.pending.data() + s.pendingPos, (uint32_t)(s.pending.size() - s.pendingPos));
        if (n < 0) {
            pclog::write("movie: the sound is not an ADPCM .ads stream; playing without it");
            s.pushOpen = false;
            s.pending.clear();
            s.pendingPos = 0;
            return;
        }
        if (n == 0)
            break;
        s.pendingPos += (size_t)n;
    }
    if (s.pendingPos > 0 && s.pendingPos == s.pending.size()) {
        s.pending.clear();
        s.pendingPos = 0;
    } else if (s.pendingPos > (1u << 20)) {
        s.pending.erase(s.pending.begin(), s.pending.begin() + (ptrdiff_t)s.pendingPos);
        s.pendingPos = 0;
    }
    if (s.soundEnded && !s.pushEnded && s.pendingPos == s.pending.size()) {
        snd_native_bgm_push_end();
        s.pushEnded = true;
    }
}

/* A PES header for a piece of a video packet longer than a unit. */
int split_header(uint8_t *u, int payload, int64_t pts)
{
    int hl = pts >= 0 ? 5 : 0;
    int len = 3 + hl + payload;
    u[0] = 0; u[1] = 0; u[2] = 1; u[3] = 0xE0;
    u[4] = (uint8_t)(len >> 8);
    u[5] = (uint8_t)len;
    u[6] = 0x80;
    u[7] = pts >= 0 ? 0x80 : 0x00;
    u[8] = (uint8_t)hl;
    if (pts >= 0) {
        u[9] = (uint8_t)(0x21 | ((pts >> 29) & 0x0E));
        u[10] = (uint8_t)(pts >> 22);
        u[11] = (uint8_t)(0x01 | ((pts >> 14) & 0xFE));
        u[12] = (uint8_t)(pts >> 7);
        u[13] = (uint8_t)(0x01 | ((pts << 1) & 0xFE));
    }
    return 9 + hl;
}

/* Hands out the rest of a long packet: one unit's worth. */
void split_unit(MpgService &s, uint8_t *unit)
{
    int hdr = s.splitPts >= 0 ? 14 : 9;
    int n = (int)s.splitRest.size();
    if (n > kUnit - hdr)
        n = kUnit - hdr;
    int h = split_header(unit, n, s.splitPts);
    std::memcpy(unit + h, s.splitRest.data(), (size_t)n);
    s.splitRest.erase(s.splitRest.begin(), s.splitRest.begin() + n);
    s.splitPts = -1;
}

/* One 4 KB unit: the next video packet (the sound on the way to the
 * pending buffer), the end marker, or zeros.  True for a video packet. */
bool fill_unit(MpgService &s, uint8_t *unit)
{
    std::memset(unit, 0, kUnit);
    if (!s.splitRest.empty()) {
        split_unit(s, unit);
        return true;
    }
    for (;;) {
        if (s.fileEnded) {
            if (s.mode == 3 && s.units > 0 && s.demux.rewind()) {
                s.fileEnded = false;             /* the menu backgrounds start again */
                s.pass++;
                s.units = 0;
                continue;
            }
            return false;                        /* zeros */
        }
        PesPacket p;
        if (!s.demux.next(p)) {
            s.fileEnded = true;                  /* this unit (zeros) is the end marker */
            if (s.sound)
                s.soundEnded = true;
            return false;
        }
        if ((p.streamId & 0xF0) == 0xE0) {
            s.units++;
            if (p.rawSize <= (size_t)kUnit) {
                std::memcpy(unit, p.raw, p.rawSize);
            } else {
                s.splitRest.assign(p.payload, p.payload + p.size);
                s.splitPts = p.pts;
                split_unit(s, unit);
            }
            return true;
        }
        if (p.streamId == 0xBD && p.subStreamId == 0xFF && s.sound && s.pass == 0 && s.pushOpen) {
            s.pending.insert(s.pending.end(), p.payload, p.payload + p.size);
            s.audioBytes += p.size;
        }
    }
}

void close_service(MpgService &s)
{
    if (!s.open)
        return;
    if (s.mp4) {
        if (s.audio)
            snd_native_pcm_stop();              /* the movie's sound ends with its stream */
        s.audio.reset();
        pclog::write("movie: MP4 stream closed (mode %d, %llu sound frames)", s.mode, (unsigned long long)s.pcmFrames);
        s.open = false;
        return;
    }
    if (s.pushOpen || s.sound)
        snd_native_bgm_stop();                   /* the movie's sound ends with its stream */
    if (s.host && s.hostFd >= 0)
        host_file_close(s.hostFd);
    pclog::write("movie: stream closed (mode %d, %llu audio bytes)", s.mode, (unsigned long long)s.audioBytes);
    s.open = false;
}

/* PC: an MP4 in the movie's place (docs/movies.md "MP4 movies"): true when
 * it plays (its frames counted, its sound opened in mode 2).  One that is
 * not an MP4 gives way to the .pss beside it (false), or plays nothing when
 * it is the movie itself (true, no frames). */
bool open_mp4(MpgService &s, const char *hostName)
{
    const std::string path = mp4_for(hostName);
    if (path.empty())
        return false;
    const bool itself = ends_with(hostName, ".mp4");
    const int64_t duration = mp4_duration(path.c_str());
    if (duration <= 0) {
        pclog::write("movie: %s is not an MP4 (no 'moov' / 'mvhd' box)%s", path.c_str(),
                     itself ? "" : ": the .pss plays");
        if (!itself)
            return false;
    }
    s.mp4 = true;
    s.mp4Path = path;
    s.mp4Frames = mp4_frame_count(duration);
    s.sound = s.mode == 2;
    if (s.sound && mf_here()) {
        s.audio.reset(new Mp4AudioReader);
        if (s.audio->open(path.c_str())) {
            snd_native_pcm_open(s.audio->rate(), s.audio->channels());
            mp4_pump_audio(s);
        } else {
            s.audio.reset();                /* (no sound: the pictures play) */
        }
    }
    s.open = true;
    pclog::write("movie: MP4 %s in place of %s (mode %d, %.2f s, %d frames%s)", path.c_str(), hostName, s.mode,
                 duration / 1e7, s.mp4Frames,
                 s.audio ? "" : (s.mode == 2 ? ", no sound" : ""));
    if (s.audio)
        pclog::write("movie: MP4 sound %d Hz, %d channel(s)", s.audio->rate(), s.audio->channels());
    return true;
}

} /* namespace */

void gt3m_mpg_open(int mode, unsigned lsn, unsigned sectors, const char *hostName)
{
    MpgService &s = g_mpg;
    close_service(s);
    s = MpgService();
    s.mode = mode;
    s.size = (uint64_t)sectors * disc::kSectorSize;
    s.host = hostName != nullptr && hostName[0] != '\0';
    if (s.host && open_mp4(s, hostName))
        return;                                 /* PC: an MP4 plays instead */
    if (s.host) {
        s.hostFd = host_file_open(hostName);
        s.base = lsn;
        if (s.hostFd < 0) {
            pclog::write("movie: %s not found", hostName);
            s.size = 0;
        } else {
            /* a loose file ends before the size rounded up to sectors */
            int64_t bytes = host_file_size(s.hostFd);
            if (bytes >= (int64_t)s.base && (uint64_t)bytes - s.base < s.size)
                s.size = (uint64_t)bytes - s.base;
        }
    } else {
        s.base = (uint64_t)lsn * disc::kSectorSize;
    }
    ByteSource src;
    src.user = &s;
    src.read = source_read;
    src.rewind = source_rewind;
    s.demux.setSource(src);
    s.sound = mode == 2;
    if (s.sound) {
        snd_native_bgm_open_push();             /* the BGM voices play the movie's sound */
        s.pushOpen = true;
    }
    s.open = true;
    pclog::write("movie: stream opened (mode %d, %s %u, %llu bytes)", mode, s.host ? "host byte" : "lsn", lsn,
                 (unsigned long long)s.size);
}

int gt3m_mpg_feed(void *buf, int size)
{
    MpgService &s = g_mpg;
    uint8_t *out = (uint8_t *)buf;
    if (!s.open) {
        std::memset(out, 0, (size_t)size);
        return 0;
    }
    int packets = 0;
    for (int off = 0; off + kUnit <= size; off += kUnit)
        packets += (s.mp4 ? mp4_fill_unit(s, out + off) : fill_unit(s, out + off)) ? 1 : 0;
    if (size % kUnit)
        std::memset(out + size - size % kUnit, 0, (size_t)(size % kUnit));
    if (s.mp4)
        mp4_pump_audio(s);
    else
        push_pending(s);
    return packets;
}

void gt3m_mpg_close(void)
{
    close_service(g_mpg);
}

void gt3m_mpg_window(long long lo, long long hi)
{
    (void)lo;
    MpgService &s = g_mpg;
    if (s.open && s.mp4) {
        /* PC: an MP4's sound, held to the pictures as a .pss's */
        if (!s.audio)
            return;
        if (!s.started) {
            s.started = true;
            snd_native_pcm_start();
        }
        mp4_pump_audio(s);
        const int64_t played = snd_native_pcm_played_us();
        const bool hold = hi != INT64_MAX && played >= 0 && played > hi;
        if (hold != s.paused) {
            snd_native_pcm_pause(hold ? 1 : 0);
            s.paused = hold;
        }
        return;
    }
    if (!s.open || !s.pushOpen)
        return;
    s.started = true;
    push_pending(s);
    if (hi == INT64_MAX) {
        if (s.paused) {
            snd_native_bgm_resume();
            s.paused = false;
        }
        return;
    }
    int64_t played = snd_native_bgm_played_us();
    if (played >= 0 && played > hi) {
        if (!s.paused) {
            snd_native_bgm_pause();
            s.paused = true;
        }
    } else if (s.paused) {
        snd_native_bgm_resume();
        s.paused = false;
    }
}
