/* pgl_exec.cpp - executes a frame's command list on Direct3D 11
 * (docs/renderer.md "Execution").
 *
 * The executor keeps the GS register state the recorded commands describe -
 * pgl's register flushes, A+D writes, the GIF blocks the game wrote (decoded
 * here as data: PACKED / REGLIST / IMAGE tags), the PRIM of the VU1 draws -
 * and turns primitives into native draws:
 *   - GS-space primitives (pgl's 2D paths, GIF blocks): the GS vertex queue
 *     of the PRIM type (points, lines, strips, fans, sprites; XYZ2 kicks,
 *     XYZ3 does not), vertices converted to native pixels of the target,
 *     batched while the state stays the same;
 *   - VU1 work (strips, tri_draw16, model parts): the vertex shader does what
 *     the VU1 programs did (docs/models.md 5).
 * The state of a draw is pgl::gs::translate of the shadows of PRIM.CTXT's
 * context: blend, depth, alpha-test passes, write masks, scissor, fog; the
 * frame / Z buffers resolve to native targets (pgl_targets.cpp), TEX0 to a
 * native texture (pgl_textures.cpp).
 *
 * Coordinates (docs/renderer.md "Pixels"): a GS window position w (pixels,
 * (X - OFX) / 16) is native w x s + 0.5 (s = native / GS size), so GS pixel
 * x covers native pixels [x s, (x + 1) s) and integer-edged sprites fill
 * whole blocks; a sprite's texture coordinates are shifted by slope x (0.5 /
 * s - 0.5) so that texel centres stay where they are at 1:1 (exact at s = 1,
 * 1:1 copies of native-size targets stay pixel exact at any s).  2D drawn
 * for the 4:3 screen and squeezed by k into the GS frame (the race's dynamic
 * aspect: CMD_DESIGN) is 1:1 in design pixels: the shift is slope x (0.5 / s
 * - 0.5 k).  A multisampled target (MSAA) maps w to native w s, without the
 * 0.5: there the 0.5 put integer edges on the pixel centres, half of the
 * edge pixels' samples outside (the left / top column and row of a
 * full-screen copy blended with what was there, again at every copy: a smear
 * along the picture's left and top edges, user report); the sprites' shift
 * is then slope x (- 0.5 k) (pix / s - 0.5 k in general), which keeps 1:1
 * copies exact as well. */
#include "engine/pgl/native/pgl_d3d.h"

#include <algorithm>
#include <intrin.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "engine/pgl/native/gs_tex_format.h"
#include "platform/frame_monitor.h"
#include "platform/input_map.h"
#include "platform/log.h"
#include "platform/settings.h"
#include "platform/window.h"

namespace pglr {

void flush_batch();

namespace {

namespace gs = pgl::gs;
namespace pr = pgl::prim;

/* GS register addresses */
enum : uint32_t {
    A_PRIM = 0x00, A_RGBAQ = 0x01, A_ST = 0x02, A_UV = 0x03, A_XYZF2 = 0x04, A_XYZ2 = 0x05,
    A_FOG = 0x0A, A_XYZF3 = 0x0C, A_XYZ3 = 0x0D, A_PRMODE = 0x1B,
    A_BITBLTBUF = 0x50, A_TRXPOS = 0x51, A_TRXREG = 0x52, A_TRXDIR = 0x53
};

/* GsContext.reg[] / gen[] order -> GS addresses (context 2: + 1) */
const uint8_t kCtxAddr[gs::REG_COUNT] = {0x4C, 0x18, 0x4A, 0x4E, 0x40, 0x42, 0x47, 0x08, 0x16, 0x14, 0x06,
                                          0x34, 0x36};
const uint8_t kGenAddr[gs::GEN_COUNT] = {0x46, 0x44, 0x45, 0x3D, 0x49, 0x1A, 0x22, 0x3B, 0x1C, 0x3F};

int8_t g_addrReg[0x80];     /* address -> reg[] index (-1) */
int8_t g_addrCtx[0x80];     /* address -> context 0 / 1 */
int8_t g_addrGen[0x80];     /* address -> gen[] index (-1) */

/* A vertex as the GS registers held it at its XYZ write. */
struct Vtx {
    uint16_t x = 0, y = 0;
    uint32_t z = 0;
    uint8_t f = 0;
    uint32_t rgba = 0;
    float s = 0, t = 0, q = 1;
    uint16_t u = 0, v = 0;
};

struct GsState {
    uint64_t ctx[2][gs::REG_COUNT] = {};
    uint64_t gen[gs::GEN_COUNT] = {};
    uint32_t prim = 0;
    uint64_t prmode = 0;
    /* vertex attribute registers */
    uint32_t rgba = 0x80808080u;
    float q = 1.0f, s = 0.0f, t = 0.0f;
    uint16_t u = 0, v = 0;
    uint8_t fog = 0;
    /* the vertex queue */
    Vtx queue[2];
    Vtx fan0;
    int count = 0;
    /* transfers */
    uint64_t bitbltbuf = 0, trxpos = 0, trxreg = 0;
};
GsState g;

struct Xfer {
    bool active = false;
    uint32_t dbp = 0, dbw = 0, dpsm = 0;
    int dx = 0, dy = 0, w = 0, h = 0;
    uint32_t bytes = 0;
    std::vector<uint8_t> data;
};
Xfer g_xfer;

/* The state of the current batch. */
struct BatchState {
    bool valid = false;
    bool skip = false;              /* nothing drawn (no passes, no target, no texture) */
    gs::DrawState ds;
    uint32_t type = 0;              /* PRIM type */
    Target *rt = nullptr;           /* colour target (the depth target for depthFrame) */
    Target *zt = nullptr;           /* depth buffer (nullptr: no depth access) */
    bool depthFrame = false;        /* FRAME with a Z format: a depth copy */
    bool tme = false, fst = false, flat = false;
    TexBinding tex;
    ID3D11SamplerState *samp = nullptr;
    float texW = 1, texH = 1;
    uint16_t ofx = 0, ofy = 0;
    float sx = 1, sy = 1;
    float pix = 0.5f;                   /* native = GS x s + pix (see the file comment) */
    uint8_t zpsm = 0x31;
    D3D11_RECT scissor = {};
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    CbPs ps = {};
    ID3D11ShaderResourceView *dateSrv = nullptr;    /* DATE: the copy of rt the destination alpha is read from */
    uint64_t tex0 = 0;                  /* (GT3PC_PGL_TRACE) */
    /* PC (CPU time): the D3D states of this state's passes (draw_passes:
     * [pass][rep], the rasterizer by cull), found once instead of at each
     * draw; nullptr not yet */
    ID3D11BlendState *blend[2][2] = {};
    ID3D11DepthStencilState *depth[2][2] = {};
    ID3D11RasterizerState *raster[4] = {};
};
BatchState g_bs;
bool g_dirty = true;
/* The 2D design squeeze of the draws (CMD_DESIGN, pglr_set_design_scale): a
 * GS pixel of them is (g_designX, g_designY) design pixels of the 4:3 screen
 * the game drew them for. */
float g_designX = 1.0f, g_designY = 1.0f;
/* The width of the GS lines in native pixels (CMD_LINE_WIDTH,
 * pglr_set_line_width): above 1 they are drawn as quads (emit_line). */
float g_lineWidth = 1.0f;
std::vector<GsVtx> g_verts;
int g_subDepth = 0;

/* PC: a TEXFLUSH while the batch samples the target it draws into (it reads
 * the target's copy).  The primitives after it must see what was drawn
 * before it: once, the batch ended there and the next primitive took a fresh
 * copy - each heat-haze / rain drop (particles.c: a TEXFLUSH per drop)
 * drew alone after a refresh of the copy, a few hundred GPU write-then-read
 * dependencies a frame (user report: replays slow).  Now the area written
 * since the copy (the target's dirty part and the batch's vertices) is kept
 * (g_fbRect); a primitive that reads none of it joins the batch (the copy
 * holds what it reads), one that reads some ends it and takes the fresh copy
 * first (feedback_check).  Cleared by a new state (its copy is fresh). */
bool g_fbPending = false;
/* (the parts written since the copy, each as drawn: the target's part from
 * before the batch, then the batch's vertices between two TEXFLUSHes, one
 * rectangle each - one bounding box of them all, the drops spread over the
 * screen, took a third of the drops for overlapping: user report, the
 * third) */
std::vector<D3D11_RECT> g_fbRects;
size_t g_segStart;                      /* g_verts' first vertex after the last TEXFLUSH */

/* The run of vertices since the last TEXFLUSH into g_fbRects (their
 * bounding box, native pixels, 2 more around). */
void fb_segment_end()
{
    if (g_verts.size() > g_segStart) {
        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        for (size_t i = g_segStart; i < g_verts.size(); i++) {
            x0 = std::min(x0, g_verts[i].x);
            y0 = std::min(y0, g_verts[i].y);
            x1 = std::max(x1, g_verts[i].x);
            y1 = std::max(y1, g_verts[i].y);
        }
        g_fbRects.push_back({(LONG)std::floor(x0) - 2, (LONG)std::floor(y0) - 2, (LONG)std::ceil(x1) + 2,
                             (LONG)std::ceil(y1) + 2});
    }
    g_segStart = g_verts.size();
}

/* GPU time by section (below, exec_begin_frame); sec_active: this frame is
 * timed */
bool sec_on();
bool sec_active();
void sec_mark(int id);

/* DATE (TEST.DATE: a pixel is written only where bit 7 of the destination
 * alpha equals DATM).  The pixel shader reads the destination alpha from a
 * copy of the target (D3D11 cannot read the bound target), taken when a DATE
 * draw state is set up, unless the draws into the target since the last copy
 * were all DATE draws: a run of DATE draws (the env-map passes of a model,
 * one per VU1 part) reads the alpha as it was before the run.  Inexact only
 * where a DATE draw that writes alpha overlaps itself or an earlier draw of
 * its run. */
struct DateRun {
    Target *rt = nullptr;               /* the target of the last DATE draw */
    uint64_t gen = 0;                   /* its gen after that draw */
} g_dateRun;

/* Debug aid: GT3PC_PGL_TRACE=1 - F9 in the game window logs the draws of the
 * next frame executed (`pgl trace` lines: target, write mask, PRIM, blend,
 * tests, texture, VU1 lighting program) and the draw states it skipped, with
 * the reason. */
int g_traceOn = -1;
bool g_tracing = false;
int  g_traceFrames = 0;                 /* frames of the F9 trace still to come (two: see flare_trace_depth) */
bool g_traceNext = false;               /* the next frame is traced too */
bool g_probeFromFlares = false;         /* the probe points are flare queries' (not GT3PC_PGL_PROBE's) */
uint32_t g_traceN = 0;
int g_traceProg = -1;                   /* the VU1 lighting program of the draw (-1: GS primitives) */
/* the VU1 constants of the draw (nullptr: GS primitives) and the mean colour
 * of its vertices (lighting modes with a colour stream only) */
const CbVu1 *g_traceVu1 = nullptr;
bool g_traceHasVcol = false;
float g_traceVcol[4] = {};

/* Debug aid: GT3PC_PGL_SKIP=<kinds> (comma separated) leaves out the draws of
 * those kinds, to find which pass makes a wrong picture: local, envmtx,
 * envmap, lights (DIFFUSE / SPEC2 / SPEC4), const - VU1 model parts by
 * lighting program; ad - draws blended with C = Ad; alpha - draws that write
 * only the alpha; self - draws that sample their own target (the in-car
 * heat-haze / rain drops, blurs). */
struct SkipSet {
    int state = -1;                     /* -1 not read, 0 off, 1 on */
    bool local = false, envmtx = false, envmap = false, lights = false, cnst = false;
    bool ad = false, alpha = false, self = false;
} g_skip;

bool skip_on()
{
    if (g_skip.state < 0) {
        g_skip.state = 0;
        const char *e = getenv("GT3PC_PGL_SKIP");
        if (e != nullptr && e[0] != 0) {
            std::string s = std::string(",") + e + ",";
            auto has = [&](const char *k) { return s.find(std::string(",") + k + ",") != std::string::npos; };
            g_skip.local = has("local");
            g_skip.envmtx = has("envmtx");
            g_skip.envmap = has("envmap");
            g_skip.lights = has("lights");
            g_skip.cnst = has("const");
            g_skip.ad = has("ad");
            g_skip.alpha = has("alpha");
            g_skip.self = has("self");
            g_skip.state = 1;
            pclog::write("pgl: GT3PC_PGL_SKIP=%s: those draws are left out (a debug aid)", e);
        }
    }
    return g_skip.state == 1;
}

/* Debug aid: GT3PC_PGL_PROBE=x,y[;x,y ...] (up to four GS pixels of the draw
 * buffer, e.g. of the 640 x 448 race screen) - in the frame F9 traces
 * (GT3PC_PGL_TRACE), each point's pixel is read back after every draw into a
 * colour target that holds it and logged when it changed (`pgl probe` lines
 * with the number of the trace line of the draw): the passes that make the
 * colour of a wrong pixel, in order, and what each left there.  Each read
 * waits for the GPU (the traced frame only). */
struct ProbeSet {
    int state = -1;                     /* -1 not read, 0 off, 1 on */
    int n = 0;
    int x[4] = {}, y[4] = {};
    Com<ID3D11Texture2D> staging;       /* 1 x 1, the targets' RGBA8 */
    std::unordered_map<uint64_t, uint32_t> last;    /* (target, point) -> the pixel read last */
} g_probe;

bool probe_on()
{
    if (g_probe.state < 0) {
        g_probe.state = 0;
        const char *e = getenv("GT3PC_PGL_PROBE");
        for (const char *s = e; s != nullptr && *s != 0 && g_probe.n < 4;) {
            int x = 0, y = 0, used = 0;
            if (sscanf(s, " %d , %d%n", &x, &y, &used) != 2 || used <= 0)
                break;
            g_probe.x[g_probe.n] = x;
            g_probe.y[g_probe.n] = y;
            g_probe.n++;
            s += used;
            while (*s == ';' || *s == ' ' || *s == '/')
                s++;
        }
        if (g_probe.n > 0) {
            g_probe.state = 1;
            pclog::write("pgl: GT3PC_PGL_PROBE=%s: %d point(s) read back after each draw of a traced frame (F9)", e,
                         g_probe.n);
        }
    }
    return g_probe.state == 1;
}

/* The execution's CPU profile (pgl_render.h ProfScope): a stack of open
 * scopes, each one's own ticks (the TSC) by kind. */
struct ProfOpen {
    uint8_t cat;
    uint64_t start, child;
};
constexpr int kProfDepth = 32;
ProfOpen g_profStack[kProfDepth];
int g_profDepth;
uint64_t g_profTicks[PROF_COUNT];
uint64_t g_profTsc0;                    /* the frame's start (exec_begin_frame): ticks against QPC */
int64_t g_profQpc0;
const char *const kProfNames[PROF_COUNT] = {"OTHER", "GS STREAM", "MODELS", "VU1 STRIPS", "UPLOADS", "EFFECTS",
                                            "READBACK", "STATE", "TEX LOOKUP", "TEX DECODE", "TARGET COPY",
                                            "D3D SUBMIT", "VTX STREAM", "MESH CHECK", "MESH DECODE", "GS FLUSH",
                                            "CLUT LOAD", "TARGET SET", "CONSTANTS"};
const char *const kProfCountNames[PCOUNT_N] = {"DRAWS", "STATES", "TEX BINDS", "DECODES", "PARTS", "GS CMDS",
                                               "UPLOADS", "COPIES", "GS VERTS", "BATCHES", "CLUTS", "CLUT MISS",
                                               "COMPOSES", "CB WRITES", "D3D SYNCS", "CLUT SLOW", "TEX SLOW",
                                               "VRAM GENS"};

/* The kind of a command's own work */
ProfCat prof_cat(uint8_t type)
{
    switch (type) {
    case CMD_REGS: case CMD_AD: case CMD_PRIM: case CMD_GIF: return PROF_GS;
    case CMD_TARGET: return PROF_TARGET_SET;
    case CMD_STRIP: case CMD_TRI16: return PROF_STRIP;
    case CMD_MODEL: return PROF_MODEL;
    case CMD_UPLOAD: case CMD_TEX1_UPLOAD: return PROF_UPLOAD;
    case CMD_READBACK: return PROF_READBACK;
    case CMD_PFX_COLOR: case CMD_PFX_SPLIT: case CMD_FLARE_QUERY: case CMD_HALF_BLEND: case CMD_POST_AA:
        return PROF_EFFECT;
    }
    return PROF_OTHER;
}

/* Debug aid: GT3PC_PGL_STATS=N logs what every N-th frame executed. */
struct Stats {
    uint32_t cmds[32];
    uint32_t draws, verts, skipped, prims;
    uint32_t skipNoPass, skipTarget, skipScissor, skipTex, skipDepthFrame;
};
Stats g_stats;
int g_statsEvery = -1;

/* ======================================================================== */
/* Logging                                                                  */
/* ======================================================================== */

void log_state(const gs::DrawState &ds)
{
    if (ds.date && (ds.frameWriteMask & gs::WRITE_A) != 0)
        log_once("pgl-date-alpha", "pgl: DATE draws that write alpha: the destination alpha is the one before"
                 " their run of DATE draws (their own alpha writes are not seen)");
    if (ds.inexact == 0)
        return;
    static std::vector<uint32_t> seen;
    const uint32_t id = ds.inexact | ((uint32_t)ds.blend.selectors << 16) | ((ds.blend.fix > 0x80 ? 1u : 0u) << 24);
    if (std::find(seen.begin(), seen.end(), id) != seen.end())
        return;
    seen.push_back(id);
    static const char *names[] = {"blend form without a fixed-function equivalent", "blend FIX > 0x80",
                                  "blend C = Ad", "reserved blend selector", "PABE", "COLCLAMP off",
                                  "partial FBMSK bytes", "dithering", "AA1 antialiasing",
                                  "PRMODECONT.AC = 0", "16-bit frame"};
    char key[64];
    snprintf(key, sizeof(key), "pgl-inexact-%04X-%02X-%d", ds.inexact, ds.blend.selectors, ds.blend.fix > 0x80);
    std::string what;
    for (int i = 0; i < 11; i++) {
        if (ds.inexact & (1u << i)) {
            if (!what.empty())
                what += ", ";
            what += names[i];
        }
    }
    log_once(key, "pgl: inexact GS state (%s): ALPHA 0x%02X FIX 0x%02X, FRAME psm 0x%02X FBMSK 0x%08X",
             what.c_str(), ds.blend.selectors, ds.blend.fix, ds.fpsm, ds.fbmsk);
}

/* GT3PC_PGL_TRACE: the texture of TEX0 as text */
void trace_tex(char *out, size_t n, uint64_t tex0v, const TexBinding *bound)
{
    const gs::Tex0 t = gs::decodeTex0(tex0v);
    char from[72] = "";
    if (bound != nullptr && bound->target != nullptr)
        snprintf(from, sizeof(from), " from the target at 0x%X", bound->target->base);
    else if (bound != nullptr && bound->hasMean)
        /* the decoded texels' mean: the palette the draw got (a wrong CLUT
         * slice or copy shows as another colour than the texture's) */
        snprintf(from, sizeof(from), " from an upload / plane, mean %02X %02X %02X a %02X", bound->mean & 0xFF,
                 (bound->mean >> 8) & 0xFF, (bound->mean >> 16) & 0xFF, bound->mean >> 24);
    else if (bound != nullptr)
        snprintf(from, sizeof(from), " from an upload / plane");
    snprintf(out, n, "tex 0x%X tbw %u %s %ux%u tfx %u tcc %u cbp 0x%X csa %u%s", (unsigned)(t.tbp0 & 0x3FFF), t.tbw,
             gstex::psm_name(t.psm), 1u << std::min<uint32_t>(t.tw, 11), 1u << std::min<uint32_t>(t.th, 11), t.tfx,
             t.tcc ? 1u : 0u, (unsigned)t.cbp, t.csa, from);
}

/* GT3PC_PGL_TRACE: a draw state compute_state gave up (frame: FRAME). */
void trace_skip(const char *why, uint64_t frame, uint64_t tex0v, bool tme)
{
    if (!g_tracing)
        return;
    const gs::Frame f = gs::decodeFrame(frame);
    char tex[192] = "untextured";
    if (tme)
        trace_tex(tex, sizeof(tex), tex0v, nullptr);
    pclog::write("pgl trace %u: skipped (%s): FRAME 0x%X psm %02X FBMSK %08X | PRIM %03X | %s", g_traceN++, why,
                 ((uint32_t)f.fbp * 32) & 0x3FFF, f.psm, f.fbmsk, gs::encodePrim(g_bs.ds.prim), tex);
}

/* GT3PC_PGL_TRACE: one draw of the current state (all its passes); area:
 * the native pixels it can touch (null: the scissor's). */
void trace_draw(bool indexed, uint32_t count, const D3D11_RECT *area)
{
    const BatchState &b = g_bs;
    const gs::DrawState &s = b.ds;
    char tex[192] = "untextured";
    if (b.tme)
        trace_tex(tex, sizeof(tex), b.tex0, &b.tex);
    char prog[24] = "";
    if (indexed && g_traceProg >= 0)
        snprintf(prog, sizeof(prog), " program %d", g_traceProg);
    /* what else makes the colour: the VU1 vertices' mean colour (GS units),
     * the LOCAL programs' specular colour (L[4]), FOGCOL and the VU1 fog
     * (F = f + 0.1 + (255 - f) min(start, w) / w) */
    std::string more;
    char part[160];
    /* where: GS primitives their vertices' box in GS pixels of the target
     * and their texel range, VU1 draws the box of their area; the scissor */
    if (!indexed && !g_verts.empty()) {
        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f, u0 = 1e30f, v0 = 1e30f, u1 = -1e30f, v1 = -1e30f;
        for (const GsVtx &v : g_verts) {
            x0 = std::min(x0, v.x);
            y0 = std::min(y0, v.y);
            x1 = std::max(x1, v.x);
            y1 = std::max(y1, v.y);
            u0 = std::min(u0, v.u);
            v0 = std::min(v0, v.v);
            u1 = std::max(u1, v.u);
            v1 = std::max(v1, v.v);
        }
        snprintf(part, sizeof(part), " | GS (%.3f,%.3f)-(%.3f,%.3f)", (x0 - b.pix) / b.sx, (y0 - b.pix) / b.sy,
                 (x1 - b.pix) / b.sx, (y1 - b.pix) / b.sy);
        more += part;
        if (b.tme) {
            snprintf(part, sizeof(part), " uv (%.3f,%.3f)-(%.3f,%.3f)", u0, v0, u1, v1);
            more += part;
        }
    } else if (area != nullptr) {
        snprintf(part, sizeof(part), " | GS about (%.1f,%.1f)-(%.1f,%.1f)", area->left / b.sx, area->top / b.sy,
                 area->right / b.sx, area->bottom / b.sy);
        more += part;
    }
    snprintf(part, sizeof(part), " | scissor GS (%.2f,%.2f)-(%.2f,%.2f) of native %dx%d", b.scissor.left / b.sx,
             b.scissor.top / b.sy, b.scissor.right / b.sx, b.scissor.bottom / b.sy, b.rt->nw, b.rt->nh);
    more += part;
    if (indexed && g_traceHasVcol) {
        snprintf(part, sizeof(part), " | vertex mean %.1f %.1f %.1f a %.1f", g_traceVcol[0], g_traceVcol[1],
                 g_traceVcol[2], g_traceVcol[3]);
        more += part;
    }
    if (indexed && g_traceVu1 != nullptr && g_traceVu1->program == VU1P_LOCAL) {
        snprintf(part, sizeof(part), " | spec %.1f %.1f %.1f", g_traceVu1->L[4][0], g_traceVu1->L[4][1],
                 g_traceVu1->L[4][2]);
        more += part;
    }
    if (s.fog) {
        snprintf(part, sizeof(part), " | fogcol %02X %02X %02X", s.fogColor.r, s.fogColor.g, s.fogColor.b);
        more += part;
        if (indexed && g_traceVu1 != nullptr) {
            snprintf(part, sizeof(part), " f %.1f start %.3f", g_traceVu1->viewCenter[3] - 0.1f,
                     g_traceVu1->limits[3]);
            more += part;
        }
    }
    pclog::write("pgl trace %u: %s %u%s | FRAME 0x%X psm %02X %dx%d FBMSK %08X | ZBUF %s0x%X | PRIM %03X | ALPHA %02X FIX"
                 " %02X%s | ATST %d ref %02X afail %d | DATE %d datm %d | ZTST %d func %d write %d | %d pass(es) | %s%s",
                 g_traceN++, indexed ? "vu1" : "gs", count, prog, b.rt->base, s.fpsm, b.rt->gsW, b.rt->gsH, s.fbmsk,
                 b.zt != nullptr ? "" : "(none) ", b.zt != nullptr ? b.zt->base : 0u, gs::encodePrim(s.prim),
                 s.blend.selectors, s.blend.fix, s.blend.enable ? "" : " (off)", (int)s.alphaFunc, s.aref,
                 (int)s.afail, s.date ? 1 : 0, s.datm ? 1 : 0, s.depthTest ? 1 : 0, (int)s.depthFunc,
                 s.depthWrite ? 1 : 0, s.passCount, tex, more.c_str());
}

/* GT3PC_PGL_PROBE: the probe points' pixels of t after the draw traced last
 * (logged when they changed). */
void probe_draw(Target &t)
{
    if (!g_tracing || !probe_on() || t.depth || !t.tex)
        return;
    if (!g_probe.staging) {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = 1;
        d.Height = 1;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev()->CreateTexture2D(&d, nullptr, g_probe.staging.put()))) {
            log_once("pgl-probe-staging", "pgl: GT3PC_PGL_PROBE: no staging texture: probes off");
            g_probe.state = 0;
            return;
        }
    }
    for (int i = 0; i < g_probe.n; i++) {
        const int x = g_probe.x[i], y = g_probe.y[i];
        if (x < 0 || y < 0 || x >= t.gsW || y >= t.gsH)
            continue;
        /* the native pixel at the GS pixel's centre (GS pixel x covers
         * native [x s, (x + 1) s)) */
        const UINT nx = (UINT)std::max(0, std::min(t.nw - 1, (int)(((float)x + 0.5f) * t.sx)));
        const UINT ny = (UINT)std::max(0, std::min(t.nh - 1, (int)(((float)y + 0.5f) * t.sy)));
        const D3D11_BOX box = {nx, ny, 0, nx + 1, ny + 1, 1};
        dc()->CopySubresourceRegion(g_probe.staging.get(), 0, 0, 0, 0, target_tex1(t), 0, &box);
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (FAILED(dc()->Map(g_probe.staging.get(), 0, D3D11_MAP_READ, 0, &m)))
            continue;
        uint32_t px;
        memcpy(&px, m.pData, 4);
        dc()->Unmap(g_probe.staging.get(), 0);
        const uint64_t key = ((uint64_t)(uintptr_t)&t << 2) | (uint64_t)i;
        auto it = g_probe.last.find(key);
        if (it != g_probe.last.end() && it->second == px)
            continue;
        char was[40] = "first read";
        if (it != g_probe.last.end())
            snprintf(was, sizeof(was), "was %02X %02X %02X a %02X", it->second & 0xFF, (it->second >> 8) & 0xFF,
                     (it->second >> 16) & 0xFF, it->second >> 24);
        pclog::write("pgl probe (%d,%d) of 0x%X %dx%d after trace %u: %02X %02X %02X a %02X (%s)", x, y, t.base,
                     t.gsW, t.gsH, g_traceN - 1, px & 0xFF, (px >> 8) & 0xFF, (px >> 16) & 0xFF, px >> 24, was);
        g_probe.last[key] = px;
    }
}

