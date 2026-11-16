/* media_services.cpp - the G002 player's decoding and the IPU stills,
 * native (media_services.h). */
#include "engine/media/native/media_services.h"

#include "engine/media/native/ipu_csc.h"
#include "engine/media/native/ipu_image.h"
#include "engine/media/native/mp4_frames.h"
#include "engine/media/native/mp4_gpu.h"
#include "engine/media/native/mp4_token.h"
#include "engine/media/native/mpeg2_video.h"
#include "engine/pgl/native/pgl_bridge.h"
#include "platform/log.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <vector>

using namespace gt3media;

/* ======================================================================== */
/* The decoder of the G002 player                                           */
/* ======================================================================== */

static_assert(GT3M_MOVIE_TBP == PGLR_MOVIE_TBP && GT3M_MOVIE_SIDE == 1 << PGLR_MOVIE_TEXEL_LOG2,
              "media_services.h names the renderer's movie texture");

/* PC: an MP4 movie (mp4_token.h): the frames its tokens name, from its
 * worker thread: the GPU's (mp4_gpu.h), else the CPU's (mp4_frames.h). */
struct Gt3mMp4 {
    std::unique_ptr<Mp4GpuFrames> gpu;
    std::unique_ptr<Mp4FrameWorker> worker;
    struct Want {
        int index, frames;
    };
    std::deque<Want> pending;       /* the tokens' frames not output yet */
    std::vector<uint8_t> frame;     /* the frame next() returned (the CPU's) */
    Mp4GpuFrame gpuFrame = {};      /* ... (the GPU's) */
    bool loop = false;
    int sequenceEnds = 0;           /* the last frames output (a one-shot movie's end) */
    int64_t frames = 0;
};

struct Gt3mVideo {
    Mpeg2VideoDecoder dec;
    VideoPicture pic;               /* the picture next() returned */
    bool havePic = false;
    int64_t loggedErrors = 0;
    int slotW = 640, slotH = 448;   /* PC: the stream's size (an MP4's frames are scaled to it) */
    Gt3mMp4 *mp4 = nullptr;         /* PC: an MP4 movie's tokens came */
    bool mp4Pic = false;            /* PC: next() returned an MP4 frame */
    bool ended = false;
};

/* PC: the last frame output was an MP4's (progressive; MenuBgMovie::upload). */
static std::atomic<int> g_progressive{0};
/* PC: the frames a second of the stream the last frame came from (an MP4's
 * frames, two pictures 1 / 59.94 s apart: 29.97; MenuBgMovie::update). */
static std::atomic<double> g_frameRate{0.0};
/* PC: the shape of the last frame's pictures (an MP4's; 0: a PSS). */
static std::atomic<double> g_aspect{0.0};

Gt3mVideo *gt3m_video_create(void)
{
    return new Gt3mVideo;
}

void gt3m_video_set_size(Gt3mVideo *v, int width, int height)
{
    if (width >= 16 && height >= 16) {
        v->slotW = width & ~15;
        v->slotH = height & ~15;
    }
}

int gt3m_movie_progressive(void)
{
    return g_progressive.load(std::memory_order_relaxed);
}

double gt3m_movie_frame_rate(void)
{
    return g_frameRate.load(std::memory_order_relaxed);
}

double gt3m_movie_aspect(void)
{
    return g_aspect.load(std::memory_order_relaxed);
}

static const Mp4GpuFrame *gpu_frame(const void *frame)
{
    if (frame == nullptr || std::memcmp(frame, kMp4GpuMagic, sizeof(kMp4GpuMagic)) != 0)
        return nullptr;
    return (const Mp4GpuFrame *)frame;
}

int gt3m_movie_gpu_show(const void *frame, int half)
{
    const Mp4GpuFrame *f = gpu_frame(frame);
    if (f == nullptr)
        return 0;
    pglr_movie_show(f->slot[half], f->seq[half]);
    return 1;
}

void gt3m_movie_gpu_taken(const void *frame)
{
    const Mp4GpuFrame *f = gpu_frame(frame);
    if (f != nullptr)
        mp4_gpu_keep_from(f->seq[0]);
}

