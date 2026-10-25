/* sndtest - test tool of the native sound back end (src/engine/sound/native).
 *
 *   sndtest selftest
 *   sndtest dump   BANK(.ins|.es) OUTDIR          every sample to WAV (+ listing)
 *   sndtest notes  BANK.ins OUT.wav SPEC...       notes through the EE voice
 *                  manager and the native SPU2.  SPEC: pPROG:NOTE[:VEL[:START_MS[:DUR_MS]]]
 *                  (a program of a music bank) or zZONE[:FACTOR[:START_MS[:DUR_MS]]]
 *                  (an effect: se_play_program's parameters, pitch x FACTOR)
 *   sndtest ads    FILE.ads OUT.wav [SECONDS] [--loop] [--threaded] [--volume V]
 *                  [--pause S1,S2]                pdibgm's path; status once a second
 *   sndtest seq    music.ins music.seq ENTRY OUT.wav [SECONDS] [--dry]
 *   sndtest engine ENGINE.es OUT.wav [RPM0 RPM1 SECONDS]
 *   sndtest tone   OUT.wav PITCH [FREQ] [MS]      an encoded sine loop played at PITCH
 *   sndtest adsr   OUT.wav ADSR1 ADSR2 [KEYOFF_MS] [MS]   the envelope on a DC sample
 *   sndtest reverb OUT.wav [MS]                   an impulse through core 0's STUDIO_3
 *   sndtest raw    BANK.ins ZONE PITCH OUT.wav [MS]  a zone on one voice, flat envelope
 *   sndtest pss    FILE.pss OUT.wav [SECONDS]        a movie's audio through the push path
 *   sndtest live   music.ins music.seq ENTRY FILE.ads OUT.wav [SECONDS [--stress]]
 *                  the game's threads in real time (audio pull, music thread, BGM feeder)
 *
 * Rendering runs the game's 60 Hz sound frame: game logic, snd_flush (the
 * EE's state block -> snd_native_send_state), then 800 samples of
 * snd_native_mix.  WAVs are 48 kHz stereo 16-bit unless noted.  The EE
 * voice manager / sequencer used here is a test copy (ee_sound.cpp). */
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "banks.h"
#include "ee_sound.h"
#include "engine/sound/native/ads_stream.h"
#include "engine/sound/native/sound_native.h"
#include "engine/sound/native/spu2.h"

using namespace spu2;

static const int FRAME = 800;           /* samples per 60 Hz sound frame */

/* ------------------------------------------------------------------------ */
/* WAV                                                                      */
/* ------------------------------------------------------------------------ */

static void put32(FILE *f, uint32_t v) { std::fwrite(&v, 4, 1, f); }
static void put16(FILE *f, uint16_t v) { std::fwrite(&v, 2, 1, f); }

static bool write_wav(const std::string &path, const int16_t *data, size_t frames, int channels, int rate,
                      int32_t loopStart = -1, int32_t loopEnd = -1)
{
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return false;
    }
    uint32_t dataBytes = (uint32_t)(frames * channels * 2);
    bool smpl = loopStart >= 0 && loopEnd > loopStart;
    uint32_t smplBytes = smpl ? 8 + 36 + 24 : 0;
    std::fwrite("RIFF", 1, 4, f);
    put32(f, 4 + 8 + 16 + 8 + dataBytes + smplBytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1);
    put16(f, (uint16_t)channels);
    put32(f, (uint32_t)rate);
    put32(f, (uint32_t)(rate * channels * 2));
    put16(f, (uint16_t)(channels * 2));
    put16(f, 16);
    std::fwrite("data", 1, 4, f);
    put32(f, dataBytes);
    std::fwrite(data, 2, frames * channels, f);
    if (smpl) {
        std::fwrite("smpl", 1, 4, f);
        put32(f, 36 + 24);
        put32(f, 0); put32(f, 0);
        put32(f, (uint32_t)(1000000000.0 / rate));
        put32(f, 60); put32(f, 0); put32(f, 0); put32(f, 0);
        put32(f, 1); put32(f, 0);
        put32(f, 0); put32(f, 0);
        put32(f, (uint32_t)loopStart); put32(f, (uint32_t)(loopEnd - 1));
        put32(f, 0); put32(f, 0);
    }
    std::fclose(f);
    return true;
}

/* ------------------------------------------------------------------------ */
/* A small ADPCM encoder for test signals (all filters x shifts per block)  */
/* ------------------------------------------------------------------------ */

static const int kF[5][2] = { { 0, 0 }, { 60, 0 }, { 115, -52 }, { 98, -55 }, { 122, -60 } };

static void encode_block(const int16_t *in, uint8_t *out, AdpcmHistory &h, uint8_t flags, int maxFilter = 4)
{
    double bestErr = 1e300;
    uint8_t best[16] = {};
    AdpcmHistory bestH;
    for (int filt = 0; filt <= maxFilter; filt++) {
        for (int shift = 0; shift <= 12; shift++) {
            uint8_t blk[16] = {};
            blk[0] = (uint8_t)(shift | (filt << 4));
            blk[1] = flags;
            int32_t p1 = h.prev1, p2 = h.prev2;
            double err = 0;
            for (int i = 0; i < 28; i++) {
                int32_t pred = (p1 * kF[filt][0] + p2 * kF[filt][1] + 32) >> 6;
                double want = (double)in[i] - pred;
                int n = (int)std::lround(want / (double)(1 << (12 - shift)));
                n = n < -8 ? -8 : (n > 7 ? 7 : n);
                int32_t s = ((int32_t)((uint32_t)(n & 15) << 28) >> (shift + 16)) + pred;
                s = s < -32768 ? -32768 : (s > 32767 ? 32767 : s);
                err += (s - in[i]) * (double)(s - in[i]);
                blk[2 + i / 2] |= (uint8_t)((n & 15) << ((i & 1) * 4));
                p2 = p1;
                p1 = s;
            }
            if (err < bestErr) {
                bestErr = err;
                std::memcpy(best, blk, 16);
                bestH.prev1 = p1;
                bestH.prev2 = p2;
            }
        }
    }
    std::memcpy(out, best, 16);
    h = bestH;
}