/* ======================================================================== */
/* State                                                                    */
/* ======================================================================== */

inline int iceil(float v)
{
    return (int)std::ceil(v - 1e-4f);
}

float z_norm(uint8_t zpsm)
{
    switch (zpsm & 0x0F) {
    case 0x0: return 1.0f / 4294967296.0f;
    case 0x1: return 1.0f / 16777216.0f;
    default: return 1.0f / 65536.0f;
    }
}

/* PC (CPU time): gs::translate is a function of FRAME, ZBUF, TEST, ALPHA,
 * FBA, SCISSOR, XYOFFSET, PABE, COLCLAMP, DTHE, PRMODECONT, SCANMSK, FOGCOL
 * and the PRIM bits alone: its results by those (direct-mapped; a frame
 * has a few dozen). */
struct TranslateMemo {
    uint64_t key[14];
    bool valid = false;
    gs::DrawState ds;
};
TranslateMemo g_translateMemo[128];

gs::DrawState translate_memo(const gs::Regs &r, uint32_t primBits)
{
    const uint64_t key[14] = {r.reg[gs::REG_FRAME], r.reg[gs::REG_ZBUF], r.reg[gs::REG_TEST], r.reg[gs::REG_ALPHA],
                              r.reg[gs::REG_FBA], r.reg[gs::REG_SCISSOR], r.reg[gs::REG_XYOFFSET],
                              r.gen[gs::GEN_PABE], r.gen[gs::GEN_COLCLAMP], r.gen[gs::GEN_DTHE],
                              r.gen[gs::GEN_PRMODECONT], r.gen[gs::GEN_SCANMSK], r.gen[gs::GEN_FOGCOL], primBits};
    uint64_t h = 0x9E3779B97F4A7C15ull;
    for (uint64_t k : key)
        h = (h ^ k) * 0xFF51AFD7ED558CCDull;
    TranslateMemo &m = g_translateMemo[(h ^ (h >> 29)) % 128];
    if (!m.valid || memcmp(m.key, key, sizeof(key)) != 0) {
        m.ds = gs::translate(r, primBits);
        memcpy(m.key, key, sizeof(key));
        m.valid = true;
    }
    return m.ds;
}