void gt3m_video_destroy(Gt3mVideo *v)
{
    if (v && v->mp4 != nullptr) {
        pclog::write("movie: %lld MP4 frames out", (long long)v->mp4->frames);
        delete v->mp4;
    } else if (v) {
        const VideoStats &s = v->dec.stats();
        pclog::write("movie: %lld pictures decoded (%lld I, %lld P, %lld B), %lld errors, %lld concealed macroblocks",
                     (long long)(s.pictures[1] + s.pictures[2] + s.pictures[3]), (long long)s.pictures[1],
                     (long long)s.pictures[2], (long long)s.pictures[3], (long long)s.errors,
                     (long long)s.concealedMbs);
    }
    delete v;
}

/* PC: an MP4 movie's token (mp4_token.h): its frame wanted; the worker
 * started with the first. */
static void mp4_token(Gt3mVideo *v, const Mp4Token &t)
{
    if (v->mp4 == nullptr)
        v->mp4 = new Gt3mMp4;
    Gt3mMp4 &m = *v->mp4;
    char path[sizeof(t.path)];
    std::memcpy(path, t.path, sizeof(path));
    path[sizeof(path) - 1] = '\0';
    const bool same = m.gpu ? m.gpu->path() == path : m.worker && m.worker->path() == path;
    if (!same) {
        m.loop = (t.flags & MP4_TOKEN_LOOP) != 0;
        m.gpu.reset();
        m.worker.reset();
        /* the GPU's way, else the CPU's (GT3PC_MOVIE_CPU=1: always, a check) */
        if (std::getenv("GT3PC_MOVIE_CPU") == nullptr)
            m.gpu = Mp4GpuFrames::open(path, (int)t.frames, m.loop);
        if (!m.gpu) {
            m.worker.reset(new Mp4FrameWorker(path, v->slotW, v->slotH, (int)t.frames, m.loop));
            m.frame.assign((size_t)v->slotW * v->slotH * 2 * 4, 0);
        }
    }
    m.pending.push_back({(int)t.index, (int)t.frames});
}

void gt3m_video_push(Gt3mVideo *v, const void *data, int size, long long pts)
{
    if (size <= 0)
        return;
    if (is_mp4_token(data, size)) {
        mp4_token(v, *(const Mp4Token *)data);
        return;
    }
    v->dec.push((const uint8_t *)data, (size_t)size, pts);
}

/* PC: what an MP4 frame's token counts against the player's input budget
 * (the ring less two elements, 480 KB at 640 x 448): about a second of
 * frames ahead, as a PSS's ring held some 0.4 s.  Tokens count for little
 * else: the feeder handed out a whole movie at once and reached its end
 * marker, after which mpegif::control refuses a stop (g_dec_state no
 * longer DEC_RUNNING) until the decoder has finished - a skipped movie
 * played to its end (user report). */
static const int kMp4TokenWeight = 16 << 10;

int gt3m_video_buffered(const Gt3mVideo *v)
{
    if (v->mp4 != nullptr)
        return (int)v->mp4->pending.size() * kMp4TokenWeight;
    return (int)v->dec.buffered();
}

void gt3m_video_end_of_stream(Gt3mVideo *v)
{
    v->ended = true;
    v->dec.setEndOfStream();
}

