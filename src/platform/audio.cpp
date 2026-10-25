/* audio.cpp - the sound output of GT3 PC (audio.h): kernel streaming, WASAPI
 * as the fallback. */
#include "platform/audio.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <avrt.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include "engine/sound/native/sound_native.h"
#include "platform/audio_ks.h"
#include "platform/frame_step.h"
#include "platform/input_map.h"
#include "platform/log.h"
#include "platform/scheduler.h"
#include "platform/settings.h"
#include "platform/settings_menu.h"

namespace audio {
namespace {

constexpr int kRate = 48000;                    /* the SPU2's rate */
constexpr float kMasterGain = 0.63f;            /* -4 dB */
constexpr float kDcPole = 0.9995f;              /* DC blocker pole: -3 dB at ~3.8 Hz */
constexpr float kLimit = 0.891f;                /* the limiter's ceiling: -1 dBFS */
constexpr float kLimitRelease = 2.08e-4f;       /* its gain back to 1 per sample: 100 ms (1 - e^(-1/4800)) */
constexpr int kNoDeviceStepMs = 5;              /* without a device: the render's pace */
constexpr ULONGLONG kRetryMs = 2000;            /* without a device: the next try */
constexpr int kMaxFrames = 16384;               /* a render call's frames at most (a converter's source) */
constexpr double kCrackleStepMs = 0.5;          /* a crackle raises the buffer by this */
constexpr double kCrackleGrace = 0.5;           /* s after a start whose underruns do not count (the start's) */
constexpr ULONGLONG kVolumePollMs = 50;         /* kernel streaming: the Windows volumes read this often */
constexpr float kVolumeGlide = 0.005f;          /* the volume's step to a new value per sample (~4 ms) */
constexpr float kPauseFade = 1.0f / 240.0f;     /* the tick step's fade a sample (5 ms) */

enum Mode { MODE_NONE, MODE_KS, MODE_WASAPI };

std::atomic<bool> g_quit{false};
std::atomic<bool> g_deviceChanged{false};
std::thread g_thread;

/* the output running (describe) */
std::atomic<int> g_mode{MODE_NONE};
std::atomic<double> g_latencyMs{0.0};
std::atomic<bool> g_ksBusy{false};              /* Output=ks and kernel streaming could not start */
std::atomic<bool> g_ksExclusive{false};         /* kernel streaming on a pin of 1 instance (others silent) */
std::atomic<float> g_volume{1.0f};              /* kernel streaming: the Windows volumes (Volumes) */

/* sched::now_seconds minus QPC seconds (the kernel streaming's times) */
double g_qpcToSched;

double qpc_seconds()
{
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return (double)t.QuadPart / (double)f.QuadPart;
}

double engine_clock()
{
    return sched::now_seconds();
}

/* ---- the output stage: gain, DC blocker, limiter, the debug dump ------- */

struct Stage {
    float x1[2] = {0, 0}, y1[2] = {0, 0};
    float gain = 1.0f;                          /* the limiter's */
    float vol = 1.0f;                           /* the Windows volumes' (g_volume, glided) */
    float run = 1.0f;                           /* 0 while the tick step pauses (faded, kPauseFade) */
    uint32_t limited = 0;                       /* frames turned down (logged) */
    FILE *dump = nullptr;
    uint32_t dumpFrames = 0;
    uint32_t dumpSinceHeader = 0;
    std::vector<int16_t> dumpBuf;

    void open_dump()
    {
        char path[MAX_PATH];
        DWORD n = GetEnvironmentVariableA("GT3PC_AUDIO_DUMP", path, sizeof(path));
        if (n == 0 || n >= sizeof(path))
            return;
        dump = fopen(path, "wb");
        if (!dump) {
            pclog::write("audio: cannot write the dump %s", path);
            return;
        }
        dumpBuf.resize(2 * kMaxFrames);
        write_header();
        pclog::write("audio: dumping the mix to %s", path);
    }