/* Encodes pcm (a multiple of 28 samples) as one looped sample. */
static std::vector<uint8_t> encode_loop(const std::vector<int16_t> &pcm)
{
    std::vector<uint8_t> out(pcm.size() / 28 * 16);
    size_t n = pcm.size() / 28;
    /* the loop's first block is encoded against the history the loop's end
     * leaves (what the decoder has after a loop jump); a few passes settle
     * it.  The very first pass of a key-on starts from a zero history. */
    AdpcmHistory start;
    for (int pass = 0; pass < 4; pass++) {
        AdpcmHistory h = start;
        for (size_t b = 0; b < n; b++) {
            uint8_t fl = ADPCM_FLAG_LOOP;
            if (b == 0)
                fl |= ADPCM_FLAG_LOOP_START;
            if (b == n - 1)
                fl |= ADPCM_FLAG_LOOP_END;
            encode_block(&pcm[b * 28], &out[b * 16], h, fl);
        }
        start = h;
    }
    return out;
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

static std::string base_name(const std::string &p)
{
    size_t s = p.find_last_of("/\\");
    std::string b = s == std::string::npos ? p : p.substr(s + 1);
    size_t d = b.find_last_of('.');
    return d == std::string::npos ? b : b.substr(0, d);
}

/* The sample rate a zone plays at for `note' (44100 * 2^((note*256 - base) / 3072)). */
static double zone_rate(const Zone &z, int note)
{
    return 44100.0 * std::pow(2.0, ((double)note * 256 - z.baseNote) / 3072.0);
}

static void decode_sample(const std::vector<uint8_t> &samples, uint32_t start, std::vector<int16_t> &pcm,
                          int32_t &loopStart, bool &looped)
{
    uint32_t len;
    int32_t loop;
    adpcm_sample_extent(samples.data(), start, (uint32_t)samples.size(), &len, &loop, &looped);
    AdpcmHistory h;
    pcm.clear();
    for (uint32_t o = 0; o + 16 <= len; o += 16) {
        int16_t s[28];
        adpcm_decode_block(&samples[start + o], s, h);
        pcm.insert(pcm.end(), s, s + 28);
    }
    loopStart = (looped && loop >= 0) ? loop / 16 * 28 : -1;
}

/* Places a bank's samples in SPU RAM (as file_read_sound_bank) and
 * relocates its addresses (inst_relocate / engn_relocate). */
static void place_inst(InstBank &b, uint32_t spu)
{
    snd_native_write_spu(spu, b.samples.data(), (uint32_t)b.samples.size());
    b.spuBase = spu;
    for (Zone &z : b.zones)
        z.addr8 += spu >> 3;
}

static void place_engn(EngnBank &b, uint32_t spu)
{
    snd_native_write_spu(spu, b.samples.data(), (uint32_t)b.samples.size());
    b.spuBase = spu;
    for (auto &set : b.sets)
        for (EngnSeg &g : set)
            g.addr += spu;
}

/* The music_init state: the native IOP side, then the EE voice manager with
 * STUDIO_3 at depth 0xFFF, effect volume 0xFFF, effects on. */
static void sound_start(bool reverb = true)
{
    snd_native_init();
    ee::snd_init();
    ee::seq_init();
    if (reverb) {
        ee::snd_set_reverb_studio3(0xFFF, 0xFFF);
        ee::snd_set_effect_volume(0xFFF, 0xFFF);
        ee::snd_set_effect_enable(1);
    }
}

struct Render {
    std::vector<int16_t> out;
    void frame()
    {
        ee::snd_flush();
        size_t at = out.size();
        out.resize(at + FRAME * 2);
        snd_native_mix(&out[at], FRAME);
    }
};

static void report_levels(const std::vector<int16_t> &out)
{
    double sum = 0;
    int peak = 0;
    for (int16_t s : out) {
        sum += (double)s * s;
        peak = std::max(peak, std::abs((int)s));
    }
    double rms = out.empty() ? 0 : std::sqrt(sum / out.size());
    std::printf("  %.2f s, peak %d (%.1f dBFS), rms %.1f dBFS\n", out.size() / 2 / 48000.0, peak,
                20 * std::log10(peak / 32768.0 + 1e-12), 20 * std::log10(rms / 32768.0 + 1e-12));
}

/* ------------------------------------------------------------------------ */
/* Commands                                                                 */
/* ------------------------------------------------------------------------ */

static int cmd_dump(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "dump BANK OUTDIR\n");
        return 2;
    }
    std::vector<uint8_t> file;
    if (!load_file(argv[0], file)) {
        std::fprintf(stderr, "cannot read %s\n", argv[0]);
        return 1;
    }
    std::string outdir = argv[1], name = base_name(argv[0]), err;
    if (file.size() >= 4 && std::memcmp(file.data(), "INST", 4) == 0) {
        InstBank b;
        if (!parse_inst(file, b, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        std::printf("%s: INST, %zu zones, %zu programs, samples 0x%zX bytes\n", name.c_str(), b.zones.size(),
                    b.programs.size(), b.samples.size());
        for (size_t i = 0; i < b.zones.size(); i++) {
            const Zone &z = b.zones[i];
            std::vector<int16_t> pcm;
            int32_t loop;
            bool looped;
            decode_sample(b.samples, z.addr8 * 8, pcm, loop, looped);
            int note = z.lo == z.hi ? z.lo : (z.baseNote + 128) >> 8;
            int rate = (int)std::lround(zone_rate(z, note));
            char path[1024];
            std::snprintf(path, sizeof(path), "%s/%s_z%03zu.wav", outdir.c_str(), name.c_str(), i);
            write_wav(path, pcm.data(), pcm.size(), 1, rate, loop, looped ? (int32_t)pcm.size() : -1);
            std::printf("  z%03zu addr 0x%05X %6zu samples %s%-6s rate %5d (note %3d, base %04X) keys %3d-%3d "
                        "vol %5d pan %3d prio %d fx %d one-shot %d adsr %04X/%04X\n",
                        i, z.addr8 * 8, pcm.size(), looped ? "loop@" : "", looped ? std::to_string(loop).c_str() : "",
                        rate, note, z.baseNote, z.lo, z.hi, z.vol, z.pan, z.prio, z.flags & 1, z.unk0A != 0,
                        z.adsr & 0xFFFF, z.adsr >> 16);
        }
        for (size_t p = 0; p < b.programs.size(); p++) {
            const Program &pr = b.programs[p];
            std::printf("  program %3zu pan %3d vol %5d zones", p, pr.pan, pr.vol);
            for (uint8_t z : pr.zones)
                std::printf(" %d", z);
            std::printf("\n");
        }
        return 0;
    }
    EngnBank e;
    if (!parse_engn(file, e, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    std::printf("%s: ENGN, sets %zu / %zu, samples 0x%zX bytes\n", name.c_str(), e.sets[0].size(), e.sets[1].size(),
                e.samples.size());
    for (int s = 0; s < 2; s++)
        for (size_t i = 0; i < e.sets[s].size(); i++) {
            const EngnSeg &g = e.sets[s][i];
            std::vector<int16_t> pcm;
            int32_t loop;
            bool looped;
            decode_sample(e.samples, g.addr, pcm, loop, looped);
            int rate = (int)std::lround(44100.0 * g.pitch / 4096.0);
            char path[1024];
            std::snprintf(path, sizeof(path), "%s/%s_s%d_%02zu.wav", outdir.c_str(), name.c_str(), s, i);
            write_wav(path, pcm.data(), pcm.size(), 1, rate, loop, looped ? (int32_t)pcm.size() : -1);
            std::printf("  set %d seg %zu addr 0x%05X %6zu samples %s rate %5d (pitch %04X at %d rpm) rpm %d-%d vol %d\n",
                        s, i, g.addr, pcm.size(), looped ? "looped" : "one-shot", rate, g.pitch, g.baseRpm, g.lo,
                        g.hi, g.vol);
        }
    return 0;
}

static int cmd_notes(int argc, char **argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "notes BANK.ins OUT.wav SPEC...\n");
        return 2;
    }
    std::vector<uint8_t> file;
    std::string err;
    InstBank b;
    if (!load_file(argv[0], file) || !parse_inst(file, b, err)) {
        std::fprintf(stderr, "%s: %s\n", argv[0], err.c_str());
        return 1;
    }
    sound_start();
    place_inst(b, 0x5040);

    struct Ev { bool prog; int idx, note, vel; double factor; int start, dur; int8_t voice; bool on, off; };
    std::vector<Ev> evs;
    int endMs = 0;
    for (int i = 2; i < argc; i++) {
        Ev e = { argv[i][0] == 'p', 0, 60, 100, 1.0, 0, 1000, -1, false, false };
        std::vector<std::string> f;
        std::string s = argv[i] + 1;
        size_t at;
        while ((at = s.find(':')) != std::string::npos) {
            f.push_back(s.substr(0, at));
            s = s.substr(at + 1);
        }
        f.push_back(s);
        e.idx = std::atoi(f[0].c_str());
        if (e.prog) {
            if (f.size() > 1) e.note = std::atoi(f[1].c_str());
            if (f.size() > 2) e.vel = std::atoi(f[2].c_str());
            if (f.size() > 3) e.start = std::atoi(f[3].c_str());
            if (f.size() > 4) e.dur = std::atoi(f[4].c_str());
        } else {
            if (f.size() > 1) e.factor = std::atof(f[1].c_str());
            if (f.size() > 2) e.start = std::atoi(f[2].c_str());
            if (f.size() > 3) e.dur = std::atoi(f[3].c_str());
        }
        endMs = std::max(endMs, e.start + e.dur + 1500);
        evs.push_back(e);
    }

    Render r;
    for (int fr = 0; fr * 1000 / 60 < endMs; fr++) {
        int now = fr * 1000 / 60;
        for (Ev &e : evs) {
            if (!e.on && now >= e.start) {
                e.on = true;
                if (e.prog) {
                    /* seq_note_on's computation for a track at full volume, pan 0x40 */
                    if (e.idx >= (int)b.programs.size())
                        continue;
                    const Program &p = b.programs[e.idx];
                    int vol = (((0x3FFF * p.vol) >> 14) * ee::g_velocity[e.vel & 0x7F]) >> 14;
                    for (uint8_t zi : p.zones) {
                        const Zone &z = b.zones[zi];
                        if (e.note < z.lo || e.note > z.hi)
                            continue;
                        int zv = (vol * z.vol) >> 14;
                        int pan = 0x40 + p.pan + z.pan - 0x80;
                        pan = pan < 0 ? 0 : (pan > 0x7F ? 0x7F : pan);
                        ee::VoiceParam vp;
                        int16_t lr[2];
                        ee::snd_pan_volume(lr, (zv * zv) >> 14, pan);
                        vp.volL = lr[0];
                        vp.volR = lr[1];
                        vp.pitch = ee::snd_note_to_pitch(e.note << 8, z.baseNote);
                        vp.addr8 = z.addr8;
                        vp.adsr = z.adsr;
                        vp.unk10 = z.unk0A;
                        vp.priority = z.prio;
                        vp.flags = z.flags;
                        ee::snd_play_voice(&e.voice, vp);
                        std::printf("  %5d ms program %d note %d -> zone %d pitch %04X vol %d/%d\n", now, e.idx,
                                    e.note, zi, vp.pitch, vp.volL, vp.volR);
                        break;
                    }
                } else {
                    /* se_play_program (0x0022BBF8), volume 1.0, the zone's pan */
                    if (e.idx >= (int)b.zones.size())
                        continue;
                    const Zone &z = b.zones[e.idx];
                    int vol = (16384 * z.vol) >> 14;
                    vol = vol > 0x3FFF ? 0x3FFF : vol;
                    ee::VoiceParam vp;
                    int16_t lr[2];
                    ee::snd_pan_volume(lr, vol, z.pan);
                    vp.volL = lr[0];
                    vp.volR = lr[1];
                    vp.pitch = (uint16_t)((ee::snd_note_to_pitch(z.lo << 8, z.baseNote) * (int)(e.factor * 4096)) >> 12);
                    vp.addr8 = z.addr8;
                    vp.adsr = z.adsr;
                    vp.unk10 = z.unk0A;
                    vp.priority = z.prio;
                    vp.flags = z.flags;
                    ee::snd_play_voice(&e.voice, vp);
                    std::printf("  %5d ms zone %d pitch %04X vol %d/%d adsr %08X\n", now, e.idx, vp.pitch, vp.volL,
                                vp.volR, vp.adsr);
                }
            }
            if (e.on && !e.off && now >= e.start + e.dur) {
                e.off = true;
                ee::snd_voice_keyoff(&e.voice);
            }
        }
        r.frame();
    }
    report_levels(r.out);
    return write_wav(argv[1], r.out.data(), r.out.size() / 2, 2, 48000) ? 0 : 1;
}

/* ---- ads ---- */

struct FileSource {
    FILE *f = nullptr;
};

static int file_read(void *user, uint32_t offset, void *dst, uint32_t size)
{
    FileSource *s = (FileSource *)user;
    if (std::fseek(s->f, (long)offset, SEEK_SET) != 0)
        return -1;
    return (int)std::fread(dst, 1, size, s->f);
}

static int cmd_ads(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "ads FILE.ads OUT.wav [SECONDS] [--loop] [--threaded] [--volume V] [--pause S1,S2]\n");
        return 2;
    }
    double seconds = 30;
    bool loop = false, threaded = false;
    double volume = 1.0, pauseAt = -1, resumeAt = -1;
    for (int i = 2; i < argc; i++) {
        if (!std::strcmp(argv[i], "--loop"))
            loop = true;
        else if (!std::strcmp(argv[i], "--threaded"))
            threaded = true;
        else if (!std::strcmp(argv[i], "--volume") && i + 1 < argc)
            volume = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--pause") && i + 1 < argc)
            std::sscanf(argv[++i], "%lf,%lf", &pauseAt, &resumeAt);
        else
            seconds = std::atof(argv[i]);
    }
    FileSource src;
    src.f = std::fopen(argv[0], "rb");
    if (!src.f) {
        std::fprintf(stderr, "cannot read %s\n", argv[0]);
        return 1;
    }
    sound_start();
    snd_native_set_bgm_threaded(threaded);
    /* ads_play: ads_set_volume, then pdibgm_start */
    int v = (int)(std::max(-1.0, std::min(1.0, volume)) * 32768.0);
    snd_native_bgm_set_volume(v);
    if (snd_native_bgm_start(file_read, &src, loop ? 1 : 0) != 0) {
        std::fprintf(stderr, "%s: not an ADPCM .ads\n", argv[0]);
        return 1;
    }
    const AdsHeader &h = snd_native_bgm().header();
    std::printf("%s: type 0x%X rate %u channels %u interleave 0x%X body 0x%X (%.2f s); voice pitch 0x%04X\n",
                argv[0], h.type, h.rate, h.channels, h.interleave, h.bodySize,
                (double)h.bodySize / (16.0 * h.channels) * 28 / h.rate, snd_native_bgm().voice_pitch());

    Render r;
    int frames = (int)(seconds * 60);
    bool paused = false;
    auto t0 = std::chrono::steady_clock::now();
    for (int fr = 0; fr < frames; fr++) {
        double t = fr / 60.0;
        if (pauseAt >= 0 && !paused && t >= pauseAt) {
            snd_native_bgm_pause();
            paused = true;
            pauseAt = -1;
            std::printf("  %.2f s: pause\n", t);
        }
        if (paused && resumeAt >= 0 && t >= resumeAt) {
            snd_native_bgm_resume();
            paused = false;
            resumeAt = -1;
            std::printf("  %.2f s: resume\n", t);
        }
        if (!threaded)
            snd_native_bgm().pump();
        else
            std::this_thread::sleep_until(t0 + std::chrono::microseconds((long long)(fr * 1000000.0 / 60)));
        r.frame();
        if (fr % 60 == 59) {
            SndNativeBgmStatus st;
            snd_native_bgm_get_status(&st);
            std::printf("  %5.2f s: status time %.3f s pos %d id %d ended %d (played %.4f s)\n", (fr + 1) / 60.0,
                        st.time / 1e6, st.pos, st.id, st.ended, snd_native_bgm_played_us() / 1e6);
        }
    }
    std::printf("  underruns %u\n", snd_native_bgm().underruns());
    snd_native_bgm_stop();
    std::fclose(src.f);
    report_levels(r.out);
    return write_wav(argv[1], r.out.data(), r.out.size() / 2, 2, 48000) ? 0 : 1;
}

