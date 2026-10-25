/* pssdump - test tool of the native movie / IPU picture decoder
 * (src/engine/media/native, docs/movies.md).
 *
 *   pssdump info FILE.pss                 stream information
 *   pssdump decode FILE.pss [options]     frames as PNG, sound as WAV
 *       --out DIR      where the PNGs go (default: no frames written)
 *       --every N      every Nth frame (default 30)
 *       --first N      start at frame N
 *       --count N      at most N frames written
 *       --field        write each frame's two halves (the stacked fields) too
 *       --wav FILE     the sound
 *       --loop N       play N times (looping, as the menu backgrounds)
 *       --yraw FILE    every frame's type byte and luma plane (analysis)
 *       --ads FILE     the raw audio through MovieOptions::audioSink (an .ads image)
 *   pssdump bench FILE.pss [--no-rgba]    decode speed (no output)
 *   pssdump check FILE.pss ...            decode every movie completely and
 *                                         report frames, errors, sound, speed
 *   pssdump play FILE.pss ...             simulated 59.94 Hz playback (a frame
 *                                         every second field, the sound pulled
 *                                         per field): under-runs, buffering
 *   pssdump ipic FILE [--out DIR] [--blocks]
 *                                         decode the IPU pictures of an
 *                                         IpicArchive (course.ipic) or one
 *                                         "ipum" picture
 *   pssdump selftest                      tables, IDCT accuracy, CSC, ADPCM
 */
#include "engine/media/native/ipu_csc.h"
#include "engine/media/native/ipu_image.h"
#include "engine/media/native/movie.h"
#include "engine/media/native/mpeg2_idct.h"
#include "engine/media/native/mpeg2_tables.h"
#include "engine/media/native/pss_audio.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

using namespace gt3media;

namespace {

/* ------------------------------------------------------------------------ */
/* PNG (stored deflate blocks) and WAV writers                              */
/* ------------------------------------------------------------------------ */

uint32_t g_crcTable[256];

void crc_init()
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_crcTable[n] = c;
    }
}

uint32_t crc32(const uint8_t *p, size_t n, uint32_t crc = 0)
{
    crc = ~crc;
    for (size_t i = 0; i < n; i++)
        crc = g_crcTable[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void put32be(std::vector<uint8_t> &v, uint32_t x)
{
    v.push_back((uint8_t)(x >> 24));
    v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));
    v.push_back((uint8_t)x);
}

void png_chunk(std::FILE *f, const char *type, const std::vector<uint8_t> &data)
{
    std::vector<uint8_t> c;
    put32be(c, (uint32_t)data.size());
    c.insert(c.end(), type, type + 4);
    c.insert(c.end(), data.begin(), data.end());
    uint32_t crc = crc32(c.data() + 4, c.size() - 4);
    put32be(c, crc);
    std::fwrite(c.data(), 1, c.size(), f);
}

/* RGBA8 picture -> RGB PNG. */
bool write_png(const std::string &path, const uint8_t *rgba, int w, int h, int stride)
{
    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (!f)
        return false;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr;
    put32be(ihdr, (uint32_t)w);
    put32be(ihdr, (uint32_t)h);
    ihdr.push_back(8);      /* bit depth */
    ihdr.push_back(2);      /* RGB */
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    png_chunk(f, "IHDR", ihdr);

    std::vector<uint8_t> raw;
    raw.reserve((size_t)(w * 3 + 1) * h);
    for (int y = 0; y < h; y++) {
        raw.push_back(0);
        const uint8_t *s = rgba + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            raw.push_back(s[x * 4 + 0]);
            raw.push_back(s[x * 4 + 1]);
            raw.push_back(s[x * 4 + 2]);
        }
    }
    std::vector<uint8_t> z;
    z.push_back(0x78);
    z.push_back(0x01);
    size_t pos = 0;
    do {
        size_t n = std::min<size_t>(65535, raw.size() - pos);
        bool last = pos + n == raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back((uint8_t)n);
        z.push_back((uint8_t)(n >> 8));
        z.push_back((uint8_t)~n);
        z.push_back((uint8_t)(~n >> 8));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
    } while (pos < raw.size());
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    put32be(z, (b << 16) | a);
    png_chunk(f, "IDAT", z);
    png_chunk(f, "IEND", std::vector<uint8_t>());
    std::fclose(f);
    return true;
}