void compute_state()
{
    ProfScope prof(PROF_STATE);
    g_profCount[PCOUNT_STATES]++;
    BatchState &b = g_bs;
    b = BatchState();
    b.valid = true;
    g_fbPending = false;                /* PC: a texture of the target's copy is fresh (g_fbRects) */
    g_fbRects.clear();
    g_segStart = 0;
    const uint32_t prim = g.prim;
    const int ctxt = (prim >> 9) & 1;
    gs::Regs r;
    for (int i = 0; i < gs::REG_COUNT; i++)
        r.reg[i] = g.ctx[ctxt][i];
    for (int i = 0; i < gs::GEN_COUNT; i++)
        r.gen[i] = g.gen[i];
    uint32_t primBits = prim;
    const bool ac = (g.gen[gs::GEN_PRMODECONT] & 1) != 0;
    if (!ac)
        primBits = (prim & 7) | ((uint32_t)g.prmode & 0x7F8);
    b.ds = translate_memo(r, primBits);
    if (!ac)
        b.ds.inexact &= (uint16_t)~gs::INEXACT_PRMODE;
    log_state(b.ds);
    b.type = prim & 7;
    switch (b.type) {
    case 0: b.topo = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST; break;
    case 1: case 2:
        /* (wide lines: quads, emit_line) */
        b.topo = g_lineWidth > 1.0f ? D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST : D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
        break;
    default: b.topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
    }
    b.tex0 = r.reg[gs::REG_TEX0];
    if (b.ds.passCount == 0) {
        b.skip = true;
        g_stats.skipNoPass++;
        trace_skip("nothing passes the tests", r.reg[gs::REG_FRAME], b.tex0, b.ds.prim.tme);
        return;
    }

    /* frame */
    const gs::Frame f = gs::decodeFrame(r.reg[gs::REG_FRAME]);
    const uint32_t base = ((uint32_t)f.fbp * 32) & 0x3FFF;
    const gs::Scissor &sc = b.ds.scissor;
    const int fw = (int)sc.x1 + 1, fh = (int)sc.y1 + 1;
    if ((f.psm & 0x30) == 0x30) {
        b.depthFrame = true;
        b.rt = depth_frame_target(base, f.fbw, f.psm, fw, fh);
    } else {
        b.rt = color_target(base, f.fbw, f.psm, fw, fh);
    }
    if (b.rt == nullptr) {
        b.skip = true;
        g_stats.skipTarget++;
        trace_skip("no target", r.reg[gs::REG_FRAME], b.tex0, b.ds.prim.tme);
        return;
    }
    /* PC: small effect buffers whose GS pixels are the data go to GS
     * resolution (pgl_targets.cpp target_gs_resolution, docs/renderer.md 5):
     *   - scan-masked rows into one (at most 64 GS pixels across: T449's
     *     wheel-face blur, the only one; the ghost cars mask the race screen):
     *     GS rows the game then averages by a 2:1 shrink;
     *   - one of at most 64 x 64 that reads itself: the flare occlusion
     *     query's work buffer, halved in place by bilinear sprites whose taps
     *     fall midway between two GS pixels (flare_reduce, flare_build_reduce).
     *     At s native pixels per GS pixel they average native pairs, the last
     *     texel is no longer the box's mean, and the copy's first native
     *     column is the pixel left of the box (x0 s is not a whole native
     *     pixel); at GS resolution the copy takes the screen at the GS pixel
     *     centres and the halvings are the GS's;
     *   - one of at most 64 x 64 drawn from a small buffer already at GS
     *     resolution: the result strip (a texel per query, read back). */
    if (!b.depthFrame) {
        const bool smallRt = b.rt->gsW <= 64 && b.rt->gsH <= 64;
        if (b.ds.scanmask >= 2 && b.rt->gsW <= 64) {
            target_gs_resolution(*b.rt, true, "takes scan-masked rows");
        } else if (smallRt && b.ds.prim.tme) {
            const gs::Tex0 t = gs::decodeTex0(b.tex0);
            const uint32_t tbp = t.tbp0 & 0x3FFF;
            int ox = 0, oy = 0;
            Target *src = (t.psm & 0x30) == 0x30 ? nullptr : registry_colour_target_at(tbp, ox, oy);
            if (src == b.rt)
                target_gs_resolution(*b.rt, false, "reads itself (the flare queries' halvings)");
            else if (src != nullptr && src->gsW <= 64 && src->gsH <= 64 && src->nw == src->gsW &&
                     src->nh == src->gsH)
                target_gs_resolution(*b.rt, false, "is drawn from a GS-resolution buffer (the flare results)");
        }
    }
    b.sx = b.rt->sx;
    b.sy = b.rt->sy;
    b.pix = b.rt->samples > 1 ? 0.0f : 0.5f;
    b.zpsm = b.ds.zpsm;

    /* depth buffer */
    if (!b.depthFrame) {
        bool needZ = b.ds.depthTest && b.ds.depthFunc != gs::Compare::Always;
        for (int p = 0; p < b.ds.passCount; p++)
            needZ = needZ || b.ds.pass[p].depthWrite;
        if (needZ) {
            const gs::Zbuf z = gs::decodeZbuf(r.reg[gs::REG_ZBUF]);
            b.zt = depth_target(((uint32_t)z.zbp * 32) & 0x3FFF, 0x30u | z.psm, *b.rt);
            if (b.zt == nullptr) {
                b.skip = true;
                return;
            }
        }
    }

    /* raster */
    b.ofx = b.ds.offset.ofx;
    b.ofy = b.ds.offset.ofy;
    b.scissor.left = std::max(0, std::min(b.rt->nw, iceil(sc.x0 * b.sx)));
    b.scissor.top = std::max(0, std::min(b.rt->nh, iceil(sc.y0 * b.sy)));
    b.scissor.right = std::max(0, std::min(b.rt->nw, iceil((sc.x1 + 1) * b.sx)));
    b.scissor.bottom = std::max(0, std::min(b.rt->nh, iceil((sc.y1 + 1) * b.sy)));
    if (b.scissor.right <= b.scissor.left || b.scissor.bottom <= b.scissor.top || sc.x1 < sc.x0 || sc.y1 < sc.y0) {
        b.skip = true;
        g_stats.skipScissor++;
        return;
    }
    b.flat = !b.ds.prim.iip;

    /* texture */
    b.tme = b.ds.prim.tme;
    b.fst = b.ds.prim.fst;
    uint32_t flags = 0;
    if (b.tme) {
        const uint64_t tex0 = r.reg[gs::REG_TEX0];
        bool bound;
        {
            ProfScope prof(PROF_TEXTURE);
            g_profCount[PCOUNT_TEXBINDS]++;
            bound = texture_bind(tex0, r.reg[gs::REG_TEX1], r.reg[gs::REG_CLAMP], r.gen[gs::GEN_TEXA],
                                 b.ds.frameWriteMask, b.rt, b.tex);
        }
        if (!bound) {
            b.skip = true;
            g_stats.skipTex++;
            trace_skip("no texture", r.reg[gs::REG_FRAME], tex0, true);
            return;
        }
        if (b.tex.target != nullptr && b.tex.target == b.zt)
            b.tex.srv = target_copy_srv(*b.zt);
        if (b.depthFrame != b.tex.depthTex) {
            log_once("pgl-depthframe", "pgl: a depth buffer drawn from a colour texture (or the reverse):"
                     " not native");
            b.skip = true;
            return;
        }
        const gs::Tex0 t = gs::decodeTex0(tex0);
        if (b.tex.depthTex && b.tex.target != nullptr && depth_log())
            pclog::write("pgl depth: frame %llu: Z texture at block 0x%X (%ux%u) read as the %dx%d pixels of the"
                         " Z buffer at block 0x%X (native %dx%d), into block 0x%X (%dx%d)",
                         (unsigned long long)frame_number(), (unsigned)(t.tbp0 & 0x3FFF), 1u << t.tw,
                         1u << t.th, b.tex.target->gsW, b.tex.target->gsH, b.tex.target->base,
                         b.tex.target->nw, b.tex.target->nh, b.rt->base, b.rt->gsW, b.rt->gsH);
        b.texW = (float)(1u << std::min<uint32_t>(t.tw, 11));
        b.texH = (float)(1u << std::min<uint32_t>(t.th, 11));
        const gs::Tex1 t1 = gs::decodeTex1(r.reg[gs::REG_TEX1]);
        const gs::Clamp cl = gs::decodeClamp(r.reg[gs::REG_CLAMP]);
        bool magLin = t1.mmag;
        const bool minLin = t1.mmin == 1 || t1.mmin == 4 || t1.mmin == 5;
        bool linear = magLin;
        if (t1.lcm || b.fst)
            linear = t1.k <= 0 ? magLin : minLin;   /* LOD = K */
        if (b.tex.linear)
            linear = true;                          /* a high-res image (registry_hires_readback) */
        if (cl.wms == 3 || cl.wmt == 3)
            log_once("pgl-region-repeat", "pgl: REGION_REPEAT texture wrap: drawn as REPEAT");
        b.samp = sampler_state(linear, linear, cl.wms == 0 || cl.wms == 3 ? 0 : 1,
                               cl.wmt == 0 || cl.wmt == 3 ? 0 : 1);
        flags |= PSF_TME | ((uint32_t)t.tfx << PSF_TFX_SHIFT) | (t.tcc ? PSF_TCC : 0) |
                 ((uint32_t)b.tex.alphaMode << PSF_AMODE_SHIFT);
        b.ps.texX0 = b.tex.x0;
        b.ps.texY0 = b.tex.y0;
        b.ps.texInvW = 1.0f / b.tex.w;
        b.ps.texInvH = 1.0f / b.tex.h;
    } else if (b.depthFrame) {
        log_once("pgl-depthframe-flat", "pgl: an untextured draw into a depth buffer as a frame: not native");
        b.skip = true;
        return;
    }
    if (b.ds.fog)
        flags |= PSF_FGE;
    if (b.ds.fba)
        flags |= PSF_FBA;
    if (b.ds.blend.enable) {
        flags |= (uint32_t)b.ds.blend.premul << PSF_PREMUL_SHIFT;
        if (b.ds.blend.c == gs::CSource::Fix)
            flags |= PSF_CFIX;
    }
    if (b.ds.blend.ad == gs::AdMode::Shader && !b.depthFrame) {
        /* C = Ad in the shader (pgl_state_gs.h AdMode): Cd and Ad from a
         * copy of the target, taken before each draw (draw_passes) */
        flags |= PSF_BLEND_AD;
        b.ps.adSel = (float)b.ds.blend.adSel;
    }
    if (b.ds.scanmask >= 2)
        flags |= (uint32_t)b.ds.scanmask << PSF_SCAN_SHIFT;
    if (b.ds.date && !b.depthFrame) {
        /* DATE: the destination alpha from a copy of the target (g_dateRun) */
        if (g_dateRun.rt == b.rt && g_dateRun.gen == b.rt->gen && b.rt->copySrv)
            b.dateSrv = b.rt->copySrv.get();
        else
            b.dateSrv = target_copy_srv(*b.rt);
        flags |= PSF_DATE | (b.ds.datm ? PSF_DATM : 0u);
    }
    b.ps.flags = flags;
    b.ps.aref = (float)b.ds.aref;
    b.ps.fix = (float)b.ds.blend.fix;
    b.ps.rowScale = b.sy;
    b.ps.fogR = b.ds.fogColor.r;
    b.ps.fogG = b.ds.fogColor.g;
    b.ps.fogB = b.ds.fogColor.b;
    const gs::Texa ta = gs::decodeTexa(r.gen[gs::GEN_TEXA]);
    b.ps.ta0 = ta.ta0;
    b.ps.ta1 = ta.ta1;
}

void ensure_state()
{
    if (g_dirty || !g_bs.valid) {
        if (!g_verts.empty())
            flush_batch();
        compute_state();
        g_dirty = false;
    }
}

/* ======================================================================== */
/* Drawing                                                                  */
/* ======================================================================== */

/* GT3PC_FLICKER (pgl_flicker.cpp): FNV-1a of n bytes, from h. */
uint32_t fnv(const void *p, size_t n, uint32_t h = 2166136261u)
{
    const uint8_t *s = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++)
        h = (h ^ s[i]) * 16777619u;
    return h;
}

/* GT3PC_FLICKER: the draw of the current state (kind 0 / 1), or the batch it
 * skipped (2 / 3), as a record of the frame; area: the native pixels it can
 * touch (null: the scissor's), data: its vertices' hash.  A skipped one is
 * kept at once (flicker_note), a draw after its passes (draw_passes: the
 * bindings checked at each). */
FlickerDraw flicker_record(int kind, uint32_t count, const D3D11_RECT *area, uint32_t data)
{
    const BatchState &b = g_bs;
    const gs::DrawState &s = b.ds;
    const bool noScissor = b.scissor.right <= b.scissor.left || b.scissor.bottom <= b.scissor.top;
    FlickerDraw d = {};
    d.kind = (uint8_t)kind;
    d.gpuVerts = d.gpuPrims = d.gpuPixels = ~0u;
    d.gpuBad = 0;
    if (kind >= 2)
        d.why = !b.valid ? 5 : s.passCount == 0 ? 1 : b.rt == nullptr ? 2 : noScissor ? 4
              : (s.prim.tme && b.tex.srv == nullptr) ? 3 : 5;
    d.prim = (uint16_t)gs::encodePrim(s.prim);
    d.count = count;
    d.rt = b.rt != nullptr ? b.rt->base : ~0u;
    if (s.prim.tme) {
        const gs::Tex0 t = gs::decodeTex0(b.tex0);
        d.tex = (uint32_t)(t.tbp0 & 0x3FFF) | (uint32_t)t.psm << 16 | 1u << 31;
        d.texMean = (kind < 2 && b.tex.hasMean) ? b.tex.mean : 0;
    }
    const uint32_t st[] = {(uint32_t)s.blend.selectors, (uint32_t)s.blend.fix, (uint32_t)s.blend.enable,
                           (uint32_t)s.alphaFunc, (uint32_t)s.aref, (uint32_t)s.afail, (uint32_t)s.date,
                           (uint32_t)s.datm, (uint32_t)s.depthTest, (uint32_t)s.depthFunc, (uint32_t)s.depthWrite,
                           (uint32_t)s.fbmsk, (uint32_t)s.passCount, (uint32_t)s.fpsm,
                           b.zt != nullptr ? b.zt->base : ~0u, (uint32_t)b.scissor.left, (uint32_t)b.scissor.top,
                           (uint32_t)b.scissor.right, (uint32_t)b.scissor.bottom};
    d.state = fnv(st, sizeof(st));
    d.data = data;
    D3D11_RECT w = b.scissor;
    if (area != nullptr) {
        if (noScissor) {
            w = *area;
        } else {
            w.left = std::max(w.left, area->left);
            w.top = std::max(w.top, area->top);
            w.right = std::min(w.right, area->right);
            w.bottom = std::min(w.bottom, area->bottom);
        }
    }
    if (b.rt != nullptr && b.sx > 0.0f && b.sy > 0.0f) {
        auto gsx = [&](LONG v) { return (int16_t)std::max(-32768.0f, std::min(32767.0f, std::floor(v / b.sx))); };
        auto gsy = [&](LONG v) { return (int16_t)std::max(-32768.0f, std::min(32767.0f, std::floor(v / b.sy))); };
        d.x0 = gsx(w.left);
        d.y0 = gsy(w.top);
        d.x1 = gsx(w.right);
        d.y1 = gsy(w.bottom);
    }
    if (kind < 2) {
        d.srv = b.tme ? (uint32_t)(uintptr_t)b.tex.srv : 0;
        d.ps = fnv(&b.ps, sizeof(b.ps));
        if (kind == 0 && !g_verts.empty()) {
            d.rgba = g_verts[0].rgba;
            d.u = g_verts[0].u;
            d.v = g_verts[0].v;
        }
    }
    return d;
}

void flicker_note(int kind, uint32_t count, const D3D11_RECT *area, uint32_t data)
{
    flicker_draw(flicker_record(kind, count, area, data));
}

void bind_targets()
{
    BatchState &b = g_bs;
    if (b.depthFrame)
        set_targets(nullptr, b.rt->dsv.get(), b.rt->nw, b.rt->nh);
    else
        set_targets(b.rt->rtv.get(), b.zt ? b.zt->dsv.get() : nullptr, b.rt->nw, b.rt->nh);
    set_scissor(b.scissor);
    if (b.tme) {
        set_srv0(b.tex.srv);
        set_sampler0(b.samp);
    }
    if (b.dateSrv != nullptr)
        set_srv1(b.dateSrv);
}

/* The passes of the current state (vertex stage already bound).  area: the
 * native pixels the draw can touch (null: the scissor's), kept for the
 * target copy's refresh (target_written_rect). */
void draw_passes(bool indexed, uint32_t count, uint32_t first, int32_t baseVertex, int cull,
                 const D3D11_RECT *area = nullptr)
{
    ProfScope prof(PROF_SUBMIT);
    BatchState &b = g_bs;
    if (skip_on()) {
        const int prog = indexed ? g_traceProg : -1;
        if ((g_skip.local && prog == VU1P_LOCAL) || (g_skip.envmtx && prog == VU1P_ENVMTX) ||
            (g_skip.envmap && prog == VU1P_ENVMAP) || (g_skip.cnst && prog == VU1P_CONST) ||
            (g_skip.lights && (prog == VU1P_DIFFUSE || prog == VU1P_SPEC2 || prog == VU1P_SPEC4)) ||
            (g_skip.ad && b.ds.blend.ad != gs::AdMode::None) ||
            (g_skip.alpha && !b.depthFrame && (b.ds.frameWriteMask & gs::WRITE_RGBA) == gs::WRITE_A) ||
            (g_skip.self && b.tme && b.tex.target != nullptr && b.tex.target == b.rt))
            return;
    }
    /* C = Ad in the shader reads Cd / Ad as the draws before this one left
     * them: a fresh copy (consecutive model parts share one draw state) */
    if ((b.ps.flags & PSF_BLEND_AD) != 0)
        b.dateSrv = target_copy_srv(*b.rt);
    bind_targets();
    ID3D11RasterizerState *&rs = b.raster[cull & 3];
    if (rs == nullptr)
        rs = raster_state(cull, true);
    set_raster(rs);
    if (g_tracing)
        trace_draw(indexed, count, area);
    FlickerDraw fd = {};
    const bool flicker = flicker_on();
    if (flicker) {
        uint32_t data;
        if (indexed) {
            const uint32_t range[3] = {count, first, (uint32_t)baseVertex};
            data = fnv(range, sizeof(range));
            if (g_traceVu1 != nullptr)          /* (the matrices, the viewport, the limits) */
                data = fnv(g_traceVu1, offsetof(CbVu1, ndcMap), data);
        } else {
            data = fnv(g_verts.data(), g_verts.size() * sizeof(GsVtx));
        }
        fd = flicker_record(indexed ? 1 : 0, count, area, data);
    }
    bool wroteRgb = false, wroteTop = false, wroteZ = false;
    ID3D11Query *gpuQuery = flicker ? flicker_query_begin() : nullptr;
    for (int p = 0; p < b.ds.passCount; p++) {
        const gs::Pass &pass = b.ds.pass[p];
        uint8_t mask = pass.writeMask & b.ds.frameWriteMask;
        bool zw = pass.depthWrite && b.zt != nullptr;
        if (b.depthFrame) {
            mask = 0;
            zw = true;
        } else if (mask == 0 && !zw) {
            continue;
        }
        CbPs c = b.ps;
        if (pass.gate != gs::AlphaGate::None)
            c.flags |= ((uint32_t)pass.gate << PSF_GATE_SHIFT) | ((uint32_t)b.ds.alphaFunc << PSF_ATST_SHIFT);
        bind_ps(b.depthFrame ? Ps::DepthCopy : b.flat ? Ps::MainFlat : Ps::Main);
        bind_ps_cb(&c, sizeof(c));
        /* C = Ad as two DEST_ALPHA passes (AdMode::TwoPass): the first adds
         * Cs Ad / 255 without writing alpha or Z (Ad and the depth test stay
         * as they were), the second adds it again with the pass's writes */
        const int reps = (!b.depthFrame && b.ds.blend.ad == gs::AdMode::TwoPass && (mask & gs::WRITE_RGB) != 0) ? 2 : 1;
        for (int rep = 0; rep < reps; rep++) {
            ID3D11BlendState *&bs = b.blend[p & 1][rep];
            ID3D11DepthStencilState *&dss = b.depth[p & 1][rep];
            if (bs == nullptr || dss == nullptr) {
                gs::Blend bl = b.ds.blend;
                uint8_t m = mask;
                bool z = zw;
                if (reps == 2 && rep == 0) {
                    m = (uint8_t)(mask & gs::WRITE_RGB);
                    z = false;
                } else if (reps == 2) {
                    bl.dst = gs::BlendFactor::One;
                }
                bs = b.depthFrame ? blend_plain(0) : blend_state(bl, m);
                if (b.depthFrame)
                    dss = depth_state(false, gs::Compare::Always, true);
                else
                    dss = depth_state(b.zt != nullptr && b.ds.depthTest, b.ds.depthFunc, z);
            }
            set_blend(bs);
            set_depth(dss);
            if (flicker) {
                /* (GT3PC_FLICKER: what the binding cache has wrong) */
                char what[256];
                const uint32_t bad = d3d_check_bound(b.tme, b.dateSrv != nullptr, indexed, what, sizeof(what));
                if (bad != 0 && (fd.bound & bad) != bad)
                    flicker_event("bound state differs from the cache: %s", what);
                fd.bound |= bad;
            }
            if (indexed)
                q::run([count, first, baseVertex] { qdc()->DrawIndexed(count, first, baseVertex); });
            else
                q::run([count, first] { qdc()->Draw(count, first); });
            g_stats.draws++;
            g_profCount[PCOUNT_DRAWS]++;
            g_stats.verts += count;
        }
        wroteRgb = wroteRgb || (mask & gs::WRITE_RGB) != 0;
        wroteTop = wroteTop || (mask & gs::WRITE_A) != 0;
        wroteZ = wroteZ || zw;
    }
    if (gpuQuery != nullptr)
        flicker_query_end(gpuQuery, indexed, first, count, baseVertex);
    if (flicker) {
        fd.rtv = (uint32_t)(uintptr_t)(b.depthFrame ? (void *)b.rt->dsv.get() : (void *)b.rt->rtv.get());
        flicker_draw(fd);
    }
    /* PC: where (the scissor's part of the area) */
    D3D11_RECT w = b.scissor;
    if (area != nullptr) {
        w.left = std::max(w.left, area->left);
        w.top = std::max(w.top, area->top);
        w.right = std::min(w.right, area->right);
        w.bottom = std::min(w.bottom, area->bottom);
    }
    if (b.depthFrame) {
        target_written_rect(*b.rt, w.left, w.top, w.right, w.bottom);
        registry_target_written(*b.rt, true, false);
        return;
    }
    if (wroteRgb || wroteTop) {
        target_written_rect(*b.rt, w.left, w.top, w.right, w.bottom);
        registry_target_written(*b.rt, wroteRgb, wroteTop);
        if (g_tracing)
            probe_draw(*b.rt);
        if (b.dateSrv != nullptr && b.ds.date) {
            /* a run of DATE draws goes on (g_dateRun; a C = Ad draw in the
             * shader alone does not start one) */
            g_dateRun.rt = b.rt;
            g_dateRun.gen = b.rt->gen;
        }
    }
    if (wroteZ) {
        /* the Z words now hold this frame's pixels (a Z buffer shared by
         * frames of two GS sizes: pgl_targets.cpp) */
        depth_target_written(*b.zt, *b.rt);
        b.zt->gen++;
        registry_target_written(*b.zt, true, false);
    }
}

}  /* namespace */

