/* racebench - a 32-car race frame through the real renderer (gt3_render.lib: the executor, its
 * thread, the D3D thread, the registry, the models, the shaders) on a windowless D3D device.
 *
 * The cars are real: car model files of data_US/cars/day, relocated as the game does (mdldump's
 * modelset_relocate / vif_model_relocate, texdump's tex1_relocate), their command streams
 * interpreted for the packets and matrices (LOD by distance).  Each frame, as in a race, every car's
 * Tex1 set is relinked to another VRAM place (tex1_set_vram_base) and uploaded (pglr_tex1_upload),
 * a quarter of the cars' lamp CLUTs written again, then each part's texture registers are written
 * (TEX0 with its CLUT load, TEX1, CLAMP) and the part recorded (pglr_model_part); the cars are drawn
 * first into a small reflection buffer (LOD 2), as the wet road does, then into the 640 x 448 view.
 * Unlit, decal textures, no fog: the picture is the textures' (made-up lights draw black).
 *
 *   racebench [frames] [--cars DIR] [--still] [--sync] [--trace] [--pixels FILE] [--hires] [--date N]
 *             [--states] [--stream N] [--repeat N] [--far] [--onetex] [--newpal] [--materials]
 *             [--aa NAME] [--scale N] [--shot N [--shotfile FILE]]
 *
 * --far: the race cameras' frustum (pgl_frustum, near 0.1, far 750: z / w -1 .. 1) and the rows 55 m
 * apart (to ~400 m, a 32-car field strung out) - the parts far away, as the clipper's depth test
 * meets them in a race; --repeat N: the view's cars drawn N times (more draws for the GPU); --onetex:
 * every part of a car with its set's first texture (a measurement: what the texture changes cost);
 * --newpal: the animated CLUTs get new contents every time (lamps fading: their textures decoded
 * again with each new palette, as in a race), not two states; --materials: a part's lighting block
 * differs by its material, as the game's do (the parts of a car change materials every few parts:
 * the constant-buffer writes and the merged draws' ends of a race, before the lights' ring);
 * --freeze: the cars where frame 0 put them in every frame (with --still every frame is the same:
 * the traced frame is the picture's, for GT3PC_PGL_PROBE).
 * --states: the render states of the cars' command streams (modelset_exec 0x19 .. 0x2F: FBMSK, the
 * alpha and DATE tests, ALPHA, depth writes) sent as the game sends them - the real DATE pattern;
 * --stream N: car N's LOD 0 command stream printed (its states and packets).
 * --hires: a 1920 x 1080 screen at a 150 % resolution scale (the targets at 3.375 x 3.616, as the
 * user plays) - the GPU's share; --date N: every N-th textured part drawn as the game draws the car
 * bodies, its alpha alone (FBMSK 0x00FFFFFF), then with DATE (a copy of the target before it:
 * ~440 a frame at N = 8, as a 32-car race).  GT3PC_PGL_STATS=N logs the GPU time (`pgl time`).
 * --cars: the car model files (default build\bin\data_US\cars\day under the current folder: run it
 * from pc_enhanced); --still: no CLUT changes; --sync: each frame waited for; --trace: the F9 trace
 * of the third last frame (trace_frame_*.bmp in the current folder); --pixels: the last frame's
 * colour buffer read back and written (raw RGBA, 640 x 448) and its hash printed - two builds'
 * hashes must agree.  Prints the frame time over the frames after 60 of warm-up, and the executor's
 * profile every 100 frames (GT3PC_PGL_PROFILE, 0 off); GT3PC_SAMPLE=N samples its threads.
 * Built against the tree's build\src\gt3_render.lib by build.cmd (docs/renderer.md 9, "Benchmark").
 * Stubbed: gpu:: (a device, no window or swap chain: the game may be open), the log (stdout),
 * four settings getters, the frame monitor, the menus' presents. */
#include "engine/pgl/native/pgl_bridge.h"
#include "engine/pgl/native/gs_tex1.h"
#include "engine/pgl/native/vif_model.h"
#include "platform/frame_monitor.h"
#include "platform/frame_step.h"
#include "platform/gpu.h"
#include "platform/input_map.h"
#include "platform/log.h"
#include "platform/settings.h"
#include "engine/pgl/native/pgl_texrepl.h"   /* PC (2026-11-13): the texture replacements' status */
#include "platform/settings_menu.h"
#include "platform/system_menu.h"

#include <windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "synchronization.lib")

static ID3D11Device *g_dev;
static ID3D11DeviceContext *g_ctx;
static ID3D11Texture2D *g_bbTex;
static ID3D11RenderTargetView *g_bb;
static void (*g_guard)();
/* the screen (--hires: 1920 x 1080 at a 150 % resolution scale, the user's) */
static int g_screenW = 640, g_screenH = 448, g_renderScale = 100;

namespace gpu {
bool init(HWND) { return true; }
ID3D11Device *device() { return g_dev; }
IDXGIAdapter *adapter() { return nullptr; } /* PC (2026-11-07): no DXGI figures in the memory line */
ID3D11DeviceContext *context()
{
    if (g_guard != nullptr)
        g_guard();
    return g_ctx;
}
void set_context_guard(void (*f)()) { g_guard = f; }
ID3D11RenderTargetView *backbuffer() { return g_bb; }
void backbuffer_size(int *w, int *h) { *w = g_screenW; *h = g_screenH; }
void leave_fullscreen() {}
int screen_count() { return 1; }
ID3D11RenderTargetView *screen_backbuffer(int) { return g_bb; }
void screen_size(int, int *w, int *h) { *w = g_screenW; *h = g_screenH; }
void canvas_size(int *w, int *h) { *w = g_screenW; *h = g_screenH; }
void canvas_rect(int, int *x, int *w) { *x = 0; *w = g_screenW; }
bool span_active() { return false; }
void want_split_screen() {}
void present() {}
void set_present_interval(int) {}
void present_info(int *i, bool *t, bool *e) { *i = 0; *t = false; *e = false; }
double refresh_hz() { return 60.0; }
}

static volatile LONG g_traceNow;            /* --trace: the F9 trace of one frame */
namespace window {
bool take_hotkey(int h)
{
    return h == input_map::kHotTrace && InterlockedExchange(&g_traceNow, 0) != 0;
}
}
namespace osd {
void present(ID3D11RenderTargetView *) {}
}
namespace song_osd {
void present(ID3D11RenderTargetView *) {}
}
namespace cockpit_osd {                     /* PC (2026-11-08): the cockpit calibration's panel */
void present(ID3D11RenderTargetView *) {}
}
namespace challenge_osd {                   /* PC (2026-11-14): the challenge modes' panel */
void present(ID3D11RenderTargetView *) {}
}
/* PC (2026-11-15): --shot N: a screenshot (platform/screenshot.h) asked for before frame N is recorded (the
 * executor takes it at the next present it runs: frame N or one before it - --still --freeze for a picture to
 * compare with --pixels'); the picture handed over written raw (R, G, B, A bytes) to --shotfile (default
 * build\tools\racebench\shot.rgba), its size and its hash (--pixels' hash, over its 32-bit pixels) printed */
static int g_shotFrame = -1;
static volatile LONG g_shotAsk;
static const char *g_shotFile = "build\\tools\\racebench\\shot.rgba";
namespace screenshot {
bool take_request() { return InterlockedExchange(&g_shotAsk, 0) != 0; }
void failed(const char *why) { printf("racebench: screenshot not taken: %s\n", why); }
void save(std::vector<uint8_t> &&px, int w, int h)
{
    uint64_t sum = 0;
    size_t lit = 0;
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
        uint32_t p;
        memcpy(&p, &px[i], 4);
        sum = sum * 1099511628211ull ^ p;
        lit += (p & 0xFFFFFF) != 0;
    }
    FILE *o = fopen(g_shotFile, "wb");
    if (o != nullptr) {
        fwrite(px.data(), 1, px.size(), o);
        fclose(o);
    }
    printf("racebench: screenshot %dx%d, hash %016llX, %zu pixels not black, written raw to %s\n", w, h,
           (unsigned long long)sum, lit, o != nullptr ? g_shotFile : "(nowhere: not written)");
}
}
namespace crash {
void describe_address(const void *, char *out, unsigned n)
{
    if (n > 0)
        out[0] = 0;
}
}
extern "C" void *pc_malloc(size_t n) { return malloc(n); }
extern "C" void pc_free(void *p) { free(p); }
extern "C" int pc_printf(const char *fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    const int r = vprintf(fmt, a);
    va_end(a);
    return r;
}