struct WavWriter {
    std::FILE *f = nullptr;
    uint32_t frames = 0;
    int channels = 0;
    int rate = 0;

    bool open(const std::string &path, int ch, int sr)
    {
        f = std::fopen(path.c_str(), "wb");
        if (!f)
            return false;
        channels = ch;
        rate = sr;
        uint8_t hdr[44] = {0};
        std::fwrite(hdr, 1, 44, f);
        return true;
    }
    void write(const int16_t *s, size_t n)
    {
        if (f) {
            std::fwrite(s, sizeof(int16_t), n * channels, f);
            frames += (uint32_t)n;
        }
    }
    static void le32(uint8_t *p, uint32_t v)
    {
        p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
    }
    void close()
    {
        if (!f)
            return;
        uint32_t data = frames * channels * 2;
        uint8_t h[44];
        std::memcpy(h, "RIFF", 4);
        le32(h + 4, 36 + data);
        std::memcpy(h + 8, "WAVEfmt ", 8);
        le32(h + 16, 16);
        h[20] = 1; h[21] = 0;
        h[22] = (uint8_t)channels; h[23] = 0;
        le32(h + 24, rate);
        le32(h + 28, rate * channels * 2);
        h[32] = (uint8_t)(channels * 2); h[33] = 0;
        h[34] = 16; h[35] = 0;
        std::memcpy(h + 36, "data", 4);
        le32(h + 40, data);
        std::fseek(f, 0, SEEK_SET);
        std::fwrite(h, 1, 44, f);
        std::fclose(f);
        f = nullptr;
    }
};

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

double now()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool read_file(const std::string &path, std::vector<uint8_t> &out)
{
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize((size_t)n);
    size_t got = n > 0 ? std::fread(out.data(), 1, (size_t)n, f) : 0;
    std::fclose(f);
    return got == (size_t)n;
}

std::string base_name(const std::string &path)
{
    size_t s = path.find_last_of("/\\");
    std::string b = s == std::string::npos ? path : path.substr(s + 1);
    size_t d = b.find_last_of('.');
    return d == std::string::npos ? b : b.substr(0, d);
}

std::string masks(unsigned m)
{
    std::string s;
    for (int i = 0; i < 8; i++)
        if (m & (1u << i)) {
            if (!s.empty())
                s += ",";
            s += std::to_string(i);
        }
    return s.empty() ? "-" : s;
}

void print_video_stats(const VideoStats &v)
{
    std::printf("  video: %lld seq, %lld gop, pictures I %lld P %lld B %lld, output %lld, slices %lld\n",
                (long long)v.sequences, (long long)v.gops, (long long)v.pictures[1], (long long)v.pictures[2],
                (long long)v.pictures[3], (long long)v.output, (long long)v.slices);
    std::printf("         mb intra %lld inter %lld skipped %lld quant %lld; frame-mc %lld field-mc %lld dual-prime %lld field-dct %lld\n",
                (long long)v.mbIntra, (long long)v.mbInter, (long long)v.mbSkipped, (long long)v.mbQuant,
                (long long)v.mbFrameMotion, (long long)v.mbFieldMotion, (long long)v.mbDualPrime,
                (long long)v.mbFieldDct);
    std::printf("         blocks %lld (dc only %lld), mv outside %lld, slices with long tails %lld / ending mid-row %lld\n",
                (long long)v.blocks, (long long)v.blocksDcOnly, (long long)v.mvOutside,
                (long long)v.sliceLongTails, (long long)v.sliceEndsMidRow);
    std::printf("         picture_structure {%s} intra_dc_precision {%s} q_scale_type {%s} intra_vlc_format {%s}\n",
                masks(v.pictureStructures).c_str(), masks(v.intraDcPrecisions).c_str(),
                masks(v.qScaleTypes).c_str(), masks(v.intraVlcFormats).c_str());
    std::printf("         alternate_scan {%s} frame_pred_frame_dct {%s} concealment_mv {%s} progressive_frame {%s} top_field_first {%s} repeat_first_field {%s}\n",
                masks(v.alternateScans).c_str(), masks(v.framePredFrameDct).c_str(),
                masks(v.concealmentVectors).c_str(), masks(v.progressiveFrames).c_str(),
                masks(v.topFieldFirsts).c_str(), masks(v.repeatFirstFields).c_str());
    std::printf("         errors %lld, concealed mbs %lld, unsupported %lld, skipped pictures %lld%s%s\n",
                (long long)v.errors, (long long)v.concealedMbs, (long long)v.unsupported,
                (long long)v.skippedPictures, v.lastError[0] ? ", last: " : "", v.lastError);
}