    void write_header()
    {
        uint32_t data = dumpFrames * 4;
        uint8_t h[44];
        memcpy(h, "RIFF", 4);
        uint32_t v = 36 + data;
        memcpy(h + 4, &v, 4);
        memcpy(h + 8, "WAVEfmt ", 8);
        v = 16;
        memcpy(h + 16, &v, 4);
        uint16_t s = 1;
        memcpy(h + 20, &s, 2);              /* PCM */
        s = 2;
        memcpy(h + 22, &s, 2);              /* stereo */
        v = kRate;
        memcpy(h + 24, &v, 4);
        v = kRate * 4;
        memcpy(h + 28, &v, 4);
        s = 4;
        memcpy(h + 32, &s, 2);
        s = 16;
        memcpy(h + 34, &s, 2);
        memcpy(h + 36, "data", 4);
        memcpy(h + 40, &data, 4);
        fseek(dump, 0, SEEK_SET);
        fwrite(h, 1, sizeof(h), dump);
        fseek(dump, 0, SEEK_END);
        fflush(dump);
    }

    /* Renders `frames' 48 kHz stereo frames into out (float, after the
     * stage); now: when the first is rendered (sched::now_seconds). */
    void render(float *out, int frames, double now)
    {
        /* the tick step's pause (platform/frame_step.h): faded out, then the
         * mix stops - the voices and the BGM stream hold where they are -
         * until it runs again (faded in) */
        const bool paused = frame_step::paused();
        if (paused && run <= 0.0f) {
            memset(out, 0, sizeof(float) * 2 * (size_t)frames);
            return;
        }
        snd_native_render(out, frames, now);
        for (int i = 0; i < frames; i++) {
            float lr[2];
            for (int c = 0; c < 2; c++) {
                const float x = out[2 * i + c] * kMasterGain;
                const float y = x - x1[c] + kDcPole * y1[c];
                x1[c] = x;
                y1[c] = y;
                lr[c] = y;
            }
            /* the limiter: stereo-linked, down at once to the ceiling, back
             * up slowly */
            const float peak = fabsf(lr[0]) > fabsf(lr[1]) ? fabsf(lr[0]) : fabsf(lr[1]);
            const float want = peak > kLimit ? kLimit / peak : 1.0f;
            if (want < gain)
                gain = want;
            else
                gain += (want - gain) * kLimitRelease;
            if (gain < 0.999f)
                limited++;
            out[2 * i] = lr[0] * gain;
            out[2 * i + 1] = lr[1] * gain;
        }
        if (dump) {
            for (int i = 0; i < 2 * frames; i++)
                dumpBuf[i] = (int16_t)lrintf(out[i] * 32767.0f);
            fwrite(dumpBuf.data(), 4, frames, dump);
            dumpFrames += frames;
            dumpSinceHeader += frames;
            if (dumpSinceHeader >= (uint32_t)kRate) {
                dumpSinceHeader = 0;
                write_header();             /* a valid file at any time: the process may just end */
            }
        }
        /* the Windows volumes kernel streaming bypasses (after the dump:
         * it keeps the game's own level) */
        const float target = g_volume.load(std::memory_order_relaxed);
        const float runTarget = paused ? 0.0f : 1.0f;
        if (vol != 1.0f || target != 1.0f || run != 1.0f || runTarget != 1.0f) {
            for (int i = 0; i < frames; i++) {
                vol += (target - vol) * kVolumeGlide;
                if (fabsf(target - vol) < 1e-5f)
                    vol = target;
                run = runTarget > run ? std::min(1.0f, run + kPauseFade) : std::max(0.0f, run - kPauseFade);
                out[2 * i] *= vol * run;
                out[2 * i + 1] *= vol * run;
            }
        }
    }
};

Stage g_stage;

/* ---- the rate converter: 48 kHz to the output's (cubic) ---------------- */

struct Converter {
    int rate = kRate;
    double step = 1.0;                          /* source frames per output frame */
    double frac = 0.0;                          /* the position between h[1] and h[2] */
    float h[4][2] = {};                         /* the last four source frames */
    std::vector<float> src;

    void reset(int outRate)
    {
        rate = outRate;
        step = (double)kRate / (double)outRate;
        frac = 0.0;
        memset(h, 0, sizeof(h));
        src.assign(2 * kMaxFrames, 0.0f);
    }

