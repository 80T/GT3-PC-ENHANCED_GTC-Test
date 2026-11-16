/* mp4_frames.cpp - an MP4 movie's frames for the G002 player's queue
 * (mp4_frames.h). */
#include "engine/media/native/mp4_frames.h"

#include "engine/media/native/mp4_movie.h"
#include "platform/log.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace gt3media {
namespace {

constexpr int kAhead = 4;                   /* frames decoded ahead */
constexpr uint8_t kAlpha = 0x80;            /* the IPU's alpha */

/* A resampling filter's taps for each output position: an area average
 * down, linear up. */
struct Taps {
    int maxN = 0;
    std::vector<int> first, count;
    std::vector<float> w;                   /* [i * maxN + j] */
};

void make_taps(int src, int dst, Taps &t)
{
    const double scale = (double)src / dst;
    t.maxN = scale > 1.0 ? (int)std::ceil(scale) + 2 : 2;
    t.first.assign((size_t)dst, 0);
    t.count.assign((size_t)dst, 0);
    t.w.assign((size_t)dst * t.maxN, 0.0f);
    for (int i = 0; i < dst; i++) {
        float *w = &t.w[(size_t)i * t.maxN];
        if (scale > 1.0) {
            const double a = i * scale, b = a + scale;
            const int s = (int)std::floor(a), e = std::min(src, (int)std::ceil(b));
            t.first[i] = s;
            for (int k = s; k < e && k - s < t.maxN; k++) {
                const double ov = std::min(b, (double)k + 1) - std::max(a, (double)k);
                w[k - s] = (float)(ov / scale);
                t.count[i] = k - s + 1;
            }
        } else {
            const double x = (i + 0.5) * scale - 0.5;
            int k0 = (int)std::floor(x);
            float f = (float)(x - k0);
            if (k0 < 0) {
                k0 = 0;
                f = 0.0f;
            }
            if (k0 >= src - 1) {
                k0 = src - 1;
                f = 0.0f;
            }
            t.first[i] = k0;
            t.count[i] = k0 + 1 < src ? 2 : 1;
            w[0] = 1.0f - f;
            w[1] = f;
        }
    }
}

/* The picture's place in the slot and the filters (for one source size). */
struct Layout {
    int srcW = 0, srcH = 0;
    int cx = 0, cy = 0, cw = 0, ch = 0;     /* the picture's rectangle in the slot */
    Taps h, v;
};

/* The whole slot: the picture's shape on screen is the draw's (pc_movie_fit,
 * [Display] MovieAspect). */
void make_layout(Layout &l, int w, int h, int slotW, int slotH)
{
    l.srcW = w;
    l.srcH = h;
    l.cw = slotW;
    l.ch = slotH;
    l.cx = l.cy = 0;
    make_taps(w, l.cw, l.h);
    make_taps(h, l.ch, l.v);
}

/* The slot pixel (x, y)'s bytes in the block layout (columns of 16 x 16
 * blocks). */
inline uint8_t *slot_pixel(uint8_t *dst, int slotH, int x, int y)
{
    const size_t block = (size_t)(x >> 4) * (size_t)(slotH >> 4) + (size_t)(y >> 4);
    return dst + block * 0x400 + (size_t)(((y & 15) << 4) | (x & 15)) * 4;
}

/* A picture (slotW x slotH blocks) into the upper (half 0) or lower (1)
 * half of a frame of twice its height: each column of blocks. */
void place(const uint8_t *pic, uint8_t *frame, int slotW, int slotH, int half)
{
    const size_t bh = (size_t)(slotH >> 4), col = bh * 0x400;
    for (int bx = 0; bx < (slotW >> 4); bx++)
        std::memcpy(frame + ((size_t)bx * 2 * bh + (size_t)half * bh) * 0x400, pic + (size_t)bx * col, col);
}

void fill_black(uint8_t *dst, int slotW, int slotH)
{
    const size_t n = (size_t)slotW * slotH;
    for (size_t i = 0; i < n; i++) {
        dst[i * 4] = dst[i * 4 + 1] = dst[i * 4 + 2] = 0;
        dst[i * 4 + 3] = kAlpha;
    }
}

/* The picture (B, G, R, x rows) scaled into its rectangle of the slot. */
void convert(const uint8_t *src, const Layout &l, uint8_t *dst, int slotW, int slotH, std::vector<float> &tmp)
{
    fill_black(dst, slotW, slotH);
    /* across: each source row to the rectangle's width (R, G, B floats) */
    tmp.resize((size_t)l.srcH * l.cw * 3);
    for (int y = 0; y < l.srcH; y++) {
        const uint8_t *row = src + (size_t)y * l.srcW * 4;
        float *o = &tmp[(size_t)y * l.cw * 3];
        for (int x = 0; x < l.cw; x++) {
            const float *w = &l.h.w[(size_t)x * l.h.maxN];
            const uint8_t *p = row + (size_t)l.h.first[x] * 4;
            float r = 0.0f, g = 0.0f, b = 0.0f;
            for (int j = 0; j < l.h.count[x]; j++, p += 4) {
                b += p[0] * w[j];
                g += p[1] * w[j];
                r += p[2] * w[j];
            }
            o[x * 3] = r;
            o[x * 3 + 1] = g;
            o[x * 3 + 2] = b;
        }
    }
    /* down: the rows to the rectangle's height, into the blocks */
    for (int y = 0; y < l.ch; y++) {
        const float *w = &l.v.w[(size_t)y * l.v.maxN];
        const int first = l.v.first[y], count = l.v.count[y];
        for (int x = 0; x < l.cw; x++) {
            float r = 0.0f, g = 0.0f, b = 0.0f;
            for (int j = 0; j < count; j++) {
                const float *p = &tmp[((size_t)(first + j) * l.cw + x) * 3];
                r += p[0] * w[j];
                g += p[1] * w[j];
                b += p[2] * w[j];
            }
            uint8_t *o = slot_pixel(dst, slotH, l.cx + x, l.cy + y);
            o[0] = (uint8_t)std::min(255.0f, r + 0.5f);
            o[1] = (uint8_t)std::min(255.0f, g + 0.5f);
            o[2] = (uint8_t)std::min(255.0f, b + 0.5f);
            o[3] = kAlpha;
        }
    }
}

} // namespace