void print_info(const MovieInfo &in)
{
    std::printf("  %dx%d, %.3f fps (frame_rate_code %d), aspect code %d\n", in.width, in.height, in.frameRate,
                in.frameRateCode, in.aspectRatioCode);
    if (in.hasAudio)
        std::printf("  audio: codec 0x%02X (%s), %d Hz, %d channels, interleave 0x%X, %u data bytes, loop %d..%d, delay %.4f s\n",
                    in.audio.codec, in.audio.codec == PSS_AUDIO_ADPCM ? "SPU2 ADPCM" : "PCM16",
                    in.audio.sampleRate, in.audio.channels, in.audio.interleave, in.audio.dataSize,
                    in.audio.loopStart, in.audio.loopEnd, in.audioDelay);
    else
        std::printf("  audio: none\n");
    std::printf("  first PTS: video %lld, audio %lld\n", (long long)in.firstVideoPts, (long long)in.firstAudioPts);
}

struct Args {
    std::vector<std::string> files;
    std::string out;
    std::string wav;
    std::string yraw;
    std::string ads;
    int every = 30;
    int first = 0;
    int count = 1 << 30;
    int loop = 1;
    bool field = false;
    bool rgba = true;
    bool blocks = false;
};

Args parse(int argc, char **argv, int from)
{
    Args a;
    for (int i = from; i < argc; i++) {
        std::string s = argv[i];
        auto val = [&](void) -> const char * { return i + 1 < argc ? argv[++i] : ""; };
        if (s == "--out")
            a.out = val();
        else if (s == "--wav")
            a.wav = val();
        else if (s == "--yraw")
            a.yraw = val();
        else if (s == "--ads")
            a.ads = val();
        else if (s == "--every")
            a.every = std::max(1, std::atoi(val()));
        else if (s == "--first")
            a.first = std::atoi(val());
        else if (s == "--count")
            a.count = std::atoi(val());
        else if (s == "--loop")
            a.loop = std::max(1, std::atoi(val()));
        else if (s == "--field")
            a.field = true;
        else if (s == "--no-rgba")
            a.rgba = false;
        else if (s == "--blocks")
            a.blocks = true;
        else
            a.files.push_back(s);
    }
    return a;
}

/* ------------------------------------------------------------------------ */
/* Commands                                                                 */
/* ------------------------------------------------------------------------ */

int cmd_info(const Args &a)
{
    for (const std::string &path : a.files) {
        std::FILE *f = std::fopen(path.c_str(), "rb");
        if (!f) {
            std::printf("%s: cannot open\n", path.c_str());
            continue;
        }
        Movie m;
        std::printf("%s\n", path.c_str());
        if (!m.open(file_source(f))) {
            std::printf("  not an MPEG-2 PSS movie\n");
        } else {
            print_info(m.info());
        }
        std::fclose(f);
    }
    return 0;
}

