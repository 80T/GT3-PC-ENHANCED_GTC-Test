/* sndplay - plays an .ads through the port's sound output
 * (src/platform/audio.cpp: kernel streaming or WASAPI, gain, DC blocker,
 * limiter, GT3PC_AUDIO_DUMP) and the native BGM player, for SECONDS; the
 * log goes to sndplay.log.  OUTPUT: auto (default), ks or wasapi.
 *
 *   sndplay FILE.ads [SECONDS] [VOLUME] [OUTPUT] [BUFFER_MS]
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <chrono>
#include <thread>

#include "engine/sound/native/sound_native.h"
#include "platform/audio.h"
#include "platform/log.h"
#include "platform/scheduler.h"
#include "platform/settings.h"
#include "platform/settings_menu.h"
#include "platform/frame_step.h"

/* ---- what audio.cpp needs of the platform (no scheduler, no gt3pc.ini) --- */

static settings::AudioOutput g_output = settings::AudioOutput::automatic;
static double g_bufferMs = 2.0;

namespace sched {
double now_seconds()
{
    static LARGE_INTEGER f, t0;
    if (f.QuadPart == 0) {
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
    }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - t0.QuadPart) / (double)f.QuadPart;
}
} // namespace sched

static int g_audioGen;

namespace settings {
AudioOutput audio_output() { return g_output; }
double audio_buffer_ms() { return g_bufferMs; }
void set_audio_buffer_ms(double ms)
{
    g_bufferMs = ms;
    g_audioGen++;
}
int audio_generation() { return g_audioGen; }
} // namespace settings

namespace settings_menu {
void notice(const char *text, double) { printf("notice: %s\n", text); }
} // namespace settings_menu

namespace frame_step {
bool paused() { return false; }
} // namespace frame_step

static int file_read(void *user, uint32_t offset, void *dst, uint32_t size)
{
    FILE *f = (FILE *)user;
    if (_fseeki64(f, offset, SEEK_SET) != 0)
        return -1;
    return (int)fread(dst, 1, size, f);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "sndplay FILE.ads [SECONDS] [VOLUME] [auto|ks|wasapi] [BUFFER_MS]\n");
        return 2;
    }
    double seconds = argc > 2 ? atof(argv[2]) : 5;
    double volume = argc > 3 ? atof(argv[3]) : 0.5;
    if (argc > 4)
        g_output = _stricmp(argv[4], "ks") == 0       ? settings::AudioOutput::ks
                   : _stricmp(argv[4], "wasapi") == 0 ? settings::AudioOutput::wasapi
                                                      : settings::AudioOutput::automatic;
    if (argc > 5 && atof(argv[5]) > 0.0)
        g_bufferMs = atof(argv[5]);
    pclog::open("sndplay.log");
    audio::init();
    FILE *f = fopen(argv[1], "rb");
    if (!f)
        return 1;
    snd_native_bgm_set_volume((int)(volume * 32768));
    if (snd_native_bgm_start(file_read, f, 0) != 0)
        return 1;
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::duration<double>(seconds)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        SndNativeBgmStatus st;
        snd_native_bgm_get_status(&st);
        printf("  %.1f s: BGM time %.3f s\n",
               std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), st.time / 1e6);
    }
    snd_native_bgm_stop();
    audio::shutdown();
    pclog::close();
    fclose(f);
    return 0;
}