/* PC (2026-11-13): the texture replacements' options (the stubs below) */
static std::vector<std::string> g_texPacks;
static std::string g_texRoot = "build\\tools\\racebench\\tex_replacements", g_texCache;
static bool g_texDump, g_texRgba, g_texLog;
static int g_texSelect;     /* --texselect N: Select mode, the interface open, the N-th texture of its list chosen and dumped */
static bool g_texOff;               /* (--textoggle: the replacements turned off) */
static int g_texToggle;             /* --textoggle N: half way the replacements off (1), and 20 frames later on again (2) */
static std::string g_texRefresh;    /* --texrefresh DIR: half way, DIR's files copied into the first --texpack folder
                                       (NAME.gone: its file NAME deleted there), then a refresh */

/* PC (2026-11-15): --aa off | fxaa | msaa2 .. msaa8 | agss2 .. agss8 ([Graphics] AntiAliasing; default off, as
 * before) */
static settings::AntiAliasing g_benchAa = settings::AntiAliasing::off;
namespace settings {
AntiAliasing anti_aliasing() { return g_benchAa; }
int msaa_samples(AntiAliasing aa)
{
    switch (aa) {
    case AntiAliasing::msaa2: case AntiAliasing::agss2: return 2;
    case AntiAliasing::msaa4: case AntiAliasing::agss4: return 4;
    case AntiAliasing::msaa8: case AntiAliasing::agss8: return 8;
    default: return 1;
    }
}
bool per_sample_shading(AntiAliasing aa)
{
    return aa == AntiAliasing::agss2 || aa == AntiAliasing::agss4 || aa == AntiAliasing::agss8;
}
int render_scale() { return g_renderScale; }
bool debug_sync_log() { return false; }
int car_reflection_min_px() { return 0; }       /* PC (2026-11-05): [Graphics] CarReflections=all - every env pass drawn */
EnvMapDetail env_map_detail() { return EnvMapDetail::scaled; }   /* PC (2026-11-06): [Graphics] EnvMapDetail=scaled - every target at the scale */
int anisotropic_filtering() { return 1; }       /* PC (2026-11-10): [Graphics] AnisotropicFiltering=off (GT3PC_PGL_ANISO=N tries it) */
/* PC (2026-11-13): [Textures] (pgl_texrepl.h) from the command line: --texdump DIR (Dump=all, DIR the
 * tex_replacements folder), --texpack DIR (a replacement folder, the first given wins; Replace=on),
 * --texcache DIR (PreCache=on, there), --texrgba (Compress=off), --texlog (each file loaded logged),
 * --texselect N (Dump=select with the interface open: the N-th texture of its list chosen - drawn green - and
 * dumped into the --texdump folder's dumps_selected), --texrefresh DIR (half way through, DIR's files copied
 * over the first --texpack folder's - a file NAME.gone deletes NAME there - and the textures refreshed),
 * --textoggle N (half way through the replacements turned off; N 2: on again 20 frames later) */
bool texture_replace() { return !g_texPacks.empty() && !g_texOff; }
TextureDump texture_dump() { return g_texSelect > 0 ? TextureDump::select : g_texDump ? TextureDump::all : TextureDump::off; }
bool texture_precache() { return !g_texCache.empty(); }
bool texture_compress() { return !g_texRgba; }
int texture_pack_count() { return (int)g_texPacks.size(); }
void texture_pack(int i, char *out, int n)
{
    snprintf(out, (size_t)n, "%s", i >= 0 && i < (int)g_texPacks.size() ? g_texPacks[(size_t)i].c_str() : "");
}
void texture_root(char *out, int n) { snprintf(out, (size_t)n, "%s\\", g_texRoot.c_str()); }
void texture_cache(char *out, int n)
{
    snprintf(out, (size_t)n, "%s\\", g_texCache.empty() ? "build\\tools\\racebench\\texcache" : g_texCache.c_str());
}
bool debug_file_log() { return g_texLog; }
}

/* PC (2026-11-07): the memory line of the decoding worker's report (platform/memstat.h): none here */
namespace memstat {
void line(char *out, size_t n, bool)
{
    if (n > 0)
        snprintf(out, n, "(not measured in the benchmark)");
}
uint32_t virtual_mb() { return 0; }
}

namespace frame_monitor {
Mode mode() { return MODE_OFF; }
uint32_t frame_seq() { return 0; }
void add(Part, double) {}
void exec_d3d_thread(float, float) {}
void exec_submit_thread(float, float) {}        /* PC (2026-11-05): the submit thread's line */
void exec_profile(const float *, int, const uint32_t *, int) {}
void exec_profile_names(const char *const *, int, const char *const *, int) {}
void exec_thread(float) {}
void gpu_section_names(const char *const *, int) {}
void gpu_sections(uint32_t, const float *, int) {}
void gpu_time(uint32_t, double) {}
void output_begin() {}
void present(ID3D11RenderTargetView *) {}
}
namespace frame_step {
bool paused() { return false; }
void present(ID3D11RenderTargetView *) {}
}
namespace settings_menu {
void present(ID3D11RenderTargetView *) {}
}
namespace system_menu {
void present(ID3D11RenderTargetView *) {}
}
namespace texture_menu {                        /* PC (2026-11-13): the texture interface: none here */
void present(ID3D11RenderTargetView *) {}
void set_backend(const Backend *) {}
}

static bool g_quiet;
namespace pclog {
void write(const char *fmt, ...)
{
    if (g_quiet)
        return;
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
    printf("\n");
}
void vwrite(const char *fmt, va_list ap)
{
    if (g_quiet)
        return;
    vprintf(fmt, ap);
    printf("\n");
}
[[noreturn]] void fatal(const char *fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    printf("FATAL: ");
    vprintf(fmt, a);
    va_end(a);
    printf("\n");
    fflush(stdout);
    ExitProcess(9);
}
}

/* ---- the files (mdldump's / texdump's relocation) ---------------------------------------------- */

typedef uint32_t Addr;
static inline uint8_t *P(Addr a) { return (uint8_t *)(uintptr_t)a; }
static inline Addr A(const void *p) { return (Addr)(uintptr_t)p; }
static inline uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline int16_t rds16(const uint8_t *p) { return (int16_t)rd16(p); }
static inline float rdf(const uint8_t *p) { float v; memcpy(&v, p, 4); return v; }
static inline void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static inline void reloc(uint8_t *field, int32_t delta) { wr32(field, rd32(field) + (uint32_t)delta); }

struct ModelSetView {
    uint8_t *m;
    uint16_t nParts() const { return rd16(m + 0x10); }
    uint16_t nPackets() const { return rd16(m + 0x12); }
    uint16_t nMaterials() const { return rd16(m + 0x14); }
    Addr parts() const { return rd32(m + 0x20); }
    Addr packets() const { return rd32(m + 0x24); }
    Addr materials() const { return rd32(m + 0x28); }
    uint8_t *part(int i) const { Addr t = parts(); return t ? P(rd32(P(t) + 4 * i)) : nullptr; }
    uint8_t *packet(int i) const { Addr t = packets(); return t ? P(rd32(P(t) + 4 * i)) : nullptr; }
};

static void vifmodel_relocate(uint8_t *vm)
{
    int32_t delta = (int32_t)(A(vm) - rd32(vm));
    wr32(vm, A(vm));
    if (rd32(vm + 4) != 0)
        reloc(vm + 4, delta);
    uint16_t n = rd16(vm + 0x0A);
    for (int i = 0; i < n; i++)
        reloc(vm + 0x10 + 8 * i, delta);
}