int cmd_decode(const Args &a)
{
    if (a.files.empty())
        return 1;
    const std::string &path = a.files[0];
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) {
        std::printf("%s: cannot open\n", path.c_str());
        return 1;
    }
    MovieOptions opt;
    opt.loop = a.loop > 1;
    opt.alpha = 0xFF;
    std::FILE *ads = nullptr;
    if (!a.ads.empty()) {
        /* the raw-audio sink: the .ads image the sound system's push path takes */
        ads = std::fopen(a.ads.c_str(), "wb");
        opt.audioSink = [](void *user, const uint8_t *d, size_t n, int64_t) {
            std::fwrite(d, 1, n, (std::FILE *)user);
        };
        opt.audioSinkUser = ads;
    }
    Movie m;
    if (!m.open(file_source(f), opt)) {
        std::printf("%s: not an MPEG-2 PSS movie\n", path.c_str());
        std::fclose(f);
        return 1;
    }
    std::printf("%s\n", path.c_str());
    print_info(m.info());
    if (!a.out.empty())
        MKDIR(a.out.c_str());
    WavWriter wav;
    if (!a.wav.empty() && m.info().hasAudio)
        wav.open(a.wav, m.info().audio.channels, m.info().audio.sampleRate);
    std::string base = base_name(path);
    std::vector<int16_t> pcm(1 << 16);
    std::FILE *yraw = a.yraw.empty() ? nullptr : std::fopen(a.yraw.c_str(), "wb");
    MovieFrame fr;
    int written = 0;
    int64_t frames = 0;
    double t0 = now();
    while (m.nextFrame(fr)) {
        frames++;
        if (fr.loop >= a.loop)
            break;
        if (!a.out.empty() && fr.index >= a.first && (fr.index - a.first) % a.every == 0 && written < a.count) {
            char name[512];
            int t = fr.picture.type;
            snprintf(name, sizeof(name), "%s/%s_%05lld_%c.png", a.out.c_str(), base.c_str(), (long long)fr.index,
                     t >= 1 && t <= 4 ? " IPBD"[t] : '?');
            write_png(name, fr.rgba, fr.width, fr.height, fr.rgbaStride);
            if (a.field) {
                int hh = fr.height / 2;
                snprintf(name, sizeof(name), "%s/%s_%05lld_upper.png", a.out.c_str(), base.c_str(), (long long)fr.index);
                write_png(name, fr.rgba, fr.width, hh, fr.rgbaStride);
                snprintf(name, sizeof(name), "%s/%s_%05lld_lower.png", a.out.c_str(), base.c_str(), (long long)fr.index);
                write_png(name, fr.rgba + (size_t)hh * fr.rgbaStride, fr.width, hh, fr.rgbaStride);
            }
            written++;
        }
        if (yraw) {
            /* frame type byte, then the luma plane */
            uint8_t t = (uint8_t)fr.picture.type;
            std::fwrite(&t, 1, 1, yraw);
            for (int y = 0; y < fr.height; y++)
                std::fwrite(fr.picture.y + (size_t)y * fr.picture.yStride, 1, fr.width, yraw);
        }
        size_t n;
        while ((n = m.audioAvailable()) > 0) {
            n = m.readAudio(pcm.data(), std::min<size_t>(n, pcm.size() / 2));
            wav.write(pcm.data(), n);
        }
    }
    if (yraw)
        std::fclose(yraw);
    /* the rest of the sound */
    size_t n;
    while (m.info().hasAudio && (n = m.readAudio(pcm.data(), pcm.size() / 2)) > 0)
        wav.write(pcm.data(), n);
    double dt = now() - t0;
    wav.close();
    if (ads) {
        std::printf("  raw audio (.ads image): %ld bytes\n", std::ftell(ads));
        std::fclose(ads);
    }
    MovieStats st = m.stats();
    std::printf("  %lld frames (%d written) in %.2f s: %.1f fps; %u audio sample frames\n", (long long)frames,
                written, dt, frames / dt, wav.frames);
    print_video_stats(st.video);
    std::printf("  demux: %lld packs, %lld video / %lld audio / %lld padding / %lld other packets, %lld resync bytes, %lld errors\n",
                (long long)st.demux.packs, (long long)st.demux.videoPackets, (long long)st.demux.audioPackets,
                (long long)st.demux.paddingPackets, (long long)st.demux.otherPackets,
                (long long)st.demux.resyncs, (long long)st.demux.errors);
    std::fclose(f);
    return 0;
}

int cmd_bench(const Args &a)
{
    for (const std::string &path : a.files) {
        std::vector<uint8_t> data;
        if (!read_file(path, data)) {
            std::printf("%s: cannot read\n", path.c_str());
            continue;
        }
        MemorySource ms;
        ms.data = data.data();
        ms.size = data.size();
        MovieOptions opt;
        opt.convertRgba = a.rgba;
        opt.decodeAudio = true;
        Movie m;
        if (!m.open(ms.source(), opt)) {
            std::printf("%s: not a movie\n", path.c_str());
            continue;
        }
        std::vector<int16_t> pcm(1 << 16);
        MovieFrame fr;
        int64_t frames = 0;
        double t0 = now();
        while (m.nextFrame(fr)) {
            frames++;
            size_t n;
            while ((n = m.audioAvailable()) > 0)
                m.readAudio(pcm.data(), std::min<size_t>(n, pcm.size() / 2));
        }
        double dt = now() - t0;
        double seconds = frames / m.info().frameRate;
        std::printf("%s: %dx%d, %lld frames in %.3f s = %.1f fps (%.1fx real time, %.1f%% of one core)%s\n",
                    base_name(path).c_str(), m.info().width, m.info().height, (long long)frames, dt, frames / dt,
                    seconds / dt, 100.0 * dt / seconds, a.rgba ? "" : " [no RGBA]");
    }
    return 0;
}

