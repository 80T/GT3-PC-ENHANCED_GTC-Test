/* kstest.cpp - checks the kernel streaming sound output
 * (src/platform/audio_ks.cpp) on this machine, without the game.
 *
 *   kstest list              the KS render filters and their sink pins
 *                            (name, WaveRT or standard, data ranges,
 *                            instances possible / in use), and the filter
 *                            and pin behind the default endpoint
 *   kstest silent [periodMs] streams SILENCE (all zero samples) for 2 s
 *                            through audio_ks::start, then prints the Info,
 *                            the number of render calls, the interval
 *                            between them and the underruns
 *   kstest silent-filter <n> [periodMs] [polled]
 *                            the same on render filter n of `list' instead
 *                            of the default endpoint's (to try the WaveRT
 *                            path on a device that is not the default);
 *                            `polled': WaveRT without notifications
 *
 * It only ever plays zeros.  The implementation is included whole (not
 * linked) so that `list' can use its internal discovery and pin queries
 * without widening audio_ks.h. */
#include "platform/audio_ks.cpp"

#include <math.h>
#include <stdlib.h>

namespace {

using namespace audio_ks;

/* ---- the log -------------------------------------------------------------- */

unsigned g_underruns = 0;
bool g_haveUnderruns = false;

void print_log(const char *line)
{
    printf("  log: %s\n", line);
    const char *p = strstr(line, " periods, ");
    if (p) {
        char *end = nullptr;
        unsigned long u = strtoul(p + 10, &end, 10);
        if (end && strncmp(end, " underruns", 10) == 0) {
            g_underruns = (unsigned)u;
            g_haveUnderruns = true;
        }
    }
}

/* ---- list ----------------------------------------------------------------- */

void print_ranges(const PinInfo &pi)
{
    for (const Range &r : pi.ranges) {
        char ch[16];
        if (r.maxChannels == 0xFFFFFFFFu)
            snprintf(ch, sizeof(ch), "any");
        else
            snprintf(ch, sizeof(ch), "%lu", (unsigned long)r.maxChannels);
        printf("      range: %s%s%s, up to %s channels, %lu-%lu bits, %lu-%lu Hz\n", r.pcm ? "PCM" : "",
               r.pcm && r.flt ? "/" : "", r.flt ? "float" : "", ch, (unsigned long)r.minBits,
               (unsigned long)r.maxBits, (unsigned long)r.minRate, (unsigned long)r.maxRate);
    }
    if (pi.ranges.empty())
        printf("      (no audio data range)\n");
}

int cmd_list()
{
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Target t;
    std::string why;
    printf("default render endpoint (eConsole):\n");
    set_log(print_log);
    bool found = find_default_target(t, why, ev, true);
    set_log(nullptr);
    if (found) {
        printf("  endpoint: %s\n", utf8(t.name.c_str()).c_str());
        printf("  jack:     %s pin %lu\n", utf8(t.jackPath.c_str()).c_str(), (unsigned long)t.jackPin);
        printf("  sink:     %s pin(s)", utf8(t.path.c_str()).c_str());
        for (ULONG p : t.pins)
            printf(" %lu", (unsigned long)p);
        printf("%s\n", t.approximate ? " (found by device, not by topology)" : "");
    } else {
        printf("  not found: %s\n", why.c_str());
    }

    std::vector<FilterEntry> filters = enum_render_filters();
    printf("\nKS render filters (KSCATEGORY_RENDER + KSCATEGORY_AUDIO): %u\n", (unsigned)filters.size());
    int index = 0;
    for (const FilterEntry &fe : filters) {
        bool isDefault = found && same_filter(fe.path, t.path);
        printf("\n[%d] %s%s\n  %s\n", index++, utf8(fe.name.c_str()).c_str(),
               isDefault ? "   <- default endpoint" : "", utf8(fe.path.c_str()).c_str());
        DWORD err = 0;
        HANDLE f = open_filter(fe.path, &err);
        if (!f) {
            char e[160];
            printf("  cannot open: %s\n", errstr(err, e, sizeof(e)));
            continue;
        }
        ULONG n = pin_count(f, ev);
        printf("  %lu pin factories\n", (unsigned long)n);
        for (ULONG i = 0; i < n; i++) {
            PinInfo pi;
            if (!describe_pin(f, ev, i, pi) || !pi.sink())
                continue;
            bool isTarget = false;
            if (isDefault)
                for (ULONG p : t.pins)
                    isTarget = isTarget || p == i;
            printf("    pin %lu '%s': sink, %s%s%s", (unsigned long)i, utf8(pi.name.c_str()).c_str(),
                   pi.looped ? "WaveRT (looped)" : "", pi.looped && pi.standard ? " + " : "",
                   pi.standard ? "standard streaming" : "");
            if (!pi.looped && !pi.standard)
                printf("no standard interface");
            if (pi.haveGlobal)
                printf(", instances %lu possible / %lu in use system-wide", (unsigned long)pi.global.PossibleCount,
                       (unsigned long)pi.global.CurrentCount);
            if (pi.haveInstances)
                printf(", %lu / %lu on this handle", (unsigned long)pi.instances.PossibleCount,
                       (unsigned long)pi.instances.CurrentCount);
            if (pi.busy())
                printf(" (BUSY)");
            printf("%s\n", isTarget ? "   <- default endpoint" : "");
            print_ranges(pi);
        }
        CloseHandle(f);
    }
    CloseHandle(ev);
    return 0;
}

/* ---- silent ----------------------------------------------------------------- */

constexpr int kMaxCalls = 20000;

struct Stats {
    double times[kMaxCalls];
    int frames[kMaxCalls];
    std::atomic<int> calls{0};
    const Info *info = nullptr;
    int firstRate = 0; /* Info::rate as the first call saw it */
};

Stats g_stats;

void render_silence(void *user, float *out, int frames, double now)
{
    Stats *st = (Stats *)user;
    int i = st->calls.load(std::memory_order_relaxed);
    if (i == 0)
        st->firstRate = st->info->rate;
    if (i < kMaxCalls) {
        st->times[i] = now;
        st->frames[i] = frames;
    }
    st->calls.store(i + 1, std::memory_order_release);
    memset(out, 0, sizeof(float) * 2 * (size_t)frames); /* silence, always */
}

int cmd_silent(double periodMs, const Target *forced)
{
    static Info info; /* static: the render thread reads it */
    memset(&info, 0, sizeof(info));
    g_stats.info = &info;
    set_log(print_log);
    printf("starting a silent stream, period %.2f ms%s\n", periodMs, forced ? " (forced filter)" : "");
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    bool ok = forced ? start_impl(forced, render_silence, &g_stats, periodMs, &info)
                     : start(render_silence, &g_stats, periodMs, &info);
    QueryPerformanceCounter(&t1);
    printf("start() took %.1f ms and returned %s\n", (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart,
           ok ? "true" : "false");
    if (!ok)
        return 2;
    for (int i = 0; i < 20 && !failed(); i++)
        Sleep(100);
    bool fail = failed();
    stop();
    set_log(nullptr);

    printf("\nInfo:\n");
    printf("  device        %s\n", info.device);
    printf("  rate          %d\n", info.rate);
    printf("  channels      %d\n", info.channels);
    printf("  bits          %d%s\n", info.bits, info.isFloat ? " float" : "");
    printf("  waveRt        %s\n", info.waveRt ? "yes" : "no (standard streaming)");
    printf("  periodFrames  %d (%.3f ms)\n", info.periodFrames, info.periodFrames * 1000.0 / info.rate);
    printf("  bufferFrames  %d (%.3f ms)\n", info.bufferFrames, info.bufferFrames * 1000.0 / info.rate);
    printf("  latencyMs     %.3f\n", info.latencyMs);

    int calls = g_stats.calls.load(std::memory_order_acquire);
    int n = calls < kMaxCalls ? calls : kMaxCalls;
    printf("\nrender calls: %d (Info::rate on the first call: %d)\n", calls, g_stats.firstRate);
    if (n >= 2) {
        double mn = 1e9, mx = 0, sum = 0, sum2 = 0;
        int late = 0, odd = 0;
        double nominal = info.periodFrames * 1000.0 / info.rate;
        for (int i = 1; i < n; i++) {
            double d = (g_stats.times[i] - g_stats.times[i - 1]) * 1000.0;
            mn = d < mn ? d : mn;
            mx = d > mx ? d : mx;
            sum += d;
            sum2 += d * d;
            if (d > 1.5 * nominal)
                late++;
        }
        for (int i = 0; i < n; i++)
            if (g_stats.frames[i] != info.periodFrames)
                odd++;
        double mean = sum / (n - 1);
        double sd = sqrt(sum2 / (n - 1) - mean * mean > 0 ? sum2 / (n - 1) - mean * mean : 0);
        printf("interval between calls (ms): min %.3f  max %.3f  mean %.3f  sd %.3f  (nominal %.3f)\n", mn, mx,
               mean, sd, nominal);
        printf("intervals over 1.5x the period: %d; calls with frames != periodFrames: %d\n", late, odd);
        double frames = 0;
        for (int i = 0; i < n - 1; i++)
            frames += g_stats.frames[i];
        double span = g_stats.times[n - 1] - g_stats.times[0];
        printf("frames delivered between the first and the last call: %.0f in %.3f s (%.1f Hz)\n", frames, span,
               span > 0 ? frames / span : 0.0);
    }
    if (g_haveUnderruns)
        printf("underruns: %u\n", g_underruns);
    else
        printf("underruns: (not reported)\n");
    printf("failed(): %s\n", fail ? "true" : "false");
    return fail ? 3 : 0;
}

} // namespace

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int rc;
    if (argc >= 2 && strcmp(argv[1], "list") == 0) {
        rc = cmd_list();
    } else if (argc >= 2 && strcmp(argv[1], "silent") == 0) {
        double ms = argc >= 3 ? atof(argv[2]) : 2.0;
        if (!(ms > 0))
            ms = 2.0;
        rc = cmd_silent(ms, nullptr);
    } else if (argc >= 3 && strcmp(argv[1], "silent-filter") == 0) {
        std::vector<FilterEntry> filters = enum_render_filters();
        int i = atoi(argv[2]);
        double ms = argc >= 4 ? atof(argv[3]) : 2.0;
        if (!(ms > 0))
            ms = 2.0;
        if (i < 0 || i >= (int)filters.size()) {
            printf("no render filter %d (see kstest list)\n", i);
            rc = 1;
        } else {
            if (argc >= 5 && strcmp(argv[4], "polled") == 0)
                g_testNoNotify = true;
            Target t;
            t.name = filters[i].name;
            t.path = filters[i].path;
            rc = cmd_silent(ms, &t); /* no pins given: start tries every sink pin of the filter */
        }
    } else {
        printf("usage: kstest list | kstest silent [periodMs] | kstest silent-filter <n> [periodMs]\n");
        rc = 1;
    }
    if (SUCCEEDED(hr))
        CoUninitialize();
    return rc;
}