/* ---- seq ---- */

static int cmd_seq(int argc, char **argv)
{
    if (argc < 4) {
        std::fprintf(stderr, "seq music.ins music.seq ENTRY OUT.wav [SECONDS] [--dry]\n");
        return 2;
    }
    std::vector<uint8_t> fi, fs;
    std::string err;
    InstBank bank;
    SeqBank seqs;
    if (!load_file(argv[0], fi) || !parse_inst(fi, bank, err) || !load_file(argv[1], fs) ||
        !parse_seqg(fs, seqs, err)) {
        std::fprintf(stderr, "cannot load: %s\n", err.c_str());
        return 1;
    }
    int entry = std::atoi(argv[2]);
    double seconds = 60;
    bool dry = false;
    for (int i = 4; i < argc; i++) {
        if (!std::strcmp(argv[i], "--dry"))
            dry = true;
        else
            seconds = std::atof(argv[i]);
    }
    if (entry < 0 || entry >= (int)seqs.entries.size()) {
        std::fprintf(stderr, "entry %d of %zu\n", entry, seqs.entries.size());
        return 1;
    }
    sound_start(!dry);
    place_inst(bank, 0xAC000);              /* music_load: music.ins at SPU 0xAC000 */
    /* menu music: bgm_play's volume 0.8 x master 1.0 -> music_set_volume */
    int vol = (int)(0.8f * 16384.0f);
    if (vol > 0x3FFF)
        vol = 0x3FFF;
    ee::Sequence *s = ee::seq_play(seqs, entry, bank, (uint16_t)vol);
    std::printf("SEQG entry %d: volume 0x%04X tempo %d\n", entry, seqs.entries[entry].volume, seqs.entries[entry].tempo);
    Render r;
    int maxVoices = 0;
    for (int fr = 0; fr < seconds * 60; fr++) {
        ee::seq_update();
        r.frame();
        maxVoices = std::max(maxVoices, ee::voices_in_use());
        if (ee::seq_finished(s)) {
            std::printf("  the sequence ended at %.2f s\n", fr / 60.0);
            break;
        }
    }
    std::printf("  at most %d voices\n", maxVoices);
    report_levels(r.out);
    return write_wav(argv[3], r.out.data(), r.out.size() / 2, 2, 48000) ? 0 : 1;
}