void flush_batch()
{
    if (g_verts.empty())
        return;
    ProfScope prof(PROF_FLUSH);         /* (the verbose monitor's profile) */
    g_profCount[PCOUNT_BATCHES]++;
    g_profCount[PCOUNT_GSVERTS] += (uint32_t)g_verts.size();
    if (g_fbPending)
        fb_segment_end();               /* (drawn now: still to be seen by what reads the copy) */
    g_segStart = 0;
    /* PC: the vertices' bounds (native pixels; a pixel more around for the
     * points' and lines' width and the edges' rounding) */
    float x0 = g_verts[0].x, y0 = g_verts[0].y, x1 = x0, y1 = y0;
    for (const GsVtx &v : g_verts) {
        x0 = std::min(x0, v.x);
        y0 = std::min(y0, v.y);
        x1 = std::max(x1, v.x);
        y1 = std::max(y1, v.y);
    }
    const D3D11_RECT area = {(LONG)std::floor(x0) - 2, (LONG)std::floor(y0) - 2, (LONG)std::ceil(x1) + 2,
                             (LONG)std::ceil(y1) + 2};
    if (!g_bs.valid || g_bs.skip) {
        if (flicker_on())
            flicker_note(2, (uint32_t)g_verts.size(), &area, fnv(g_verts.data(), g_verts.size() * sizeof(GsVtx)));
        g_verts.clear();
        return;
    }
    ID3D11Buffer *vb = nullptr;
    const uint32_t first = stream_vertices(g_verts.data(), sizeof(GsVtx), (uint32_t)g_verts.size(), &vb);
    set_vb(vb, sizeof(GsVtx));
    set_topology(g_bs.topo);
    bind_vs(Vs::Gs);
    CbGs cb = {(float)g_bs.rt->nw, (float)g_bs.rt->nh, 0, 0};
    bind_vs_cb(&cb, sizeof(cb));
    draw_passes(false, (uint32_t)g_verts.size(), first, 0, 0, &area);
    g_verts.clear();
}

namespace {

/* ======================================================================== */
/* GS-space primitives                                                      */
/* ======================================================================== */

GsVtx convert(const Vtx &v)
{
    const BatchState &b = g_bs;
    GsVtx o;
    const float wx = (float)((int32_t)v.x - (int32_t)b.ofx) * 0.0625f;
    const float wy = (float)((int32_t)v.y - (int32_t)b.ofy) * 0.0625f;
    o.x = wx * b.sx + b.pix;
    o.y = wy * b.sy + b.pix;
    o.z = pr::depthOf(v.z, b.zpsm);
    if (!b.tme) {
        o.u = o.v = 0;
        o.q = 1;
    } else if (b.fst) {
        o.u = (float)v.u * 0.0625f;
        o.v = (float)v.v * 0.0625f;
        o.q = 1;
    } else {
        o.u = v.s * b.texW;
        o.v = v.t * b.texH;
        o.q = v.q;
    }
    o.rgba = v.rgba;
    o.fog = (float)v.f;
    return o;
}

/* PC: a primitive after a TEXFLUSH (g_fbPending): where it reads its own
 * target (its texture coordinates in the target's native pixels, two more
 * around for the filter) - if that meets what was drawn since the copy
 * (g_fbRect), the batch is drawn and the state taken again, the copy
 * refreshed there, before it joins. */
void feedback_check(const Vtx *v, int n)
{
    if (!g_fbPending)
        return;
    const BatchState &b = g_bs;
    if (!b.tme || b.tex.target == nullptr || b.tex.target != b.rt || b.tex.w <= 0.0f || b.tex.h <= 0.0f) {
        g_fbPending = false;
        return;
    }
    const Target &t = *b.rt;
    const float kx = (float)t.nw / b.tex.w, ky = (float)t.nh / b.tex.h;
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (int i = 0; i < n; i++) {
        float tu, tv;
        if (b.fst) {
            tu = (float)v[i].u * 0.0625f;
            tv = (float)v[i].v * 0.0625f;
        } else {
            const float q = v[i].q != 0.0f ? v[i].q : 1.0f;
            tu = v[i].s * b.texW / q;
            tv = v[i].t * b.texH / q;
        }
        const float px = (tu - b.tex.x0) * kx, py = (tv - b.tex.y0) * ky;
        x0 = std::min(x0, px);
        y0 = std::min(y0, py);
        x1 = std::max(x1, px);
        y1 = std::max(y1, py);
    }
    const float l = std::floor(x0) - 2.0f, tp = std::floor(y0) - 2.0f;
    const float r = std::ceil(x1) + 2.0f, bt = std::ceil(y1) + 2.0f;
    bool meets = false;
    for (const D3D11_RECT &w : g_fbRects) {
        if (r > (float)w.left && l < (float)w.right && bt > (float)w.top && tp < (float)w.bottom) {
            meets = true;
            break;
        }
    }
    if (!meets)
        return;                         /* the copy holds what it reads */
    flush_batch();
    g_dirty = true;
    ensure_state();                     /* the copy refreshed where written (g_fbPending cleared) */
}

void emit_point(const Vtx &k)
{
    ensure_state();
    feedback_check(&k, 1);
    if (g_bs.skip)
        return;
    g_verts.push_back(convert(k));
}

void emit_line(const Vtx &a, const Vtx &k)
{
    ensure_state();
    const Vtx pv[2] = {a, k};
    feedback_check(pv, 2);
    if (g_bs.skip)
        return;
    if (g_lineWidth <= 1.0f) {
        g_verts.push_back(convert(k));
        g_verts.push_back(convert(a));
        return;
    }
    /* a wide line (CMD_LINE_WIDTH): a quad g_lineWidth native pixels across,
     * its ends squared off half that beyond the points (a strip's joints
     * covered) */
    const GsVtx A = convert(a), B = convert(k);
    float dx = B.x - A.x, dy = B.y - A.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len > 1e-4f) {
        dx /= len;
        dy /= len;
    } else {
        dx = 1.0f;
        dy = 0.0f;
    }
    const float h = g_lineWidth * 0.5f;
    const float nx = -dy * h, ny = dx * h, ex = dx * h, ey = dy * h;
    GsVtx c[4] = {A, A, B, B};
    c[0].x = A.x - ex + nx; c[0].y = A.y - ey + ny;
    c[1].x = A.x - ex - nx; c[1].y = A.y - ey - ny;
    c[2].x = B.x + ex + nx; c[2].y = B.y + ey + ny;
    c[3].x = B.x + ex - nx; c[3].y = B.y + ey - ny;
    g_verts.push_back(c[0]);
    g_verts.push_back(c[2]);
    g_verts.push_back(c[1]);
    g_verts.push_back(c[2]);
    g_verts.push_back(c[3]);
    g_verts.push_back(c[1]);
}

void emit_tri(const Vtx &k, const Vtx &a, const Vtx &b)
{
    ensure_state();
    const Vtx pv[3] = {k, a, b};
    feedback_check(pv, 3);
    if (g_bs.skip)
        return;
    g_verts.push_back(convert(k));
    g_verts.push_back(convert(a));
    g_verts.push_back(convert(b));
}

void emit_sprite(const Vtx &a, const Vtx &k)
{
    ensure_state();
    const Vtx pv[2] = {a, k};
    feedback_check(pv, 2);
    if (g_bs.skip)
        return;
    GsVtx A = convert(a), B = convert(k);
    if (g_bs.tme && !g_bs.fst) {
        /* STQ sprites: S / Q per corner */
        A.u /= (A.q != 0 ? A.q : 1);
        A.v /= (A.q != 0 ? A.q : 1);
        B.u /= (B.q != 0 ? B.q : 1);
        B.v /= (B.q != 0 ? B.q : 1);
        A.q = B.q = 1;
    }
    if (g_bs.tme) {
        /* keep the texel centres of a 1:1 mapping (see the file comment) -
         * in design pixels for a design-squeezed texture (CMD_DESIGN: a GS
         * pixel is 1 / k of one; slope x (pix / s - 0.5 k)); a render
         * target's copy stays in GS pixels (1:1 to its native texels) */
        const bool design = g_bs.tex.target == nullptr;
        const float kx = design ? g_designX : 1.0f;
        const float ky = design ? g_designY : 1.0f;
        const float dxw = (float)((int32_t)k.x - (int32_t)a.x) * 0.0625f;
        const float dyw = (float)((int32_t)k.y - (int32_t)a.y) * 0.0625f;
        if (dxw != 0) {
            const float cu = (B.u - A.u) / dxw * (g_bs.pix / g_bs.sx - 0.5f * kx);
            A.u += cu;
            B.u += cu;
        }
        if (dyw != 0) {
            const float cv = (B.v - A.v) / dyw * (g_bs.pix / g_bs.sy - 0.5f * ky);
            A.v += cv;
            B.v += cv;
        }
    }
    GsVtx c[4];
    for (int i = 0; i < 4; i++) {
        c[i] = B;
        c[i].x = (i & 1) ? B.x : A.x;
        c[i].u = (i & 1) ? B.u : A.u;
        c[i].y = (i & 2) ? B.y : A.y;
        c[i].v = (i & 2) ? B.v : A.v;
    }
    g_verts.push_back(c[0]);
    g_verts.push_back(c[1]);
    g_verts.push_back(c[2]);
    g_verts.push_back(c[1]);
    g_verts.push_back(c[3]);
    g_verts.push_back(c[2]);
}

/* The GS vertex queue: a vertex arrives (kick: XYZ2 / XYZF2). */
void vertex(const Vtx &v, bool kick)
{
    switch (g.prim & 7) {
    case 0:     /* POINT */
        if (kick)
            emit_point(v);
        break;
    case 1:     /* LINE */
        g.queue[g.count++] = v;
        if (g.count == 2) {
            if (kick)
                emit_line(g.queue[0], v);
            g.count = 0;
        }
        break;
    case 2:     /* LINE_STRIP */
        if (g.count == 0) {
            g.queue[0] = v;
            g.count = 1;
        } else {
            if (kick)
                emit_line(g.queue[0], v);
            g.queue[0] = v;
        }
        break;
    case 3:     /* TRIANGLE */
        if (g.count < 2) {
            g.queue[g.count++] = v;
        } else {
            if (kick)
                emit_tri(v, g.queue[0], g.queue[1]);
            g.count = 0;
        }
        break;
    case 4:     /* TRI_STRIP */
        if (g.count < 2) {
            g.queue[g.count++] = v;
        } else {
            if (kick)
                emit_tri(v, g.queue[0], g.queue[1]);
            g.queue[0] = g.queue[1];
            g.queue[1] = v;
        }
        break;
    case 5:     /* TRI_FAN */
        if (g.count == 0) {
            g.fan0 = v;
            g.count = 1;
        } else if (g.count == 1) {
            g.queue[0] = v;
            g.count = 2;
        } else {
            if (kick)
                emit_tri(v, g.fan0, g.queue[0]);
            g.queue[0] = v;
        }
        break;
    case 6:     /* SPRITE */
        if (g.count == 0) {
            g.queue[0] = v;
            g.count = 1;
        } else {
            if (kick)
                emit_sprite(g.queue[0], v);
            g.count = 0;
        }
        break;
    default:
        break;
    }
}

Vtx current_vertex(uint16_t x, uint16_t y, uint32_t z, uint8_t f)
{
    Vtx v;
    v.x = x;
    v.y = y;
    v.z = z;
    v.f = f;
    v.rgba = g.rgba;
    v.s = g.s;
    v.t = g.t;
    v.q = g.q;
    v.u = g.u;
    v.v = g.v;
    return v;
}

/* ======================================================================== */
/* Register writes                                                          */
/* ======================================================================== */

void set_prim(uint32_t prim)
{
    prim &= 0x7FF;
    if (prim != g.prim)
        g_dirty = true;
    g.prim = prim;
    g.count = 0;
}

void start_transfer(uint64_t trxdir)
{
    const uint32_t xdir = (uint32_t)(trxdir & 3);
    g_xfer.active = false;
    if (xdir == 3)
        return;
    if (xdir != 0) {
        log_once(xdir == 1 ? "pgl-xfer-l2h" : "pgl-xfer-l2l", "pgl: GS %s transfer in a GIF block: not native",
                 xdir == 1 ? "local -> host" : "local -> local");
        return;
    }
    g_xfer.dbp = (uint32_t)(g.bitbltbuf >> 32) & 0x3FFF;
    g_xfer.dbw = (uint32_t)(g.bitbltbuf >> 48) & 0x3F;
    g_xfer.dpsm = (uint32_t)(g.bitbltbuf >> 56) & 0x3F;
    g_xfer.dx = (int)((g.trxpos >> 32) & 0x7FF);
    g_xfer.dy = (int)((g.trxpos >> 48) & 0x7FF);
    g_xfer.w = (int)(g.trxreg & 0xFFF);
    g_xfer.h = (int)((g.trxreg >> 32) & 0xFFF);
    g_xfer.bytes = gstex::image_bytes(g_xfer.dpsm, (uint32_t)g_xfer.w, (uint32_t)g_xfer.h);
    g_xfer.data.clear();
    g_xfer.active = g_xfer.bytes > 0;
}

void image_data(const uint8_t *p, size_t bytes)
{
    if (!g_xfer.active)
        return;
    const size_t want = g_xfer.bytes - g_xfer.data.size();
    const size_t take = std::min(want, bytes);
    g_xfer.data.insert(g_xfer.data.end(), p, p + take);
    if (g_xfer.data.size() >= g_xfer.bytes) {
        flush_batch();
        ProfScope prof(PROF_UPLOAD);    /* (an upload inside a GIF block) */
        g_profCount[PCOUNT_UPLOADS]++;
        registry_upload(g_xfer.dbp, g_xfer.dbw, g_xfer.dpsm, g_xfer.dx, g_xfer.dy, g_xfer.w, g_xfer.h,
                        g_xfer.data.data(), g_xfer.data.size());
        g_xfer.active = false;
        g_dirty = true;
    }
}

