/* pgl_host_image.cpp - PC (vector fonts, pgl_vfont.h; docs/renderer.md
 * "Host images"): pictures the game makes on the CPU at native resolution,
 * each standing for a GS texture no VRAM holds (pglr_host_image_add).  A draw
 * whose TEX0 is an image's samples it, bilinear: texture_bind asks
 * host_image_bind first, for every TEX0 of TBW 0 and PSMCT32.
 *
 * The game changes an image between pglr_host_image_lock / _unlock; the
 * executor takes what changed under the same lock when a draw binds it - the
 * whole image into a new texture after a new size or generation, else its
 * dirty rows (queued to the D3D thread, the rows copied along).  The entries
 * sit in a fixed array, published by a count: added on the game thread,
 * read on the executor's, never moved or released. */
#include "engine/pgl/native/pgl_d3d.h"

#include <algorithm>
#include <atomic>
#include <mutex>

#include "platform/log.h"

namespace pglr {

namespace {

constexpr int kHostMax = 4096;              /* (TBP0's 14 bits would allow 16384) */
/* TBP0 .. TFX: an image's name in a TEX0.  Above them are the CLUT's fields,
 * nothing to a PSMCT32 texture: the vector fonts write TEX0 whole, but a draw
 * through gfx_set_texture_buffer (the stills, game/app/pc_still.h) keeps what
 * the last texture left there. */
constexpr uint64_t kHostTex0Bits = (1ull << 37) - 1;
/* dirty rows of more bytes than this make a new texture (a device call, not
 * a copy through the D3D thread's ring) */
constexpr size_t kHostRowsMax = 4u << 20;

struct HostEntry {
    PglrHostImage *img = nullptr;
    uint64_t tex0 = 0;
    std::mutex lock;
    Com<ID3D11Texture2D> tex;
    Com<ID3D11ShaderResourceView> srv;
    int texW = 0, texH = 0;
    uint32_t generation = 0;
};

HostEntry g_hosts[kHostMax];
std::atomic<int> g_hostCount{0};
std::atomic<float> g_textScale{1.0f}, g_textScaleX{1.0f};

/* (the executor, under the entry's lock) the texture of the image's pixels */
bool host_make_texture(HostEntry &e, const PglrHostImage &im)
{
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = (UINT)im.w;
    d.Height = (UINT)im.h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = im.pixels;
    init.SysMemPitch = (UINT)im.w * 4;
    e.srv.reset();
    e.tex.reset();
    e.texW = e.texH = 0;
    if (FAILED(dev()->CreateTexture2D(&d, &init, e.tex.put())) ||
        FAILED(dev()->CreateShaderResourceView(e.tex.get(), nullptr, e.srv.put()))) {
        e.srv.reset();
        e.tex.reset();
        log_once("pgl-host-image-fail", "pgl: a host image's texture (%dx%d) could not be made", im.w, im.h);
        return false;
    }
    e.texW = im.w;
    e.texH = im.h;
    return true;
}

}  /* namespace */

void host_text_scale(float sx, float sy)
{
    g_textScaleX.store(sx, std::memory_order_relaxed);
    g_textScale.store(sy, std::memory_order_relaxed);
}

bool host_image_bind(uint64_t tex0Key, TexBinding &out)
{
    const uint32_t idx = (uint32_t)(tex0Key & 0x3FFF);
    if (idx == PGLR_MOVIE_TBP)              /* (never an image: kHostMax) */
        return movie_bind(out);
    if (idx >= (uint32_t)g_hostCount.load(std::memory_order_acquire))
        return false;
    HostEntry &e = g_hosts[idx];
    if (e.tex0 != (tex0Key & kHostTex0Bits))
        return false;
    std::lock_guard<std::mutex> hold(e.lock);
    PglrHostImage &im = *e.img;
    if (im.w <= 0 || im.h <= 0 || im.pixels == nullptr || im.gsW <= 0 || im.gsH <= 0)
        return false;                       /* (nothing in it yet: not drawn) */
    const size_t rowsBytes = im.dirtyY1 > im.dirtyY0 ? (size_t)(im.dirtyY1 - im.dirtyY0) * (size_t)im.w * 4 : 0;
    if (!e.tex || e.texW != im.w || e.texH != im.h || e.generation != im.generation || rowsBytes > kHostRowsMax) {
        if (!host_make_texture(e, im))
            return false;
        e.generation = im.generation;
        im.dirtyY0 = im.dirtyY1 = 0;
    } else if (rowsBytes != 0) {
        const int y0 = std::max(0, im.dirtyY0), y1 = std::min(im.h, im.dirtyY1);
        if (y1 > y0) {
            const D3D11_BOX box = {0, (UINT)y0, 0, (UINT)im.w, (UINT)y1, 1};
            const UINT rowBytes = (UINT)im.w * 4;
            ID3D11Texture2D *tex = e.tex.get();
            q::run_with(im.pixels + (size_t)y0 * im.w, (size_t)(y1 - y0) * rowBytes,
                        [tex, box, rowBytes](const uint8_t *p) { qdc()->UpdateSubresource(tex, 0, &box, p, rowBytes, 0); });
        }
        im.dirtyY0 = im.dirtyY1 = 0;
    }
    out.srv = e.srv.get();
    out.x0 = 0;
    out.y0 = 0;
    out.w = (float)im.gsW;
    out.h = (float)im.gsH;
    out.alphaMode = 0;
    out.target = nullptr;
    out.depthTex = false;
    out.mean = 0;
    out.hasMean = false;
    out.linear = true;                      /* (more pixels than texels: never nearest) */
    out.src = nullptr;
    out.fromFront = false;
    return true;
}

}  /* namespace pglr */

using namespace pglr;

extern "C" uint64_t pglr_host_image_add(PglrHostImage *img, uint32_t tw, uint32_t th)
{
    const int n = g_hostCount.load(std::memory_order_relaxed);
    if (img == nullptr || n >= kHostMax) {
        log_once("pgl-host-image-full", "pgl: no room for more host images (%d)", kHostMax);
        return 0;
    }
    /* TBP0 = the entry, TBW 0, PSMCT32, TW / TH, TCC 1 (RGBA), TFX MODULATE */
    const uint64_t tex0 = (uint64_t)n | ((uint64_t)(tw & 0xF) << 26) | ((uint64_t)(th & 0xF) << 30) | (1ull << 34);
    HostEntry &e = g_hosts[n];
    e.img = img;
    e.tex0 = tex0;
    img->id = n;
    g_hostCount.store(n + 1, std::memory_order_release);
    return tex0;
}

extern "C" void pglr_host_image_lock(PglrHostImage *img)
{
    if (img != nullptr && img->id >= 0 && img->id < kHostMax)
        g_hosts[img->id].lock.lock();
}

extern "C" void pglr_host_image_unlock(PglrHostImage *img)
{
    if (img != nullptr && img->id >= 0 && img->id < kHostMax)
        g_hosts[img->id].lock.unlock();
}

extern "C" float pglr_text_scale(void)
{
    return g_textScale.load(std::memory_order_relaxed);
}

extern "C" float pglr_text_scale_x(void)
{
    return g_textScaleX.load(std::memory_order_relaxed);
}
