/* audio_ks.h - low-latency sound output through WDM kernel streaming.
 *
 * WASAPI shared mode puts the Windows audio engine (audiodg) between the
 * program and the driver, and that costs 10-30 ms.  This back end skips the
 * engine and talks to the driver's render pin itself, as ASIO4ALL and
 * PortAudio's WDM-KS host API do: it finds the KS filter behind the default
 * render endpoint (through the Core Audio device topology, no WASAPI
 * stream), creates an instance of its sink pin and streams to it.
 *
 * Two kinds of pins are handled.  WaveRT (HD Audio, most drivers since
 * Vista): the driver maps a looped DMA buffer into the process; the back end
 * writes ahead of the hardware's play position, woken by the driver's
 * notification event twice per buffer (period = half the buffer), or, when
 * the driver has no notifications, by a high-resolution timer that polls
 * the position.  Standard streaming (older and some USB drivers): 4 packets
 * of one period each are queued with IOCTL_KS_WRITE_STREAM and each one is
 * refilled and queued again as it completes.
 *
 * A pin is one hardware stream: while the Windows audio engine plays
 * anything on the device it holds the pin and start() fails ("busy"), and
 * while this back end holds it other programs get no sound from that
 * device.  The caller falls back to WASAPI when start() fails.
 *
 * The render thread runs with the MMCSS "Pro Audio" task at critical
 * priority.  It does not allocate, lock or log; it counts underruns and the
 * counts are logged by stop().
 *
 * Links: ole32, avrt, ksuser, setupapi (pulled in by #pragma comment in the
 * .cpp, so no build change is needed). */
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace audio_ks {

/* Called on the stream's real-time thread once per period: fill `frames'
 * frames of interleaved stereo float at the stream's rate (Info::rate),
 * nominal range -1..1 (the back end clips to the device format).  `frames'
 * is Info::periodFrames except, rarely, one frame more or less when the
 * driver's buffer does not split evenly.  `now' is the time the period
 * began, in seconds from QueryPerformanceCounter / its frequency (an
 * absolute time, comparable between calls and with other QPC readings). */
typedef void (*RenderFn)(void *user, float *out, int frames, double now);

struct Info {
    char device[128];   /* the endpoint's friendly name (UTF-8) */
    int rate;           /* the pin's rate (48000 when the pin takes it) */
    int channels;       /* the pin's channel count (2 preferred; more: L / R
                           go to the first two, the rest are silent; mono:
                           their mean) */
    int bits;           /* container bits of a sample: 16, 24 (packed), 32
                           (32-bit integer, 24 valid bits in 32, or float) */
    bool isFloat;
    bool waveRt;        /* WaveRT (looped buffer) or standard streaming
                           (packets) */
    int periodFrames;   /* frames per RenderFn call */
    int bufferFrames;   /* frames in the device buffer (WaveRT) or in all the
                           packets (standard streaming) */
    double latencyMs;   /* output latency estimate: what can be queued ahead
                           of the DAC (the buffer, or the polled mode's fill
                           target) plus the pin's own reported latency (the
                           FIFO and the chipset / codec delays) when the
                           driver gives it */
    int instances;      /* the pin's possible instances (system-wide when the
                           driver reports it): 1 = the device is this
                           program's alone while it plays (the Windows audio
                           engine, so every other program, gets no sound from
                           it); more = shared; 0 = not reported */
};

/* Opens the kernel streaming render pin of the default render device (the
 * endpoint Windows uses for eConsole) at 48 kHz stereo if possible (else
 * the nearest the pin offers: 44.1 / 96 kHz, more channels, 16 / 24 /
 * 32-bit, float), with a period of about periodMs (clamped to 0.5..100 ms
 * and to what the pin allows), and starts the stream: fn is called on a
 * thread of its own with MMCSS "Pro Audio" priority.  Returns false (and
 * logs why through the log sink) when kernel streaming is not possible: no
 * KS filter for the default device, no free pin instance (another program
 * or the Windows audio engine holds it), no format in common, a failure to
 * start.  Each driver call is given at most 1 s.  info may be null; when it
 * is not, *info is filled completely before the first RenderFn call (the
 * render thread is created after it, and no RenderFn call is made before
 * that: the device starts on silence), so fn may read Info::rate on its
 * first call.  A stream already running is stopped first. */
bool start(RenderFn fn, void *user, double periodMs, Info *info);

/* The last start() failed because the device is busy: a pin with no free
 * instance, or one that refused every format while others hold it (the
 * thread that calls start). */
bool start_busy();

/* Stops the stream and closes the pin (joins the thread), then logs the
 * counts (periods, underruns).  Safe to call when not started. */
void stop();

/* The stream failed after it started (device removed, I/O error, no
 * notification for 1 s, a storm of 32 underruns within one second): the
 * render thread has ended and silenced the buffer; the caller stops and
 * restarts or falls back. */
bool failed();

/* The underruns of the stream running so far (0 after each start): a rise
 * is a crackle.  Any thread. */
uint32_t underruns();

/* PC (a diagnostic, GT3PC_AUDIO_TIMING=1: audio.cpp logs it): WaveRT with
 * notifications, the refills of the last 10 s - how far into the half it
 * plays the hardware was at each notification, how much of that half was
 * left once the other was written (late: none; tight: under 16 frames), the
 * longest gap between notifications.  True with a line in out when a new
 * window is done since the last call.  Any thread. */
bool timing_line(char *out, size_t size);

/* Optional log sink (default: none): one line per call, no newline.  Called
 * from start() and stop() only, never from the render thread. */
void set_log(void (*log)(const char *line));

} // namespace audio_ks