static void modelset_relocate(uint8_t *m)
{
    int32_t delta = (int32_t)(A(m) - rd32(m + 4));
    if (delta == 0)
        return;
    wr32(m + 4, A(m));
    ModelSetView v = {m};
    if (rd32(m + 0x20) != 0) {
        reloc(m + 0x20, delta);
        for (int i = 0; i < v.nParts(); i++) {
            uint8_t *f = P(v.parts()) + 4 * i;
            if (rd32(f) != 0)
                reloc(f, delta);
        }
    }
    if (rd32(m + 0x24) != 0) {
        reloc(m + 0x24, delta);
        for (int i = 0; i < v.nPackets(); i++) {
            uint8_t *f = P(v.packets()) + 4 * i;
            if (rd32(f) != 0) {
                reloc(f, delta);
                vifmodel_relocate(P(rd32(f)));
            }
        }
    }
    if (rd32(m + 0x28) != 0) {
        reloc(m + 0x28, delta);
        for (int i = 0; i < v.nMaterials(); i++) {
            uint8_t *f = P(v.materials()) + 0x50 * i + 0x48;
            if (rd32(f) != 0)
                reloc(f, delta);
        }
    }
    if (rd32(m + 0x30) != 0)
        reloc(m + 0x30, delta);
}

/* tex1_relocate (texdump memtest) */
static void tex1_relocate(uint8_t *img)
{
    uint32_t self = rd32(img + 4);
    uint32_t delta = (uint32_t)(uintptr_t)img - self;
    auto rl = [&](uint8_t *field) {
        uint32_t v = rd32(field);
        if (v != 0) {
            v += delta;
            memcpy(field, &v, 4);
        }
    };
    wr32(img + 4, (uint32_t)(uintptr_t)img);
    rl(img + 0x08);
    rl(img + 0x18);
    rl(img + 0x1C);
    rl(img + 0x20);
    uint32_t nXfer = rd16(img + 0x16);
    uint8_t *xfer = P(rd32(img + 0x1C));
    for (uint32_t i = 0; xfer != nullptr && i < nXfer; i++)
        rl(xfer + i * 0xC);
    uint8_t *tbl = P(rd32(img + 0x20));
    for (uint32_t i = 0; tbl != nullptr && i < rd32(tbl); i++)
        rl(tbl + 4 + 4 * i);
    rl(img + 0x24);
    uint8_t *anim = P(rd32(img + 0x24));
    if (anim != nullptr) {
        rl(anim + 4);
        uint8_t *ch = P(rd32(anim + 4));
        for (uint32_t i = 0; ch != nullptr && i < rd16(anim); i++, ch += 0x20) {
            rl(ch + 8);
            rl(ch + 12);
            uint8_t *keys = P(rd32(ch + 12));
            for (uint32_t k = 0; keys != nullptr && k < rd16(ch + 6); k++)
                rl(keys + 8 * k + 4);
        }
    }
}

/* tex1_set_vram_base (pgl_tex1.cpp): TBP0, CBP (14 bits), MIPTBPs, every DBP get the delta */
static void tex1_set_vram_base(uint8_t *img, uint32_t base)
{
    const int32_t delta = (int32_t)(base - rd16(img + 0x10));
    if (delta == 0)
        return;
    const uint16_t b16 = (uint16_t)base;
    memcpy(img + 0x10, &b16, 2);
    const uint16_t nTex = rd16(img + 0x14);
    uint8_t *tex = P(rd32(img + 0x18));
    for (int i = 0; tex != nullptr && i < nTex; i++) {
        uint64_t t0;
        memcpy(&t0, tex + 0x28 * i, 8);
        const uint64_t tbp = ((t0 & 0x3FFF) + (uint64_t)delta) & 0x3FFF;
        const uint64_t cbp = (((t0 >> 37) & 0x3FFF) + (uint64_t)delta) & 0x3FFF;
        t0 = (t0 & ~0x3FFFull & ~(0x3FFFull << 37)) | tbp | cbp << 37;
        memcpy(tex + 0x28 * i, &t0, 8);
        uint64_t t1;
        memcpy(&t1, tex + 0x28 * i + 8, 8);
        if (((t1 >> 2) & 7) != 0) {
            for (int k = 0; k < 2; k++) {
                uint64_t m;
                memcpy(&m, tex + 0x28 * i + 0x10 + 8 * k, 8);
                for (int s = 0; s < 3; s++) {
                    const uint64_t f = (((m >> (20 * s)) & 0x3FFF) + (uint64_t)delta) & 0x3FFF;
                    m = (m & ~(0x3FFFull << (20 * s))) | f << (20 * s);
                }
                memcpy(tex + 0x28 * i + 0x10 + 8 * k, &m, 8);
            }
        }
    }
    const uint16_t nXfer = rd16(img + 0x16);
    uint8_t *xfer = P(rd32(img + 0x1C));
    for (int i = 0; xfer != nullptr && i < nXfer; i++) {
        uint16_t dbp = rd16(xfer + 0xC * i + 4);
        dbp = (uint16_t)(dbp + delta);
        memcpy(xfer + 0xC * i + 4, &dbp, 2);
    }
}

/* ---- matrices (row vectors, v' = v M) ------------------------------------------------------------- */

struct Mat { float m[4][4]; };
static Mat identity()
{
    Mat r = {};
    for (int i = 0; i < 4; i++)
        r.m[i][i] = 1.0f;
    return r;
}
static Mat mul(const Mat &a, const Mat &b)
{
    Mat r = {};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 4; k++)
                r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}
static Mat rotation(float deg, float x, float y, float z)
{
    float len = sqrtf(x * x + y * y + z * z);
    Mat r = identity();
    if (len == 0.0f)
        return r;
    x /= len; y /= len; z /= len;
    float a = deg * 3.14159265f / 180.0f;
    float c = cosf(a), s = sinf(a), t = 1.0f - c;
    float g[3][3] = {
        {t * x * x + c, t * x * y - s * z, t * x * z + s * y},
        {t * x * y + s * z, t * y * y + c, t * y * z - s * x},
        {t * x * z - s * y, t * y * z + s * x, t * z * z + c},
    };
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            r.m[i][j] = g[j][i];
    return r;
}

/* ---- the command interpreter (mdldump's: modelset_run_part / modelset_exec) ------------------------ */

/* --states: the render states of the command streams (modelset_exec 0x19 .. 0x2F, T451; their start
 * as modelset_begin_draw sets it) as GS registers: FBMSK, TEST, ALPHA, ZBUF.ZMSK */
struct RS {
    uint32_t fbmsk = 0;
    bool ate = true, date = false, zmsk = false;
    uint32_t atst = 6, aref = 0x20, afail = 0, datm = 1;
    uint32_t abcd = 0x44, fix = 0;
    bool operator==(const RS &o) const
    {
        return fbmsk == o.fbmsk && ate == o.ate && date == o.date && zmsk == o.zmsk && atst == o.atst &&
               aref == o.aref && afail == o.afail && datm == o.datm && abcd == o.abcd && fix == o.fix;
    }
};

struct DrawCall {
    uint8_t *packet;
    Mat mtx;
    RS rs;
};

struct Interp {
    int lod;
    std::vector<DrawCall> &calls;
    ModelSetView ms;
    Mat stack[3][64];
    int sp[3] = {0, 0, 0};
    int matrixMode = 0;
    int depth = 0;