/* A write of a GS register in its 64-bit format (A+D, REGLIST, pgl). */
void write_reg(uint32_t addr, uint64_t v)
{
    addr &= 0xFF;
    if (addr < 0x80 && g_addrReg[addr] >= 0) {
        const int c = g_addrCtx[addr];
        const int i = g_addrReg[addr];
        if (i == gs::REG_TEX2) {
            /* TEX2: PSM and the CLUT fields of TEX0 */
            const uint64_t mask = (0x3Full << 20) | (~0ull << 37);
            const uint64_t t0 = (g.ctx[c][gs::REG_TEX0] & ~mask) | (v & mask);
            g.ctx[c][gs::REG_TEX2] = v;
            if ((t0 & ~(7ull << 61)) != (g.ctx[c][gs::REG_TEX0] & ~(7ull << 61)))
                g_dirty = true;
            g.ctx[c][gs::REG_TEX0] = t0;
            if ((v >> 61) != 0) {
                registry_clut_load(t0);
                g_dirty = true;
            }
            return;
        }
        if (i == gs::REG_TEX0 && (v >> 61) != 0) {
            registry_clut_load(v);
            g_dirty = true;
        }
        if (g.ctx[c][i] != v) {
            g.ctx[c][i] = v;
            g_dirty = true;
        }
        return;
    }
    if (addr < 0x80 && g_addrGen[addr] >= 0) {
        const int i = g_addrGen[addr];
        if (i == gs::GEN_TEXFLUSH) {
            /* TEXFLUSH: what was drawn so far is what the next texture reads
             * see.  A batch that samples the target it draws into (or its Z
             * buffer) reads the copy taken when its state was resolved, so
             * primitives that read the ones before them through a TEXFLUSH
             * (the flare query's in-place reduction, flare_reduce /
             * flare_build_reduce: each halving reads the previous one; blur
             * passes over themselves) must not share it: the batch ends and
             * the next primitive resolves its texture again (a fresh copy). */
            if (g_bs.valid && g_bs.tme && g_bs.tex.target != nullptr &&
                (g_bs.tex.target == g_bs.rt || g_bs.tex.target == g_bs.zt)) {
                if (g_bs.tex.target == g_bs.rt && !g_bs.depthFrame) {
                    /* PC: what the next primitives must see (g_fbRects):
                     * first the target's part written since the copy
                     * before this batch (all if written somewhere unknown),
                     * then each run of this batch's vertices that a
                     * TEXFLUSH ends */
                    if (!g_fbPending) {
                        const Target &t = *g_bs.rt;
                        g_fbRects.clear();
                        if (t.rectGen != t.gen)
                            g_fbRects.push_back({0, 0, (LONG)t.nw, (LONG)t.nh});
                        else if (t.dirtyR > t.dirtyL && t.dirtyB > t.dirtyT)
                            g_fbRects.push_back({t.dirtyL, t.dirtyT, t.dirtyR, t.dirtyB});
                        g_fbPending = true;
                    }
                    fb_segment_end();
                } else {
                    flush_batch();
                    g_dirty = true;
                }
            }
            return;
        }
        if (g.gen[i] != v) {
            g.gen[i] = v;
            g_dirty = true;
        }
        return;
    }
    switch (addr) {
    case A_PRIM:
        set_prim((uint32_t)v);
        break;
    case A_RGBAQ:
        g.rgba = (uint32_t)v;
        g.q = pgl::u2f((uint32_t)(v >> 32));
        break;
    case A_ST:
        g.s = pgl::u2f((uint32_t)v);
        g.t = pgl::u2f((uint32_t)(v >> 32));
        break;
    case A_UV:
        g.u = (uint16_t)(v & 0x3FFF);
        g.v = (uint16_t)((v >> 16) & 0x3FFF);
        break;
    case A_FOG:
        g.fog = (uint8_t)(v >> 56);
        break;
    case A_XYZF2:
    case A_XYZF3:
        g.fog = (uint8_t)(v >> 56);
        vertex(current_vertex((uint16_t)v, (uint16_t)(v >> 16), (uint32_t)(v >> 32) & 0xFFFFFF, g.fog),
               addr == A_XYZF2);
        break;
    case A_XYZ2:
    case A_XYZ3:
        vertex(current_vertex((uint16_t)v, (uint16_t)(v >> 16), (uint32_t)(v >> 32), g.fog), addr == A_XYZ2);
        break;
    case A_PRMODE:
        if (g.prmode != v)
            g_dirty = true;
        g.prmode = v;
        break;
    case A_BITBLTBUF:
        g.bitbltbuf = v;
        break;
    case A_TRXPOS:
        g.trxpos = v;
        break;
    case A_TRXREG:
        g.trxreg = v;
        break;
    case A_TRXDIR:
        start_transfer(v);
        break;
    default:
        break;      /* SIGNAL, FINISH, LABEL, HWREG ... */
    }
}

/* One PACKED qword for register descriptor reg. */
void packed(uint32_t reg, const uint8_t *qw)
{
    uint32_t w[4];
    memcpy(w, qw, 16);
    uint64_t lo, hi;
    memcpy(&lo, qw, 8);
    memcpy(&hi, qw + 8, 8);
    switch (reg) {
    case 0x0:
        write_reg(A_PRIM, w[0] & 0x7FF);
        break;
    case 0x1:
        g.rgba = (w[0] & 0xFF) | ((w[1] & 0xFF) << 8) | ((w[2] & 0xFF) << 16) | ((w[3] & 0xFF) << 24);
        break;
    case 0x2:
        g.s = pgl::u2f(w[0]);
        g.t = pgl::u2f(w[1]);
        g.q = pgl::u2f(w[2]);
        break;
    case 0x3:
        g.u = (uint16_t)(w[0] & 0x3FFF);
        g.v = (uint16_t)(w[1] & 0x3FFF);
        break;
    case 0x4: {
        const bool adc = ((w[3] >> 15) & 1) != 0;
        g.fog = (uint8_t)((w[3] >> 4) & 0xFF);
        vertex(current_vertex((uint16_t)w[0], (uint16_t)w[1], (w[2] >> 4) & 0xFFFFFF, g.fog), !adc);
        break;
    }
    case 0x5: {
        const bool adc = ((w[3] >> 15) & 1) != 0;
        vertex(current_vertex((uint16_t)w[0], (uint16_t)w[1], w[2], g.fog), !adc);
        break;
    }
    case 0xA:
        g.fog = (uint8_t)((w[3] >> 4) & 0xFF);
        break;
    case 0xE:
        write_reg((uint32_t)(hi & 0xFF), lo);
        break;
    case 0xF:
    case 0xB:
        break;
    default:            /* TEX0_1/2, CLAMP_1/2, XYZF3, XYZ3: the low 64 bits */
        write_reg(reg, lo);
        break;
    }
}

void exec_gif(const CmdGif &c)
{
    const uint8_t *p = static_cast<const uint8_t *>(c.data);
    uint32_t left = c.qwc;
    if (p == nullptr)
        return;
    while (left > 0) {
        uint64_t lo, hi;
        memcpy(&lo, p, 8);
        memcpy(&hi, p + 8, 8);
        p += 16;
        left--;
        const uint32_t nloop = (uint32_t)(lo & 0x7FFF);
        const bool pre = ((lo >> 46) & 1) != 0;
        const uint32_t primField = (uint32_t)((lo >> 47) & 0x7FF);
        const uint32_t flg = (uint32_t)((lo >> 58) & 3);
        uint32_t nreg = (uint32_t)((lo >> 60) & 0xF);
        if (nreg == 0)
            nreg = 16;
        if (flg == 0) {
            if (pre)
                write_reg(A_PRIM, primField);
            for (uint32_t l = 0; l < nloop; l++) {
                for (uint32_t r = 0; r < nreg; r++) {
                    if (left == 0)
                        return;
                    packed((uint32_t)((hi >> (4 * r)) & 0xF), p);
                    p += 16;
                    left--;
                }
            }
        } else if (flg == 1) {
            const uint32_t total = nloop * nreg;
            const uint32_t qwords = (total + 1) / 2;
            if (qwords > left)
                return;
            for (uint32_t i = 0; i < total; i++) {
                const uint32_t reg = (uint32_t)((hi >> (4 * (i % nreg))) & 0xF);
                uint64_t v;
                memcpy(&v, p + 8 * i, 8);
                if (reg == 0xE || reg == 0xF)
                    continue;
                write_reg(reg, v);
            }
            p += 16 * qwords;
            left -= qwords;
        } else {
            const uint32_t q = std::min(nloop, left);
            image_data(p, (size_t)q * 16);
            p += 16 * q;
            left -= q;
        }
    }
}

void exec_regs(const CmdRegs &c)
{
    for (int i = 0; i < gs::REG_COUNT; i++)
        if (c.mask & (1ull << i))
            write_reg(kCtxAddr[i], c.reg[i]);
    for (int i = 0; i < gs::REG_COUNT; i++)
        if (c.mask & (1ull << (16 + i)))
            write_reg(kCtxAddr[i] + 1u, c.reg[i]);
    for (int i = 0; i < gs::GEN_COUNT; i++)
        if (c.mask & (1ull << (32 + i)))
            write_reg(kGenAddr[i], c.gen[i]);
}

void exec_prim(const CmdPrim &c)
{
    set_prim(c.prim);
    const pr::GsVertex *v = reinterpret_cast<const pr::GsVertex *>(&c + 1);
    for (int i = 0; i < c.n; i++) {
        g.rgba = v[i].rgba;
        g.u = v[i].u;
        g.v = v[i].v;
        g.fog = v[i].f;
        g.s = g.t = 0;
        g.q = 1;
        vertex(current_vertex(v[i].x, v[i].y, v[i].z, v[i].f), v[i].kick);
    }
}

/* ======================================================================== */
/* VU1 draws                                                                */
/* ======================================================================== */

bool light_program(uint32_t prog, int32_t &program, int32_t &e)
{
    e = 16;
    switch (prog) {
    case 0x04E: program = VU1P_DEFAULT; return true;
    case 0x057: program = VU1P_SPEC2; e = 4; return true;
    case 0x082: program = VU1P_SPEC2; e = 8; return true;
    case 0x0AF: program = VU1P_SPEC2; return true;
    case 0x0DE: program = VU1P_SPEC4; e = 4; return true;
    case 0x115: program = VU1P_SPEC4; e = 8; return true;
    case 0x14C: program = VU1P_SPEC4; return true;
    case 0x185: program = VU1P_DIFFUSE; return true;
    case 0x1AD: program = VU1P_LOCAL; e = 4; return true;
    case 0x1E1: program = VU1P_LOCAL; e = 8; return true;
    case 0x21A: program = VU1P_LOCAL; return true;
    case 0x257: program = VU1P_ENVMTX; return true;
    case 0x297: program = VU1P_ENVMAP; return true;
    case 0x2DB: program = VU1P_CONST; return true;
    case 0x2F7: program = VU1P_FLARE; return true;
    default: program = VU1P_DEFAULT; return false;
    }
}

/* PC: where a VU1 draw of the vertices in box [bmin, bmax] (object space)
 * lands in the target (native pixels): its 8 corners through C and the
 * viewport as vs_vu1 / project_vu1 take them, 2 pixels around.  False when
 * unknown (no box, a corner at or behind the eye): the scissor's then.  For
 * the target's written part (target_written_rect): the copies that draws
 * reading the target take (DATE, C = Ad, a texture of it) are refreshed
 * there only - each one a whole MSAA resolve before (user: MSAA 8x slow). */
bool vu1_area(const PglrVu1 &vu, const float *bmin, const float *bmax, D3D11_RECT &out)
{
    if (bmin == nullptr || bmax == nullptr)
        return false;
    const BatchState &b = g_bs;
    const float offX = (float)b.ofx * 0.0625f, offY = (float)b.ofy * 0.0625f;
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (int i = 0; i < 8; i++) {
        const float P[3] = {(i & 1) ? bmax[0] : bmin[0], (i & 2) ? bmax[1] : bmin[1], (i & 4) ? bmax[2] : bmin[2]};
        float pc[4];
        for (int k = 0; k < 4; k++)
            pc[k] = P[0] * vu.m[k] + P[1] * vu.m[4 + k] + P[2] * vu.m[8 + k] + vu.m[12 + k];
        if (!(pc[3] > 1e-4f))
            return false;
        const float gx = vu.viewScale[0] * pc[0] / pc[3] + vu.viewCenter[0];
        const float gy = vu.viewScale[1] * pc[1] / pc[3] + vu.viewCenter[1];
        const float px = (gx - offX) * b.sx + b.pix, py = (gy - offY) * b.sy + b.pix;
        x0 = std::min(x0, px);
        y0 = std::min(y0, py);
        x1 = std::max(x1, px);
        y1 = std::max(y1, py);
    }
    if (!(x1 - x0 < 1e6f && y1 - y0 < 1e6f))
        return false;
    out = {(LONG)std::floor(x0) - 2, (LONG)std::floor(y0) - 2, (LONG)std::ceil(x1) + 2, (LONG)std::ceil(y1) + 2};
    return true;
}

/* One VU1 run: the program state vu, PRIM prim, an indexed triangle list
 * (its vertices in box [bmin, bmax] when known: the area it writes). */
void vu1_draw(const PglrVu1 &vu, uint32_t prim, CbVu1 &cb, ID3D11Buffer *vb, ID3D11Buffer *ib,
              uint32_t indexCount, uint32_t firstIndex, int32_t baseVertex, const float *bmin = nullptr,
              const float *bmax = nullptr)
{
    flush_batch();
    set_prim(prim);         /* the program's GIF tag has PRE */
    if (vu.prog == 0 || indexCount == 0)
        return;
    ensure_state();
    BatchState &b = g_bs;
    if (b.skip || b.depthFrame) {
        if (b.skip && flicker_on())
            flicker_note(3, indexCount, nullptr, 0);
        return;
    }
    memcpy(cb.C, vu.m, sizeof(cb.C));
    memcpy(cb.viewScale, vu.viewScale, 16);
    memcpy(cb.viewCenter, vu.viewCenter, 16);
    memcpy(cb.limits, vu.limits, 16);
    const float offX = (float)b.ofx * 0.0625f, offY = (float)b.ofy * 0.0625f;
    const float nw = (float)b.rt->nw, nh = (float)b.rt->nh;
    cb.ndcMap[0] = 2.0f * vu.viewScale[0] * b.sx / nw;
    cb.ndcMap[1] = 2.0f * ((vu.viewCenter[0] - offX) * b.sx + b.pix) / nw;
    cb.ndcMap[2] = 2.0f * vu.viewScale[1] * b.sy / nh;
    cb.ndcMap[3] = 2.0f * ((vu.viewCenter[1] - offY) * b.sy + b.pix) / nh;
    cb.zNorm = z_norm(b.zpsm);
    cb.texW = b.tme ? b.texW : 1.0f;
    cb.texH = b.tme ? b.texH : 1.0f;
    cb.flatShade = b.flat ? 1 : 0;

    /* culling: vu1Prog + 0xA culls the clockwise (GL y-up) faces, + 0x14
     * the counter-clockwise ones (docs/models.md 4.6) */
    const uint32_t off = vu.prog >= 0x33E ? vu.prog - 0x33E : vu.prog - 0x320;
    int cull = off == 0x0A ? 1 : off == 0x14 ? 2 : 0;
    const bool mirrored = (vu.viewScale[0] < 0) != (vu.viewScale[1] > 0);
    if (mirrored && cull != 0)
        cull = 3 - cull;

    bind_vs(Vs::Vu1);
    bind_vs_cb(&cb, sizeof(cb));
    set_vb(vb, sizeof(gt3vif::Vertex));
    set_ib(ib);
    set_topology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_traceVu1 = &cb;
    D3D11_RECT area;
    draw_passes(true, indexCount, firstIndex, baseVertex, cull, vu1_area(vu, bmin, bmax, area) ? &area : nullptr);
    g_traceVu1 = nullptr;
}

/* The box of n VU1 vertices (pos and pos2). */
void vu1_box(const gt3vif::Vertex *v, size_t n, float *bmin, float *bmax)
{
    for (int k = 0; k < 3; k++) {
        bmin[k] = 1e30f;
        bmax[k] = -1e30f;
    }
    for (size_t i = 0; i < n; i++)
        for (int k = 0; k < 3; k++) {
            bmin[k] = std::min(bmin[k], std::min(v[i].pos[k], v[i].pos2[k]));
            bmax[k] = std::max(bmax[k], std::max(v[i].pos[k], v[i].pos2[k]));
        }
}

std::vector<gt3vif::Vertex> g_vu1Verts;
std::vector<uint32_t> g_vu1Index;

void strip_indices(uint32_t first, uint32_t len)
{
    for (uint32_t i = 0; i + 2 < len; i++) {
        const uint32_t a = first + i;
        if ((i & 1) == 0) {
            g_vu1Index.push_back(a + 2);
            g_vu1Index.push_back(a);
            g_vu1Index.push_back(a + 1);
        } else {
            g_vu1Index.push_back(a + 2);
            g_vu1Index.push_back(a + 1);
            g_vu1Index.push_back(a);
        }
    }
}

