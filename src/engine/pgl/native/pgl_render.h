/* pgl_render.h - the native renderer of pgl (Direct3D 11), internal.
 *
 * pgl (the port's src/engine/pgl/*.cpp) records what its PS2 packets meant
 * through pgl_bridge.h; this renderer keeps the records in a command list,
 * executes the list when the frame is kicked and presents the displayed
 * frame buffer.  Modules:
 *   pgl_render.cpp    the bridge: recording, frame lists, sub-lists, the
 *                     immediate transfers, presentation entry points
 *   pgl_exec.cpp      execution: the GS register state, the GIF decoder,
 *                     primitive assembly, draw state -> D3D11, batching,
 *                     the VU1 draws, the effect recipes
 *   pgl_targets.cpp   native render targets by VRAM address, presentation,
 *                     readbacks
 *   pgl_textures.cpp  the VRAM address registry and the texture cache
 *                     (gstex decoding)
 *   pgl_d3d.cpp       D3D11 state objects, shaders, dynamic buffers
 *   pgl_models.cpp    decoded VifModel meshes (gt3vif) and the vertex data
 *                     of the VU1 draws
 * docs/renderer.md describes the design.  Native code: STL, D3D11 (no
 * prelude); runs on game threads (one at a time), every D3D11 block under
 * fpu::DefaultFpuMode. */
#ifndef PGL_NATIVE_PGL_RENDER_H
#define PGL_NATIVE_PGL_RENDER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <d3d11.h>
/* winuser.h's DrawState (DrawStateA / W) would rename pgl::gs::DrawState */
#ifdef DrawState
#undef DrawState
#endif

#include "engine/pgl/native/pgl_bridge.h"
#include "engine/pgl/native/pgl_queue.h"
#include "engine/pgl/native/pgl_state_gs.h"
#include "engine/pgl/native/pgl_state_prim.h"
#include "engine/pgl/native/gs_tex_format.h"
#include "engine/pgl/native/vif_model.h"