    Interp(int l, std::vector<DrawCall> &c, uint8_t *m) : lod(l), calls(c), ms{m}
    {
        for (int i = 0; i < 3; i++)
            stack[i][0] = identity();
    }
    Mat &cur() { return stack[matrixMode][sp[matrixMode]]; }
    void draw(int packet)
    {
        if (packet < 0 || packet >= ms.nPackets() || ms.packet(packet) == nullptr)
            return;
        if (g_dumpStream) {
            const uint8_t *vm = ms.packet(packet);
            printf("%*sdraw packet %d (mode 0x%02X, %u parts)\n", depth * 2, "", packet, vm[8], rd16(vm + 0x0A));
        }
        calls.push_back({ms.packet(packet), stack[0][sp[0]], rs});
    }
    RS rs;
    /* the render-state commands into rs */
    void state_op(uint8_t op, const uint8_t *p)
    {
        switch (op) {
        case 0x1A: rs.ate = true; break;
        case 0x1B: rs.ate = false; break;
        case 0x1C: rs.atst = p[0] & 7; rs.aref = p[1]; break;
        case 0x1D: rs.date = true; break;
        case 0x1E: rs.date = false; break;
        case 0x1F: if (p[0] == 5) rs.datm = 1; else if (p[0] == 2) rs.datm = 0; break;
        case 0x20: rs.abcd = p[0]; rs.fix = p[1]; break;
        case 0x24: rs.fbmsk = rd32(p); break;
        case 0x25: rs.zmsk = true; break;
        case 0x26: rs.zmsk = false; break;
        case 0x2F: rs.afail = p[0] & 3; break;
        default: break;
        }
    }
    static bool g_dumpStream;
    /* --stream: the render-state commands (T451 modelset_exec 0x17 .. 0x2F) as they come */
    void dump_op(uint8_t op, const uint8_t *p)
    {
        if (!g_dumpStream)
            return;
        switch (op) {
        case 0x17: printf("%*sdepth test on\n", depth * 2, ""); break;
        case 0x18: printf("%*sdepth test off\n", depth * 2, ""); break;
        case 0x19: printf("%*sdepth func %u\n", depth * 2, "", p[0]); break;
        case 0x1A: printf("%*salpha test on\n", depth * 2, ""); break;
        case 0x1B: printf("%*salpha test off\n", depth * 2, ""); break;
        case 0x1C: printf("%*salpha func %u ref 0x%02X\n", depth * 2, "", p[0], p[1]); break;
        case 0x1D: printf("%*sDATE on\n", depth * 2, ""); break;
        case 0x1E: printf("%*sDATE off\n", depth * 2, ""); break;
        case 0x1F: printf("%*sDATE mode %u\n", depth * 2, "", p[0]); break;
        case 0x20: printf("%*sblend abcd 0x%02X fix 0x%02X\n", depth * 2, "", p[0], p[1]); break;
        case 0x24: printf("%*sframe mask (bits not written) %08X\n", depth * 2, "", rd32(p)); break;
        case 0x25: printf("%*sdepth writes off\n", depth * 2, ""); break;
        case 0x26: printf("%*sdepth writes on\n", depth * 2, ""); break;
        case 0x2F: printf("%*salpha fail %u\n", depth * 2, "", p[0]); break;
        case 0x2B: printf("%*stexture table %u\n", depth * 2, "", p[0]); break;
        case 0x29: printf("%*stexture base %u\n", depth * 2, "", p[0]); break;
        case 0x2A: printf("%*smaterial base %u\n", depth * 2, "", p[0]); break;
        default: break;
        }
    }
    void runPart(int part)
    {
        if (part < 0 || part >= ms.nParts() || depth > 16)
            return;
        uint8_t *p = ms.part(part);
        if (p == nullptr)
            return;
        depth++;
        if (g_dumpStream)
            printf("%*spart %d\n", depth * 2, "", part);
        for (int guard = 0; guard < 1000000; guard++) {
            uint8_t op = *p++;
            if (op == 0)
                break;
            dump_op(op, p);
            state_op(op, p);
            switch (op) {
            case 0x02: runPart(p[0]); p += 1; break;
            case 0x03: runPart(rd16(p)); p += 2; break;
            case 0x04: draw(p[0]); p += 1; break;
            case 0x05: draw(rd16(p)); p += 2; break;
            case 0x35: {
                int first = rd16(p);
                p += 4;
                draw(first);
                break;
            }
            case 0x34: p += 4; break;
            case 0x0A: {
                int n = p[0];
                p += 1 + n * 12 + 2;
                break;
            }
            case 0x0B: break;
            case 0x2B: p += 1; break;
            case 0x2C: p += 2; break;
            case 0x21: p += 4; break;
            case 0x22: case 0x23: break;
            case 0x08: p += (int8_t)p[0]; break;
            case 0x09: p += rds16(p); break;
            case 0x06: {
                p += 2;
                unsigned n = p[0];
                p += 1;
                if (n == 0)
                    p += 0;
                else {
                    p += 0;
                    p += rds16(p);
                }
                break;
            }
            case 0x07: {
                p += 0x10;
                int n = p[0];
                p += 1;
                int k = lod;
                if (!(k < n))
                    k = n - 1;
                p += k * 2;
                p += rds16(p);
                break;
            }
            case 0x0C:
                if (sp[matrixMode] < 63) {
                    stack[matrixMode][sp[matrixMode] + 1] = cur();
                    sp[matrixMode]++;
                }
                break;
            case 0x0D: if (sp[matrixMode] > 0) sp[matrixMode]--; break;
            case 0x0E: matrixMode = p[0] < 3 ? p[0] : 0; p += 1; break;
            case 0x0F: {
                Mat m;
                for (int i = 0; i < 16; i++)
                    m.m[i / 4][i % 4] = rdf(p + 4 * i);
                cur() = m;
                p += 0x40;
                break;
            }
            case 0x10: {
                Mat m;
                for (int i = 0; i < 16; i++)
                    m.m[i / 4][i % 4] = rdf(p + 4 * i);
                cur() = mul(m, cur());
                p += 0x40;
                break;
            }
            case 0x11: {
                Mat t = identity();
                t.m[3][0] = rdf(p); t.m[3][1] = rdf(p + 4); t.m[3][2] = rdf(p + 8);
                cur() = mul(t, cur());
                p += 12;
                break;
            }
            case 0x12: {
                Mat s = identity();
                s.m[0][0] = rdf(p); s.m[1][1] = rdf(p + 4); s.m[2][2] = rdf(p + 8);
                cur() = mul(s, cur());
                p += 12;
                break;
            }
            case 0x13: cur() = mul(rotation(rdf(p), rdf(p + 4), rdf(p + 8), rdf(p + 12)), cur()); p += 16; break;
            case 0x14: cur() = mul(rotation(rdf(p), 1, 0, 0), cur()); p += 4; break;
            case 0x15: cur() = mul(rotation(rdf(p), 0, 1, 0), cur()); p += 4; break;
            case 0x16: cur() = mul(rotation(rdf(p), 0, 0, 1), cur()); p += 4; break;
            case 0x19: case 0x1F: case 0x29: case 0x2A: case 0x2F: p += 1; break;
            case 0x1C: case 0x20: p += 2; break;
            case 0x24: case 0x27: case 0x32: case 0x36: p += 4; break;
            case 0x31: p += 12; break;
            case 0x28: case 0x33: case 0x37: p += 16; break;
            default: break;
            }
        }
        depth--;
    }
};

/* ---- the cars -------------------------------------------------------------------------------------- */

struct Car {
    std::string name;
    std::vector<uint8_t> file;          /* the loaded, relocated file (its pointers into it) */
    uint8_t *body = nullptr;            /* the body ModelSet */
    std::vector<uint8_t *> sets;        /* its Tex1 sets */
    std::vector<DrawCall> lod[3];
    uint8_t *anim = nullptr;            /* the first small upload's data of its set (a CLUT) */
};
static bool g_animate = true;
bool Interp::g_dumpStream = false;
static int g_dumpCar = -1;                  /* --stream N: car N's LOD 0 command stream printed */
static int g_carsLoaded;