int gt3m_video_next(Gt3mVideo *v, int *width, int *height)
{
    if (v->mp4 != nullptr) {
        /* PC: an MP4's frame: from its worker, -1 while it is not ready */
        Gt3mMp4 &m = *v->mp4;
        v->mp4Pic = false;
        if (m.pending.empty())
            return 0;
        const Gt3mMp4::Want w = m.pending.front();
        if (m.gpu ? !m.gpu->take(w.index, &m.gpuFrame) : !m.worker->take(w.index, m.frame.data()))
            return -1;
        m.pending.pop_front();
        if (w.index == w.frames - 1)
            m.sequenceEnds++;               /* (the last frame: a one-shot movie ends there) */
        m.frames++;
        v->mp4Pic = true;
        g_progressive.store(1, std::memory_order_relaxed);
        g_frameRate.store(30000.0 / 1001.0, std::memory_order_relaxed);
        g_aspect.store(m.gpu ? m.gpu->aspect() : m.worker->aspect(), std::memory_order_relaxed);
        *width = v->slotW;
        *height = v->slotH * 2;
        return 1;
    }
    v->havePic = v->dec.next(v->pic);
    if (v->havePic) {
        g_progressive.store(0, std::memory_order_relaxed);
        g_frameRate.store(v->dec.sequence().frameRate, std::memory_order_relaxed);
        g_aspect.store(0.0, std::memory_order_relaxed);
    }
    const VideoStats &s = v->dec.stats();
    if (s.errors + s.unsupported > v->loggedErrors) {
        v->loggedErrors = s.errors + s.unsupported;
        if (v->loggedErrors <= 10)
            pclog::write("movie: stream error (%s)", s.lastError);
    }
    if (!v->havePic)
        return 0;
    *width = v->pic.codedWidth;
    *height = v->pic.codedHeight;
    return 1;
}

/* The CSC of the PS2 player (mpeg_player_on_picture -> ipu_csc_rgb32):
 * the frame store's macroblocks in order, i.e. columns of 16 x 16 blocks,
 * each converted to 0x400 bytes of RGB32 with alpha 0x80. */
void gt3m_video_output_blocks(Gt3mVideo *v, void *dst, int dstSize)
{
    if (v->mp4Pic) {
        /* PC: an MP4's frame is in the layout already, or the GPU's two
         * pictures' numbers in place of its pixels */
        if (v->mp4->gpu) {
            std::memcpy(dst, &v->mp4->gpuFrame, std::min((size_t)dstSize, sizeof(Mp4GpuFrame)));
            return;
        }
        const size_t n = std::min((size_t)dstSize, v->mp4->frame.size());
        std::memcpy(dst, v->mp4->frame.data(), n);
        return;
    }
    if (!v->havePic)
        return;
    const VideoPicture &p = v->pic;
    const int bw = p.codedWidth / 16, bh = p.codedHeight / 16;
    const int maxBlocks = dstSize / 0x400;
    uint8_t *out = (uint8_t *)dst;
    int n = 0;
    for (int bx = 0; bx < bw; bx++) {
        for (int by = 0; by < bh; by++, n++) {
            if (n >= maxBlocks)
                return;
            ipu_csc_rgba(p.y + (size_t)by * 16 * p.yStride + bx * 16, p.yStride,
                         p.cb + (size_t)by * 8 * p.cStride + bx * 8,
                         p.cr + (size_t)by * 8 * p.cStride + bx * 8, p.cStride,
                         16, 16, out + (size_t)n * 0x400, 64, kIpuAlpha);
        }
    }
}

int gt3m_video_sequence_ends(const Gt3mVideo *v)
{
    if (v->mp4 != nullptr)
        return v->mp4->sequenceEnds;
    return (int)v->dec.stats().sequenceEnds;
}

int gt3m_video_errors(const Gt3mVideo *v)
{
    const VideoStats &s = v->dec.stats();
    return (int)(s.errors + s.unsupported);
}

/* ======================================================================== */
/* IPU stills                                                               */
/* ======================================================================== */

int gt3m_ipu_decode_image(void *dst, const void *img)
{
    IpuImageStats st;
    if (ipu_image_decode(img, (size_t)-1, (uint8_t *)dst, 0, IpuLayout::Macroblocks, kIpuAlpha, &st))
        return 0;
    pclog::write("ipu: picture not decoded (%s, %d macroblocks)", st.error, st.macroblocks);
    return -1;
}

int gt3m_ipu_decode_image_n(void *dst, unsigned dstSize, const void *img)
{
    IpuImageStats st;
    if (ipu_image_decode(img, (size_t)-1, (uint8_t *)dst, 0, IpuLayout::Macroblocks, kIpuAlpha, &st, dstSize))
        return 0;
    pclog::write("ipu: picture not decoded (%s, %d macroblocks)", st.error, st.macroblocks);
    return -1;
}