/* The game's pattern, simulated: a 59.94 Hz vblank; the sound is pulled
 * as an audio device would (44100 / 59.94 sample frames per field), a
 * frame is taken every second field (MenuBgMovie::update takes one on the
 * odd field).  Reports under-runs, buffering and the A/V position. */
int cmd_play(const Args &a)
{
    for (const std::string &path : a.files) {
        std::FILE *f = std::fopen(path.c_str(), "rb");
        if (!f)
            continue;
        Movie m;
        if (!m.open(file_source(f))) {
            std::fclose(f);
            continue;
        }
        const MovieInfo &in = m.info();
        const double vblank = 60000.0 / 1001.0;
        std::vector<int16_t> pcm(8192);
        double audioDue = 0;
        int64_t audioOut = 0, underruns = 0, fields = 0, frames = 0, missed = 0;
        size_t maxAudio = 0;
        bool videoDone = false;
        const int64_t audioTotal = in.hasAudio && in.audio.codec == PSS_AUDIO_ADPCM
            ? (int64_t)in.audio.dataSize / 16 / in.audio.channels * 28 : 0;
        while (!videoDone || (in.hasAudio && audioOut < audioTotal)) {
            fields++;
            if ((fields & 1) && !videoDone) {
                MovieFrame fr;
                if (m.nextFrame(fr))
                    frames++;
                else
                    videoDone = true;
            }
            if (in.hasAudio) {
                audioDue += in.audio.sampleRate / vblank;
                size_t want = (size_t)audioDue;
                audioDue -= (double)want;
                size_t got = m.readAudio(pcm.data(), want);
                if (got < want && audioOut + (int64_t)got < audioTotal)
                    underruns++;                    /* not the end of the sound */
                audioOut += (int64_t)got;
                maxAudio = std::max(maxAudio, m.audioAvailable());
                if (got == 0 && videoDone)
                    break;
            }
            if (fields > 4000000)
                break;
        }
        (void)missed;
        double videoSec = frames / in.frameRate;
        double playSec = fields / vblank;
        std::printf("%s: %lld fields (%.2f s), %lld frames (%.2f s of movie at %.3f fps)", base_name(path).c_str(),
                    (long long)fields, playSec, (long long)frames, videoSec, in.frameRate);
        if (in.hasAudio)
            std::printf(", audio %.2f s, under-runs %lld, most buffered %.3f s", audioOut / (double)in.audio.sampleRate,
                        (long long)underruns, maxAudio / (double)in.audio.sampleRate);
        std::printf("\n");
        std::fclose(f);
    }
    return 0;
}