/* ---- engine ---- */

static int cmd_engine(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "engine ENGINE.es OUT.wav [RPM0 RPM1 SECONDS]\n");
        return 2;
    }
    std::vector<uint8_t> f;
    std::string err;
    EngnBank b;
    if (!load_file(argv[0], f) || !parse_engn(f, b, err)) {
        std::fprintf(stderr, "%s: %s\n", argv[0], err.c_str());
        return 1;
    }
    double rpm0 = argc > 2 ? std::atof(argv[2]) : 800, rpm1 = argc > 3 ? std::atof(argv[3]) : 8000;
    double seconds = argc > 4 ? std::atof(argv[4]) : 10;
    sound_start();
    place_engn(b, 0xAC000);
    ee::EngineVoice es;
    ee::engine_setup(es, b, 0, 0);
    Render r;
    int n = (int)(seconds * 60);
    for (int fr = 0; fr < n; fr++) {
        double t = (double)fr / (n - 1);
        /* up and back down */
        double x = t < 0.5 ? t * 2 : (1 - t) * 2;
        es.rpm = (uint16_t)(rpm0 + (rpm1 - rpm0) * x);
        ee::engine_update(es);
        r.frame();
    }
    ee::engine_stop(es);
    for (int fr = 0; fr < 30; fr++)
        r.frame();
    report_levels(r.out);
    return write_wav(argv[1], r.out.data(), r.out.size() / 2, 2, 48000) ? 0 : 1;
}

