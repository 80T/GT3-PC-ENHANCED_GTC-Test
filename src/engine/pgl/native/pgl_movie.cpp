/* pgl_movie.cpp - PC: a movie decoded by the GPU, drawn at its own size
 * (docs/movies.md "MP4 movies", docs/renderer.md "Movies").
 *
 * The movie's pictures are in textures shared with its own device
 * (media/native/mp4_gpu.*).  A draw whose TEX0 has TBP0 PGLR_MOVIE_TBP, TBW 0
 * and PSMCT32 samples the one shown: host_image_bind hands it here.  The
 * shown texture is the renderer's: the picture wanted is copied into it on the
 * D3D thread, in order with the draws, so a slot is free again as soon as
 * that is done and the draws never hold one.
 *
 * The shared textures are opened and let go on the executor's thread, as
 * everything Com::reset releases must be; a slot's other side (the movie's
 * device) can go at any time: a shared resource lives as long as anyone holds
 * it. */
#include "engine/pgl/native/pgl_d3d.h"
#include "engine/pgl/native/pgl_queue.h"

#include <dxgi.h>

#include <atomic>
#include <mutex>

#include "platform/log.h"

namespace pglr {

namespace {

constexpr DWORD kCopyWaitMs = 20;           /* (a slot being written: the picture before stays a frame) */

/* What the movie's thread hands over. */
std::mutex g_lock;
void *g_handles[PGLR_MOVIE_SLOTS];
int g_count, g_w, g_h;
std::atomic<uint32_t> g_generation{0};

std::atomic<uint64_t> g_wanted{0};          /* seq << 8 | slot (0: none) */
std::atomic<uint32_t> g_stamp[PGLR_MOVIE_SLOTS];
std::atomic<int> g_busy[PGLR_MOVIE_SLOTS];

/* The executor's. */
uint32_t g_openGeneration;
Com<ID3D11Texture2D> g_slot[PGLR_MOVIE_SLOTS];
Com<IDXGIKeyedMutex> g_mutex[PGLR_MOVIE_SLOTS];
Com<ID3D11Texture2D> g_shown;
Com<ID3D11ShaderResourceView> g_shownSrv;
uint64_t g_shownWanted;

void movie_close()
{
    for (int i = 0; i < PGLR_MOVIE_SLOTS; i++) {
        g_mutex[i].reset();
        g_slot[i].reset();
    }
    g_shownSrv.reset();
    g_shown.reset();
    g_shownWanted = 0;
}

}  /* namespace */

void movie_frame()
{
    const uint32_t generation = g_generation.load(std::memory_order_acquire);
    if (generation == g_openGeneration)
        return;
    std::lock_guard<std::mutex> hold(g_lock);
    g_openGeneration = g_generation.load(std::memory_order_relaxed);
    movie_close();
    for (int i = 0; i < g_count; i++) {
        if (FAILED(dev()->OpenSharedResource((HANDLE)g_handles[i], IID_PPV_ARGS(g_slot[i].put()))) ||
            FAILED(g_slot[i]->QueryInterface(IID_PPV_ARGS(g_mutex[i].put())))) {
            log_once("pgl-movie-open", "pgl: a movie's shared texture does not open here");
            movie_close();
            return;
        }
    }
    if (g_count <= 0)
        return;
    D3D11_TEXTURE2D_DESC d;
    g_slot[0]->GetDesc(&d);
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = 0;
    if (FAILED(dev()->CreateTexture2D(&d, nullptr, g_shown.put())) ||
        FAILED(dev()->CreateShaderResourceView(g_shown.get(), nullptr, g_shownSrv.put()))) {
        log_once("pgl-movie-shown", "pgl: no texture for a %dx%d movie", g_w, g_h);
        movie_close();
    }
}

bool movie_bind(TexBinding &out)
{
    movie_frame();
    if (!g_shownSrv)
        return false;
    const uint64_t wanted = g_wanted.load(std::memory_order_acquire);
    if (wanted != 0 && wanted != g_shownWanted) {
        const int slot = (int)(wanted & 0xFF);
        const uint32_t seq = (uint32_t)(wanted >> 8);
        if (slot < PGLR_MOVIE_SLOTS && g_slot[slot]) {
            g_busy[slot].fetch_add(1, std::memory_order_acq_rel);
            ID3D11Texture2D *dst = g_shown.get(), *src = g_slot[slot].get();
            IDXGIKeyedMutex *mutex = g_mutex[slot].get();
            q::run([slot, seq, dst, src, mutex] {
                /* (the slot taken for a later picture meanwhile: not this one) */
                if (g_stamp[slot].load(std::memory_order_acquire) == seq && mutex->AcquireSync(0, kCopyWaitMs) == S_OK) {
                    qdc()->CopyResource(dst, src);
                    mutex->ReleaseSync(0);
                }
                g_busy[slot].fetch_sub(1, std::memory_order_acq_rel);
            });
        }
        g_shownWanted = wanted;
    }
    out.srv = g_shownSrv.get();
    out.x0 = 0;
    out.y0 = 0;
    out.w = (float)(1 << PGLR_MOVIE_TEXEL_LOG2);    /* (the whole TEX0 space) */
    out.h = (float)(1 << PGLR_MOVIE_TEXEL_LOG2);
    out.alphaMode = 0;
    out.target = nullptr;
    out.depthTex = false;
    out.mean = 0;
    out.hasMean = false;
    out.linear = true;
    out.src = nullptr;
    out.fromFront = false;
    return true;
}

}  /* namespace pglr */

using namespace pglr;

extern "C" void pglr_movie_attach(void *const *handles, int count, int w, int h)
{
    std::lock_guard<std::mutex> hold(g_lock);
    g_count = count;
    g_w = w;
    g_h = h;
    for (int i = 0; i < count && i < PGLR_MOVIE_SLOTS; i++)
        g_handles[i] = handles[i];
    g_wanted.store(0, std::memory_order_release);
    for (auto &s : g_stamp)
        s.store(0, std::memory_order_relaxed);
    g_generation.fetch_add(1, std::memory_order_release);
}

extern "C" void pglr_movie_stamp(int slot, uint32_t seq)
{
    if (slot >= 0 && slot < PGLR_MOVIE_SLOTS)
        g_stamp[slot].store(seq, std::memory_order_release);
}

extern "C" int pglr_movie_busy(int slot)
{
    return slot >= 0 && slot < PGLR_MOVIE_SLOTS ? g_busy[slot].load(std::memory_order_acquire) != 0 : 0;
}

extern "C" void pglr_movie_show(int slot, uint32_t seq)
{
    if (slot >= 0 && slot < PGLR_MOVIE_SLOTS && seq != 0)
        g_wanted.store(((uint64_t)seq << 8) | (uint32_t)slot, std::memory_order_release);
}