void exec_strip(const CmdStrip &c)
{
    const int n = c.n;
    if (n < 3 || c.xyz == nullptr)
        return;
    g_vu1Verts.assign((size_t)n, gt3vif::Vertex());
    for (int i = 0; i < n; i++) {
        gt3vif::Vertex &v = g_vu1Verts[(size_t)i];
        memset(&v, 0, sizeof(v));
        memcpy(v.pos, c.xyz + 3 * i, 12);
        memcpy(v.pos2, v.pos, 12);
        if (c.st != nullptr) {
            memcpy(v.st, c.st + 2 * i, 8);
            memcpy(v.st2, v.st, 8);
        }
        const uint32_t col = c.rgba != nullptr ? c.rgba[i] : c.constRgba;
        memcpy(v.rgba, &col, 4);
        memcpy(v.rgba2, &col, 4);
    }
    g_vu1Index.clear();
    uint32_t o = 0;
    for (int k = 0; k < c.nStrips && o < (uint32_t)n; k++) {
        uint32_t len = c.lengths[k];
        if (o + len > (uint32_t)n)
            len = (uint32_t)n - o;
        strip_indices(o, len);
        o += len;
    }
    if (g_vu1Index.empty())
        return;
    /* PC: the GS primitives batched so far first: streaming them can restart
     * the vertex stream (DISCARD), and vertices streamed before that are in
     * the buffer's old memory, which the draw no longer reads (vu1_draw drew
     * the batch after these were streamed: the first strip after a restart
     * lost its vertices - a key of the name entry, the OK bar, for a frame,
     * every couple of seconds in the menus: user report) */
    flush_batch();
    ID3D11Buffer *vb = nullptr, *ib = nullptr;
    const uint32_t bv = stream_vertices(g_vu1Verts.data(), sizeof(gt3vif::Vertex), (uint32_t)n, &vb);
    const uint32_t bi = stream_indices(g_vu1Index.data(), (uint32_t)g_vu1Index.size(), &ib);
    CbVu1 cb = {};
    cb.program = VU1P_DEFAULT;
    float bmin[3], bmax[3];
    vu1_box(g_vu1Verts.data(), g_vu1Verts.size(), bmin, bmax);
    vu1_draw(c.vu, c.prim, cb, vb, ib, (uint32_t)g_vu1Index.size(), bi, (int32_t)bv, bmin, bmax);
}

void exec_tri16(const CmdTri16 &c)
{
    if (c.nTris <= 0 || c.xyzw == nullptr)
        return;
    const int n = c.nTris * 3;
    g_vu1Verts.assign((size_t)n, gt3vif::Vertex());
    for (int i = 0; i < n; i++) {
        gt3vif::Vertex &v = g_vu1Verts[(size_t)i];
        memset(&v, 0, sizeof(v));
        for (int k = 0; k < 3; k++)
            v.pos[k] = v.pos2[k] = (float)c.xyzw[4 * i + k];
        if (c.st != nullptr) {
            v.st[0] = v.st2[0] = (float)c.st[2 * i] / 4096.0f;
            v.st[1] = v.st2[1] = (float)c.st[2 * i + 1] / 4096.0f;
        }
        const uint32_t col = c.rgba != nullptr ? c.rgba[i] : 0x80808080u;
        memcpy(v.rgba, &col, 4);
        memcpy(v.rgba2, &col, 4);
    }
    g_vu1Index.clear();
    for (int t = 0; t < c.nTris; t++) {
        g_vu1Index.push_back((uint32_t)(3 * t + 2));
        g_vu1Index.push_back((uint32_t)(3 * t));
        g_vu1Index.push_back((uint32_t)(3 * t + 1));
    }
    flush_batch();                          /* PC: before the stream writes (exec_strip) */
    ID3D11Buffer *vb = nullptr, *ib = nullptr;
    const uint32_t bv = stream_vertices(g_vu1Verts.data(), sizeof(gt3vif::Vertex), (uint32_t)n, &vb);
    const uint32_t bi = stream_indices(g_vu1Index.data(), (uint32_t)g_vu1Index.size(), &ib);
    CbVu1 cb = {};
    cb.program = VU1P_DEFAULT;
    float bmin[3], bmax[3];
    vu1_box(g_vu1Verts.data(), g_vu1Verts.size(), bmin, bmax);
    vu1_draw(c.vu, c.prim, cb, vb, ib, (uint32_t)g_vu1Index.size(), bi, (int32_t)bv, bmin, bmax);
}

/* GT3PC_FLARE_LOG: the texel of every flare glow / result disc of the frame,
 * in draw order (= query order), logged at the frame's end. */
std::string g_flareGlows;
/* The flare queries' result strip of this frame (exec_flare_query): the glow
 * draws read their texel from it on the GPU (exec_model). */
Target *g_flareStrip = nullptr;

void exec_model(const CmdModel &c)
{
    flush_batch();
    const gt3vif::Mesh *mesh = nullptr;
    ID3D11Buffer *vb = nullptr, *ib = nullptr;
    uint32_t baseV = 0, baseI = 0, firstBatch = 0, endBatch = 0;
    g_profCount[PCOUNT_PARTS]++;
    bool found;
    {
        ProfScope prof(PROF_MESH);
        found = model_mesh(c.part, &mesh, &vb, &ib, &baseV, &baseI, &firstBatch, &endBatch);
    }
    if (!found)
        return;
    CbVu1 cb = {};
    int32_t program, e;
    if (!light_program(c.part.lightProg, program, e)) {
        char key[40];
        snprintf(key, sizeof(key), "pgl-vu1prog-%X", c.part.lightProg);
        log_once(key, "pgl: unknown VU1 lighting program 0x%X: drawn unlit", c.part.lightProg);
    }
    ID3D11ShaderResourceView *flareSrv = nullptr;
    if (program == VU1P_FLARE) {
        /* PC: a result disc's texel from the strip on the GPU (the queries
         * before it in this list wrote it; flare_readback no longer waits
         * for the GPU: docs/renderer.md 8); other flare draws (the unoccluded
         * discs) carry their texel */
        const bool gpu = c.part.flareStripH > 0;
        if (gpu) {
            Target *strip = g_flareStrip;
            const int q = c.part.flareQ;
            if (strip == nullptr || strip->base != c.part.flareStripBlock || q < 0 || (q >> 6) >= strip->gsH)
                return;                     /* (no query wrote it: 0, not drawn) */
            flareSrv = target_srv(*strip);
            if (flareSrv == nullptr)
                return;
            cb.flareGpu = 1.0f;
            cb.flareX = (float)(q & 0x3F) * strip->sx;
            cb.flareY = (float)(q >> 6) * strip->sy;
        }
        /* (the CPU's copy: GT3PC_FLARE_LOG's readback, else stale) */
        const uint32_t o = c.part.flareResults != nullptr ? (c.part.flareResults[c.part.flareIndex] & 0xFF) : 0;
        if (c.part.part == 0 && g_flareGlows.size() < 400 && flare_log(false)) {
            char v[8];
            snprintf(v, sizeof(v), " %02X", o);
            g_flareGlows += v;
        }
        if (!gpu) {
            if (o == 0)
                return;
            cb.flareO = (float)o;
        }
    }
    cb.program = program;
    cb.specExp = e;
    cb.format = (int32_t)(mesh->mode >> 5);
    cb.morph = c.part.morphWeight;
    memcpy(cb.L, c.part.light, sizeof(cb.L));
    g_traceProg = program;
    if (flareSrv != nullptr)
        q::run([flareSrv] { qdc()->VSSetShaderResources(0, 1, &flareSrv); });
    for (uint32_t bi = firstBatch; bi < endBatch && bi < mesh->batches.size(); bi++) {
        const gt3vif::Batch &bt = mesh->batches[bi];     /* PC: the part's batches only (model_mesh) */
        if (bt.part != (uint16_t)c.part.part || bt.indexCount == 0)
            continue;
        if (bt.flags & (gt3vif::BATCH_NO_GIFTAG | gt3vif::BATCH_NOT_TRIANGLES))
            continue;
        const uint32_t prim = ((uint32_t)bt.prim & ~c.part.primMask) | (c.part.primAttr & c.part.primMask);
        g_traceHasVcol = false;
        if (g_tracing && (mesh->attribs & gt3vif::ATTR_COLOR) != 0 && bt.vertexCount != 0 &&
            (size_t)bt.firstVertex + bt.vertexCount <= mesh->vertices.size()) {
            /* (GT3PC_PGL_TRACE) the batch's mean vertex colour */
            double sum[4] = {0, 0, 0, 0};
            for (uint32_t i = 0; i < bt.vertexCount; i++)
                for (int k = 0; k < 4; k++)
                    sum[k] += mesh->vertices[bt.firstVertex + i].rgba[k];
            for (int k = 0; k < 4; k++)
                g_traceVcol[k] = (float)(sum[k] / bt.vertexCount);
            g_traceHasVcol = true;
        }
        vu1_draw(c.vu, prim, cb, vb, ib, bt.indexCount, baseI + bt.firstIndex, (int32_t)baseV,
                 bt.hasBounds ? bt.bmin : nullptr, bt.hasBounds ? bt.bmax : nullptr);
    }
    if (flareSrv != nullptr) {
        /* (unbound: the strip is the next queries' output) */
        q::run([] {
            ID3D11ShaderResourceView *none = nullptr;
            qdc()->VSSetShaderResources(0, 1, &none);
        });
    }
    g_traceProg = -1;
    g_traceHasVcol = false;
}

/* ======================================================================== */
/* Effect recipes                                                           */
/* ======================================================================== */

/* PC: a pass over a target, queued for the D3D thread (pgl_queue.h): its
 * fixed state - the textures unbound, rtv the only target over vp, the
 * rasterizer, blend (factor: nullptr for none) and depth states (found by
 * the caller) - and its draw: srv0 / srv1 bound (sampler s if not null), a
 * triangle over the target, the textures unbound again. */
void queue_pass_state(ID3D11RenderTargetView *rtv, const D3D11_VIEWPORT &vp, ID3D11RasterizerState *rs,
                      ID3D11BlendState *bs, const float *factor, ID3D11DepthStencilState *ds)
{
    float f[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    const bool hasFactor = factor != nullptr;
    if (hasFactor)
        memcpy(f, factor, sizeof(f));
    q::run([rtv, vp, rs, bs, f, hasFactor, ds] {
        ID3D11ShaderResourceView *none[2] = {nullptr, nullptr};
        qdc()->PSSetShaderResources(0, 2, none);
        qdc()->OMSetRenderTargets(1, &rtv, nullptr);
        qdc()->RSSetViewports(1, &vp);
        qdc()->RSSetState(rs);
        qdc()->OMSetBlendState(bs, hasFactor ? f : nullptr, 0xFFFFFFFFu);
        qdc()->OMSetDepthStencilState(ds, 0);
    });
}

void queue_pass_draw(ID3D11ShaderResourceView *srv0, ID3D11ShaderResourceView *srv1, ID3D11SamplerState *s)
{
    q::run([srv0, srv1, s] {
        ID3D11ShaderResourceView *srvs[2] = {srv0, srv1};
        ID3D11ShaderResourceView *none[2] = {nullptr, nullptr};
        qdc()->PSSetShaderResources(0, 2, srvs);
        if (s != nullptr)
            qdc()->PSSetSamplers(0, 1, &s);
        qdc()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        qdc()->Draw(3, 0);
        qdc()->PSSetShaderResources(0, 2, none);
    });
}

void exec_pfx_color(const CmdPfxColor &c)
{
    flush_batch();
    Target *src = find_color_target(c.src);
    Target *dst = color_target(c.dst, c.fbw, 0, c.w, c.h);
    if (src == nullptr || dst == nullptr) {
        log_once("pgl-pfx-color-none", "pgl: colour correction without its frame target: skipped");
        return;
    }
    ID3D11ShaderResourceView *srcSrv = (src == dst) ? target_copy_srv(*src, true) : target_srv(*src);
    uint32_t curve[256];
    for (int i = 0; i < 256; i++)
        curve[i] = (uint32_t)c.curve[3 * i] | ((uint32_t)c.curve[3 * i + 1] << 8) |
                   ((uint32_t)c.curve[3 * i + 2] << 16) | 0xFF000000u;
    Com<ID3D11Texture2D> ctex;
    Com<ID3D11ShaderResourceView> csrv;
    if (!make_texture(256, 1, curve, ctex, csrv))
        return;
    d3d_invalidate();
    const D3D11_VIEWPORT vp = {0, 0, (float)dst->nw, (float)dst->nh, 0, 1};
    queue_pass_state(dst->rtv.get(), vp, raster_state(0, false), blend_plain(gs::WRITE_RGB), nullptr,
                     depth_state(false, gs::Compare::Always, false));
    bind_vs(Vs::Full);
    bind_ps(Ps::PfxColor);
    CbPfx cb = {};
    for (int k = 0; k < 3; k++) {
        cb.colR[k] = (float)((c.colR >> (8 * k)) & 0xFF);
        cb.colG[k] = (float)((c.colG >> (8 * k)) & 0xFF);
        cb.colB[k] = (float)((c.colB >> (8 * k)) & 0xFF);
    }
    cb.srcScale[0] = (float)src->nw / (float)dst->nw;
    cb.srcScale[1] = (float)src->nh / (float)dst->nh;
    bind_ps_cb(&cb, sizeof(cb));
    queue_pass_draw(srcSrv, csrv.get(), nullptr);
    d3d_invalidate();
    dst->gen++;
    registry_target_written(*dst, true, false);
    if (g_tracing)
        pclog::write("pgl trace %u: colour correction of the frame at 0x%X into 0x%X (%dx%d)", g_traceN++, c.src,
                     c.dst, c.w, c.h);
}

/* pfx_split_rgb_planes (the afterimage's capture, graphics.md 8.6): the
 * frame at c.src (the TEX0 texture: the frame buffer shown last) split into
 * the planes at c.r / c.g / c.b, which drawAfterimage then reads as PSMT8H
 * (registry_split_planes). */
void exec_pfx_split(const CmdPfxSplit &c)
{
    flush_batch();
    Target *src = find_color_target(c.src);
    if (src == nullptr) {
        log_once("pgl-pfx-split-none", "pgl: afterimage capture of block 0x%X, which no draw wrote: skipped", c.src);
        return;
    }
    const uint32_t blocks[3] = {c.r, c.g, c.b};
    registry_split_planes(*src, blocks, c.w, c.h);
    if (g_tracing)
        pclog::write("pgl trace %u: afterimage capture of the frame at 0x%X into the planes at 0x%X / 0x%X / 0x%X"
                     " (%dx%d)", g_traceN++, c.src, c.r, c.g, c.b, c.w, c.h);
}

/* One halving step of the flare reduction on one axis: a texels down to b
 * (pgl_flare.cpp flare_reduce / flare_build_reduce). */
struct FlareStep { int a, b; };

/* The next size of a halving (an odd size goes to the even one below, 1
 * stays 1). */
int flare_half(int n)
{
    int h = n >> 1;
    if (((h << 1) ^ n) != 0)
        h <<= 1;
    return h < 1 ? 1 : h;
}

/* The weight of each copied texel of one axis in texel 0 after the steps:
 * the work buffer (64 texels, clamped) holds `copied' texels of the copy,
 * the rest stale (taken as 0).  Each step writes texels 0..b-1 from a
 * bilinear sample at t = 0.5 + X a / b of the buffer as it was (the sprite's
 * texel mapping, docs/renderer.md 8): in place, so a later step can read a
 * texel an earlier level left (odd sizes reach texel a). */
void flare_axis_weights(int copied, const FlareStep *steps, int nSteps, float *out)
{
    static float m[64][64], n[64][64];
    for (int k = 0; k < 64; k++)
        for (int c = 0; c < 64; c++)
            m[k][c] = (k == c && k < copied) ? 1.0f : 0.0f;
    for (int s = 0; s < nSteps; s++) {
        const int a = steps[s].a, b = std::min(steps[s].b, 64);
        for (int x = 0; x < b; x++) {
            const float t = 0.5f + (float)x * (float)a / (float)b;
            const int i = (int)std::floor(t);
            const float f = t - (float)i;
            const int i0 = std::min(std::max(i, 0), 63), i1 = std::min(std::max(i + 1, 0), 63);
            for (int c = 0; c < 64; c++)
                n[x][c] = m[i0][c] * (1.0f - f) + m[i1][c] * f;
        }
        for (int x = 0; x < b; x++)
            memcpy(m[x], n[x], sizeof(m[x]));
    }
    for (int c = 0; c < 68; c++)
        out[c] = c < 64 ? m[0][c] : 0.0f;
}

/* GT3PC_PGL_TRACE: the probe points the next traced frame reads (the
 * centres of the flare queries the shape passed in; F9 traces two frames). */
int g_flareProbeN = 0;
int g_flareProbeX[4], g_flareProbeY[4];
uint32_t g_flareProbePassed[4];

/* GT3PC_PGL_TRACE: what a flare query saw - the screen's depth in the
 * shape's pixels (read back: the traced frame only) against the light's Z,
 * the pixels that passed and the result the weights give.  The queries the
 * shape passed in are remembered as probe points for the next traced frame:
 * its probes show which draws paint those pixels (and whether they write
 * Z). */
void flare_trace_depth(const PglrFlareQuery &c, const CbFlare &cb, Target *zt)
{
    ID3D11Texture2D *ztex = zt != nullptr && zt->tex ? target_tex1(*zt) : nullptr;
    if (ztex == nullptr) {
        pclog::write("pgl trace %u:   (no depth test)", g_traceN++);
        return;
    }
    static Com<ID3D11Texture2D> stage;
    static UINT stageW = 0, stageH = 0;
    D3D11_TEXTURE2D_DESC d = {};
    ztex->GetDesc(&d);
    if (!stage || stageW != d.Width || stageH != d.Height) {
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        d.MiscFlags = 0;
        stage.reset();
        if (FAILED(dev()->CreateTexture2D(&d, nullptr, stage.put())))
            return;
        stageW = d.Width;
        stageH = d.Height;
    }
    dc()->CopyResource(stage.get(), ztex);
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(dc()->Map(stage.get(), 0, D3D11_MAP_READ, 0, &m)))
        return;
    const float zn = z_norm((uint8_t)c.zpsm);
    uint32_t inShape = 0, passed = 0;
    double zmin = 1e30, zmax = -1e30, sum = 0.0;
    for (int y = 0; y < cb.box[3]; y++) {
        for (int x = 0; x < cb.box[2]; x++) {
            const int gx = std::min(std::max(c.box[0] + x, 0), c.frameW - 1);
            const int gy = std::min(std::max(c.box[1] + y, 0), c.frameH - 1);
            bool inside = false;
            for (int r = 0; r < c.nRects; r++)
                inside = inside || (gx >= c.rect[r][0] && gx < c.rect[r][2] && gy >= c.rect[r][1] && gy < c.rect[r][3]);
            if (!inside)
                continue;
            const int fy = c.halfY ? gy / 2 : gy;
            const UINT nx = (UINT)std::min((int)stageW - 1, (int)(((float)gx + 0.5f) * cb.scale[0]));
            const UINT ny = (UINT)std::min((int)stageH - 1, (int)(((float)fy + 0.5f) * cb.scale[1]));
            float sz;
            memcpy(&sz, (const uint8_t *)m.pData + (size_t)ny * m.RowPitch + (size_t)nx * 8, 4);
            const bool ok = c.ztest == 1 || (c.ztest == 2 && cb.z >= sz) || (c.ztest == 3 && cb.z > sz);
            inShape++;
            if (ok) {
                passed++;
                sum += (double)cb.wx[x] * (double)cb.wy[y] * 255.0;
            }
            zmin = std::min(zmin, (double)sz / zn);
            zmax = std::max(zmax, (double)sz / zn);
        }
    }
    dc()->Unmap(stage.get(), 0);
    const uint32_t a = (uint32_t)std::min(255.0, std::floor(sum + 0.5));
    pclog::write("pgl trace %u:   light Z 0x%06X; %u px in the shape, %u passed; scene Z 0x%06X .. 0x%06X;"
                 " A %u, result %u", g_traceN++, c.z, inShape, passed, inShape ? (uint32_t)zmin : 0u,
                 inShape ? (uint32_t)zmax : 0u, a, std::min(255u, (c.cs * a) >> 7));
    if (passed > 0) {
        /* keep the four with the most pixels passing */
        int slot = g_flareProbeN < 4 ? g_flareProbeN++ : -1;
        if (slot < 0) {
            slot = 0;
            for (int i = 1; i < 4; i++)
                if (g_flareProbePassed[i] < g_flareProbePassed[slot])
                    slot = i;
            if (g_flareProbePassed[slot] >= passed)
                return;
        }
        g_flareProbeX[slot] = c.box[0] + c.box[2] / 2;
        g_flareProbeY[slot] = c.box[1] + c.box[3] / 2;
        g_flareProbePassed[slot] = passed;
    }
}