/* ---- tone / adsr / reverb: one voice straight on the SPU2 model ---- */

static void raw_voice_setup(uint32_t addr, uint16_t pitch, uint16_t adsr1, uint16_t adsr2, uint16_t vol)
{
    Spu2 &p = snd_native_spu();
    p.set_voice_addr(0, 0, VA_SSA, addr);
    p.set_voice_param(0, 0, VP_PITCH, pitch);
    p.set_voice_param(0, 0, VP_ADSR1, adsr1);
    p.set_voice_param(0, 0, VP_ADSR2, adsr2);
    p.set_voice_param(0, 0, VP_VOLL, vol);
    p.set_voice_param(0, 0, VP_VOLR, vol);
}

static int cmd_tone(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "tone OUT.wav PITCH [FREQ] [MS]\n");
        return 2;
    }
    uint16_t pitch = (uint16_t)std::strtoul(argv[1], nullptr, 0);
    double freq = argc > 2 ? std::atof(argv[2]) : 1000.0;
    int ms = argc > 3 ? std::atoi(argv[3]) : 2000;
    /* 48 periods of `freq' at a 48 kHz source rate: 48000 / freq * 48 samples,
     * rounded to whole blocks (freq 1000: 2304 samples = 82.3 blocks -> the
     * loop is made of exactly 84 blocks = 2352 samples = 49 periods) */
    int periods = 49;
    int len = (int)std::lround(48000.0 / freq * periods / 28) * 28;
    double f = 48000.0 * periods / len;    /* the exact frequency of the loop */
    std::vector<int16_t> pcm(len);
    for (int i = 0; i < len; i++)
        pcm[i] = (int16_t)std::lround(20000 * std::sin(2 * 3.14159265358979 * f * i / 48000.0));
    std::vector<uint8_t> adpcm = encode_loop(pcm);
    snd_native_init();
    snd_native_write_spu(0x10000, adpcm.data(), (uint32_t)adpcm.size());
    raw_voice_setup(0x10000, pitch, 0x000F, 0x0000, 0x3FFF);
    snd_native_spu().set_switch(0, SW_KON, 1);
    std::vector<int16_t> out((size_t)ms * 48 * 2);
    snd_native_mix(out.data(), ms * 48);
    std::printf("tone: loop of %d samples, source %.4f Hz, pitch 0x%04X -> expected %.4f Hz\n", len, f, pitch,
                f * pitch / 4096.0);
    return write_wav(argv[0], out.data(), out.size() / 2, 2, 48000) ? 0 : 1;
}