struct Mp4FrameWorker::Impl {
    std::string path;
    int slotW, slotH, frames;
    bool loop;
    size_t frameBytes;

    std::mutex m;
    std::condition_variable cv;
    struct Frame {
        int index;
        std::vector<uint8_t> px;
    };
    std::deque<Frame> ready;
    int produce = 0;                        /* the next frame the thread makes */
    int resetTo = -1;                       /* a jump asked for */
    bool quit = false;
    std::atomic<double> aspect{0.0};        /* the picture's shape (0: not open yet) */
    std::thread thread;

    void run();
};

double Mp4FrameWorker::aspect() const
{
    return impl_->aspect.load(std::memory_order_relaxed);
}

Mp4FrameWorker::Mp4FrameWorker(const std::string &path, int width, int height, int frames, bool loop)
    : path_(path), impl_(new Impl)
{
    Impl &i = *impl_;
    i.path = path;
    i.slotW = std::max(16, width & ~15);
    i.slotH = std::max(16, height & ~15);
    i.frames = std::max(1, frames);
    i.loop = loop;
    i.frameBytes = (size_t)i.slotW * i.slotH * 2 * 4;      /* two pictures */
    i.thread = std::thread([this] { impl_->run(); });
}

Mp4FrameWorker::~Mp4FrameWorker()
{
    {
        std::lock_guard<std::mutex> l(impl_->m);
        impl_->quit = true;
    }
    impl_->cv.notify_all();
    if (impl_->thread.joinable())
        impl_->thread.join();
}