/* A lens-flare occlusion query (pgl_flare.cpp flare_occlusion_query,
 * docs/renderer.md 8): the box of the screen tested natively against the
 * screen's depth (ps_flare), the result written into the query's texel of
 * the result strip, which flare_readback reads back for the glows.  This
 * stands in for the GS work after the shape (the copy into the work buffer,
 * the bilinear halvings, the Cs x As store): emulated at the output scale
 * they gave no usable result.  The result is the GS's: the halvings are
 * applied as weights of the copied pixels (flare_axis_weights). */
void exec_flare_query(const PglrFlareQuery &c)
{
    flush_batch();
    /* The screen and its Z buffer: the very targets the shape's draw - the
     * draw just before, into this frame with this Z buffer - used.  (By
     * address alone the screen's block also names other buffers: the car
     * icons' 64 x 32 ones share the race screen's 0x1A60; a lookup that took
     * one of them read an empty Z buffer, every light passed and glows showed
     * through the course.) */
    Target *rt = nullptr, *zt = nullptr;
    if (g_bs.rt != nullptr && !g_bs.depthFrame && g_bs.rt->base == c.frameBlock) {
        rt = g_bs.rt;
        if (g_bs.zt != nullptr && g_bs.zt->base == c.zBlock)
            zt = g_bs.zt;
    }
    if (rt == nullptr)
        rt = color_target(c.frameBlock, c.fbw, c.fpsm, c.frameW, c.frameH);
    /* the strip's size, as the PS2's store set its draw buffer
     * (flare_set_target(occVram, 64, occHeight)): another buffer known at
     * the same block must not stand in for it */
    target_size(c.resultBlock, 64, 0, 64, c.resultH);
    Target *strip = color_target(c.resultBlock, 1, 0, 64, c.resultH);
    if (rt == nullptr || strip == nullptr) {
        log_once("pgl-flare-query-none", "pgl: a flare query without its screen or result strip: skipped");
        return;
    }
    if (c.ztest >= 2 && zt == nullptr) {
        zt = depth_target(c.zBlock, 0x30u | (c.zpsm & 0x0F), *rt);
        if (zt == nullptr || !zt->tex) {
            log_once("pgl-flare-query-z", "pgl: a flare query without the screen's Z buffer: skipped");
            return;
        }
    }
    if (c.ztest < 2)
        zt = nullptr;
    target_gs_resolution(*strip, false, "holds the flare query results");
    g_flareStrip = strip;                   /* (the glow draws read it: exec_model) */
    const int qx = c.q & 0x3F, qy = c.q >> 6;
    if (qy >= strip->gsH)
        return;

    static CbFlare cb;
    memset(&cb, 0, sizeof(cb));
    /* the copy (flare_copy_visibility): w + 1 x h + 1 screen pixels from the
     * box's origin, 1:1, at most the 64 x 64 work buffer; the reduction
     * (flare_run_reduce) of w x h: none below 2, the prebuilt square one (w
     * on both axes) below 18, else flare_reduce's (both axes in step) */
    const int w = c.box[2], h = c.box[3];
    cb.box[0] = c.box[0];
    cb.box[1] = c.box[1];
    cb.box[2] = std::min(w + 1, 64);
    cb.box[3] = std::min(h + 1, 64);
    FlareStep sx[16], sy[16];
    int nSteps = 0;
    if (w >= 2) {
        int a = std::min(w, 64), b = w < 18 ? a : std::min(h, 64);
        while ((a >= 2 || b >= 2) && nSteps < 16) {
            const int na = flare_half(a), nb = flare_half(b);
            sx[nSteps] = FlareStep{a, na};
            sy[nSteps] = FlareStep{b, nb};
            nSteps++;
            a = na;
            b = nb;
        }
    }
    flare_axis_weights(cb.box[2], sx, nSteps, cb.wx);
    flare_axis_weights(cb.box[3], sy, nSteps, cb.wy);
    cb.screen[0] = c.frameW;
    cb.screen[1] = c.frameH;
    memcpy(cb.rect, c.rect, sizeof(cb.rect));
    cb.nRects = c.nRects;
    cb.ztest = c.ztest;
    cb.cs = (int32_t)c.cs;
    cb.halfY = c.halfY;
    cb.scale[0] = zt != nullptr ? (float)zt->nw / (float)rt->gsW : 1.0f;
    cb.scale[1] = zt != nullptr ? (float)zt->nh / (float)rt->gsH : 1.0f;
    cb.z = (float)c.z * z_norm((uint8_t)c.zpsm);

    ID3D11ShaderResourceView *srv = zt != nullptr ? target_srv(*zt) : nullptr;     /* (a resolve first) */
    d3d_invalidate();
    const D3D11_VIEWPORT vp = {(float)qx * strip->sx, (float)qy * strip->sy, strip->sx, strip->sy, 0, 1};
    queue_pass_state(strip->rtv.get(), vp, raster_state(0, false), blend_plain(gs::WRITE_RGB | gs::WRITE_A),
                     nullptr, depth_state(false, gs::Compare::Always, false));
    bind_vs(Vs::Full);
    bind_ps(Ps::Flare);
    bind_flare_cb(cb);
    queue_pass_draw(srv, nullptr, nullptr);
    d3d_invalidate();
    strip->gen++;
    registry_target_written(*strip, true, true);
    if (g_tracing) {
        pclog::write("pgl trace %u: flare query %d: box %d,%d %dx%d, %d rect(s), ztest %d, Z 0x%06X, Cs 0x%02X;"
                     " screen 0x%X %dx%d (native %dx%d), Z buffer 0x%X (native %dx%d), strip 0x%X %dx%d",
                     g_traceN++, c.q, c.box[0], c.box[1], c.box[2], c.box[3], c.nRects, c.ztest, c.z, c.cs,
                     rt->base, rt->gsW, rt->gsH, rt->nw, rt->nh, zt ? zt->base : 0u, zt ? zt->nw : 0,
                     zt ? zt->nh : 0, strip->base, strip->gsW, strip->gsH);
        flare_trace_depth(c, cb, zt);
    }
}

/* Draws at half strength (pglr_half_blend; docs/renderer.md, "Fields"): the
 * buffer's contents at the begin, and the buffer then. */
Target *g_halfRt = nullptr;
Com<ID3D11Texture2D> g_halfSnap;
Com<ID3D11ShaderResourceView> g_halfSnapSrv;
Com<ID3D11BlendState> g_halfBlend;

/* The begin keeps a copy of the buffer; the end draws that copy over it with
 * 0.5 / 0.5 blending (RGB: the draws' alpha stays, as on the rows they drew
 * on the PS2), so where the draws changed nothing it stays as it is. */
void exec_half_blend(const CmdHalfBlend &c)
{
    flush_batch();
    if (c.begin != 0) {
        g_halfRt = nullptr;
        Target *rt = color_target(c.base, c.fbw, c.psm, c.w, c.h);
        ID3D11Texture2D *src = rt != nullptr && rt->tex ? target_tex1(*rt) : nullptr;     /* (a resolve) */
        if (src == nullptr)
            return;
        D3D11_TEXTURE2D_DESC d, have = {};
        src->GetDesc(&d);
        if (g_halfSnap)
            g_halfSnap->GetDesc(&have);
        if (!g_halfSnap || have.Width != d.Width || have.Height != d.Height || have.Format != d.Format) {
            g_halfSnapSrv.reset();
            g_halfSnap.reset();
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            d.CPUAccessFlags = 0;
            d.MiscFlags = 0;
            d.Usage = D3D11_USAGE_DEFAULT;
            D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
            sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MipLevels = 1;
            if (FAILED(dev()->CreateTexture2D(&d, nullptr, g_halfSnap.put())) ||
                FAILED(dev()->CreateShaderResourceView(g_halfSnap.get(), &sv, g_halfSnapSrv.put()))) {
                g_halfSnap.reset();
                g_halfSnapSrv.reset();
                log_once("pgl-half-blend", "pgl: no copy for draws at half strength: drawn at full strength");
                return;
            }
        }
        ID3D11Texture2D *snap = g_halfSnap.get();
        q::run([snap, src] { qdc()->CopyResource(snap, src); });
        g_halfRt = rt;
        return;
    }
    Target *rt = g_halfRt;
    g_halfRt = nullptr;
    if (rt == nullptr || !rt->rtv)
        return;
    D3D11_TEXTURE2D_DESC have;
    g_halfSnap->GetDesc(&have);
    if ((int)have.Width != rt->nw || (int)have.Height != rt->nh)
        return;                             /* resized meanwhile (target_gs_resolution) */
    if (!g_halfBlend) {
        D3D11_BLEND_DESC bd = {};
        D3D11_RENDER_TARGET_BLEND_DESC &t = bd.RenderTarget[0];
        t.BlendEnable = TRUE;
        t.SrcBlend = D3D11_BLEND_BLEND_FACTOR;
        t.DestBlend = D3D11_BLEND_INV_BLEND_FACTOR;
        t.BlendOp = D3D11_BLEND_OP_ADD;
        t.SrcBlendAlpha = D3D11_BLEND_ZERO;
        t.DestBlendAlpha = D3D11_BLEND_ONE;
        t.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        t.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN |
                                  D3D11_COLOR_WRITE_ENABLE_BLUE;
        if (FAILED(dev()->CreateBlendState(&bd, g_halfBlend.put())))
            return;
    }
    d3d_invalidate();
    const D3D11_VIEWPORT vp = {0, 0, (float)rt->nw, (float)rt->nh, 0, 1};
    const float half[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    queue_pass_state(rt->rtv.get(), vp, raster_state(0, false), g_halfBlend.get(), half,
                     depth_state(false, gs::Compare::Always, false));
    bind_vs(Vs::Full);
    bind_ps(Ps::Present);
    CbPresent cb = {0, 0, 1, 1};
    bind_ps_cb(&cb, sizeof(cb));
    queue_pass_draw(g_halfSnapSrv.get(), nullptr, sampler_state(false, false, 1, 1));
    d3d_invalidate();
    rt->gen++;
    registry_target_written(*rt, true, false);
    if (g_tracing)
        pclog::write("pgl trace %u: the draws since the begin at half strength in 0x%X (%dx%d)", g_traceN++,
                     rt->base, rt->gsW, rt->gsH);
}

/* FXAA over a race view's 3D picture (pglr_post_aa; docs/renderer.md
 * "Anti-aliasing"), when it is the anti-aliasing chosen: the w x h buffer at
 * c.base filtered in place from a copy of it. */
void exec_post_aa(const CmdPostAa &c)
{
    if (settings::anti_aliasing() != settings::AntiAliasing::fxaa)
        return;
    flush_batch();
    Target *t = find_color_target_sized(c.base, c.w, c.h);
    if (t == nullptr || t->gsRes || !t->rtv)
        return;
    ID3D11ShaderResourceView *src = target_copy_srv(*t, true);    /* (all of it: target_copy_srv) */
    if (src == nullptr)
        return;
    d3d_invalidate();
    const D3D11_VIEWPORT vp = {0, 0, (float)t->nw, (float)t->nh, 0, 1};
    queue_pass_state(t->rtv.get(), vp, raster_state(0, false), blend_plain(gs::WRITE_RGB | gs::WRITE_A), nullptr,
                     depth_state(false, gs::Compare::Always, false));
    bind_vs(Vs::Full);
    bind_ps(Ps::Fxaa);
    const CbPresent cb = {1.0f / (float)t->nw, 1.0f / (float)t->nh, 0, 0};
    bind_ps_cb(&cb, sizeof(cb));
    queue_pass_draw(src, nullptr, sampler_state(true, true, 1, 1));
    d3d_invalidate();
    t->gen++;
    registry_target_written(*t, true, false);
    if (g_tracing)
        pclog::write("pgl trace %u: FXAA over the %dx%d picture at 0x%X", g_traceN++, c.w, c.h, c.base);
}

}  /* namespace */

/* ======================================================================== */
/* The execution's CPU profile (pgl_render.h)                               */
/* ======================================================================== */

bool g_profOn;
uint32_t g_profCount[PCOUNT_N];

void prof_push(ProfCat c)
{
    if (g_profDepth < kProfDepth)
        g_profStack[g_profDepth] = ProfOpen{(uint8_t)c, __rdtsc(), 0};
    g_profDepth++;
}

void prof_pop()
{
    if (--g_profDepth >= kProfDepth || g_profDepth < 0) {
        if (g_profDepth < 0)
            g_profDepth = 0;
        return;
    }
    const ProfOpen &o = g_profStack[g_profDepth];
    const uint64_t elapsed = __rdtsc() - o.start;
    g_profTicks[o.cat] += elapsed > o.child ? elapsed - o.child : 0;
    if (g_profDepth > 0 && g_profDepth - 1 < kProfDepth)
        g_profStack[g_profDepth - 1].child += elapsed;
}

/* ======================================================================== */
/* The list                                                                 */
/* ======================================================================== */

void exec_init()
{
    memset(g_addrReg, -1, sizeof(g_addrReg));
    memset(g_addrCtx, 0, sizeof(g_addrCtx));
    memset(g_addrGen, -1, sizeof(g_addrGen));
    for (int i = 0; i < gs::REG_COUNT; i++) {
        g_addrReg[kCtxAddr[i]] = (int8_t)i;
        g_addrCtx[kCtxAddr[i]] = 0;
        g_addrReg[kCtxAddr[i] + 1] = (int8_t)i;
        g_addrCtx[kCtxAddr[i] + 1] = 1;
    }
    for (int i = 0; i < gs::GEN_COUNT; i++)
        g_addrGen[kGenAddr[i]] = (int8_t)i;
    g.gen[gs::GEN_COLCLAMP] = 1;
    g.gen[gs::GEN_PRMODECONT] = 1;
    d3d_init();
    targets_init();
    q::start();                             /* PC: the D3D thread (pgl_queue.h) */
}