static int cmd_adsr(int argc, char **argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "adsr OUT.wav ADSR1 ADSR2 [KEYOFF_MS] [MS]\n");
        return 2;
    }
    uint16_t a1 = (uint16_t)std::strtoul(argv[1], nullptr, 0), a2 = (uint16_t)std::strtoul(argv[2], nullptr, 0);
    int koff = argc > 3 ? std::atoi(argv[3]) : 1000;
    int ms = argc > 4 ? std::atoi(argv[4]) : koff + 1000;
    /* a DC sample: every nibble 7 at shift 0, filter 0 = 0x7000 */
    uint8_t blk[16];
    std::memset(blk, 0x77, 16);
    blk[0] = 0x00;
    blk[1] = ADPCM_FLAG_LOOP_START | ADPCM_FLAG_LOOP | ADPCM_FLAG_LOOP_END;
    snd_native_init();
    snd_native_write_spu(0x10000, blk, 16);
    raw_voice_setup(0x10000, 0x1000, a1, a2, 0x3FFF);
    std::vector<int16_t> out((size_t)ms * 48 * 2);
    /* the envelope (ENVX) itself as a mono track */
    std::vector<int16_t> env((size_t)ms * 48);
    snd_native_spu().set_switch(0, SW_KON, 1);
    for (int i = 0; i < ms * 48; i++) {
        if (i == koff * 48)
            snd_native_spu().set_switch(0, SW_KOFF, 1);
        snd_native_mix(&out[2 * i], 1);
        env[i] = (int16_t)snd_native_spu().get_voice_param(0, 0, VP_ENVX);
    }
    std::string envPath = std::string(argv[0]) + ".env.wav";
    write_wav(envPath, env.data(), env.size(), 1, 48000);
    return write_wav(argv[0], out.data(), out.size() / 2, 2, 48000) ? 0 : 1;
}

static int cmd_reverb(int argc, char **argv)
{
    if (argc < 1) {
        std::fprintf(stderr, "reverb OUT.wav [MS]\n");
        return 2;
    }
    int ms = argc > 1 ? std::atoi(argv[1]) : 3000;
    /* a one-block click (one-shot) through core 0 with the reverb send on:
     * STUDIO_3, EVOL 0x7FFF, effects on */
    uint8_t blk[32] = {};
    blk[0] = 0x00;
    blk[1] = 0;
    blk[2] = 0x07;                      /* first sample 0x7000, the rest 0 */
    blk[16] = 0x0C;                     /* a second, silent block: shift 12 */
    blk[17] = ADPCM_FLAG_LOOP_END;
    snd_native_init();
    Spu2 &p = snd_native_spu();
    snd_native_write_spu(0x10000, blk, 32);
    for (int c = 0; c < 2; c++) {
        p.set_effect_attr(c, EFFECT_STUDIO_3, 0x7FFF, 0x7FFF, 0, 0);
        p.set_effect_enable(c, true);
    }
    raw_voice_setup(0x10000, 0x1000, 0x000F, 0x0000, 0x3FFF);
    p.set_switch(0, SW_VMIXEL, 1);
    p.set_switch(0, SW_VMIXER, 1);
    p.set_switch(0, SW_KON, 1);
    std::vector<int16_t> out((size_t)ms * 48 * 2);
    snd_native_mix(out.data(), ms * 48);
    report_levels(out);
    return write_wav(argv[0], out.data(), out.size() / 2, 2, 48000) ? 0 : 1;
}

/* ---- selftest ---- */

static int g_fail = 0;
static void check(bool ok, const char *what)
{
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        g_fail++;
}

static int cmd_selftest()
{
    std::printf("selftest\n");
    /* gaussian table */
    const int16_t *g = gauss_table();
    bool sums = true, sym = true;
    for (int i = 0; i < 256; i++) {
        int s = g[i * 4] + g[i * 4 + 1] + g[i * 4 + 2] + g[i * 4 + 3];
        if (s < 0x7F80 - 1 || s > 0x7F80 + 1)     /* the hardware's rows: 0x7F80 +- 1 (rounding) */
            sums = false;
        for (int k = 0; k < 4; k++)
            if (g[i * 4 + k] != g[(255 - i) * 4 + 3 - k])
                sym = false;
    }
    check(sums, "gauss: every phase sums to 0x7F80 +- 1");
    check(sym, "gauss: phase i mirrors phase 255 - i");
    check(g[0] == 0x12C7 && g[1] == 0x59B3 && g[2] == 0x1307 && g[3] == -1,
          "gauss: phase 0 = 12C7 59B3 1307 -0001 (the hardware's)");
    check(g[128 * 4] == 412 && g[128 * 4 + 1] == 15855 && g[128 * 4 + 2] == 15948 && g[128 * 4 + 3] == 424,
          "gauss: phase 128 = 019C 3DEF 3E4C 01A8 (the hardware's)");

    /* ADPCM */
    uint8_t blk[16] = { 0x00, 0x00, 0x21, 0xF7 };
    int16_t s[28];
    AdpcmHistory h;
    adpcm_decode_block(blk, s, h);
    check(s[0] == 0x1000 && s[1] == 0x2000 && s[2] == 0x7000 && s[3] == -0x1000 && s[4] == 0,
          "adpcm: filter 0 shift 0 nibbles");
    uint8_t blk2[16] = { 0x1C, 0x00 };  /* filter 1 (60/64), shift 12, zero nibbles */
    AdpcmHistory h2;
    h2.prev1 = 6400;
    adpcm_decode_block(blk2, s, h2);
    check(s[0] == (6400 * 60 + 32) >> 6, "adpcm: filter 1 prediction");

    /* envelope: attack rate 0 linear reaches max in 3 samples; release 0 */
    {
        snd_native_init();
        uint8_t dc[16];
        std::memset(dc, 0x77, 16);
        dc[0] = 0;
        dc[1] = 7;
        snd_native_write_spu(0x10000, dc, 16);
        raw_voice_setup(0x10000, 0x1000, 0x000F, 0x0000, 0x3FFF);
        Spu2 &p = snd_native_spu();
        p.set_switch(0, SW_KON, 1);
        int16_t o[2];
        int n = 0;
        while (p.get_voice_param(0, 0, VP_ENVX) < 0x7FFF && n < 100) {
            snd_native_mix(o, 1);
            n++;
        }
        check(n == 3, "adsr: attack rate 0 (linear, +0x3800 per tick) reaches 0x7FFF in 3 ticks");
        p.set_switch(0, SW_KOFF, 1);
        n = 0;
        while (p.get_voice_param(0, 0, VP_ENVX) > 0 && n < 100) {
            snd_native_mix(o, 1);
            n++;
        }
        check(n == 2, "adsr: release rate 0 (exponential off: -0x4000 per tick) reaches 0 in 2 ticks");
    }
    /* status: a one-shot voice ends at its ENDX; the mask drops it */
    {
        snd_native_init();
        uint8_t two[32] = {};
        two[1] = 0;
        two[17] = ADPCM_FLAG_LOOP_END;
        snd_native_write_spu(0x20000, two, 32);
        SndNativeState stt;
        std::memset(&stt, 0, sizeof(stt));
        for (auto &v : stt.voice)
            v.flags = 0x50;
        stt.core[0].mmix = 0xFC0;
        stt.core[1].mmix = 0xFCC;
        stt.voice[5].flags = 0x50 | SND_NATIVE_V_KEY_ON | SND_NATIVE_V_ONE_SHOT | SND_NATIVE_V_PITCH |
                             SND_NATIVE_V_VOLUME | SND_NATIVE_V_ADSR;
        stt.voice[5].pitch = 0x1000;
        stt.voice[5].volL = stt.voice[5].volR = 0x3FFF;
        stt.voice[5].addr = 0x20000;
        stt.voice[5].adsr1 = 0x000F;
        snd_native_send_state(&stt);
        check(snd_native_voice_active_mask() == (1ull << 5), "status: key-on makes voice 5 active");
        int16_t o[2 * 100];
        snd_native_mix(o, 100);         /* 2 blocks = 56 samples, then the end */
        stt.voice[5].flags = 0x50 | SND_NATIVE_V_ONE_SHOT;
        snd_native_send_state(&stt);
        check(snd_native_voice_active_mask() == 0, "status: the one-shot ended (ENDX)");
    }
    /* pitch: 0x1000 steps one sample per tick; 0x800 every other */
    {
        snd_native_init();
        std::vector<int16_t> ramp(28 * 8);
        for (size_t i = 0; i < ramp.size(); i++)
            ramp[i] = (int16_t)(i * 100);
        std::vector<uint8_t> a = encode_loop(ramp);
        snd_native_write_spu(0x30000, a.data(), (uint32_t)a.size());
        raw_voice_setup(0x30000, 0x0800, 0x000F, 0x0000, 0x3FFF);
        snd_native_spu().set_switch(0, SW_KON, 1);
        std::vector<int16_t> o(200);
        snd_native_mix(o.data(), 100);
        uint32_t nax = snd_native_spu().get_voice_addr(0, 0, VA_NAX);
        check(nax == 0x30000 + 16, "pitch 0x800: 100 ticks = 50 samples, in the second block");
    }
    std::printf("%d failure(s)\n", g_fail);
    return g_fail ? 1 : 0;
}