bool Mp4FrameWorker::take(int index, uint8_t *dst)
{
    Impl &i = *impl_;
    std::unique_lock<std::mutex> l(i.m);
    /* frames before it (a jump forward): dropped */
    auto it = std::find_if(i.ready.begin(), i.ready.end(), [index](const Impl::Frame &f) { return f.index == index; });
    if (it != i.ready.end()) {
        std::memcpy(dst, it->px.data(), i.frameBytes);
        i.ready.erase(i.ready.begin(), it + 1);
        l.unlock();
        i.cv.notify_all();
        return true;
    }
    /* not there and not next: decode from there */
    if (i.resetTo != index && !(i.ready.empty() && i.produce == index)) {
        i.resetTo = index;
        l.unlock();
        i.cv.notify_all();
    }
    return false;
}

void Mp4FrameWorker::Impl::run()
{
    MfScope mf;
    Mp4VideoReader r;
    const bool ok = mf.ok() && r.open(path.c_str());
    if (ok) {
        pclog::write("movie: %s: %d x %d pictures (pixel aspect %.3f), %d frames of two %d x %d pictures (59.94 a "
                     "second)", path.c_str(), r.width(), r.height(), r.pixel_aspect(), frames, slotW, slotH);
        aspect.store((double)r.width() * r.pixel_aspect() / r.height(), std::memory_order_relaxed);
    }
    Layout layout;
    std::vector<uint8_t> cur, nxt;          /* the source picture shown, the next one decoded */
    int64_t curT = 0, nxtT = 0, convertedT = -1;
    bool haveCur = false, haveNxt = false, ended = false;
    const size_t pictureBytes = frameBytes / 2;
    std::vector<uint8_t> last;              /* the last picture converted (slotW x slotH blocks) */
    std::vector<float> tmp;
    int n = 0;
    for (;;) {
        {
            std::unique_lock<std::mutex> l(m);
            cv.wait(l, [&] {
                return quit || resetTo >= 0 || ((int)ready.size() < kAhead && (n < frames || loop));
            });
            if (quit)
                return;
            if (resetTo >= 0) {
                n = resetTo;
                resetTo = -1;
                ready.clear();
                if (ok)
                    r.seek(mp4_frame_time(n));
                haveCur = haveNxt = ended = false;
                convertedT = -1;
            }
            if (n >= frames) {              /* (loop) the start again */
                n = 0;
                if (ok)
                    r.seek(0);
                haveCur = haveNxt = ended = false;
                convertedT = -1;
            }
            produce = n;
        }
        Frame f;
        f.index = n;
        f.px.resize(frameBytes);
        for (int half = 0; half < 2; half++) {
            /* the picture shown at this half's time (frame n's, then half a
             * frame later): the last at or before it (0.5 ms of rounding) */
            const int64_t t = mp4_frame_time(2 * (int64_t)n + half) / 2 + 5000;
            while (ok && !ended) {
                if (!haveNxt) {
                    int64_t ft;
                    const uint8_t *px;
                    if (!r.next(&ft, &px)) {
                        ended = true;
                        break;
                    }
                    nxt.assign(px, px + (size_t)r.width() * r.height() * 4);
                    nxtT = ft;
                    haveNxt = true;
                    if (layout.srcW != r.width() || layout.srcH != r.height())
                        make_layout(layout, r.width(), r.height(), slotW, slotH);
                }
                if (!haveCur || nxtT <= t) {    /* (the first picture shows from frame 0) */
                    cur.swap(nxt);
                    curT = nxtT;
                    haveCur = true;
                    haveNxt = false;
                    if (curT <= t)
                        continue;
                }
                break;
            }
            /* the picture converted once (a slower movie shows it again) */
            if (!haveCur) {
                last.resize(pictureBytes);
                fill_black(last.data(), slotW, slotH);
                convertedT = -1;
            } else if (curT != convertedT || last.size() != pictureBytes) {
                last.resize(pictureBytes);
                convert(cur.data(), layout, last.data(), slotW, slotH, tmp);
                convertedT = curT;
            }
            place(last.data(), f.px.data(), slotW, slotH, half);
        }
        {
            std::lock_guard<std::mutex> l(m);
            if (resetTo < 0)
                ready.push_back(std::move(f));
            n++;
            produce = n;
        }
        cv.notify_all();
    }
}

} // namespace gt3media