    /* `frames' stereo float frames at the output's rate; now: when the
     * first is rendered.  The source frames a call takes are rendered at
     * once (one engine render per output period: its blocks' timing). */
    void render(float *out, int frames, double now)
    {
        if (rate == kRate) {
            while (frames > 0) {
                const int n = frames < kMaxFrames ? frames : kMaxFrames;
                g_stage.render(out, n, now);
                out += 2 * n;
                frames -= n;
                now += (double)n / kRate;
            }
            return;
        }
        const int maxOut = (int)((kMaxFrames - 2) / step);
        while (frames > maxOut) {               /* (never: a period is far shorter) */
            render(out, maxOut, now);
            out += 2 * maxOut;
            frames -= maxOut;
            now += (double)maxOut / rate;
        }
        int need = 0;
        double f = frac;
        for (int i = 0; i < frames; i++) {
            f += step;
            while (f >= 1.0) {
                f -= 1.0;
                need++;
            }
        }
        if (need > 0)
            g_stage.render(src.data(), need, now);
        int k = 0;
        for (int i = 0; i < frames; i++) {
            const float t = (float)frac;
            for (int c = 0; c < 2; c++) {
                /* Catmull-Rom between h[1] and h[2] */
                const float p0 = h[0][c], p1 = h[1][c], p2 = h[2][c], p3 = h[3][c];
                const float a = -0.5f * p0 + 1.5f * p1 - 1.5f * p2 + 0.5f * p3;
                const float b = p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
                const float cc = -0.5f * p0 + 0.5f * p2;
                out[2 * i + c] = ((a * t + b) * t + cc) * t + p1;
            }
            frac += step;
            while (frac >= 1.0) {
                frac -= 1.0;
                memmove(h[0], h[1], sizeof(h[0]) * 3);
                h[3][0] = src[2 * k];
                h[3][1] = src[2 * k + 1];
                k++;
            }
        }
    }
};

Converter g_conv;

/* ---- kernel streaming ---------------------------------------------------- */

/* (start fills it before its first render call) */
audio_ks::Info g_ksInfo;

void ks_render(void *, float *out, int frames, double now)
{
    if (g_conv.rate != g_ksInfo.rate && g_ksInfo.rate > 0)
        g_conv.reset(g_ksInfo.rate);            /* the stream's first call */
    g_conv.render(out, frames, now + g_qpcToSched);
}

void ks_log(const char *line)
{
    pclog::write("audio: %s", line);
}

/* ---- WASAPI (shared; the fallback) --------------------------------------- */

template <class T> void release(T *&p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

struct Wasapi {
    IMMDevice *dev = nullptr;
    IAudioClient *client = nullptr;
    IAudioRenderClient *render = nullptr;
    HANDLE event = nullptr;
    UINT32 bufferFrames = 0, periodFrames = 0;
    int rate = kRate, channels = 2, bits = 32;
    bool isFloat = true, lowLatency = false;
    double latencyMs = 0.0;
    std::thread thread;
    std::atomic<bool> quit{false}, failed{false};
    std::vector<float> mix;

    bool ok() const { return render != nullptr; }

    void close()
    {
        quit = true;
        if (thread.joinable())
            thread.join();
        if (client)
            client->Stop();
        release(render);
        release(client);
        release(dev);
        if (event) {
            CloseHandle(event);
            event = nullptr;
        }
        failed = false;
    }

    /* the mix format's rate, channels and sample type; false: one this
     * cannot write */
    bool take_format(const WAVEFORMATEX *wf)
    {
        channels = wf->nChannels;
        rate = (int)wf->nSamplesPerSec;
        bits = wf->wBitsPerSample;
        isFloat = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
            isFloat = ((const WAVEFORMATEXTENSIBLE *)wf)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        if (isFloat)
            return bits == 32;
        return bits == 16 || bits == 24 || bits == 32;
    }

    bool open(IMMDeviceEnumerator *en, double periodMs)
    {
        HRESULT hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
        if (FAILED(hr))
            return false;
        WAVEFORMATEX *wf = nullptr;
        /* IAudioClient3: the engine's smallest period the driver allows (at
         * least periodMs) */
        IAudioClient3 *c3 = nullptr;
        lowLatency = false;
        hr = dev->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, (void **)&c3);
        if (SUCCEEDED(hr) && SUCCEEDED(c3->GetMixFormat(&wf)) && take_format(wf)) {
            UINT32 def = 0, fund = 0, minP = 0, maxP = 0;
            if (SUCCEEDED(c3->GetSharedModeEnginePeriod(wf, &def, &fund, &minP, &maxP)) && fund > 0) {
                UINT32 want = (UINT32)ceil(periodMs * rate / 1000.0);
                want = (want + fund - 1) / fund * fund;
                if (want < minP)
                    want = minP;
                if (want > maxP)
                    want = maxP;
                if (SUCCEEDED(c3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, want, wf, nullptr))) {
                    periodFrames = want;
                    lowLatency = true;
                    client = c3;
                    c3 = nullptr;
                }
            }
        }
        release(c3);
        if (!lowLatency) {
            /* the engine's default period */
            if (wf) {
                CoTaskMemFree(wf);
                wf = nullptr;
            }
            hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&client);
            if (SUCCEEDED(hr))
                hr = client->GetMixFormat(&wf);
            if (SUCCEEDED(hr) && !take_format(wf))
                hr = E_FAIL;
            if (SUCCEEDED(hr))
                hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, wf, nullptr);
            REFERENCE_TIME defP = 0, minP = 0;
            if (SUCCEEDED(hr) && SUCCEEDED(client->GetDevicePeriod(&defP, &minP)))
                periodFrames = (UINT32)(defP * rate / 10000000);
            if (FAILED(hr)) {
                pclog::write("audio: WASAPI: the output device refused the stream (0x%08X)", (unsigned)hr);
                if (wf)
                    CoTaskMemFree(wf);
                close();
                return false;
            }
        }
        if (wf)
            CoTaskMemFree(wf);
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event || FAILED(client->SetEventHandle(event)) || FAILED(client->GetBufferSize(&bufferFrames)) ||
            FAILED(client->GetService(__uuidof(IAudioRenderClient), (void **)&render))) {
            pclog::write("audio: WASAPI: the output device could not be set up");
            close();
            return false;
        }
        REFERENCE_TIME lat = 0;
        client->GetStreamLatency(&lat);
        latencyMs = bufferFrames * 1000.0 / rate + lat / 10000.0;
        mix.assign(2 * (size_t)(bufferFrames + 16), 0.0f);
        BYTE *buf = nullptr;
        if (SUCCEEDED(render->GetBuffer(bufferFrames, &buf)))
            render->ReleaseBuffer(bufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);
        if (FAILED(client->Start())) {
            close();
            return false;
        }
        quit = false;
        failed = false;
        g_conv.reset(rate);                     /* (the thread's from here) */
        thread = std::thread([this] { thread_main(); });
        pclog::write("audio: WASAPI shared%s, %d Hz %d-bit%s %d channels, period %u frames, buffer %u frames "
                     "(%.1f ms latency)", lowLatency ? " (low latency)" : "", rate, bits, isFloat ? " float" : "",
                     channels, periodFrames, bufferFrames, latencyMs);
        return true;
    }

    /* The float stereo mix at the device's rate into its format. */
    void write(BYTE *buf, UINT32 frames)
    {
        for (UINT32 i = 0; i < frames; i++) {
            const float l = mix[2 * i], r = mix[2 * i + 1];
            for (int c = 0; c < channels; c++) {
                float v = channels == 1 ? (l + r) * 0.5f : (c == 0 ? l : (c == 1 ? r : 0.0f));
                v = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
                const size_t at = (size_t)i * channels + c;
                if (isFloat) {
                    ((float *)buf)[at] = v;
                } else if (bits == 16) {
                    ((int16_t *)buf)[at] = (int16_t)lrintf(v * 32767.0f);
                } else if (bits == 24) {
                    const int32_t s = (int32_t)lrintf(v * 8388607.0f);
                    buf[3 * at] = (BYTE)s;
                    buf[3 * at + 1] = (BYTE)(s >> 8);
                    buf[3 * at + 2] = (BYTE)(s >> 16);
                } else {
                    ((int32_t *)buf)[at] = (int32_t)lrint(v * 2147483647.0);
                }
            }
        }
    }

    void thread_main()
    {
        HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        DWORD task = 0;
        HANDLE avrt = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
        if (avrt)
            AvSetMmThreadPriority(avrt, AVRT_PRIORITY_CRITICAL);
        else
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        while (!quit) {
            if (WaitForSingleObject(event, 200) != WAIT_OBJECT_0)
                continue;
            UINT32 padding = 0;
            HRESULT hr = client->GetCurrentPadding(&padding);
            if (SUCCEEDED(hr) && padding < bufferFrames) {
                const UINT32 frames = bufferFrames - padding;
                BYTE *buf = nullptr;
                hr = render->GetBuffer(frames, &buf);
                if (SUCCEEDED(hr)) {
                    g_conv.render(mix.data(), (int)frames, sched::now_seconds());
                    write(buf, frames);
                    hr = render->ReleaseBuffer(frames, 0);
                }
            }
            if (FAILED(hr)) {
                failed = true;
                break;
            }
        }
        if (avrt)
            AvRevertMmThreadCharacteristics(avrt);
        if (SUCCEEDED(hrCom))
            CoUninitialize();
    }
};