/* ---- live: the threads as the game will have them ----
 * An "audio device" thread pulls 10 ms blocks of snd_native_mix in real
 * time; the "music thread" runs seq_update + snd_flush at 60 Hz and polls
 * the BGM status; the BGM player's feeder thread reads the .ads.  Checks
 * that nothing deadlocks or underruns; writes what the device got. */
static int cmd_live(int argc, char **argv)
{
    if (argc < 5) {
        std::fprintf(stderr, "live music.ins music.seq ENTRY FILE.ads OUT.wav [SECONDS]\n");
        return 2;
    }
    std::vector<uint8_t> fi, fs;
    std::string err;
    InstBank bank;
    SeqBank seqs;
    if (!load_file(argv[0], fi) || !parse_inst(fi, bank, err) || !load_file(argv[1], fs) ||
        !parse_seqg(fs, seqs, err)) {
        std::fprintf(stderr, "cannot load: %s\n", err.c_str());
        return 1;
    }
    int entry = std::atoi(argv[2]);
    double seconds = argc > 5 ? std::atof(argv[5]) : 5;
    bool stress = argc > 6 && !std::strcmp(argv[6], "--stress");
    FileSource src;
    src.f = std::fopen(argv[3], "rb");
    if (!src.f)
        return 1;
    sound_start();
    snd_native_set_bgm_threaded(true);
    place_inst(bank, 0xAC000);
    ee::seq_play(seqs, entry, bank, 0x2000);
    snd_native_bgm_set_volume(16384);
    if (snd_native_bgm_start(file_read, &src, 1) != 0)
        return 1;

    std::vector<int16_t> out;
    std::mutex outLock;
    bool quit = false;
    auto t0 = std::chrono::steady_clock::now();
    std::thread audio([&] {
        int16_t buf[480 * 2];
        for (int blk = 0;; blk++) {
            std::this_thread::sleep_until(t0 + std::chrono::milliseconds(10 * blk));
            {
                std::lock_guard<std::mutex> g(outLock);
                if (quit)
                    break;
            }
            snd_native_mix(buf, 480);
            std::lock_guard<std::mutex> g(outLock);
            out.insert(out.end(), buf, buf + 960);
        }
    });
    int frames = (int)(seconds * 60);
    for (int fr = 0; fr < frames; fr++) {
        std::this_thread::sleep_until(t0 + std::chrono::microseconds((long long)(fr * 1000000.0 / 60)));
        ee::seq_update();
        ee::snd_flush();
        if (stress && fr % 45 == 20) {          /* restart, pause, resume the BGM */
            switch ((fr / 45) % 3) {
            case 0: snd_native_bgm_start(file_read, &src, 1); break;
            case 1: snd_native_bgm_pause(); break;
            case 2: snd_native_bgm_resume(); break;
            }
        }
        if (fr % 60 == 59) {
            SndNativeBgmStatus st;
            snd_native_bgm_get_status(&st);
            std::printf("  %.1f s: voices %d, BGM time %.3f s, underruns %u\n", (fr + 1) / 60.0,
                        ee::voices_in_use(), st.time / 1e6, snd_native_bgm().underruns());
        }
    }
    {
        std::lock_guard<std::mutex> g(outLock);
        quit = true;
    }
    audio.join();
    snd_native_bgm_stop();
    std::fclose(src.f);
    report_levels(out);
    return write_wav(argv[4], out.data(), out.size() / 2, 2, 48000) ? 0 : 1;
}

