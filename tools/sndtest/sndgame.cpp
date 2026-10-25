/* sndgame - test harness of the integrated sound: the port's engine/sound
 * (sound_driver.cpp, sound_seq.cpp) and engine/pdi (T520_pdispu2.cpp,
 * T521_pdibgm.cpp), compiled exactly as the game compiles them, over the
 * native SPU2 (engine/sound/native), driven as music_init / music_thread /
 * ads_play / CarSound drive them (game_side.cpp).  The few platform calls
 * those files make are provided here: the interrupt lock (single-threaded:
 * no-ops), the game's printf, memalign, and gt3.vol reads (platform/disc.h)
 * from the extracted disc.
 *
 *   sndgame music music.ins music.seq ENTRY OUT.wav [SECONDS]
 *   sndgame ads   gt3.vol LSN SIZE OUT.wav [SECONDS] [--loop] [--pause S1,S2]
 *   sndgame both  music.ins music.seq ENTRY gt3.vol LSN SIZE OUT.wav [SECONDS]
 *   sndgame engine ENGINE.es OUT.wav [SECONDS]
 *
 * Each 60 Hz frame: game logic, the music thread's tick (seq_update,
 * snd_flush -> pdispu2_send_state -> the native SPU2, pdibgm_update), then
 * 800 samples of snd_native_mix. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmath>
#include <string>
#include <vector>

#include "banks.h"
#include "engine/sound/native/ads_stream.h"
#include "engine/sound/native/sound_native.h"
#include "platform/disc.h"
#include "platform/host_file.h"

/* ---- what the engine files need from the platform ------------------------ */

extern "C" {
void EI(void) {}
int DIntr(void) { return 1; }
int EIntr(void) { return 1; }
int intr_suspend(void) { return 1; }
int pc_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}
void *pc_memalign(size_t align, size_t size)
{
    (void)align;
    return malloc(size);
}
void *pc_malloc(size_t size) { return malloc(size); }
void pc_free(void *p) { free(p); }
int host_file_open(const char *) { return -1; }
void host_file_close(int) {}
int host_file_read_at(int, uint64_t, void *, uint32_t) { return -1; }
/* game/app/pc_sound.h: no game ticks here (the blocks unscheduled: at once) */
double pc_sound_block_time(void) { return -1.0; }

/* game_side.cpp */
void gs_music_init(void);
int gs_music_load(const uint8_t *ins, uint32_t insSize, uint8_t *seq);
void gs_music_play(int entry, float volume);
void gs_music_stop(void);
void gs_tick(void);
int gs_voices_playing(void);
void gs_ads_play(int lsn, int sectors, int loop, float volume);
void gs_ads_stop(void);
void gs_ads_pause(int on);
float gs_ads_status(int *pos, int *id, int *ended);
int gs_engine_load(const uint8_t *es, uint32_t esSize, uint32_t spuAddr);
void gs_engine_rpm(int rpm, int volume);
void gs_engine_stop(void);
}

namespace disc {
static FILE *g_vol = nullptr;
bool open(const char *path)
{
    g_vol = fopen(path, "rb");
    return g_vol != nullptr;
}
uint64_t size() { return 0; }
bool read_bytes(uint64_t offset, void *buf, uint32_t bytes)
{
    if (!g_vol)
        return false;
    if (_fseeki64(g_vol, (int64_t)offset, SEEK_SET) != 0)
        return false;
    return fread(buf, 1, bytes, g_vol) == bytes;
}
} // namespace disc

/* ---- rendering ------------------------------------------------------------- */

static std::vector<int16_t> g_out;

static void frame()
{
    gs_tick();
    size_t at = g_out.size();
    g_out.resize(at + 800 * 2);
    snd_native_mix(&g_out[at], 800);
}

static bool write_wav(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    uint32_t data = (uint32_t)(g_out.size() * 2);
    uint32_t v;
    uint16_t s;
    fwrite("RIFF", 1, 4, f);
    v = 36 + data; fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    v = 16; fwrite(&v, 4, 1, f);
    s = 1; fwrite(&s, 2, 1, f);
    s = 2; fwrite(&s, 2, 1, f);
    v = 48000; fwrite(&v, 4, 1, f);
    v = 192000; fwrite(&v, 4, 1, f);
    s = 4; fwrite(&s, 2, 1, f);
    s = 16; fwrite(&s, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data, 4, 1, f);
    fwrite(g_out.data(), 2, g_out.size(), f);
    fclose(f);
    double sum = 0;
    int peak = 0;
    for (int16_t x : g_out) {
        sum += (double)x * x;
        peak = std::max(peak, std::abs((int)x));
    }
    printf("  %s: %.2f s, peak %d, rms %.1f dBFS\n", path, g_out.size() / 2 / 48000.0, peak,
           20 * std::log10(std::sqrt(sum / (g_out.size() + 1)) / 32768.0 + 1e-12));
    return true;
}

