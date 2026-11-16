/* beats.cpp - beat markers made from a song's sound (beats.h). */
#include "beats.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <numeric>

namespace beats {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kAnalysisRate = 11025.0;   /* the sound brought down to about this */
constexpr int kFft = 512;                   /* the window: ~46 ms */
constexpr double kFps = 100.0;              /* onset frames a second (about) */
constexpr int kBands = 24;
constexpr double kBandLo = 30.0, kBandHi = 5000.0;
constexpr double kFloorDb = 80.0;           /* levels kept within this of the loudest */
constexpr double kTightness = 400.0;        /* the penalty of a beat spacing off the period */
constexpr double kLead = 0.018;             /* s: the beats put this much earlier (the discs' place them so) */
constexpr int kBeatsPerBar = 4;
constexpr double kAccentStd = 5.0;          /* an accent: an onset this many deviations up */
constexpr double kAccentGap = 0.25;         /* s from a bar's beat at least */

using Complex = std::complex<double>;

/* In-place radix-2 FFT of a power-of-two size. */
void fft(std::vector<Complex> &a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const Complex w(std::cos(-2 * kPi / (double)len), std::sin(-2 * kPi / (double)len));
        for (size_t i = 0; i < n; i += len) {
            Complex wk(1.0, 0.0);
            for (size_t k = 0; k < len / 2; k++) {
                const Complex u = a[i + k], v = a[i + k + len / 2] * wk;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                wk *= w;
            }
        }
    }
}

/* The sound's onset strength every hop and its band levels (dB). */
struct Onsets {
    double fps = 0;                         /* frames a second */
    double offset = 0;                      /* s: frame t is at t / fps + offset */
    std::vector<double> strength;
    std::vector<std::array<float, kBands>> level;
};

bool onsets(const float *mono, size_t frames, int rate, Onsets *out)
{
    /* down to ~11 kHz: the mean of each `d' samples */
    const int d = std::max(1, (int)std::lround(rate / kAnalysisRate));
    const double sr = (double)rate / d;
    std::vector<float> x(frames / (size_t)d);
    for (size_t i = 0; i < x.size(); i++) {
        float s = 0;
        for (int k = 0; k < d; k++)
            s += mono[i * (size_t)d + (size_t)k];
        x[i] = s / (float)d;
    }
    const int hop = std::max(1, (int)std::lround(sr / kFps));
    if (x.size() < (size_t)kFft * 4)
        return false;
    const size_t n = (x.size() - kFft) / (size_t)hop + 1;
    /* each bin's band (-1: outside 30 Hz .. 5 kHz) */
    int band[kFft / 2 + 1];
    for (int k = 0; k <= kFft / 2; k++) {
        const double f = k * sr / kFft;
        band[k] = f < kBandLo || f >= kBandHi ? -1 : (int)(kBands * std::log(f / kBandLo) / std::log(kBandHi / kBandLo));
    }
    std::vector<double> win(kFft);
    for (int i = 0; i < kFft; i++)
        win[i] = 0.5 - 0.5 * std::cos(2 * kPi * i / kFft);
    out->level.assign(n, {});
    std::vector<Complex> a(kFft);
    float loudest = -1000.0f;
    for (size_t t = 0; t < n; t++) {
        for (int i = 0; i < kFft; i++)
            a[i] = Complex(x[t * (size_t)hop + (size_t)i] * win[i], 0.0);
        fft(a);
        double e[kBands] = {};
        for (int k = 0; k <= kFft / 2; k++)
            if (band[k] >= 0)
                e[band[k]] += std::norm(a[k]);
        for (int b = 0; b < kBands; b++) {
            out->level[t][b] = (float)(10.0 * std::log10(e[b] + 1e-10));
            loudest = std::max(loudest, out->level[t][b]);
        }
    }
    for (auto &l : out->level)
        for (float &v : l)
            v = std::max(v, loudest - (float)kFloorDb);
    /* the onset strength: the bands' rises, summed */
    out->strength.assign(n, 0.0);
    for (size_t t = 1; t < n; t++)
        for (int b = 0; b < kBands; b++)
            out->strength[t] += std::max(0.0, (double)out->level[t][b] - out->level[t - 1][b]);
    out->fps = sr / hop;
    out->offset = (kFft / 2.0) / sr;        /* (a frame's time: its window's middle) */
    return true;
}