/* ---- pss: a movie's audio through the push path ----
 * Demuxes MPEG private stream 1 (substream 0xFF; each payload's 4-byte
 * prefix dropped) as pdistr's 'MPG1' service did, pushes it a frame at a
 * time (as the movie feeder would) and renders. */
static int cmd_pss(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "pss FILE.pss OUT.wav [SECONDS]\n");
        return 2;
    }
    double seconds = argc > 2 ? std::atof(argv[2]) : 20;
    FILE *f = std::fopen(argv[0], "rb");
    if (!f)
        return 1;
    /* enough of the file for `seconds' of audio (the audio is ~1/20 of a PSS) */
    std::vector<uint8_t> d((size_t)(seconds * 4.0e6) + 0x100000);
    d.resize(std::fread(d.data(), 1, d.size(), f));
    std::fclose(f);
    std::vector<uint8_t> audio;
    size_t i = 0, packets = 0;
    while (i + 6 <= d.size()) {
        if (d[i] != 0 || d[i + 1] != 0 || d[i + 2] != 1) {
            i++;
            continue;
        }
        uint8_t sid = d[i + 3];
        if (sid == 0xBA) {                      /* MPEG-2 pack header + stuffing */
            i += 14 + (d[i + 13] & 7);
            continue;
        }
        if (sid < 0xBB) {
            i++;
            continue;
        }
        size_t len = (d[i + 4] << 8) | d[i + 5];
        if (sid == 0xBD && i + 9 <= d.size()) {
            size_t hl = d[i + 8];
            size_t p = i + 9 + hl, end = i + 6 + len;
            if (end <= d.size() && p + 4 <= end && d[p] == 0xFF) {
                audio.insert(audio.end(), d.begin() + p + 4, d.begin() + end);
                packets++;
            }
        }
        i += 6 + len;
    }
    std::printf("%s: %zu audio packets, %zu bytes of stream\n", argv[0], packets, audio.size());
    sound_start();
    snd_native_bgm_set_volume(32768);       /* mpegif::feed: pdibgm_set_volume(1.0) */
    snd_native_bgm_open_push();
    Render r;
    size_t pos = 0;
    for (int fr = 0; fr < seconds * 60; fr++) {
        /* the movie feeder delivers about a frame of audio a frame, here
         * whatever fits */
        while (pos < audio.size()) {
            uint32_t n = (uint32_t)std::min<size_t>(0x2000, audio.size() - pos);
            int took = snd_native_bgm_push(&audio[pos], n);
            if (took < 0) {
                std::fprintf(stderr, "not an ADPCM .ads stream\n");
                return 1;
            }
            pos += (size_t)took;
            if ((uint32_t)took < n)
                break;
        }
        if (pos >= audio.size())
            snd_native_bgm_push_end();
        r.frame();
        if (fr % 120 == 119) {
            SndNativeBgmStatus st;
            snd_native_bgm_get_status(&st);
            std::printf("  %5.2f s: status time %.3f s pos %d, %zu of %zu bytes pushed\n", (fr + 1) / 60.0,
                        st.time / 1e6, st.pos, pos, audio.size());
        }
    }
    snd_native_bgm_stop();
    report_levels(r.out);
    return write_wav(argv[1], r.out.data(), r.out.size() / 2, 2, 48000) ? 0 : 1;
}

/* ---- raw: one zone of a bank on one voice, flat envelope, no EE ---- */
static int cmd_raw(int argc, char **argv)
{
    if (argc < 4) {
        std::fprintf(stderr, "raw BANK.ins ZONE PITCH OUT.wav [MS]\n");
        return 2;
    }
    std::vector<uint8_t> file;
    std::string err;
    InstBank b;
    if (!load_file(argv[0], file) || !parse_inst(file, b, err)) {
        std::fprintf(stderr, "%s: %s\n", argv[0], err.c_str());
        return 1;
    }
    int zone = std::atoi(argv[1]);
    uint16_t pitch = (uint16_t)std::strtoul(argv[2], nullptr, 0);
    int ms = argc > 4 ? std::atoi(argv[4]) : 3000;
    if (zone < 0 || zone >= (int)b.zones.size())
        return 1;
    snd_native_init();
    place_inst(b, 0x5040);
    raw_voice_setup(b.zones[zone].addr8 * 8, pitch, 0x000F, 0x0000, 0x3FFF);
    snd_native_spu().set_switch(0, SW_KON, 1);
    std::vector<int16_t> out((size_t)ms * 48 * 2);
    snd_native_mix(out.data(), ms * 48);
    return write_wav(argv[3], out.data(), out.size() / 2, 2, 48000) ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr,
                     "sndtest selftest | dump | notes | ads | seq | engine | tone | adsr | reverb (see main.cpp)\n");
        return 2;
    }
    std::string cmd = argv[1];
    int n = argc - 2;
    char **a = argv + 2;
    if (cmd == "selftest") return cmd_selftest();
    if (cmd == "dump") return cmd_dump(n, a);
    if (cmd == "notes") return cmd_notes(n, a);
    if (cmd == "ads") return cmd_ads(n, a);
    if (cmd == "seq") return cmd_seq(n, a);
    if (cmd == "engine") return cmd_engine(n, a);
    if (cmd == "tone") return cmd_tone(n, a);
    if (cmd == "adsr") return cmd_adsr(n, a);
    if (cmd == "reverb") return cmd_reverb(n, a);
    if (cmd == "live") return cmd_live(n, a);
    if (cmd == "pss") return cmd_pss(n, a);
    if (cmd == "raw") return cmd_raw(n, a);
    std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
    return 2;
}