/* ---- default device changes --------------------------------------------- */

class DeviceNotify : public IMMNotificationClient {
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG r = InterlockedDecrement(&refs_);
        if (r == 0)
            delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *out = static_cast<IMMNotificationClient *>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override
    {
        if (flow == eRender && role == eConsole)
            g_deviceChanged = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    LONG refs_ = 1;
};

/* ---- the Windows volumes, kernel streaming ------------------------------- */

/* Kernel streaming bypasses the Windows audio engine, which applies the
 * game's volume mixer slider (its session's volume and mute) and, on a
 * device whose volume is not in hardware, the device's volume and mute too:
 * read here (the output thread, every kVolumePollMs) and applied by the
 * stage.  A hardware volume the driver applies to every stream, ours too. */
struct Volumes {
    IAudioEndpointVolume *endpoint = nullptr;   /* the device's (null: both in hardware) */
    bool swVolume = false, swMute = false;
    ISimpleAudioVolume *session = nullptr;      /* the game's slider (its session: opened here, listed by the mixer) */

    void open(IMMDeviceEnumerator *en)
    {
        close();
        IMMDevice *dev = nullptr;
        if (en == nullptr || FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)))
            return;
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void **)&endpoint))) {
            DWORD hw = 0;
            if (FAILED(endpoint->QueryHardwareSupport(&hw)))
                hw = 0;
            swVolume = (hw & ENDPOINT_HARDWARE_SUPPORT_VOLUME) == 0;
            swMute = (hw & ENDPOINT_HARDWARE_SUPPORT_MUTE) == 0;
            if (!swVolume && !swMute)
                release(endpoint);
        }
        IAudioSessionManager *sm = nullptr;
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager), CLSCTX_ALL, nullptr, (void **)&sm))) {
            if (FAILED(sm->GetSimpleAudioVolume(nullptr, FALSE, &session)))
                session = nullptr;
            sm->Release();
        }
        dev->Release();
        pclog::write("audio: kernel streaming applies the game's volume mixer slider%s; the device's volume %s, its "
                     "mute %s", session ? "" : " (NOT FOUND)", swVolume ? "here (software)" : "by the driver (hardware)",
                     swMute ? "here (software)" : "by the driver (hardware)");
    }

    void close()
    {
        release(endpoint);
        release(session);
        swVolume = swMute = false;
    }

    /* the gain they ask for (1: none open) */
    float gain()
    {
        float g = 1.0f;
        BOOL mute = FALSE;
        if (endpoint) {
            float db = 0.0f;
            if (swVolume && SUCCEEDED(endpoint->GetMasterVolumeLevel(&db)))
                g *= powf(10.0f, db / 20.0f);
            if (swMute && SUCCEEDED(endpoint->GetMute(&mute)) && mute)
                g = 0.0f;
        }
        if (session) {
            float v = 1.0f;
            if (SUCCEEDED(session->GetMasterVolume(&v)))
                g *= v;
            if (SUCCEEDED(session->GetMute(&mute)) && mute)
                g = 0.0f;
        }
        return g;
    }
};