static bool load_car(const std::string &path, Car &c)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (f == nullptr)
        return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    c.file.assign((size_t)n + 64, 0);
    if (fread(c.file.data(), 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        return false;
    }
    fclose(f);
    uint8_t *buf = c.file.data();
    if (!(rd32(buf) == 0 && rd32(buf + 4) == 0x40 && memcmp(buf + 0x40, "GTCI", 4) == 0))
        return false;
    const uint32_t body = rd32(buf + 8);
    if (body == 0 || memcmp(buf + body, "GTM1", 4) != 0)
        return false;
    /* the Tex1 sets, found in the file as it is */
    std::vector<gstex::Tex1Set> found = gstex::tex1_scan(buf, (size_t)n);
    c.body = buf + body;
    modelset_relocate(c.body);
    for (const gstex::Tex1Set &s : found) {
        uint8_t *img = buf + s.offset;
        tex1_relocate(img);
        pglr_tex1_loaded(img);
        c.sets.push_back(img);
    }
    ModelSetView v = {c.body};
    std::vector<const void *> packets;
    for (int i = 0; i < v.nPackets(); i++)
        if (v.packet(i) != nullptr) {
            pglr_model_loaded(v.packet(i));
            packets.push_back(v.packet(i));
        }
    pglr_modelset_loaded(packets.data(), (int)packets.size());
    for (int l = 0; l < 3; l++) {
        Interp::g_dumpStream = l == 0 && g_dumpCar == g_carsLoaded;
        if (Interp::g_dumpStream)
            printf("racebench: %s, LOD 0 command stream:\n", path.c_str());
        Interp in(l, c.lod[l], c.body);
        in.runPart(0);
        Interp::g_dumpStream = false;
    }
    g_carsLoaded++;
    if (!c.sets.empty()) {
        uint8_t *img = c.sets[0];
        const uint16_t nXfer = rd16(img + 0x16);
        uint8_t *xfer = P(rd32(img + 0x1C));
        for (int i = 0; xfer != nullptr && i < nXfer && c.anim == nullptr; i++) {
            const uint8_t *x = xfer + 0xC * i;
            const uint32_t bytes = (uint32_t)rd16(x + 8) * rd16(x + 10) * 4;
            if (x[7] == 0 && bytes <= 1024 && bytes >= 256)      /* a CT32 CLUT */
                c.anim = P(rd32(x));
        }
    }
    return !c.sets.empty() && !c.lod[0].empty();
}

/* ---- the frame ------------------------------------------------------------------------------------ */

enum { R_PRIM = 0, R_TEX0 = 6, R_CLAMP = 8, R_TEX1 = 0x14, R_XYOFFSET = 0x18, R_PRMODECONT = 0x1A, R_SCISSOR = 0x40,
       R_ALPHA = 0x42, R_COLCLAMP = 0x46, R_TEST = 0x47, R_FRAME = 0x4C, R_ZBUF = 0x4E };

constexpr uint32_t kTexFirst = 0x2500, kTexEnd = 0x4000;

static void frame_state(uint32_t fbp, uint32_t fbw, int w, int h, uint32_t zbp)
{
    pglr_ad(R_FRAME, (uint64_t)fbp | ((uint64_t)fbw << 16));
    pglr_ad(R_ZBUF, (uint64_t)zbp | (1ull << 24));
    pglr_ad(R_XYOFFSET, 0);
    pglr_ad(R_SCISSOR, ((uint64_t)(w - 1) << 16) | ((uint64_t)(h - 1) << 48));
    pglr_ad(R_TEST, (1ull << 16) | (2ull << 17));
    pglr_ad(R_ALPHA, 0x44);
    pglr_ad(R_PRMODECONT, 1);
    pglr_ad(R_COLCLAMP, 1);
}

/* the target cleared: a black sprite over it, Z 0 written (TEST ZTST ALWAYS), as the game's clear */
static void clear(int w, int h)
{
    static uint64_t blank[8];
    constexpr uint64_t flgReglist = 1ull << 58;
    pglr_ad(R_TEST, (1ull << 16) | (1ull << 17));
    blank[0] = 1ull | (2ull << 60) | flgReglist;
    blank[1] = 1 /* RGBAQ */ | ((uint64_t)R_PRIM << 4);
    blank[2] = 0x3F800000ull << 32;
    blank[3] = 6;                                   /* SPRITE */
    blank[4] = 1ull | (2ull << 60) | flgReglist;
    blank[5] = 5 /* XYZ2 */ | (5ull << 4);
    blank[6] = 0;
    blank[7] = (uint64_t)(w << 4) | ((uint64_t)(h << 4) << 16);
    pglr_gif(blank, 4, 0);
    pglr_ad(R_TEST, (1ull << 16) | (2ull << 17));
}

/* the cars of a pass: viewport w x h, LOD by distance (or lodAll), their sets in moving slots */
static int g_dateEvery = 0;                 /* --date N: every N-th textured part the car bodies' way */
static uint32_t g_dateCount;
static bool g_states = false;               /* --states: the command streams' render states */
static int g_repeat = 1;                    /* --repeat N: the race screen's cars N times (draws for the GPU) */
static bool g_far = false;                  /* --far: the race camera's frustum, the rows strung out */
static bool g_oneTex = false;               /* --onetex: every part of a car with one texture (a measurement) */
static bool g_newPal = false;               /* --newpal: the animated CLUTs new every time (lamps fading) */
static bool g_materials = false;            /* --materials: a part's lighting block by its material (as the game) */
static bool g_freeze = false;               /* --freeze: the cars where frame 0 put them, every frame (with --still: the
                                             * traced frame is the picture's) */
/* PC (2026-11-13) --wander 1 | 2: the reflection buffer and its Z change places every four frames (0x2300 / 0x2400,
 * then 0x2400 / 0x2300), as a buffer the game allocates anew each frame moves in VRAM, and a sprite of the race screen
 * shows the buffer (so the picture depends on it).  1: with the game's notices (pglr_buffer_alloc / _freed, as
 * VramAllocator gives them): the targets left behind are retired and their surfaces taken again; 2: without: every
 * target kept, as before.  The two pictures must be the same; the targets' counts are printed. */
static int g_wander = 0;
/* PC (2026-11-14) --detail X,Y: the reflection buffer drawn at X x Y times the output scale (pglr_target_detail, as
 * the game's road reflection with [Graphics] ReflectionResolution), back to 1 x 1 once the frame's draws read it;
 * with --wander 2 the sprite shows it.  1,1: the picture must be the same as without the option. */
static float g_detailX = 0.0f, g_detailY = 0.0f;

/* (--wander) the 128 x 128 buffer at block tbp as a sprite at the race screen's corner: DECAL, no blend, no Z test */
static void buffer_sprite(uint32_t tbp)
{
    static uint64_t pkt[10];
    constexpr uint64_t flgReglist = 1ull << 58;
    pglr_ad(R_TEX0, (uint64_t)tbp | (2ull << 14) | (7ull << 26) | (7ull << 30) | (1ull << 34) | (1ull << 35));
    pglr_ad(R_TEX1, 0);
    pglr_ad(R_CLAMP, 5);
    pglr_ad(R_TEST, (1ull << 16) | (1ull << 17));
    pkt[0] = 1ull | (2ull << 60) | flgReglist;
    pkt[1] = 1 /* RGBAQ */ | ((uint64_t)R_PRIM << 4);
    pkt[2] = 0x80808080ull | (0x3F800000ull << 32);
    pkt[3] = 6 | 0x10 | 0x100;                      /* SPRITE, TME, FST */
    pkt[4] = 1ull | (4ull << 60) | flgReglist;
    pkt[5] = 3 /* UV */ | (5ull << 4) /* XYZ2 */ | (3ull << 8) | (5ull << 12);
    pkt[6] = 0;
    pkt[7] = 0;
    pkt[8] = (uint64_t)(128 << 4) | ((uint64_t)(128 << 4) << 16);
    pkt[9] = (uint64_t)(128 << 4) | ((uint64_t)(128 << 4) << 16);
    pglr_gif(pkt, 5, 0);
    pglr_ad(R_TEST, (1ull << 16) | (2ull << 17));
}