namespace pglr {

/* ======================================================================== */
/* COM helper                                                               */
/* ======================================================================== */

template <class T> class Com {
public:
    Com() = default;
    ~Com() { reset(); }
    Com(const Com &o) : p_(o.p_) { if (p_) p_->AddRef(); }
    Com &operator=(const Com &o) { if (this != &o) { reset(); p_ = o.p_; if (p_) p_->AddRef(); } return *this; }
    Com(Com &&o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    Com &operator=(Com &&o) noexcept { if (this != &o) { reset(); p_ = o.p_; o.p_ = nullptr; } return *this; }
    /* (PC: released on the D3D thread after the calls queued before, which
     * may use it: q::release_queued) */
    void reset() { if (p_) { if (!q::release_queued(p_)) p_->Release(); p_ = nullptr; } }
    T *get() const { return p_; }
    T **put() { reset(); return &p_; }
    T *operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

private:
    T *p_ = nullptr;
};

/* ======================================================================== */
/* Commands                                                                 */
/* ======================================================================== */

enum CmdType : uint8_t {
    CMD_REGS,           /* pgl's register flush */
    CMD_AD,             /* one A+D write */
    CMD_TARGET,         /* a render target's size */
    CMD_PRIM,           /* GS vertices */
    CMD_GIF,            /* a GIF block in the packet buffer */
    CMD_STRIP,          /* VU1 primitive program */
    CMD_TRI16,          /* tri_draw16 */
    CMD_MODEL,          /* a VU1 model part */
    CMD_UPLOAD,         /* an image upload */
    CMD_TEX1_UPLOAD,    /* a Tex1 set upload */
    CMD_READBACK,       /* a download */
    CMD_SUBCALL,        /* a recorded sub-list */
    CMD_PFX_COLOR,      /* the colour correction recipe */
    CMD_PFX_SPLIT,      /* the plane split recipe */
    CMD_FLARE_QUERY,    /* a lens-flare occlusion query (PglrFlareQuery) */
    CMD_HALF_BLEND,     /* the begin / end of draws at half strength (pglr_half_blend) */
    CMD_DESIGN,         /* the 2D design squeeze of the draws that follow (pglr_set_design_scale) */
    CMD_LINE_WIDTH,     /* the width of the lines that follow (pglr_set_line_width) */
    CMD_POST_AA,        /* a race view's 3D picture done: FXAA over it when chosen (pglr_post_aa) */
    CMD_GPU_SECTION,    /* GPU timing: the draws after it are this section's (pglr_gpu_section) */
    CMD_TEX_SOURCE      /* PC: the texture selected is a VRAM-less Tex1 set's (pglr_texture_source) */
};

struct CmdRegs   { uint64_t mask; uint64_t reg[13]; uint64_t gen[10]; };
struct CmdAd     { uint32_t addr; uint32_t pad; uint64_t value; };
struct CmdTarget { uint32_t base, fbw, psm; int32_t w, h; };
struct CmdPrim   { uint32_t prim, flags; int32_t n; /* GsVertex[n] follow */ };
struct CmdGif    { const void *data; uint32_t qwc, flags; };
struct CmdStrip  {
    PglrVu1 vu; uint32_t prim; const float *xyz; const float *st; const uint32_t *rgba;
    uint32_t constRgba; int32_t n; int32_t nStrips; uint32_t flags; uint8_t lengths[64];
};
struct CmdTri16  { PglrVu1 vu; uint32_t prim; const int16_t *xyzw; const int16_t *st; const uint32_t *rgba; int32_t nTris; uint32_t flags; };
struct CmdModel  { PglrVu1 vu; PglrModelPart part; uint32_t flags; };
struct CmdUpload { uint32_t dbp, dbw, dpsm; int32_t dx, dy, w, h; const void *data; };
/* vramBase: the block the set was linked for when the upload was recorded
 * (tex1_upload_packet baked its BITBLTBUF headers into the packet then; only
 * the pixels are DMA references, read when the list runs). */
struct CmdTex1   { const void *img; uint32_t vramBase; };
/* flags: READBACK_FLARE, the flare result strip's (pglr_readback_flare) */
struct CmdReadback {
    uint32_t sbp, sbw, spsm; int32_t sx, sy, w, h; void *dst;
    PglrReadbackFunc cb; uint32_t madr, arg;
    uint32_t flags;
};
enum : uint32_t { READBACK_FLARE = 1u };
struct CmdSubcall { const void *key; };
struct CmdPfxColor { uint32_t src, dst, fbw; int32_t w, h; uint32_t colR, colG, colB; uint8_t curve[768]; };
struct CmdPfxSplit { uint32_t src, r, g, b; int32_t w, h; };
struct CmdHalfBlend { int32_t begin; uint32_t base, fbw, psm; int32_t w, h; };
struct CmdDesign { float kx, ky; };
struct CmdLineWidth { float px; };
struct CmdPostAa { uint32_t base; int32_t w, h; };
struct CmdSection { int32_t id; };
struct CmdTexSource { const void *img; uint32_t vramBase; uint64_t tex0; };

/* A command list: commands in an arena, in program order. */
class CmdList {
public:
    void clear() { data_.clear(); offs_.clear(); }
    bool empty() const { return offs_.empty(); }
    size_t size() const { return offs_.size(); }
    /* Appends a command of type t with `bytes' of payload; returns it. */
    void *add(CmdType t, size_t bytes);
    CmdType type(size_t i) const { return (CmdType)data_[offs_[i]]; }
    const void *payload(size_t i) const { return &data_[offs_[i] + 16]; }

private:
    std::vector<uint8_t> data_;
    std::vector<uint32_t> offs_;
};

/* ======================================================================== */
/* Render targets                                                           */
/* ======================================================================== */

struct Target {
    uint32_t base = 0;          /* 64-word blocks */
    bool     depth = false;     /* a Z buffer (else colour) */
    uint32_t fbw = 0;           /* buffer width, 64-pixel units */
    uint32_t psm = 0;           /* the format it was created for (CT32 / CT24 / CT16 / Z*) */
    int      gsW = 0, gsH = 0;  /* GS size */
    int      nw = 0, nh = 0;    /* native size */
    float    sx = 1, sy = 1;    /* native pixels per GS pixel */
    bool     gsRes = false;     /* kept at GS resolution (target_gs_resolution) */
    int      samples = 1;       /* MSAA samples per pixel (docs/renderer.md "Anti-aliasing") */
    Com<ID3D11Texture2D> tex;
    Com<ID3D11RenderTargetView> rtv;
    Com<ID3D11DepthStencilView> dsv;
    /* its texture view (single-sampled targets; a multisampled one is read
     * through its resolve: target_srv) */
    Com<ID3D11ShaderResourceView> srv;
    Com<ID3D11ShaderResourceView> srvMs;        /* a multisampled target's (its resolve passes) */
    uint32_t objId = 0;         /* registry object (0: not registered yet) */
    uint64_t gen = 1;           /* bumped by draws into it */
    /* the copy that draws reading the target sample (graphics.md 7.2); of a
     * multisampled target, its resolve */
    Com<ID3D11Texture2D> copy;
    Com<ID3D11ShaderResourceView> copySrv;
    Com<ID3D11DepthStencilView> copyDsv;        /* a multisampled depth target's resolve, written so */
    Com<ID3D11RenderTargetView> copyRtv;        /* a multisampled colour target's (a resolve of a part) */
    uint64_t copyGen = 0;
    /* PC: where it was written since the copy was brought up to date (native
     * pixels, [l, r) x [t, b); empty: r <= l), valid while rectGen == gen
     * (target_written_rect; any other gen++ makes the next refresh whole).
     * A colour copy is refreshed there only: the in-car heat-haze / rain
     * drops read the target they draw into, a TEXFLUSH and a fresh copy
     * before each, which copied (or with MSAA resolved) the whole screen a
     * few hundred times a frame (user report: replays slow). */
    int dirtyL = 0, dirtyT = 0, dirtyR = 0, dirtyB = 0;
    uint64_t rectGen = 0;
    uint64_t lastUse = 0;
};

/* ======================================================================== */
/* Interfaces between the modules                                           */
/* ======================================================================== */

/* pgl_targets.cpp */
void targets_init();
float output_scale();
/* Before a frame executes: its picture's aspect (0: the 4:3 picture, else a
 * race frame drawn at the window's aspect; pglr_set_picture_aspect), and
 * whether it is drawn over the screens' canvas (pglr_set_picture_multi). */
void targets_frame_aspect(float aspect, bool multi);
/* A draw buffer's GS size (CMD_TARGET, gfx_set_draw_buffer / _target_b);
 * fbwPixels: the buffer width in pixels. */
void target_size(uint32_t base, uint32_t fbwPixels, uint32_t psm, int w, int h);
/* The colour target at base for a draw: the size known for base, else the
 * fallback (the scissor's extent).  Created when missing. */
Target *color_target(uint32_t base, uint32_t fbw, uint32_t psm, int fallbackW, int fallbackH);
/* The depth target at zbase for draws into `color' (D3D11 wants equal
 * sizes: one native resource per (zbase, native size), whatever the GS sizes
 * of the colour targets). */
Target *depth_target(uint32_t zbase, uint32_t zpsm, const Target &color);
/* A draw into `color' wrote z (depth_target's): z's texels now stand for
 * color's GS pixels (its GS size and FBW), which a later Z texture read
 * (the depth copies) maps them by. */
void depth_target_written(Target &z, const Target &color);
/* The depth target at base drawn as a frame (FRAME with a Z format, the
 * depth copies of graphics.md 8.3), in that frame's GS geometry. */
Target *depth_frame_target(uint32_t base, uint32_t fbw, uint32_t psm, int fallbackW, int fallbackH);
/* GT3PC_DEPTH_LOG=1: true while depth-target lines are to be logged (the
 * first 600; each call counts one). */
bool depth_log();
/* GT3PC_FLARE_LOG=1: true while readback / flare glow lines are to be
 * logged (the first 600; `line' counts one). */
bool flare_log(bool line);
/* The most recently used target at base (nullptr if none). */
Target *find_color_target(uint32_t base);
Target *find_depth_target(uint32_t base);
/* The copy of a target the draws that read it sample (made when stale):
 * refreshed where written since (DATE, C = Ad, a draw reading its own
 * target), or all of it (whole: a read of the whole picture). */
ID3D11ShaderResourceView *target_copy_srv(Target &t, bool whole = false);
/* A draw wrote [l, r) x [top, b) of t (native pixels, clamped here): gen
 * bumped, the part kept for the copy's refresh (Target::dirty*). */
void target_written_rect(Target &t, int l, int top, int r, int b);
/* The copies' refreshes this frame, whole and partial (GT3PC_PGL_STATS;
 * pgl_exec zeroes them per frame). */
extern uint32_t g_copiesWhole, g_copiesPart;
/* What a draw samples a target through: its own view, or for a
 * multisampled one its resolve (made when the target changed since). */
ID3D11ShaderResourceView *target_srv(Target &t);
/* The single-sampled texture of what the target holds (copies, readbacks):
 * its own, or its resolve. */
ID3D11Texture2D *target_tex1(Target &t);
/* Before a frame executes: the anti-aliasing setting (settings.h) applied -
 * a new MSAA sample count re-creates every target at the output scale with
 * it (colour kept, depth cleared). */
void targets_frame_settings();
/* The colour target at base of GS size w x h, if there is one. */
Target *find_color_target_sized(uint32_t base, int w, int h);
/* A colour target at GS resolution from now on (its pixels resolved into
 * it: linear or point samples at the GS pixel centres; `why' for the log);
 * false if it already is (or is a depth target). */
bool target_gs_resolution(Target &t, bool linear, const char *why);
void present_target(uint32_t base, uint32_t psm, uint32_t fbw, int dx, int dy, int dw, int dh);
void present_black();
void present_last();
void present_again();           /* what is shown, as it is (pglr_present_again) */
void readback(const CmdReadback &r);
/* The readbacks' wait for the GPU in the frame's execution, ms (the frame
 * monitor's GPU WAIT: pglr_kick). */
extern double g_readbackWait;

/* PC: the execution's CPU profile (the frame monitor's verbose view,
 * platform/frame_monitor.h; pgl_exec.cpp): the time of each kind of work
 * without what it calls that is profiled too (a scope's own time), in the
 * frames of that view (g_profOn, set as a frame begins), and counts. */
enum ProfCat : uint8_t {
    PROF_OTHER,         /* the command loop, sub-lists, what no scope holds */
    PROF_GS,            /* GS register writes, vertices, GIF blocks: the GS stream into batches */
    PROF_MODEL,         /* VU1 model parts (exec_model) */
    PROF_STRIP,         /* VU1 strips / tri16: their vertices decoded on the CPU */
    PROF_UPLOAD,        /* image and Tex1 uploads into the registry */
    PROF_EFFECT,        /* effect recipes: colour correction, plane split, flare queries, half blend, FXAA */
    PROF_READBACK,      /* downloads */
    PROF_STATE,         /* a draw state from the GS registers (compute_state) */
    PROF_TEXTURE,       /* a texture's lookup (texture_bind) */
    PROF_DECODE,        /* a texture decoded (a cache miss) */
    PROF_TARGETS,       /* target copies and resolves (target_copy_srv, target_srv) */
    PROF_SUBMIT,        /* the D3D bindings and draw calls (draw_passes) */
    PROF_STREAM,        /* vertices / indices into the dynamic buffers */
    PROF_MESH,          /* a model's mesh found / checked (model_mesh) */
    PROF_MESH_DECODE,   /* a model decoded (a new or changed model, a preXyz part) */
    PROF_FLUSH,         /* a GS batch into its draw (flush_batch: the bounds, the vertex stage) */
    PROF_CLUT,          /* a CLUT load (registry_clut_load: TEX0 / TEX2 with CLD) */
    PROF_TARGET_SET,    /* a frame's targets sized (CMD_TARGET) */
    PROF_CB,            /* shader constants written (bind_vs_cb, bind_ps_cb) */
    PROF_COUNT
};
enum ProfCount : uint8_t {
    PCOUNT_DRAWS, PCOUNT_STATES, PCOUNT_TEXBINDS, PCOUNT_DECODES, PCOUNT_PARTS, PCOUNT_GSCMDS, PCOUNT_UPLOADS,
    PCOUNT_COPIES, PCOUNT_GSVERTS, PCOUNT_BATCHES, PCOUNT_CLUTS, PCOUNT_CLUTMISS, PCOUNT_COMPOSES, PCOUNT_CBS,
    PCOUNT_SYNCS, PCOUNT_CLUTSLOW, PCOUNT_TEXSLOW, PCOUNT_REGIONS, PCOUNT_N
};
extern bool g_profOn;
extern uint32_t g_profCount[PCOUNT_N];
void prof_push(ProfCat c);
void prof_pop();
struct ProfScope {
    bool on;
    /* (the executor's thread only: the D3D thread's calls are not profiled) */
    explicit ProfScope(ProfCat c) : on(g_profOn && !q::on_d3d_thread()) { if (on) prof_push(c); }
    ~ProfScope() { if (on) prof_pop(); }
    ProfScope(const ProfScope &) = delete;
    ProfScope &operator=(const ProfScope &) = delete;
};
uint64_t frame_number();
void next_frame();
/* PC: the execution's clock in seconds, set as each frame begins executing
 * (exec_begin_frame): when the caches' entries were last used (their
 * eviction by age and memory, not by a count of frames - at 235 frames a
 * second 300 frames were a second and a half, and every lap decoded the
 * course's models and textures again where they came back into view). */
extern double g_execSeconds;
/* GT3PC_PGL_TRACE: the picture of frame `frame' (just traced) written at its
 * present (trace_frame_N.bmp). */
void trace_dump_next(uint64_t frame);
/* The game's FPU mode while the renderer executes (callbacks into game
 * code run in it). */
void set_game_fpu_mode(unsigned int mode);
unsigned int game_fpu_mode();

/* pgl_textures.cpp */
struct TexBinding {
    ID3D11ShaderResourceView *srv = nullptr;
    float x0 = 0, y0 = 0, w = 1, h = 1;     /* the GS texels the native texture holds */
    int   alphaMode = 0;                    /* 0 the stored alpha, 1 TA0 (CT24), 2 CT16 bit 7 */
    Target *target = nullptr;               /* a render target sampled (else an upload) */
    bool  depthTex = false;                 /* a depth target's SRV */
    /* (GT3PC_PGL_TRACE) the mean of the decoded texels, R | G << 8 | B << 16
     * | A << 24 (GS alpha); hasMean false: not decoded here (a target, a
     * surface, a plane) */
    uint32_t mean = 0;
    bool  hasMean = false;
    bool  linear = false;                   /* sampled bilinear whatever TEX1 says (a high-res image) */
};
void registry_init();
/* PC (docs/renderer.md "High-resolution images"): a readback of src's w x h
 * GS pixels at (x, y) gave `pixels' (PSMCT32): their native pixels are kept,
 * by the content, and an upload of the same pixels later is sampled from
 * them (the car icons: drawn at the output scale, read back to 64 x 32,
 * uploaded again by the race display and the result board). */
void registry_hires_readback(Target &src, int x, int y, int w, int h, const void *pixels);
void registry_upload(uint32_t dbp, uint32_t dbw, uint32_t dpsm, int dx, int dy, int w, int h,
                     const void *data, size_t bytes);
/* A Tex1 set's uploads at the VRAM base vramBase (the set's base when the
 * upload was recorded: registry_tex1_base then). */
void registry_tex1_upload(const void *img, uint32_t vramBase);
uint32_t registry_tex1_base(const void *img);
/* A set (re)loaded at img (the game thread, the executor idle): a new load
 * stamp; its textures are decoded ahead (pgl_prewarm.cpp). */
void registry_tex1_loaded(void *img);
/* PC (ROADMAP 18; CMD_TEX_SOURCE): the draws with TEX0 tex0 sample the set
 * at img, linked for vramBase but never uploaded (it found no VRAM), and the
 * CLUT loads of that TEX0's CBP read it; img nullptr: none (every frame
 * starts so). */
void registry_texture_source(const void *img, uint32_t vramBase, uint64_t tex0);
/* A draw into t writes its blocks (the RGB / Z layer, the top-byte layer). */
void registry_target_written(Target &t, bool rgb, bool top);
void registry_target_released(Target &t);
/* The colour target whose pixels the GS memory at `block' holds (the owner
 * of its RGB layer) and where that block lies in it (GS pixels); nullptr if
 * the block's owner is not a colour target. */
Target *registry_colour_target_at(uint32_t block, int &ox, int &oy);
/* pfx_split_rgb_planes (graphics.md 8.6): the R, G, B bytes of src's GS
 * pixels 0..w-1 x 0..h-1 (a capture of its native pixels) become the top
 * bytes - the PSMT8H / T4HL / T4HH texels - of the w x h PSMCT32 planes at
 * blocks[0], [1], [2]. */
void registry_split_planes(Target &src, const uint32_t blocks[3], int w, int h);
void registry_clut_load(uint64_t tex0);      /* a TEX0 / TEX2 write with CLD != 0 */
/* Resolves the texture of TEX0 / TEX1 / CLAMP / TEXA; false: nothing to
 * sample (the caller skips the draw).  writeMask: the colour channels the
 * draw writes (pgl::gs::WRITE_*; none of R, G, B: a PSMCT32 texture is read
 * for its alpha, from the owner of its top bytes).  drawing: the colour
 * target of the draw (sampling it gives its copy). */
bool texture_bind(uint64_t tex0, uint64_t tex1, uint64_t clamp, uint64_t texa, uint8_t writeMask,
                  Target *drawing, TexBinding &out);
void registry_end_frame();

/* pgl_models.cpp */
/* The decoded mesh of a part's model (cached) and its buffers; preXyz parts
 * are decoded for the draw.  The part's batches lie in [firstBatch,
 * endBatch) of mesh->batches (their part still to be checked).  false:
 * nothing to draw. */
bool model_mesh(const PglrModelPart &part, const gt3vif::Mesh **mesh, ID3D11Buffer **vb,
                ID3D11Buffer **ib, uint32_t *baseVertex, uint32_t *baseIndex, uint32_t *firstBatch,
                uint32_t *endBatch);
/* pglr_model_written (any thread): the model is decoded again at its next
 * use. */
void models_written(const void *model);
/* pglr_model_loaded (the game thread): as models_written, and the model is
 * decoded ahead (pgl_prewarm.cpp). */
void models_loaded(const void *model);
void models_end_frame();

/* pgl_prewarm.cpp: PC (docs/renderer.md 7, "Decoding ahead") - what the game
 * loads, decoded on a worker thread before its first draw.  A job holds a
 * copy of what it decodes (never the game's memory) and is run, then
 * deleted, on the worker. */
namespace pre {
struct Job {
    virtual ~Job() = default;
    virtual void run() = 0;
};
void start();                           /* (pglr_init; GT3PC_PREWARM=0: no worker) */
bool on();
void post(Job *job);                    /* (taken over) */
/* The log's counts: made on the worker (n of them, bytes decoded), taken at
 * a draw, decoded at a draw after all (the executor's thread). */
enum Kind { MODEL, TEXTURE };
void made(Kind k, uint32_t n, size_t bytes);
void taken(Kind k);
void late(Kind k);
/* QPC seconds, as g_execSeconds counts them (any thread). */
double seconds();
}  /* namespace pre */

/* pgl_flicker.cpp: GT3PC_FLICKER (docs/renderer.md "Debug aids") - the last
 * pictures presented and the draws of their frames kept; F8 writes the
 * pictures that differ from both neighbours (a flicker of one frame) and
 * what their frames drew differently. */
struct FlickerDraw {
    uint8_t  kind;              /* 0 GS primitives, 1 VU1, 2 a skipped GS batch, 3 a skipped VU1 draw */
    uint8_t  why;               /* skipped: 1 nothing passes, 2 no target, 3 no texture, 4 scissor, 5 other */
    uint16_t prim;              /* PRIM */
    uint32_t count;             /* vertices / indices */
    uint32_t rt;                /* FRAME's base block (~0: none) */
    uint32_t tex;               /* TEX0: TBP0 | PSM << 16 | 1 << 31 (0: untextured) */
    uint32_t texMean;           /* the decoded texels' mean (0: not decoded there) */
    uint32_t state;             /* a hash of the blend, the tests, the masks, the scissor */
    uint32_t data;              /* a hash of the vertices (GS) or of the VU1 matrices and range */
    int16_t  x0, y0, x1, y1;    /* the GS pixels of rt it can touch */
    uint32_t srv;               /* the texture view it samples (its address's low bits; 0: none) */
    uint32_t ps;                /* a hash of its pixel shader constants (CbPs) */
    uint32_t rgba;              /* GS primitives: the first vertex's colour */
    float    u, v;              /* ... its texel coordinates */
    uint32_t bound;             /* the D3D state the binding cache had wrong at its draw (d3d_check_bound bits) */
    uint32_t rtv;               /* the render target view drawn into (its address's low bits) */
    /* what the GPU did with it (a pipeline statistics query, read frames
     * later; ~0: not read): vertices read, primitives rasterized, pixel
     * shader invocations */
    uint32_t gpuVerts, gpuPrims, gpuPixels;
    /* what the GPU held for it that differs from what was written (read with
     * the counts; d3d_verify_resolve bits: 1 VS constants, 2 PS constants,
     * 4 vertices, 8 indices) */
    uint32_t gpuBad;
};
bool flicker_on();
void flicker_draw(const FlickerDraw &d);
/* A pipeline statistics query begun for the draw to be kept next; ended
 * after its passes by flicker_query_end, which also copies what the GPU
 * holds for the draw (d3d_verify_copy).  nullptr: none. */
ID3D11Query *flicker_query_begin();
void flicker_query_end(ID3D11Query *q, bool indexed, uint32_t first, uint32_t count, int32_t baseVertex);
/* Something of note in the frame executing, before the next draw (a
 * stream restarted, a texture decoded, a target made, a binding wrong). */
void flicker_event(const char *fmt, ...);
void flicker_end_frame(uint64_t frame);             /* the frame executed: its draws kept */
/* present_now: the picture shown (uv: its part of the target, gsW x gsH GS
 * pixels; nullptr: black), the displayed buffer's base. */
void flicker_capture(ID3D11ShaderResourceView *picture, const float uv[4], int gsW, int gsH, uint32_t disp);
void flicker_poll();                                /* present_now: F8 */

/* pgl_exec.cpp */
void exec_init();
void exec_list(const CmdList &list);
void exec_begin_frame();
void exec_end_frame();

/* Sub-lists (pgl_render.cpp) */
const CmdList *sub_list(const void *key);

/* Logging */
void log_once(const char *key, const char *fmt, ...);

}  /* namespace pglr */

#endif /* PGL_NATIVE_PGL_RENDER_H */