/* ---- the output thread: opens, watches and reopens the output ---------- */

Wasapi g_wasapi;
Volumes g_volumes;
bool g_ksGaveUp;                                /* Output=auto: kernel streaming failed at the largest buffer */

/* The notice over the picture when kernel streaming finds the device busy
 * (audio_ks::start_busy): Output=auto plays through WASAPI instead,
 * Output=ks has no sound and tries again every 2 s (kRetryMs). */
void notice_ks_busy(bool fallback)
{
    char menu[32], text[320];
    input_map::hotkey_text(input_map::kHotSettingsMenu, menu, sizeof(menu));
    snprintf(text, sizeof(text),
             "AUDIO: KERNEL STREAMING: THE DEVICE IS BUSY\n"
             "(WINDOWS OR ANOTHER PROGRAM IS PLAYING ON IT).\n%s",
             fallback ? "PLAYING THROUGH WASAPI INSTEAD." : "NO SOUND: TRYING AGAIN EVERY 2 S.");
    if (!fallback) {
        const size_t at = strlen(text);
        snprintf(text + at, sizeof(text) - at, "\nOR: %s%sAUDIO OUTPUT, AUTO OR WASAPI.", menu,
                 menu[0] != '\0' ? ", " : "THE SETTINGS MENU, ");
    }
    settings_menu::notice(text, 8.0);
}