static bool load_music(const char *insPath, const char *seqPath, std::vector<uint8_t> &ins,
                       std::vector<uint8_t> &seq)
{
    if (!load_file(insPath, ins) || !load_file(seqPath, seq)) {
        fprintf(stderr, "cannot read %s / %s\n", insPath, seqPath);
        return false;
    }
    if (!gs_music_load(ins.data(), (uint32_t)ins.size(), seq.data())) {
        fprintf(stderr, "music_load failed\n");
        return false;
    }
    return true;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "sndgame music|ads|both|engine ... (see sndgame.cpp)\n");
        return 2;
    }
    std::string cmd = argv[1];
    snd_native_init();
    snd_native_set_bgm_threaded(false);     /* deterministic: the BGM is pumped here */
    gs_music_init();

    if (cmd == "music" && argc >= 6) {
        std::vector<uint8_t> ins, seq;
        if (!load_music(argv[2], argv[3], ins, seq))
            return 1;
        double seconds = argc > 6 ? atof(argv[6]) : 20;
        gs_music_play(atoi(argv[4]), 0.8f);         /* menu_bgm_play's 0.8 */
        int maxv = 0;
        for (int fr = 0; fr < seconds * 60; fr++) {
            frame();
            maxv = std::max(maxv, gs_voices_playing());
        }
        printf("  at most %d voices\n", maxv);
        return write_wav(argv[5]) ? 0 : 1;
    }
    if ((cmd == "ads" && argc >= 6) || (cmd == "both" && argc >= 9)) {
        int a = cmd == "ads" ? 2 : 5;
        std::vector<uint8_t> ins, seq;
        if (cmd == "both") {
            if (!load_music(argv[2], argv[3], ins, seq))
                return 1;
            gs_music_play(atoi(argv[4]), 0.8f);
        }
        if (!disc::open(argv[a])) {
            fprintf(stderr, "cannot open %s\n", argv[a]);
            return 1;
        }
        int lsn = atoi(argv[a + 1]);
        int sectors = (int)((strtoull(argv[a + 2], nullptr, 0) + 0x7FF) >> 11);
        const char *out = argv[a + 3];
        double seconds = argc > a + 4 ? atof(argv[a + 4]) : 20;
        bool loop = false;
        double pauseAt = -1, resumeAt = -1;
        for (int i = a + 5; i < argc; i++) {
            if (!strcmp(argv[i], "--loop"))
                loop = true;
            else if (!strcmp(argv[i], "--pause") && i + 1 < argc)
                sscanf(argv[++i], "%lf,%lf", &pauseAt, &resumeAt);
        }
        gs_ads_play(lsn, sectors, loop ? 1 : 0, cmd == "both" ? 0.5f : 0.8f);
        for (int fr = 0; fr < seconds * 60; fr++) {
            double t = fr / 60.0;
            if (pauseAt >= 0 && t >= pauseAt) {
                gs_ads_pause(1);
                printf("  %.2f s: pdibgm_pause\n", t);
                pauseAt = -1;
            }
            if (resumeAt >= 0 && pauseAt < 0 && t >= resumeAt) {
                gs_ads_pause(0);
                printf("  %.2f s: pdibgm_resume\n", t);
                resumeAt = -1;
            }
            snd_native_bgm().pump();
            frame();
            if (fr % 120 == 119) {
                int pos, id, ended;
                float sec = gs_ads_status(&pos, &id, &ended);
                printf("  %5.2f s: pdibgm_get_status %.3f s pos %d id %d ended %d, EE voices %d\n", (fr + 1) / 60.0,
                       sec, pos, id, ended, gs_voices_playing());
            }
        }
        gs_ads_stop();
        return write_wav(out) ? 0 : 1;
    }
    if (cmd == "engine" && argc >= 4) {
        std::vector<uint8_t> es;
        if (!load_file(argv[2], es) || !gs_engine_load(es.data(), (uint32_t)es.size(), 0xAC000)) {
            fprintf(stderr, "cannot load %s\n", argv[2]);
            return 1;
        }
        double seconds = argc > 4 ? atof(argv[4]) : 8;
        int n = (int)(seconds * 60);
        for (int fr = 0; fr < n; fr++) {
            double t = (double)fr / (n - 1);
            double x = t < 0.5 ? t * 2 : (1 - t) * 2;
            gs_engine_rpm((int)(800 + 7200 * x), 0x2000);
            frame();
        }
        gs_engine_stop();
        for (int fr = 0; fr < 30; fr++)
            frame();
        return write_wav(argv[3]) ? 0 : 1;
    }
    fprintf(stderr, "bad arguments\n");
    return 2;
}