/* Every movie, completely: the summary the docs quote. */
int cmd_check(const Args &a)
{
    std::vector<std::string> files;
    for (const std::string &p : a.files) {
        if (p.size() > 4 && (p.substr(p.size() - 4) == ".pss" || p.substr(p.size() - 4) == ".PSS")) {
            files.push_back(p);
            continue;
        }
        /* a directory: its .pss files (listed by the caller's shell would
         * be simpler; keep the tool free of platform calls) */
        std::printf("%s: pass .pss files\n", p.c_str());
    }
    int bad = 0;
    double total = 0, totalSeconds = 0;
    std::printf("%-26s %9s %6s %4s %4s %4s %4s %4s %4s %4s %9s %7s %6s %8s\n", "movie", "size", "frames", "I", "P", "B",
                "err", "conc", "uns", "skip", "audio", "a.sec", "fps", "crc");
    for (const std::string &path : files) {
        std::FILE *f = std::fopen(path.c_str(), "rb");
        if (!f) {
            std::printf("%s: cannot open\n", path.c_str());
            bad++;
            continue;
        }
        std::fseek(f, 0, SEEK_END);
        long fileSize = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        Movie m;
        if (!m.open(file_source(f))) {
            std::printf("%-26s not a movie\n", base_name(path).c_str());
            std::fclose(f);
            bad++;
            continue;
        }
        std::vector<int16_t> pcm(1 << 16);
        MovieFrame fr;
        int64_t frames = 0, audioFrames = 0;
        double t0 = now();
        int64_t lastPts = -1;
        int ptsOrderErrors = 0;
        uint32_t crc = 0;
        double tCrc = 0;
        while (m.nextFrame(fr)) {
            frames++;
            /* a checksum of the decoded planes (compares builds; not timed) */
            double tc = now();
            const VideoPicture &p = fr.picture;
            for (int y = 0; y < p.height; y++)
                crc = crc32(p.y + (size_t)y * p.yStride, p.width, crc);
            for (int y = 0; y < p.height / 2; y++) {
                crc = crc32(p.cb + (size_t)y * p.cStride, p.width / 2, crc);
                crc = crc32(p.cr + (size_t)y * p.cStride, p.width / 2, crc);
            }
            tCrc += now() - tc;
            if (fr.picture.pts >= 0) {
                if (lastPts >= 0 && fr.picture.pts <= lastPts)
                    ptsOrderErrors++;
                lastPts = fr.picture.pts;
            }
            size_t n;
            while ((n = m.audioAvailable()) > 0)
                audioFrames += (int64_t)m.readAudio(pcm.data(), std::min<size_t>(n, pcm.size() / 2));
        }
        size_t n;
        while (m.info().hasAudio && (n = m.readAudio(pcm.data(), pcm.size() / 2)) > 0)
            audioFrames += (int64_t)n;
        double dt = now() - t0 - tCrc;
        MovieStats st = m.stats();
        const VideoStats &v = st.video;
        int64_t pics = v.pictures[1] + v.pictures[2] + v.pictures[3];
        bool ok = v.errors == 0 && v.concealedMbs == 0 && v.unsupported == 0 && v.skippedPictures == 0 &&
                  v.sliceLongTails == 0 && v.sliceEndsMidRow == 0 &&
                  frames == pics && ptsOrderErrors == 0 && st.demux.resyncs == 0 && st.demux.errors == 0 &&
                  st.audio.errors == 0 && st.audio.badFrames == 0;
        double asec = m.info().hasAudio ? (double)audioFrames / m.info().audio.sampleRate : 0;
        std::fclose(f);
        std::printf("%-26s %9ld %6lld %4lld %4lld %4lld %4lld %4lld %4lld %4lld %9lld %7.2f %6.1f %08X %s\n",
                    base_name(path).c_str(), fileSize, (long long)frames, (long long)v.pictures[1],
                    (long long)v.pictures[2], (long long)v.pictures[3], (long long)v.errors,
                    (long long)v.concealedMbs, (long long)v.unsupported, (long long)v.skippedPictures,
                    (long long)audioFrames, asec, frames / dt, crc, ok ? "ok" : "FAIL");
        if (!ok) {
            bad++;
            print_video_stats(v);
            if (ptsOrderErrors)
                std::printf("  PTS out of order: %d\n", ptsOrderErrors);
        }
        total += dt;
        totalSeconds += frames / m.info().frameRate;
    }
    std::printf("%d movies, %d failed; decoding took %.1f s for %.1f s of video (%.1fx real time)\n",
                (int)files.size(), bad, total, totalSeconds, total > 0 ? totalSeconds / total : 0);
    return bad ? 1 : 0;
}

int cmd_ipic(const Args &a)
{
    for (const std::string &path : a.files) {
        std::vector<uint8_t> data;
        if (!read_file(path, data)) {
            std::printf("%s: cannot read\n", path.c_str());
            continue;
        }
        if (!a.out.empty())
            MKDIR(a.out.c_str());
        struct Item {
            std::string name;
            size_t offset;
        };
        std::vector<Item> items;
        int n = ipic_archive_count(data.data(), data.size());
        if (n > 0) {
            for (int i = 0; i < n; i++) {
                IpicEntryInfo e;
                if (ipic_archive_entry(data.data(), data.size(), i, &e))
                    items.push_back({e.name, e.offset});
            }
        } else {
            items.push_back({base_name(path), 0});
        }
        int bad = 0;
        double t0 = now();
        for (const Item &it : items) {
            IpuImageInfo info;
            if (!ipu_image_info(data.data() + it.offset, data.size() - it.offset, &info)) {
                std::printf("  %-16s not an ipum picture\n", it.name.c_str());
                bad++;
                continue;
            }
            std::vector<uint8_t> px((size_t)info.width * info.height * 4);
            IpuImageStats st;
            double t1 = now();
            bool ok = ipu_image_decode(data.data() + it.offset, data.size() - it.offset, px.data(), 0,
                                       a.blocks ? IpuLayout::Macroblocks : IpuLayout::Linear, 0xFF, &st);
            double dt = now() - t1;
            std::vector<uint8_t> lin;
            const uint8_t *img = px.data();
            if (a.blocks) {
                lin.resize(px.size());
                ps2_blocks_to_rgba(px.data(), info.width, info.height, lin.data(), info.width * 4, false);
                img = lin.data();
            }
            /* the picture must end exactly: zero bits, then 0x000001B0 */
            bool exact = st.tailBits >= 0 && st.tailBits < 8 && st.tailZero && st.endCode == 0x1B0;
            std::printf("  %-16s %dx%d flags 0x%02X size %u: %d mbs, %d quant, %d field-dct, %d jumps, end +%d bits %s 0x%03X, %.2f ms %s%s\n",
                        it.name.c_str(), info.width, info.height, info.flags, info.size, st.macroblocks,
                        st.quantChanges, st.fieldDct, st.addressJumps, st.tailBits, st.tailZero ? "zero" : "NONZERO",
                        st.endCode, dt * 1000, ok && exact ? "ok" : "FAIL ", ok ? "" : st.error);
            if (!ok || !exact)
                bad++;
            if (!a.out.empty())
                write_png(a.out + "/" + it.name + ".png", img, info.width, info.height, info.width * 4);
        }
        std::printf("%s: %d pictures, %d failed, %.1f ms\n", path.c_str(), (int)items.size(), bad, (now() - t0) * 1000);
    }
    return 0;
}