void set_running(Mode mode, double latencyMs)
{
    g_mode = mode;
    g_latencyMs = latencyMs;
    snd_native_set_output_latency((int)(latencyMs * kRate / 1000.0));
}

/* Opens the output [Audio] asks for: kernel streaming, WASAPI. */
Mode open_output(IMMDeviceEnumerator *en)
{
    const settings::AudioOutput want = settings::audio_output();
    const double periodMs = settings::audio_buffer_ms();
    g_ksBusy = false;
    g_ksExclusive = false;
    if (want == settings::AudioOutput::ks ||
        (want == settings::AudioOutput::automatic && !g_ksGaveUp)) {
        audio_ks::Info &info = g_ksInfo;
        memset(&info, 0, sizeof(info));
        g_qpcToSched = sched::now_seconds() - qpc_seconds();
        g_conv.reset(kRate);
        if (audio_ks::start(ks_render, nullptr, periodMs, &info)) {
            set_running(MODE_KS, info.latencyMs);
            pclog::write("audio: kernel streaming to %s: %d Hz %d-bit%s %d channels, %s, period %d frames, "
                         "buffer %d frames (%.1f ms latency)", info.device, info.rate, info.bits,
                         info.isFloat ? " float" : "", info.channels, info.waveRt ? "WaveRT" : "standard streaming",
                         info.periodFrames, info.bufferFrames, info.latencyMs);
            g_volumes.open(en);
            g_volume = g_volumes.gain();
            if (info.instances == 1) {
                /* not changed (the user's call, 2026-10-20): only said */
                static bool warned;
                g_ksExclusive = true;
                pclog::write("audio: WARNING: kernel streaming has %s to itself: other programs (voice chat, a "
                             "browser) are silent on it while the game plays; [Audio] Output=wasapi shares it",
                             info.device);
                if (!warned)
                    settings_menu::notice("AUDIO: KERNEL STREAMING HAS THIS DEVICE TO ITSELF.\n"
                                          "OTHER PROGRAMS (VOICE CHAT, BROWSER) ARE SILENT ON IT\n"
                                          "WHILE THE GAME PLAYS. TO SHARE IT: F2, AUDIO OUTPUT, WASAPI.",
                                          12.0);
                warned = true;
            }
            return MODE_KS;
        }
        if (want == settings::AudioOutput::ks) {
            g_ksBusy = true;
            return MODE_NONE;
        }
        pclog::write("audio: kernel streaming not possible: WASAPI instead");
        if (audio_ks::start_busy())
            notice_ks_busy(true);
    }
    if (en) {
        if (g_wasapi.open(en, periodMs)) {
            set_running(MODE_WASAPI, g_wasapi.latencyMs);
            return MODE_WASAPI;
        }
    }
    return MODE_NONE;
}

bool timing_log();

void close_output(Mode mode)
{
    if (mode == MODE_KS) {
        audio_ks::stop();
        char line[400];
        if (timing_log() && audio_ks::timing_line(line, sizeof(line)))
            pclog::write("audio: %s; the longest wait for the sound lock %.2f ms (the last, to the stop)", line,
                         snd_native_take_render_lock_wait());
    } else if (mode == MODE_WASAPI)
        g_wasapi.close();
    g_volumes.close();
    g_volume = 1.0f;
    g_ksExclusive = false;
    set_running(MODE_NONE, 0.0);
}