void exec_list(const CmdList &list)
{
    for (size_t i = 0; i < list.size(); i++) {
        const void *p = list.payload(i);
        g_stats.cmds[list.type(i) & 31]++;
        const ProfCat cat = prof_cat(list.type(i));
        ProfScope prof(cat);                        /* (the verbose monitor's profile) */
        if (cat == PROF_GS || cat == PROF_TARGET_SET)
            g_profCount[PCOUNT_GSCMDS]++;
        else if (cat == PROF_UPLOAD)
            g_profCount[PCOUNT_UPLOADS]++;
        switch (list.type(i)) {
        case CMD_REGS:
            exec_regs(*static_cast<const CmdRegs *>(p));
            break;
        case CMD_AD: {
            const CmdAd &c = *static_cast<const CmdAd *>(p);
            write_reg(c.addr, c.value);
            break;
        }
        case CMD_TARGET: {
            const CmdTarget &c = *static_cast<const CmdTarget *>(p);
            flush_batch();
            target_size(c.base, c.fbw, c.psm, c.w, c.h);
            g_dirty = true;
            break;
        }
        case CMD_PRIM:
            exec_prim(*static_cast<const CmdPrim *>(p));
            break;
        case CMD_GIF:
            exec_gif(*static_cast<const CmdGif *>(p));
            break;
        case CMD_STRIP:
            exec_strip(*static_cast<const CmdStrip *>(p));
            break;
        case CMD_TRI16:
            exec_tri16(*static_cast<const CmdTri16 *>(p));
            break;
        case CMD_MODEL:
            exec_model(*static_cast<const CmdModel *>(p));
            break;
        case CMD_UPLOAD: {
            const CmdUpload &c = *static_cast<const CmdUpload *>(p);
            flush_batch();
            registry_upload(c.dbp, c.dbw, c.dpsm, c.dx, c.dy, c.w, c.h, c.data,
                            gstex::image_bytes(c.dpsm, (uint32_t)c.w, (uint32_t)c.h));
            g_dirty = true;
            break;
        }
        case CMD_TEX1_UPLOAD: {
            const CmdTex1 &c = *static_cast<const CmdTex1 *>(p);
            flush_batch();
            registry_tex1_upload(c.img, c.vramBase);
            g_dirty = true;
            break;
        }
        case CMD_TEX_SOURCE: {
            /* PC: a VRAM-less set's texture (ROADMAP 18; the binding found again) */
            const CmdTexSource &c = *static_cast<const CmdTexSource *>(p);
            flush_batch();
            registry_texture_source(c.img, c.vramBase, c.tex0);
            g_dirty = true;
            break;
        }
        case CMD_READBACK:
            flush_batch();
            readback(*static_cast<const CmdReadback *>(p));
            g_dirty = true;
            break;
        case CMD_SUBCALL: {
            const CmdList *s = sub_list(static_cast<const CmdSubcall *>(p)->key);
            if (s != nullptr && g_subDepth < 4) {
                g_subDepth++;
                exec_list(*s);
                g_subDepth--;
            }
            break;
        }
        case CMD_PFX_COLOR:
            exec_pfx_color(*static_cast<const CmdPfxColor *>(p));
            g_dirty = true;
            break;
        case CMD_PFX_SPLIT:
            exec_pfx_split(*static_cast<const CmdPfxSplit *>(p));
            g_dirty = true;
            break;
        case CMD_FLARE_QUERY:
            exec_flare_query(*static_cast<const PglrFlareQuery *>(p));
            g_dirty = true;
            break;
        case CMD_HALF_BLEND:
            exec_half_blend(*static_cast<const CmdHalfBlend *>(p));
            g_dirty = true;
            break;
        case CMD_DESIGN: {
            const CmdDesign &c = *static_cast<const CmdDesign *>(p);
            g_designX = c.kx;
            g_designY = c.ky;
            break;
        }
        case CMD_LINE_WIDTH: {
            const float px = static_cast<const CmdLineWidth *>(p)->px;
            if (px != g_lineWidth) {
                flush_batch();          /* (the lines so far at their width; the next state's topology) */
                g_lineWidth = px;
                g_dirty = true;
            }
            break;
        }
        case CMD_POST_AA:
            exec_post_aa(*static_cast<const CmdPostAa *>(p));
            g_dirty = true;
            break;
        case CMD_GPU_SECTION:
            if (sec_active()) {
                flush_batch();
                sec_mark(static_cast<const CmdSection *>(p)->id);
            }
            break;
        }
    }
}

/* ======================================================================== */
/* GPU time by section (PC: GT3PC_GPU_SECTIONS=N)                           */
/* ======================================================================== */

namespace {

/* A frame's timestamps at its section marks (pglr_gpu_section), read back
 * kSecFrames frames later without waiting (a frame not ready by then is
 * dropped); the means of every N frames logged. */
constexpr int kSecMarks = 48, kSecFrames = 4;
struct SecFrame {
    Com<ID3D11Query> disjoint;
    Com<ID3D11Query> ts[kSecMarks];
    int id[kSecMarks] = {};
    int n = 0;
    bool pending = false;
    uint32_t seq = 0;                   /* the frame monitor's frame (frame_monitor::gpu_sections) */
};
SecFrame g_sec[kSecFrames];
int g_secCur;
int g_secEvery = -1;                    /* -1 not read yet, 0 off */
bool g_secActive;                       /* this frame is timed (sec_begin_frame .. sec_end_frame) */
double g_secSum[PGLR_SEC_COUNT];
uint32_t g_secFrames;
const char *const kSecNames[PGLR_SEC_COUNT] = {"other", "scene", "effects", "copy", "replay blur", "weather",
                                               "smoke", "flare", "post"};

/* Timed: GT3PC_GPU_SECTIONS (logged), or the frame monitor's verbose view
 * (shown there: platform/frame_monitor.h) */
bool sec_on()
{
    if (g_secEvery < 0) {
        const char *e = getenv("GT3PC_GPU_SECTIONS");
        g_secEvery = (e != nullptr && atoi(e) > 0) ? atoi(e) : 0;
        if (g_secEvery > 0)
            pclog::write("pgl: GPU time by section, logged every %d frames (GT3PC_GPU_SECTIONS)", g_secEvery);
        frame_monitor::gpu_section_names(kSecNames, PGLR_SEC_COUNT);
    }
    return g_secEvery > 0 || frame_monitor::mode() == frame_monitor::MODE_VERBOSE;
}

bool sec_active()
{
    return g_secActive;
}

void sec_collect(SecFrame &f)
{
    if (!f.pending)
        return;
    f.pending = false;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
    if (dc()->GetData(f.disjoint.get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || dj.Disjoint ||
        dj.Frequency == 0)
        return;
    UINT64 t[kSecMarks];
    for (int i = 0; i < f.n; i++)
        if (dc()->GetData(f.ts[i].get(), &t[i], sizeof(t[i]), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            return;
    float frameMs[PGLR_SEC_COUNT] = {};
    for (int i = 0; i + 1 < f.n; i++)
        if (f.id[i] >= 0 && f.id[i] < PGLR_SEC_COUNT)
            frameMs[f.id[i]] += (float)((double)(t[i + 1] - t[i]) * 1000.0 / (double)dj.Frequency);
    frame_monitor::gpu_sections(f.seq, frameMs, PGLR_SEC_COUNT);
    if (g_secEvery <= 0)
        return;
    for (int s = 0; s < PGLR_SEC_COUNT; s++)
        g_secSum[s] += frameMs[s];
    if (++g_secFrames < (uint32_t)g_secEvery)
        return;
    char line[512];
    int at = snprintf(line, sizeof(line), "pgl gpu sections (ms a frame, mean of %u):", g_secFrames);
    double total = 0.0;
    for (int s = 0; s < PGLR_SEC_COUNT; s++) {
        const double ms = g_secSum[s] / g_secFrames;
        total += ms;
        if (ms >= 0.005 && at < (int)sizeof(line))
            at += snprintf(line + at, sizeof(line) - at, " %s %.2f,", kSecNames[s], ms);
    }
    if (at < (int)sizeof(line))
        snprintf(line + at, sizeof(line) - at, " total %.2f", total);
    pclog::write("%s", line);
    memset(g_secSum, 0, sizeof(g_secSum));
    g_secFrames = 0;
}

void sec_mark(int id)
{
    SecFrame &f = g_sec[g_secCur];
    if (f.n >= kSecMarks)
        return;
    if (!f.ts[f.n]) {
        D3D11_QUERY_DESC d = {D3D11_QUERY_TIMESTAMP, 0};
        if (FAILED(dev()->CreateQuery(&d, f.ts[f.n].put())))
            return;
    }
    ID3D11Query *ts = f.ts[f.n].get();
    q::run([ts] { qdc()->End(ts); });
    f.id[f.n] = id;
    f.n++;
}

void sec_begin_frame()
{
    g_secActive = false;
    if (!sec_on())
        return;
    SecFrame &f = g_sec[g_secCur];
    sec_collect(f);
    f.n = 0;
    if (!f.disjoint) {
        D3D11_QUERY_DESC d = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        if (FAILED(dev()->CreateQuery(&d, f.disjoint.put())))
            return;
    }
    ID3D11Query *dj = f.disjoint.get();
    q::run([dj] { qdc()->Begin(dj); });
    f.seq = frame_monitor::frame_seq();
    g_secActive = true;
    sec_mark(PGLR_SEC_OTHER);
}

void sec_end_frame()
{
    /* (as the frame began: the verbose view may have changed meanwhile) */
    if (!g_secActive)
        return;
    g_secActive = false;
    SecFrame &f = g_sec[g_secCur];
    sec_mark(PGLR_SEC_COUNT);           /* (the end) */
    ID3D11Query *dj = f.disjoint.get();
    q::run([dj] { qdc()->End(dj); });
    f.pending = true;
    g_secCur = (g_secCur + 1) % kSecFrames;
}

}  /* namespace */

double g_execSeconds;

void exec_begin_frame()
{
    {
        LARGE_INTEGER c, f;
        QueryPerformanceCounter(&c);
        QueryPerformanceFrequency(&f);
        g_execSeconds = (double)c.QuadPart / (double)f.QuadPart;
    }
    /* the CPU profile: in the frames of the verbose monitor */
    g_profOn = frame_monitor::mode() == frame_monitor::MODE_VERBOSE;
    g_profDepth = 0;
    memset(g_profTicks, 0, sizeof(g_profTicks));
    memset(g_profCount, 0, sizeof(g_profCount));
    if (g_profOn) {
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        g_profQpc0 = q.QuadPart;
        g_profTsc0 = __rdtsc();
    }
    sec_begin_frame();
    d3d_invalidate();
    g_dirty = true;
    g_bs.valid = false;
    g_designX = g_designY = 1.0f;
    g_lineWidth = 1.0f;
    g_flareStrip = nullptr;
    if (g_traceOn < 0) {
        const char *e = getenv("GT3PC_PGL_TRACE");
        g_traceOn = (e != nullptr && atoi(e) > 0) ? 1 : 0;
        if (g_traceOn == 1)
            pclog::write("pgl: F9 logs the draws of the next frame (GT3PC_PGL_TRACE)");
    }
    if (g_traceOn == 1) {
        /* F9: the Trace hotkey (gt3pc.ini [Hotkeys]) */
        if (window::take_hotkey(input_map::kHotTrace)) {
            g_tracing = true;
            g_traceN = 0;
            g_traceFrames = 2;
            g_flareProbeN = 0;
            g_probe.last.clear();
            pclog::write("pgl trace: frame %llu begins", (unsigned long long)frame_number());
            probe_on();
        } else if (g_traceNext) {
            /* the second traced frame: unless GT3PC_PGL_PROBE set points,
             * the centres of the flare queries the shape passed in */
            g_traceNext = false;
            g_tracing = true;
            g_traceN = 0;
            g_probe.last.clear();
            pclog::write("pgl trace: frame %llu begins (the second traced frame)", (unsigned long long)frame_number());
            if (!probe_on() && g_flareProbeN > 0) {
                g_probe.n = g_flareProbeN;
                for (int i = 0; i < g_flareProbeN; i++) {
                    g_probe.x[i] = g_flareProbeX[i];
                    g_probe.y[i] = g_flareProbeY[i];
                    pclog::write("pgl trace: probing (%d,%d), the centre of a flare query %u pixels of whose shape"
                                 " passed", g_flareProbeX[i], g_flareProbeY[i], g_flareProbePassed[i]);
                }
                g_probe.state = 1;
                g_probeFromFlares = true;
            }
        }
    }
}

void exec_end_frame()
{
    flush_batch();
    sec_end_frame();
    g_bs.valid = false;
    g_dirty = true;
    g_dateRun = DateRun();      /* (targets may go at next_frame) */
    g_halfRt = nullptr;         /* (a half-strength run left open) */
    if (g_tracing) {
        pclog::write("pgl trace: frame %llu ends (%u lines)", (unsigned long long)frame_number(), g_traceN);
        if (g_traceFrames == 2)
            trace_dump_next(frame_number());    /* (its picture at its present) */
        g_tracing = false;
        if (--g_traceFrames > 0) {
            g_traceNext = true;
        } else if (g_probeFromFlares) {
            g_probe.n = 0;
            g_probe.state = 0;
            g_probeFromFlares = false;
        }
    }
    q::run([] {
        ID3D11ShaderResourceView *none[2] = {nullptr, nullptr};
        qdc()->PSSetShaderResources(0, 2, none);
        qdc()->OMSetRenderTargets(0, nullptr, nullptr);
    });
    /* the D3D thread's time and the executor's waits for it (all frames:
     * the counts start over) */
    double d3dMs = 0.0, syncMs = 0.0;
    uint32_t syncs = 0;
    q::take_stats(&d3dMs, &syncMs, &syncs);
    if (g_statsEvery < 0) {
        const char *e = getenv("GT3PC_PGL_STATS");
        g_statsEvery = (e != nullptr && atoi(e) > 0) ? atoi(e) : 0;
    }
    if (g_statsEvery > 0 && (frame_number() % (uint64_t)g_statsEvery) == 0) {
        const Stats &s = g_stats;
        pclog::write("pgl stats frame %llu: regs %u ad %u target %u prim %u gif %u strip %u tri16 %u model %u"
                     " upload %u tex1 %u readback %u sub %u pfx %u/%u | draws %u verts %u | skipped:"
                     " nopass %u target %u scissor %u tex %u | target copies whole %u, partial %u",
                     (unsigned long long)frame_number(), s.cmds[CMD_REGS], s.cmds[CMD_AD], s.cmds[CMD_TARGET],
                     s.cmds[CMD_PRIM], s.cmds[CMD_GIF], s.cmds[CMD_STRIP], s.cmds[CMD_TRI16], s.cmds[CMD_MODEL],
                     s.cmds[CMD_UPLOAD], s.cmds[CMD_TEX1_UPLOAD], s.cmds[CMD_READBACK], s.cmds[CMD_SUBCALL],
                     s.cmds[CMD_PFX_COLOR], s.cmds[CMD_PFX_SPLIT], s.draws, s.verts, s.skipNoPass, s.skipTarget,
                     s.skipScissor, s.skipTex, g_copiesWhole, g_copiesPart);
    }
    if (g_profOn) {
        /* the profile into the frame monitor: ticks as ms by the frame's
         * own TSC / QPC ratio */
        LARGE_INTEGER q, f;
        QueryPerformanceCounter(&q);
        QueryPerformanceFrequency(&f);
        const uint64_t ticks = __rdtsc() - g_profTsc0;
        const double ms = (double)(q.QuadPart - g_profQpc0) * 1000.0 / (double)f.QuadPart;
        float out[PROF_COUNT];
        for (int c = 0; c < PROF_COUNT; c++)
            out[c] = ticks > 0 ? (float)((double)g_profTicks[c] * ms / (double)ticks) : 0.0f;
        g_profCount[PCOUNT_COPIES] = g_copiesWhole + g_copiesPart;
        g_profCount[PCOUNT_SYNCS] = syncs;
        static bool named;
        if (!named) {
            frame_monitor::exec_profile_names(kProfNames, PROF_COUNT, kProfCountNames, PCOUNT_N);
            named = true;
        }
        frame_monitor::exec_profile(out, PROF_COUNT, g_profCount, PCOUNT_N);
        frame_monitor::exec_d3d_thread(q::running() ? (float)d3dMs : -1.0f, (float)syncMs);
        g_profOn = false;
    }
    memset(&g_stats, 0, sizeof(g_stats));
    g_copiesWhole = g_copiesPart = 0;
    if (!g_flareGlows.empty()) {
        if (flare_log(true))
            pclog::write("pgl flare: frame %llu: glows (draw order), texel R:%s", (unsigned long long)frame_number(),
                         g_flareGlows.c_str());
        g_flareGlows.clear();
    }
    registry_end_frame();
    models_end_frame();
    if (flicker_on())
        flicker_end_frame(frame_number());
    next_frame();
}

}  /* namespace pglr */