/* (--states) rs as the GS registers, those that differ from what was sent last (all: first) */
static void send_states(const RS &rs, uint32_t fbp, uint32_t fbw, uint32_t zbp, RS &sent, bool &any)
{
    if (any && rs == sent)
        return;
    if (!any || rs.fbmsk != sent.fbmsk)
        pglr_ad(R_FRAME, (uint64_t)fbp | ((uint64_t)fbw << 16) | ((uint64_t)rs.fbmsk << 32));
    pglr_ad(R_TEST, (rs.ate ? 1ull : 0ull) | (uint64_t)rs.atst << 1 | (uint64_t)rs.aref << 4 | (uint64_t)rs.afail << 12 |
                        (rs.date ? 1ull << 14 : 0ull) | (uint64_t)rs.datm << 15 | (1ull << 16) | (2ull << 17));
    if (!any || rs.abcd != sent.abcd || rs.fix != sent.fix)
        pglr_ad(R_ALPHA, (uint64_t)rs.abcd | ((uint64_t)rs.fix << 32));
    if (!any || rs.zmsk != sent.zmsk)
        pglr_ad(R_ZBUF, (uint64_t)zbp | (1ull << 24) | (rs.zmsk ? 1ull << 32 : 0ull));
    sent = rs;
    any = true;
}

static void draw_cars(std::vector<Car> &cars, int frame, int w, int h, int lodAll, bool upload, uint32_t fbp,
                      uint32_t fbw, uint32_t zbp)
{
    RS sent;
    bool anySent = false;
    const float f = 1.6f, aspect = (float)w / (float)h, zn = g_far ? 0.1f : 0.5f, zf = g_far ? 750.0f : 2000.0f;
    /* (--far: pgl_frustum's z, -1 .. 1, near / far of T383's race cameras) */
    const float A = g_far ? (zf + zn) / (zf - zn) : zf / (zf - zn),
                B = g_far ? -2.0f * zf * zn / (zf - zn) : -zn * zf / (zf - zn);
    Mat proj = {};
    proj.m[0][0] = f / aspect;
    proj.m[1][1] = f;
    proj.m[2][2] = A;
    proj.m[2][3] = 1.0f;
    proj.m[3][2] = B;
    PglrVu1 vu = {};
    vu.viewScale[0] = (float)w * 0.5f;
    vu.viewScale[1] = -(float)h * 0.5f;
    vu.viewScale[2] = -8388607.0f;
    vu.viewCenter[0] = (float)w * 0.5f;
    vu.viewCenter[1] = (float)h * 0.5f;
    vu.viewCenter[2] = 8388608.0f;
    for (int i = 0; i < 4; i++) {
        vu.limits[i] = 1e6f;
        vu.vBB0[i] = 1e6f;
    }
    vu.prog = 0x320 + 0xA;
    vu.projZ[0] = A;
    vu.projZ[1] = B;
    vu.projZ[2] = 1.0f;
    uint32_t slotBlocks = 0;
    for (const Car &c : cars)
        slotBlocks = std::max<uint32_t>(slotBlocks, rd16(c.sets[0] + 0x12));
    const uint32_t slots = std::max<uint32_t>(1, (kTexEnd - kTexFirst) / std::max<uint32_t>(slotBlocks, 1));
    for (size_t i = 0; i < cars.size(); i++) {
        Car &c = cars[i];
        const int row = (int)i / 4, col = (int)i % 4;
        const float x = (col - 1.5f) * 3.2f,
                    z = 7.0f + row * (g_far ? 55.0f : 9.0f) + 0.37f * (float)((g_freeze ? 0 : frame) % 10);
        const int lod = lodAll >= 0 ? lodAll : (z < 15.0f ? 0 : z < 40.0f ? 1 : 2);
        Mat place = rotation(90.0f + 7.0f * (float)i, 0, 1, 0);
        place.m[3][0] = x;
        place.m[3][1] = -0.6f;
        place.m[3][2] = z;
        /* the camera 6 m up, looking 14 degrees down */
        Mat cam = identity();
        cam.m[3][1] = -6.0f;
        place = mul(mul(place, cam), rotation(-14.0f, 1, 0, 0));
        /* the set to a place that moves from frame to frame (the race's allocator: pages of 32 blocks
         * anywhere in the texture area) */
        uint8_t *img = c.sets[0];
        const uint32_t span = (kTexEnd - kTexFirst - rd16(img + 0x12)) / 32;
        const uint32_t at = (uint32_t)((i * 977 + (size_t)frame * 331 + (size_t)(lodAll + 1) * 113) % span);
        (void)slots;
        tex1_set_vram_base(img, kTexFirst + at * 32);
        /* a quarter of the cars' lamps animate: a CLUT of the set written again (clut_anim_apply_all) */
        if (upload && g_animate && (i % 4) == (size_t)(frame % 4) && c.anim != nullptr) {
            if (g_newPal)
                c.anim[frame & 63] = (uint8_t)(frame >> 6);     /* (--newpal: a new palette each time) */
            else
                c.anim[frame & 63] ^= 0x01;
        }
        if (upload)
            pglr_tex1_upload(img, 0);
        const uint16_t nTex = rd16(img + 0x14);
        const uint8_t *tex = P(rd32(img + 0x18));
        int k = 0;
        for (const DrawCall &d : c.lod[std::min(lod, 2)]) {
            const Mat m = mul(mul(d.mtx, place), proj);
            memcpy(vu.m, m.m, sizeof(vu.m));
            if (g_states)
                send_states(d.rs, fbp, fbw, zbp, sent, anySent);
            const gt3vif::VifModelHeader *hdr = reinterpret_cast<const gt3vif::VifModelHeader *>(d.packet);
            const gt3vif::VifPartRecord *parts = reinterpret_cast<const gt3vif::VifPartRecord *>(hdr + 1);
            for (int p = 0; p < hdr->nParts; p++) {
                if (parts[p].tex != 0 && nTex != 0) {
                    /* (the part's own; --onetex: the set's first for every part) */
                    const uint8_t *t = tex + 0x28 * (g_oneTex ? 0 : (parts[p].tex - 1) % nTex);
                    uint64_t t0, t1, cl;
                    memcpy(&t0, t, 8);
                    memcpy(&t1, t + 8, 8);
                    memcpy(&cl, t + 0x20, 8);
                    /* DECAL: the picture is the textures' (the lit models' vertices hold no colours) */
                    t0 = (t0 & ~(3ull << 35)) | (1ull << 35);
                    pglr_ad(R_TEX0, t0);
                    pglr_ad(R_TEX1, t1);
                    pglr_ad(R_CLAMP, cl);
                }
                PglrModelPart part = {};
                part.model = d.packet;
                part.part = p;
                const uint32_t lm = hdr->mode & 0x1F;
                /* (unlit: the picture by the textures and vertex colours alone - made-up lights draw black) */
                part.lightProg = 0x04E;
                (void)lm;
                part.lightOfs = 0;
                for (int q = 0; q < 16; q++)
                    for (int e = 0; e < 4; e++)
                        part.light[q][e] = 0.25f + 0.01f * (float)(q + e);
                /* (--materials: the block differs by the part's material - the game loads a material's
                 * colours into it as texmat_select finds a new one, a constant-buffer write that ended the
                 * merged draw before the lights' ring; unlit here, so the picture is the same) */
                if (g_materials)
                    part.light[9][1] = 0.5f + 0.001f * (float)((parts[p].tex >> 9) & 0x7F);
                part.lightQwords = 8;
                part.colorScale = 128.0f;
                part.alphaScale = 128.0f;
                part.primMask = 0x20;           /* FGE off (no fog set up: fully fogged, black) */
                part.primAttr = 0;
                if (g_dateEvery > 0 && parts[p].tex != 0 && (g_dateCount++ % g_dateEvery) == 0) {
                    /* the car bodies' way (renderer.md 9): the part's alpha alone (FBMSK 0x00FFFFFF), then
                     * the part with DATE - a copy of the target before it */
                    pglr_ad(R_FRAME, (uint64_t)fbp | ((uint64_t)fbw << 16) | (0x00FFFFFFull << 32));
                    pglr_model_part(&vu, &part, 0);
                    pglr_ad(R_FRAME, (uint64_t)fbp | ((uint64_t)fbw << 16));
                    pglr_ad(R_TEST, (1ull << 16) | (2ull << 17) | (1ull << 14));
                    pglr_model_part(&vu, &part, 0);
                    pglr_ad(R_TEST, (1ull << 16) | (2ull << 17));
                } else {
                    pglr_model_part(&vu, &part, 0);
                }
            }
            k++;
        }
    }
    if (g_states) {
        RS start;                                       /* (frame_state's again) */
        start.ate = false;
        send_states(start, fbp, fbw, zbp, sent, anySent);
    }
}