/* A crackle (an underrun) with kernel streaming: the buffer 0.5 ms longer
 * (written to gt3pc.ini: the output restarts with it).  At the largest:
 * false. */
bool raise_buffer(uint32_t underruns)
{
    const double cur = settings::audio_buffer_ms();
    if (cur >= settings::kAudioBufferMax - 1e-9)
        return false;
    const double next = cur + kCrackleStepMs < settings::kAudioBufferMax ? cur + kCrackleStepMs
                                                                         : settings::kAudioBufferMax;
    pclog::write("audio: a crackle (%u underrun%s) at a %.1f ms buffer: raising it to %.1f ms", underruns,
                 underruns == 1 ? "" : "s", cur, next);
    settings_menu::notice("Audio buffer insufficient. Raising buffer by 0.5ms...", 2.0);
    settings::set_audio_buffer_ms(next);
    return true;
}

void log_schedule()
{
    SndNativeScheduleInfo si;
    snd_native_schedule_info(&si);
    pclog::write("audio: scheduling delay %.1f ms, %u blocks (%u late, %u applied early), limiter on %u frames",
                 si.delayMs, si.blocks, si.lateBlocks, si.droppedBlocks, g_stage.limited);
}

/* GT3PC_AUDIO_TIMING=1 (a diagnostic): the kernel stream's refill timing in
 * the log every 10 s. */
bool timing_log()
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("GT3PC_AUDIO_TIMING");
        on = e != nullptr && atoi(e) > 0 ? 1 : 0;
        if (on)
            pclog::write("audio: logging the kernel stream's refill timing every 10 s (GT3PC_AUDIO_TIMING)");
    }
    return on != 0;
}

void thread_main()
{
    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *en = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  (void **)&en);
    DeviceNotify *notify = nullptr;
    if (SUCCEEDED(hr)) {
        notify = new DeviceNotify();
        en->RegisterEndpointNotificationCallback(notify);
    } else {
        pclog::write("audio: no MMDevice enumerator (0x%08X): no WASAPI", (unsigned)hr);
    }
    audio_ks::set_log(ks_log);

    Mode mode = MODE_NONE;
    int gen = settings::audio_generation();
    settings::AudioOutput output = settings::audio_output();
    ULONGLONG nextTry = 0, nextLog = GetTickCount64() + 60000, nextVolume = 0;
    bool reported = false;
    double owed = 0.0, last = sched::now_seconds();
    uint32_t ksUnderruns = 0;                   /* the kernel stream's so far */
    double openedAt = 0.0;

    while (!g_quit) {
        const ULONGLONG tick = GetTickCount64();
        if (settings::audio_output() != output) {
            output = settings::audio_output();
            g_ksGaveUp = false;                 /* the player chose again */
        }
        /* kernel streaming: a crackle raises the buffer (the generation
         * changes: restarted below); a stream that fails at the largest
         * buffer leaves Output=auto to WASAPI */
        bool reopen = false;
        if (mode == MODE_KS) {
            const uint32_t u = audio_ks::underruns();
            const bool failed = audio_ks::failed();
            if (u != ksUnderruns) {
                const uint32_t added = u - ksUnderruns;
                ksUnderruns = u;
                const bool counted = failed || sched::now_seconds() - openedAt >= kCrackleGrace;
                if (counted && !raise_buffer(added) && failed && output == settings::AudioOutput::automatic) {
                    g_ksGaveUp = true;
                    pclog::write("audio: kernel streaming fails even at the largest buffer: WASAPI from now on "
                                 "(until [Audio] Output changes)");
                    settings_menu::notice("Persistent insufficient audio buffer. Switching output...", 2.0);
                }
            }
            if (failed) {
                pclog::write("audio: kernel streaming failed: reopening");
                reopen = true;
            }
            if (tick >= nextVolume) {
                g_volume = g_volumes.gain();
                nextVolume = tick + kVolumePollMs;
            }
        }
        /* what ends the output running: a change of [Audio] or of the
         * default device, a failure */
        if (settings::audio_generation() != gen) {
            gen = settings::audio_generation();
            pclog::write("audio: the settings changed: restarting the output");
            reopen = true;
        }
        if (g_deviceChanged.exchange(false)) {
            pclog::write("audio: the default output device changed: reopening");
            reopen = true;
        }
        if (mode == MODE_WASAPI && g_wasapi.failed) {
            pclog::write("audio: WASAPI: the output device was lost: reopening");
            reopen = true;
        }
        if (reopen) {
            if (mode != MODE_NONE)
                log_schedule();
            close_output(mode);
            mode = MODE_NONE;
            nextTry = tick + 200;
            reported = false;
        }
        if (mode == MODE_NONE && tick >= nextTry) {
            mode = open_output(en);
            ksUnderruns = 0;
            openedAt = sched::now_seconds();
            nextVolume = tick + kVolumePollMs;
            if (mode == MODE_NONE) {
                if (!reported) {
                    pclog::write(g_ksBusy ? "audio: kernel streaming not possible (Output=ks): rendering on without"
                                            " a device, trying again every 2 s"
                                          : "audio: no output device: rendering on without one");
                    if (g_ksBusy && audio_ks::start_busy())
                        notice_ks_busy(false);
                }
                reported = true;
                nextTry = tick + kRetryMs;
            }
        }
        if (tick >= nextLog) {
            log_schedule();
            nextLog = tick + 60000;
        }
        /* GT3PC_AUDIO_TIMING=1: the kernel stream's refill timing every 10 s
         * (audio_ks::timing_line) */
        if (mode == MODE_KS && timing_log()) {
            char line[400];
            if (audio_ks::timing_line(line, sizeof(line)))
                pclog::write("audio: %s; the longest wait for the sound lock %.2f ms", line,
                             snd_native_take_render_lock_wait());
        }
        if (mode != MODE_NONE) {
            Sleep(20);
            last = sched::now_seconds();
            owed = 0.0;
            continue;
        }
        /* no device: the engine renders at real-time pace */
        const double now = sched::now_seconds();
        owed += (now - last) * kRate;
        last = now;
        if (owed > kRate / 4)
            owed = kRate / 4;
        float tmp[2 * 256];
        while (owed >= 256.0) {
            g_stage.render(tmp, 256, now);
            owed -= 256.0;
        }
        Sleep(kNoDeviceStepMs);
    }

    close_output(mode);
    if (en) {
        if (notify)
            en->UnregisterEndpointNotificationCallback(notify);
        en->Release();
    }
    if (notify)
        notify->Release();
    if (SUCCEEDED(hrCom))
        CoUninitialize();
}

} // namespace