double mean_of(const std::vector<double> &v)
{
    return v.empty() ? 0.0 : std::accumulate(v.begin(), v.end(), 0.0) / (double)v.size();
}

double std_of(const std::vector<double> &v)
{
    const double m = mean_of(v);
    double s = 0;
    for (double x : v)
        s += (x - m) * (x - m);
    return v.empty() ? 0.0 : std::sqrt(s / (double)v.size());
}

/* The beats (frames): Ellis's dynamic programming over the onset strength
 * at period p. */
std::vector<size_t> track(const std::vector<double> &o, double p)
{
    const size_t n = o.size();
    /* the local score: the onsets smoothed at p / 32 */
    std::vector<double> local(n, 0.0);
    const int half = (int)std::ceil(p);
    std::vector<double> g(2 * half + 1);
    for (int i = -half; i <= half; i++)
        g[i + half] = std::exp(-0.5 * std::pow(i * 32.0 / p, 2));
    for (size_t t = 0; t < n; t++) {
        double s = 0;
        for (int i = -half; i <= half; i++) {
            const long u = (long)t + i;
            if (u >= 0 && u < (long)n)
                s += o[(size_t)u] * g[i + half];
        }
        local[t] = s;
    }
    const double localMax = *std::max_element(local.begin(), local.end());
    std::vector<double> score(n, 0.0);
    std::vector<long> back(n, -1);
    const long reachLo = (long)std::lround(p / 2), reachHi = (long)std::lround(2 * p);
    bool first = true;
    for (size_t t = 0; t < n; t++) {
        double best = 0.0;
        long from = -1;
        for (long d = reachLo; d <= reachHi; d++) {
            const long u = (long)t - d;
            if (u < 0)
                break;
            const double l = std::log((double)d / p);
            const double s = score[(size_t)u] - kTightness * l * l;
            if (from < 0 || s > best) {
                best = s;
                from = u;
            }
        }
        score[t] = local[t] + (from >= 0 ? best : 0.0);
        /* (the first beat: where the onsets start) */
        if (first && local[t] < 0.01 * localMax) {
            back[t] = -1;
        } else {
            back[t] = from;
            first = false;
        }
    }
    /* the last beat: the last local maximum of the score above half the
     * maxima's median */
    std::vector<double> maxima;
    for (size_t t = 1; t + 1 < n; t++)
        if (score[t] > score[t - 1] && score[t] >= score[t + 1])
            maxima.push_back(score[t]);
    if (maxima.empty())
        return {};
    std::nth_element(maxima.begin(), maxima.begin() + (long)maxima.size() / 2, maxima.end());
    const double med = maxima[maxima.size() / 2];
    long last = -1;
    for (size_t t = n - 2; t >= 1; t--)
        if (score[t] > score[t - 1] && score[t] >= score[t + 1] && score[t] >= 0.5 * med) {
            last = (long)t;
            break;
        }
    std::vector<size_t> beats;
    for (long t = last; t >= 0; t = back[(size_t)t])
        beats.push_back((size_t)t);
    std::reverse(beats.begin(), beats.end());
    /* the beats in silence at the ends dropped: the smoothed local score
     * under half its rms */
    double rms = 0;
    for (double v : local)
        rms += v * v;
    rms = std::sqrt(rms / (double)n);
    auto weak = [&](size_t t) { return local[t] < 0.5 * rms; };
    while (!beats.empty() && weak(beats.front()))
        beats.erase(beats.begin());
    while (!beats.empty() && weak(beats.back()))
        beats.pop_back();
    return beats;
}

/* The beat each bar starts on (0 .. kBeatsPerBar - 1): where the band
 * levels change most from the half bar before to the half bar after
 * (chords change at bar lines; the kick drum's hits chose worse on the
 * discs' songs). */