static LONG WINAPI on_crash(EXCEPTION_POINTERS *e)
{
    const CONTEXT *c = e->ContextRecord;
    printf("CRASH code %08lX at eip %08lX, base %p\n", e->ExceptionRecord->ExceptionCode, c->Eip,
           (void *)GetModuleHandleA(nullptr));
    fflush(stdout);
    ExitProcess(3);
}

int main(int argc, char **argv)
{
    SetUnhandledExceptionFilter(on_crash);
    setvbuf(stdout, nullptr, _IOLBF, 1 << 16);
    int frames = 400;
    bool sync = false, trace = false;
    const char *pixels = nullptr;
    std::string carsDir = "build\\bin\\data_US\\cars\\day";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--sync") == 0)
            sync = true;
        else if (strcmp(argv[i], "--still") == 0)
            g_animate = false;
        else if (strcmp(argv[i], "--trace") == 0)
            trace = true;
        else if (strcmp(argv[i], "--cars") == 0 && i + 1 < argc)
            carsDir = argv[++i];
        else if (strcmp(argv[i], "--date") == 0 && i + 1 < argc)
            g_dateEvery = atoi(argv[++i]);
        else if (strcmp(argv[i], "--stream") == 0 && i + 1 < argc)
            g_dumpCar = atoi(argv[++i]);
        else if (strcmp(argv[i], "--states") == 0)
            g_states = true;
        else if (strcmp(argv[i], "--repeat") == 0 && i + 1 < argc)
            g_repeat = std::max(1, atoi(argv[++i]));
        else if (strcmp(argv[i], "--far") == 0)
            g_far = true;
        else if (strcmp(argv[i], "--onetex") == 0)
            g_oneTex = true;
        else if (strcmp(argv[i], "--newpal") == 0)
            g_newPal = true;
        else if (strcmp(argv[i], "--materials") == 0)
            g_materials = true;
        else if (strcmp(argv[i], "--freeze") == 0)
            g_freeze = true;
        else if (strcmp(argv[i], "--wander") == 0 && i + 1 < argc)
            g_wander = atoi(argv[++i]);
        else if (strcmp(argv[i], "--detail") == 0 && i + 1 < argc) {
            if (sscanf(argv[++i], "%f,%f", &g_detailX, &g_detailY) != 2)
                g_detailX = g_detailY = 0.0f;
        }
        else if (strcmp(argv[i], "--hires") == 0) {
            g_screenW = 1920;
            g_screenH = 1080;
            g_renderScale = 150;
        } else if (strcmp(argv[i], "--aa") == 0 && i + 1 < argc) {
            /* PC (2026-11-15): the anti-aliasing (AGSS: the per-sample shaders) */
            static const char *const names[] = {"off", "fxaa", "msaa2", "msaa4", "msaa8", "agss2", "agss4", "agss8"};
            const char *v = argv[++i];
            for (int k = 0; k < (int)(sizeof(names) / sizeof(names[0])); k++)
                if (_stricmp(v, names[k]) == 0)
                    g_benchAa = (settings::AntiAliasing)k;
        } else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc)
            g_renderScale = std::max(10, std::min(400, atoi(argv[++i])));     /* PC (2026-11-15): the resolution scale */
        else if (strcmp(argv[i], "--pixels") == 0 && i + 1 < argc)
            pixels = argv[++i];
        else if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc)
            g_shotFrame = atoi(argv[++i]);              /* PC (2026-11-15): a screenshot at frame N */
        else if (strcmp(argv[i], "--shotfile") == 0 && i + 1 < argc)
            g_shotFile = argv[++i];
        else if (strcmp(argv[i], "--texdump") == 0 && i + 1 < argc) {
            g_texRoot = argv[++i];
            g_texDump = true;
        } else if (strcmp(argv[i], "--texpack") == 0 && i + 1 < argc)
            g_texPacks.push_back(argv[++i]);
        else if (strcmp(argv[i], "--texcache") == 0 && i + 1 < argc)
            g_texCache = argv[++i];
        else if (strcmp(argv[i], "--texrgba") == 0)
            g_texRgba = true;
        else if (strcmp(argv[i], "--texlog") == 0)
            g_texLog = true;
        else if (strcmp(argv[i], "--texselect") == 0 && i + 1 < argc)
            g_texSelect = atoi(argv[++i]);
        else if (strcmp(argv[i], "--texrefresh") == 0 && i + 1 < argc)
            g_texRefresh = argv[++i];
        else if (strcmp(argv[i], "--textoggle") == 0 && i + 1 < argc)
            g_texToggle = atoi(argv[++i]);
        else if (strcmp(argv[i], "--capture") == 0 && i + 1 < argc) {
            /* PC (2026-11-06): the frame capture of frame N (pgl_capture.h: capture\frame_N\ next to this exe) */
            static char capEnv[64];
            snprintf(capEnv, sizeof(capEnv), "GT3PC_PGL_CAPTURE=%s", argv[++i]);
            _putenv(capEnv);
        } else
            frames = atoi(argv[i]);
    }
    if (getenv("GT3PC_PGL_PROFILE") == nullptr)
        _putenv("GT3PC_PGL_PROFILE=100");
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    /* (the game's feature levels, platform/gpu.cpp: 11.1 has the rasterizer-ordered views of the DATE draws'
     * alpha mirror, the default list stops at 11.0 without them) */
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 4, D3D11_SDK_VERSION, &g_dev,
                                 nullptr, &g_ctx)))
        return 1;
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = (UINT)g_screenW;
    d.Height = (UINT)g_screenH;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    g_dev->CreateTexture2D(&d, nullptr, &g_bbTex);
    g_dev->CreateRenderTargetView(g_bbTex, nullptr, &g_bb);
    pglr_init();

    /* 32 different cars */
    const std::string dir = carsDir + "\\";
    std::vector<Car> cars;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "*").c_str(), &fd);
    std::vector<std::string> names;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.')
                names.push_back(fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    std::sort(names.begin(), names.end());
    cars.reserve(32);
    for (size_t i = 0; i < names.size() && cars.size() < 32; i += 7) {
        cars.emplace_back();
        cars.back().name = names[i];
        if (!load_car(dir + names[i], cars.back()))
            cars.pop_back();
    }
    if (cars.empty()) {
        printf("racebench: no car model files in %s (--cars DIR)\n", carsDir.c_str());
        fflush(stdout);
        ExitProcess(2);
    }
    size_t calls[3] = {0, 0, 0}, partsAll = 0;
    uint32_t maxBlocks = 0;
    for (const Car &c : cars) {
        for (int l = 0; l < 3; l++)
            calls[l] += c.lod[l].size();
        for (const DrawCall &dc : c.lod[0])
            partsAll += reinterpret_cast<const gt3vif::VifModelHeader *>(dc.packet)->nParts;
        maxBlocks = std::max<uint32_t>(maxBlocks, rd16(c.sets[0] + 0x12));
    }
    printf("racebench: %zu cars (%s .. %s), packets drawn at LOD 0/1/2: %zu / %zu / %zu, parts at LOD 0 %zu;"
           " the largest set %u blocks\n", cars.size(), cars.front().name.c_str(), cars.back().name.c_str(),
           calls[0], calls[1], calls[2], partsAll, maxBlocks);

    LARGE_INTEGER qf, t0, t1;
    QueryPerformanceFrequency(&qf);
    const int warm = 60;
    std::vector<uint32_t> px;
    const bool texOn = g_texDump || !g_texPacks.empty() || g_texSelect > 0;
    for (int fr = 0; fr < warm + frames; fr++) {
        /* PC (2026-11-13): the texture replacements: the folders listed and the files asked for loaded before
         * the frames that count (the game draws on meanwhile) */
        if (texOn && (fr == 20 || fr == 40 || fr == warm - 1)) {
            pglr_sync();
            for (int w = 0; w < 6000; w++) {
                pglr::texrepl::Status st;
                pglr::texrepl::status(&st);
                if (st.listed && st.waiting == 0)
                    break;
                Sleep(10);
            }
        }
        if (fr == warm)
            QueryPerformanceCounter(&t0);
        if (g_texSelect > 0 && fr == 5)
            pglr::texrepl::menu_open(true);
        if (g_texSelect > 0 && fr == warm + frames / 2) {
            for (int k = 0; k < g_texSelect; k++)
                pglr::texrepl::select_step(1);
        }
        if ((g_texToggle >= 1 && fr == warm + frames / 2) || (g_texToggle >= 2 && fr == warm + frames / 2 + 20)) {
            g_texOff = !g_texOff;
            pglr::texrepl::settings_changed();
            pglr::texrepl::Status st;
            pglr::texrepl::status(&st);
            printf("racebench: toggle: the replacements %s at frame %d (%u loaded, %u waiting)\n", g_texOff ? "off" : "on",
                   fr, st.loaded, st.waiting);
        }
        if (!g_texRefresh.empty() && !g_texPacks.empty() && fr == warm + frames / 2) {
            int copied = 0, deleted = 0;
            WIN32_FIND_DATAA rf;
            HANDLE rh = FindFirstFileA((g_texRefresh + "\\*").c_str(), &rf);
            if (rh != INVALID_HANDLE_VALUE) {
                do {
                    if (rf.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                        continue;
                    std::string name = rf.cFileName;
                    if (name.size() > 5 && name.compare(name.size() - 5, 5, ".gone") == 0) {
                        name.resize(name.size() - 5);
                        deleted += DeleteFileA((g_texPacks[0] + "\\" + name).c_str()) != 0;
                    } else {
                        copied += CopyFileA((g_texRefresh + "\\" + name).c_str(), (g_texPacks[0] + "\\" + name).c_str(),
                                            FALSE) != 0;
                    }
                } while (FindNextFileA(rh, &rf));
                FindClose(rh);
            }
            printf("racebench: refresh: %d files copied, %d deleted: %s\n", copied, deleted,
                   pglr::texrepl::refresh() ? "asked" : "refused");
        }
        if (!g_texRefresh.empty() && fr > warm + frames / 2 && fr <= warm + frames / 2 + 30 && (fr % 10) == 0) {
            pglr_sync();
            for (int w = 0; w < 3000; w++) {
                pglr::texrepl::Status st;
                pglr::texrepl::status(&st);
                if (!st.refreshing && st.waiting == 0)
                    break;
                Sleep(10);
            }
        }
        if (trace && fr == warm + frames - 3)
            InterlockedExchange(&g_traceNow, 1);
        if (fr == g_shotFrame)
            InterlockedExchange(&g_shotAsk, 1);         /* PC (2026-11-15): --shot */
        pglr_begin_frame();
        /* the reflection buffer: 128 x 128 at block 0x2300, its own Z at 0x2400 (--wander: the two change places
         * every four frames) */
        const bool swapped = g_wander != 0 && ((fr / 4) & 1) != 0;
        const uint32_t rb = swapped ? 0x2400 : 0x2300, rz = swapped ? 0x2300 : 0x2400;
        if (g_wander == 1) {
            pglr_buffer_alloc(rb);
            pglr_buffer_alloc(rz);
        }
        if (g_detailX > 0.0f)
            pglr_target_detail(rb, 128, 128, g_detailX, g_detailY);  /* PC (2026-11-14): --detail */
        pglr_target(rb, 128, 0, 128, 128);
        frame_state(rb / 32, 2, 128, 128, rz / 32);
        clear(128, 128);
        draw_cars(cars, fr, 128, 128, 2, true, rb / 32, 2, rz / 32);
        /* the race screen, its Z at 0x1180 */
        pglr_target(0, 640, 0, 640, 448);
        frame_state(0, 10, 640, 448, 0x1180 / 32);
        clear(640, 448);
        for (int rep = 0; rep < g_repeat; rep++)
            draw_cars(cars, fr + rep * 3, 640, 448, -1, true, 0, 10, 0x1180 / 32);
        if (g_wander != 0)
            buffer_sprite(rb);
        if (g_detailX > 0.0f)
            pglr_target_detail(rb, 128, 128, 1.0f, 1.0f);           /* PC (2026-11-14): --detail (read: back) */
        if (g_wander == 1) {
            pglr_buffer_freed(rb);
            pglr_buffer_freed(rz);
        }
        const bool last = fr == warm + frames - 1;
        if (last && pixels != nullptr) {
            px.assign(640 * 448, 0);
            pglr_readback(0, 10, 0, 0, 0, 640, 448, px.data(), nullptr, 0, 0);
        }
        pglr_display(0, 0, 640, 0, 0, 640, 448);
        pglr_end_frame();
        pglr_kick();
        if (sync || last)
            pglr_sync();
    }
    QueryPerformanceCounter(&t1);
    const double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)qf.QuadPart / frames;
    printf("racebench: %d frames, %.3f ms a frame (%.1f a second)\n", frames, ms, 1000.0 / ms);
    {
        int alive = 0;
        unsigned created = 0, reused = 0, retired = 0;
        pglr_target_stats(&alive, &created, &reused, &retired);
        printf("racebench: targets: %d alive; %u surfaces created, %u taken from retired targets, %u targets retired\n",
               alive, created, reused, retired);
    }
    if (pixels != nullptr) {
        FILE *o = fopen(pixels, "wb");
        if (o != nullptr) {
            fwrite(px.data(), 4, px.size(), o);
            fclose(o);
        }
        uint64_t sum = 0;
        size_t lit = 0;
        for (uint32_t p : px) {
            sum = sum * 1099511628211ull ^ p;
            lit += (p & 0xFFFFFF) != 0;
        }
        printf("racebench: the picture's hash %016llX, %zu pixels not black\n", (unsigned long long)sum, lit);
    }
    if (g_texSelect > 0) {
        pglr::texrepl::Status st;
        pglr::texrepl::status(&st);
        const bool queued = pglr::texrepl::dump_selected();
        printf("racebench: select: %u textures listed, chosen %016llX (number %d, %ux%u, pixels %s): dump %s\n", st.seen,
               (unsigned long long)st.selHash, st.selIndex + 1, st.selW, st.selH, st.selPixels ? "here" : "not here",
               queued ? "queued" : "refused");
    }
    if (!g_texRefresh.empty()) {
        pglr::texrepl::Status st;
        pglr::texrepl::status(&st);
        printf("racebench: refresh: %u done: %u changed, %u new, %u gone\n", st.refreshes, st.refChanged, st.refNew,
               st.refGone);
    }
    if (texOn) {
        pglr::texrepl::Status st;
        for (int w = 0; w < 12000; w++) {
            pglr::texrepl::status(&st);
            if (st.dumpsWaiting == 0 && st.cacheTodo == 0 && st.waiting == 0)
                break;
            Sleep(10);
        }
        printf("racebench: textures: %u folders, %u replacement files, %u loaded, %u failed; %u dumps written;"
               " %u pre-cache files made ahead\n", st.folders, st.files, st.loaded, st.failed, st.dumped, st.cacheMade);
    }
    fflush(stdout);
    ExitProcess(0);
}