/* GT3PC_SND_TRACE=1 / 2: the sound trace (sound_native.h, docs/sound.md
 * 3.6) into the log. */
static void trace_to_log(const char *line)
{
    pclog::write("%s", line);
}

void init()
{
    snd_native_init();
    /* the scheduled engine (sound_native.h): each state block applied at the
     * sample its vblank maps to, rendered by the output's thread */
    snd_native_set_scheduled(1, engine_clock);
    char level[8];
    DWORD n = GetEnvironmentVariableA("GT3PC_SND_TRACE", level, sizeof(level));
    if (n > 0 && n < sizeof(level) && atoi(level) > 0) {
        snd_native_set_trace(atoi(level), trace_to_log);
        pclog::write("snd: tracing sound memory uploads, key-ons and voice reads (GT3PC_SND_TRACE=%d)",
                     atoi(level));
    }
    g_stage.open_dump();
    g_conv.reset(kRate);
    g_quit = false;
    g_thread = std::thread(thread_main);
}

void shutdown()
{
    g_quit = true;
    if (g_thread.joinable())
        g_thread.join();
    if (g_stage.dump) {
        g_stage.write_header();
        fclose(g_stage.dump);
        g_stage.dump = nullptr;
    }
}

void describe(char *out, size_t n)
{
    SndNativeScheduleInfo si;
    snd_native_schedule_info(&si);
    switch (g_mode.load()) {
    case MODE_KS:
        snprintf(out, n, "KS%s %.1f + %.1f MS", g_ksExclusive ? " EXCLUSIVE" : "", g_latencyMs.load(), si.delayMs);
        break;
    case MODE_WASAPI:
        snprintf(out, n, "WASAPI %.1f + %.1f MS", g_latencyMs.load(), si.delayMs);
        break;
    default:
        snprintf(out, n, g_ksBusy ? "NONE (KS BUSY)" : "NONE");
        break;
    }
}

} // namespace audio