int bar_phase(const Onsets &on, const std::vector<size_t> &beats)
{
    const size_t nb = beats.size();
    if (nb < (size_t)kBeatsPerBar * 2)
        return 0;
    const int halfBar = kBeatsPerBar / 2;
    /* each beat's mean band levels up to the next beat */
    std::vector<std::array<double, kBands>> seg(nb, std::array<double, kBands>{});
    for (size_t i = 0; i + 1 < nb; i++) {
        const size_t a = beats[i], b = std::max(beats[i + 1], a + 1);
        for (size_t t = a; t < b; t++)
            for (int k = 0; k < kBands; k++)
                seg[i][k] += on.level[t][k];
        for (int k = 0; k < kBands; k++)
            seg[i][k] /= (double)(b - a);
    }
    std::vector<double> change(nb, 0.0);
    for (size_t i = (size_t)halfBar; i + (size_t)halfBar < nb; i++) {
        double d = 0;
        for (int k = 0; k < kBands; k++) {
            double before = 0, after = 0;
            for (int j = 1; j <= halfBar; j++)
                before += seg[i - (size_t)j][k];
            for (int j = 0; j < halfBar; j++)
                after += seg[i + (size_t)j][k];
            d += std::fabs(after - before) / halfBar;
        }
        change[i] = d;
    }
    double best = -1e300;
    int phase = 0;
    for (int k = 0; k < kBeatsPerBar; k++) {
        double s = 0;
        for (size_t i = (size_t)k; i < nb; i += kBeatsPerBar)
            s += change[i];
        if (s > best) {
            best = s;
            phase = k;
        }
    }
    return phase;
}

} // namespace

bool analyze(const float *mono, size_t frames, int rate, double bpm, Result *out)
{
    *out = Result();
    Onsets on;
    if (rate <= 0 || !(bpm >= kMinBpm && bpm <= kMaxBpm) || !onsets(mono, frames, rate, &on))
        return false;
    /* the onset strength normalised */
    std::vector<double> o = on.strength;
    const double sd = std_of(o);
    if (sd <= 0)
        return false;
    for (double &v : o)
        v /= sd;
    const std::vector<size_t> b = track(o, on.fps * 60.0 / bpm);
    if (b.size() < (size_t)kBeatsPerBar * 2)
        return false;
    auto when = [&](size_t t) { return std::max(0.0, t / on.fps + on.offset - kLead); };
    for (size_t t : b)
        out->beats.push_back(when(t));
    std::vector<double> iv;
    for (size_t i = 1; i < b.size(); i++)
        iv.push_back((double)(b[i] - b[i - 1]));
    std::nth_element(iv.begin(), iv.begin() + (long)iv.size() / 2, iv.end());
    out->bpm = 60.0 * on.fps / iv[iv.size() / 2];
    const int phase = bar_phase(on, b);
    for (size_t i = (size_t)phase; i < b.size(); i += kBeatsPerBar)
        out->bars.push_back(when(b[i]));
    /* the cuts: the bars, and in each bar its strongest accent - an onset
     * peak kAccentStd deviations above the mean, kAccentGap from the bar's
     * beats (about as many cuts as the discs' songs have) */
    const double m = mean_of(o), s = std_of(o);
    out->cuts = out->bars;
    for (size_t i = 0; i + 1 < out->bars.size(); i++) {
        const double from = out->bars[i] + kAccentGap, to = out->bars[i + 1] - kAccentGap;
        double best = m + kAccentStd * s;
        long at = -1;
        for (size_t t = 1; t + 1 < o.size(); t++) {
            const double tt = when(t);
            if (tt < from || tt > to || o[t] < o[t - 1] || o[t] < o[t + 1] || o[t] <= best)
                continue;
            best = o[t];
            at = (long)t;
        }
        if (at >= 0)
            out->cuts.push_back(when((size_t)at));
    }
    std::sort(out->cuts.begin(), out->cuts.end());
    return true;
}

} // namespace beats