int cmd_selftest()
{
    int fails = 0;
    char msg[256];
    if (!mpeg2_tables_selftest(msg, sizeof(msg))) {
        std::printf("tables: FAIL %s\n", msg);
        fails++;
    } else {
        std::printf("tables: ok\n");
    }

    /* IDCT against the double-precision reference, IEEE 1180 style:
     * random blocks in [-L, H], 10000 each. */
    struct Range { int lo, hi; } ranges[] = {{-256, 255}, {-5, 5}, {-300, 300}};
    std::mt19937 rng(1180);
    for (const Range &r : ranges) {
        for (int sign = 1; sign >= -1; sign -= 2) {
            long long sumErr[64] = {0}, sumSq[64] = {0};
            int peak = 0;
            const int N = 10000;
            for (int it = 0; it < N; it++) {
                int16_t blk[64];
                for (int i = 0; i < 64; i++)
                    blk[i] = (int16_t)(sign * (int)(rng() % (r.hi - r.lo + 1) + r.lo));
                /* the standard test feeds DCT coefficients of random
                 * pixels; random coefficients in range are a harsher test */
                int got[64], ref[64];
                idct_int(blk, got);
                idct_reference(blk, ref);
                for (int i = 0; i < 64; i++) {
                    int e = got[i] - ref[i];
                    peak = std::max(peak, std::abs(e));
                    sumErr[i] += e;
                    sumSq[i] += (long long)e * e;
                }
            }
            double worstMse = 0, worstMe = 0, omse = 0, ome = 0;
            for (int i = 0; i < 64; i++) {
                worstMse = std::max(worstMse, (double)sumSq[i] / N);
                worstMe = std::max(worstMe, std::fabs((double)sumErr[i] / N));
                omse += (double)sumSq[i];
                ome += (double)sumErr[i];
            }
            omse /= 64.0 * N;
            ome = std::fabs(ome) / (64.0 * N);
            bool ok = peak <= 1 && worstMse <= 0.06 && worstMe <= 0.015 && omse <= 0.02 && ome <= 0.0015;
            std::printf("idct [%d,%d] x%d: peak %d, pmse %.5f, pme %.5f, omse %.5f, ome %.5f %s\n", r.lo, r.hi,
                        sign, peak, worstMse, worstMe, omse, ome, ok ? "ok" : "FAIL");
            if (!ok)
                fails++;
        }
    }

    /* CSC: grey, black, white, and the extremes against the formula. */
    uint8_t rgb[3];
    ipu_csc_pixel(16, 128, 128, rgb);
    std::printf("csc Y16: %d %d %d\n", rgb[0], rgb[1], rgb[2]);
    ipu_csc_pixel(235, 128, 128, rgb);
    std::printf("csc Y235: %d %d %d\n", rgb[0], rgb[1], rgb[2]);
    ipu_csc_pixel(255, 255, 255, rgb);
    std::printf("csc Y255 Cb255 Cr255: %d %d %d\n", rgb[0], rgb[1], rgb[2]);
    for (int y = 0; y < 256; y += 5)
        for (int cb = 0; cb < 256; cb += 7)
            for (int cr = 0; cr < 256; cr += 11) {
                int l = (0x95 * std::max(0, y - 16)) >> 6;
                int rr = (l + ((0xCC * (cr - 128)) >> 6) + 1) >> 1;
                int gg = (l + ((-0x68 * (cr - 128)) >> 6) + ((-0x32 * (cb - 128)) >> 6) + 1) >> 1;
                int bb = (l + ((0x102 * (cb - 128)) >> 6) + 1) >> 1;
                rr = std::min(255, std::max(0, rr));
                gg = std::min(255, std::max(0, gg));
                bb = std::min(255, std::max(0, bb));
                ipu_csc_pixel(y, cb, cr, rgb);
                if (rgb[0] != rr || rgb[1] != gg || rgb[2] != bb) {
                    std::printf("csc mismatch at %d %d %d\n", y, cb, cr);
                    fails++;
                    y = cb = cr = 256;
                }
            }

    /* The picture conversion (SIMD path) against the per-pixel formula. */
    {
        const int W = 70, H = 34;          /* not a multiple of 16: both paths */
        std::vector<uint8_t> py(W * H), pu(W / 2 * H / 2), pv(W / 2 * H / 2), out(W * H * 4);
        for (auto &b : py) b = (uint8_t)rng();
        for (auto &b : pu) b = (uint8_t)rng();
        for (auto &b : pv) b = (uint8_t)rng();
        py[0] = 255; pu[0] = 0; pv[0] = 255;
        py[1] = 0; py[W] = 255; pu[1] = 255; pv[1] = 0;
        ipu_csc_rgba(py.data(), W, pu.data(), pv.data(), W / 2, W, H, out.data(), W * 4, 0x80);
        int bad = 0;
        for (int yy = 0; yy < H; yy++)
            for (int xx = 0; xx < W; xx++) {
                ipu_csc_pixel(py[yy * W + xx], pu[(yy / 2) * (W / 2) + xx / 2], pv[(yy / 2) * (W / 2) + xx / 2], rgb);
                const uint8_t *o = &out[(yy * W + xx) * 4];
                if (o[0] != rgb[0] || o[1] != rgb[1] || o[2] != rgb[2] || o[3] != 0x80)
                    bad++;
            }
        std::printf("csc picture: %d mismatches %s\n", bad, bad ? "FAIL" : "ok");
        if (bad)
            fails++;
    }

    /* ADPCM: a frame of filter 1 shift 12 nibbles -> known values. */
    uint8_t frame[16] = {0x1C, 0x00};
    for (int i = 2; i < 16; i++)
        frame[i] = 0x71;                /* nibbles 1, 7 */
    int16_t out[28];
    int hist[2] = {0, 0};
    spu2_adpcm_frame(frame, out, 1, hist);
    std::printf("adpcm: %d %d %d %d ... %d\n", out[0], out[1], out[2], out[3], out[27]);
    std::printf("%s\n", fails ? "SELFTEST FAILED" : "selftest ok");
    return fails ? 1 : 0;
}

void usage()
{
    std::printf(
        "pssdump info FILE.pss...\n"
        "pssdump decode FILE.pss [--out DIR] [--every N] [--first N] [--count N] [--field] [--wav FILE] [--ads FILE]\n"
        "               [--loop N] [--yraw FILE]\n"
        "pssdump bench FILE.pss... [--no-rgba]\n"
        "pssdump check FILE.pss...\n"
        "pssdump play FILE.pss...       (simulated 59.94 Hz playback: buffering, A/V)\n"
        "pssdump ipic FILE [--out DIR] [--blocks]\n"
        "pssdump selftest\n");
}

} /* namespace */

int main(int argc, char **argv)
{
    crc_init();
    if (argc < 2) {
        usage();
        return 1;
    }
    std::string cmd = argv[1];
    Args a = parse(argc, argv, 2);
    if (cmd == "info")
        return cmd_info(a);
    if (cmd == "decode")
        return cmd_decode(a);
    if (cmd == "bench")
        return cmd_bench(a);
    if (cmd == "check")
        return cmd_check(a);
    if (cmd == "play")
        return cmd_play(a);
    if (cmd == "ipic")
        return cmd_ipic(a);
    if (cmd == "selftest")
        return cmd_selftest();
    usage();
    return 1;
}
