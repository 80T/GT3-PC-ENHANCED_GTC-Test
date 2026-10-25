/* pgl_port.h - the internal header of the port's pgl (the graphics library,
 * G001; canonical header engine/pgl/G001_flare.h).
 *
 * The library's C stays the decompiled C wherever it computes state:
 * GsContext and its GS register shadows, the attribute and matrix stacks,
 * the lighting blocks, the text renderer, the Tex1 sets.  What was hardware
 * is native:
 *   - the scratchpad context 0x70003000 is the global g_pglCtx;
 *   - the VU0 registers and data memory are pgl::g_vu0 (pgl_math.h): the
 *     library's VU0 asm is the C++ of pgl_math;
 *   - GS / VIF / DMA packets are records for the native renderer
 *     (pgl_bridge.h).  The packet buffer stays plain memory: pgl still
 *     hands out gfx_alloc_tail scratch arrays from it and the GIF blocks
 *     the game writes itself (gfx_open_direct, the text renderer) live in
 *     it until the frame is executed.
 * docs/renderer.md describes the split; docs/pgl.md the state model.
 *
 * Only the port's pgl files (src/engine/pgl/*.cpp) include this. */
#ifndef PGL_PORT_H
#define PGL_PORT_H

#include "types.h"
#include "libc.h"
#include "eekernel.h"
#include "libgraph.h"
#include "engine/pgl/G001_flare.h"
#include "engine/pgl/native/pgl_math.h"
#include "engine/pgl/native/pgl_bridge.h"

#include <stddef.h>
#include <stdarg.h>

/* ======================================================================== */
/* The context                                                              */
/* ======================================================================== */

/* GsContext lived in scratchpad RAM at 0x70003000; one global here. */
extern "C" GsContext g_pglCtx;
#undef GS_CTX
#define GS_CTX      (&g_pglCtx)
#define GS_SPR_CTX  (&g_pglCtx)

/* The VU0 of the library and the game (pgl_math.h). */
#define VU0         (pgl::g_vu0)

/* tgtA as the decomp read it (a union view in the original). */
#define GS_TGT_A(ctx) ((ctx)->tgtA)

/* ======================================================================== */
/* GS register views of the shadows (libgraph's sceGs* layouts)             */
/* ======================================================================== */

typedef struct GsScissor {
    u64 SCAX0 : 11; u64 pad11 : 5;
    u64 SCAX1 : 11; u64 pad27 : 5;
    u64 SCAY0 : 11; u64 pad43 : 5;
    u64 SCAY1 : 11; u64 pad59 : 5;
} GsScissor;

typedef struct GsTest {
    u64 ATE : 1;  u64 ATST : 3; u64 AREF : 8; u64 AFAIL : 2;
    u64 DATE : 1; u64 DATM : 1; u64 ZTE : 1;  u64 ZTST : 2;
    u64 pad19 : 45;
} GsTest;

typedef struct GsZbuf {
    u64 ZBP : 9;  u64 pad09 : 15; u64 PSM : 4; u64 pad28 : 4;
    u64 ZMSK : 1; u64 pad33 : 31;
} GsZbuf;

typedef struct GsAlpha {
    u64 ABCD : 8; u64 pad08 : 24; u64 FIX : 8; u64 pad40 : 24;
} GsAlpha;

typedef struct GsXyoffset {
    u64 OFX : 16; u64 pad16 : 16; u64 OFY : 16; u64 pad48 : 16;
} GsXyoffset;

typedef struct GsTex0 {
    u64 TBP0 : 14; u64 TBW : 6; u64 PSM : 6; u64 TW : 4; u64 TH : 4;
    u64 TCC : 1;   u64 TFX : 2; u64 CBP : 14; u64 CPSM : 4; u64 CSM : 1;
    u64 CSA : 5;   u64 CLD : 3;
} GsTex0;
typedef GsTex0 GsTex0Bits;
#define GS_TEX0_BITS(ctx, r)    ((GsTex0 *)&(ctx)->reg[r])

typedef struct GsTex1 {
    u64 LCM : 1;  u64 pad01 : 1; u64 MXL : 3; u64 MMAG : 1; u64 MMIN : 3;
    u64 MTBA : 1; u64 pad10 : 9; u64 L : 2;   u64 pad21 : 11; u64 K : 12;
    u64 pad44 : 20;
} GsTex1;

typedef struct GsClamp {
    u64 WMS : 2;   u64 WMT : 2;   u64 MINU : 10; u64 MAXU : 10;
    u64 MINV : 10; u64 MAXV : 10; u64 pad44 : 20;
} GsClamp;

typedef struct GsFogcol {
    u64 FCR : 8; u64 FCG : 8; u64 FCB : 8; u64 pad24 : 40;
} GsFogcol;

typedef struct GsTex2 {
    u64 pad00 : 20; u64 PSM : 6; u64 pad26 : 11; u64 CBP : 14;
    u64 CPSM : 4;   u64 CSM : 1; u64 CSA : 5;     u64 CLD : 3;
} GsTex2;

typedef struct GsMiptbp {
    u64 TBP1 : 14; u64 TBW1 : 6; u64 TBP2 : 14; u64 TBW2 : 6;
    u64 TBP3 : 14; u64 TBW3 : 6; u64 pad60 : 4;
} GsMiptbp;

typedef struct GsTexa {
    u64 TA0 : 8; u64 pad08 : 7; u64 AEM : 1; u64 pad16 : 16;
    u64 TA1 : 8; u64 pad40 : 24;
} GsTexa;

static_assert(sizeof(GsScissor) == 8 && sizeof(GsTest) == 8 && sizeof(GsZbuf) == 8 &&
              sizeof(GsAlpha) == 8 && sizeof(GsXyoffset) == 8 && sizeof(GsTex0) == 8 &&
              sizeof(GsTex1) == 8 && sizeof(GsClamp) == 8 && sizeof(GsFogcol) == 8 &&
              sizeof(GsTex2) == 8 && sizeof(GsMiptbp) == 8 && sizeof(GsTexa) == 8,
              "GS register views");

/* Byte / half / word views of a shadow. */
#define SHADOW_U8(r)   ((u8 *)&(r))
#define SHADOW_U16(r)  ((u16 *)&(r))
#define SHADOW_U32(r)  ((u32 *)&(r))

/* GS_FIELD_* of G001_flare.h hand a "shift, width" field macro on to their
 * GS_FIELD_*_ forms; MSVC's traditional preprocessor passes the expansion
 * as one argument there.  One more rescan (PGL_PP_EXPAND) splits it. */
#define PGL_PP_EXPAND(x)            x
#undef GS_FIELD_MASK
#undef GS_FIELD_VAL
#undef GS_FIELD_SET
#undef GS_FIELD_GET
#define GS_FIELD_MASK(f)            PGL_PP_EXPAND(GS_FIELD_MASK_(f))
#define GS_FIELD_VAL(f, v)          PGL_PP_EXPAND(GS_FIELD_VAL_(f, v))
#define GS_FIELD_SET(r, f, v)       PGL_PP_EXPAND(GS_FIELD_SET_(r, f, v))
#define GS_FIELD_GET(r, f)          PGL_PP_EXPAND(GS_FIELD_GET_(r, f))

/* Register fields as "shift, width" (GS_FIELD_* of G001_flare.h). */
#define GS_TEST_ATE         0, 1
#define GS_TEST_ATST        1, 3
#define GS_TEST_AREF        4, 8
#define GS_TEST_AFAIL       12, 2
#define GS_TEST_DATE        14, 1
#define GS_TEST_DATM        15, 1
#define GS_TEST_ZTE         16, 1
#define GS_TEST_ZTST        17, 2
#define GS_ZBUF_ZMSK_F      32, 1
#define GS_TEX0_PSM         20, 6
#define GS_TEX0_TCC_F       34, 1
#define GS_SCANMSK_MSK      0, 2

enum { GS_ZTST_NEVER = 0, GS_ZTST_ALWAYS = 1, GS_ZTST_GEQUAL = 2, GS_ZTST_GREATER = 3 };
#define GS_SCANMSK_EVEN     2           /* rows with even Y not drawn */
#define GS_SCANMSK_ODD      3           /* rows with odd Y not drawn */
#define GS_CLD_KEEP         0
#define GS_CLD_LOAD         1
#define GS_CLUT_CT24        SCE_GS_PSMCT24

/* Values of the context init and the switches. */
#define GS_DTHE_ON              1
#define GS_COLCLAMP_CLAMP       1
#define GS_PRMODECONT_AC        1
#define GS_TEXA_TA1_DEFAULT     0x80
#define GS_TEXA_AEM             0x0000000000008000ull
#define GS_TEX0_TCC             0x0000000400000000ull
#define GS_TEX1_DEFAULT         0x60                    /* MMAG 1, MMIN 1 */
#define GS_TEX2_DEFAULT         0x2000000001300000ull   /* PSMT8, CLD 1 */
#define GS_ZBUF_ZMSK            0x0000000100000000ull
#define GS_DIRTY_TEX_STATE      0x000000801E801E80ull
#define GS_ENABLE_GEN_CTX1      0x000003FF00001FFFull   /* list 0's emit mask at a frame start */
#define GS_REG_BITS(first, last) (((2ull << (last)) - 1) & ~((1ull << (first)) - 1))
#define GS_DIRTY_CTX2_BITS(m)   ((u64)(m) << 16)
#define GS_DIRTY_BOTH_BITS(m)   ((u64)(m) * 0x10001ull)

/* PRIM types and attribute bits (GsContext.primAttr holds IIP..AA1). */
enum {
    GS_PRIM_POINT          = 0,
    GS_PRIM_LINE           = 1,
    GS_PRIM_LINE_STRIP     = 2,
    GS_PRIM_TRIANGLE       = 3,
    GS_PRIM_TRIANGLE_STRIP = 4,
    GS_PRIM_TRIANGLE_FAN   = 5,
    GS_PRIM_SPRITE         = 6,
    GS_PRIM_IIP            = 0x008,
    GS_PRIM_TME            = 0x010,
    GS_PRIM_FGE            = 0x020,
    GS_PRIM_ABE            = 0x040,
    GS_PRIM_AA1            = 0x080,
    GS_PRIM_FST            = 0x100,
    GS_PRIM_CTXT2          = 0x200
};
#define GS_PRIM_UNTEXTURED(attr, type)  ((u16)(((attr) | (type)) & ~GS_PRIM_TME))

/* GS pixel storage formats. */
enum {
    GS_PSMCT32 = 0x00, GS_PSMCT24 = 0x01, GS_PSMCT16 = 0x02, GS_PSMCT16S = 0x0A,
    GS_PSMT8 = 0x13, GS_PSMT4 = 0x14, GS_PSMT8H = 0x1B, GS_PSMT4HL = 0x24, GS_PSMT4HH = 0x2C,
    GS_PSMZ32 = 0x30, GS_PSMZ24 = 0x31, GS_PSMZ16 = 0x32, GS_PSMZ16S = 0x3A
};

/* GIF tags: the fields (G001_flare.h) and the register descriptors. */
#define GIF_FLG_PACKED          (0ull << 58)
#define GIF_TAG_LO(nloop, eop)  ((u64)(u32)((nloop) | ((eop) ? GIF_EOP : 0)))
#define GIF_AD_TAG(nloop, eop)  (GIF_NREG(1) | GIF_TAG_LO(nloop, eop))
enum {
    GIF_REG_PRIM  = 0x0,
    GIF_REG_RGBAQ = 0x1,
    GIF_REG_ST    = 0x2,
    GIF_REG_UV    = 0x3,
    GIF_REG_XYZF2 = 0x4,
    GIF_REG_XYZ2  = 0x5,
    GIF_REG_XYZF3 = 0xC,
    GIF_REG_XYZ3  = 0xD
};
#define GS_Q_ONE                0x3F800000u
#define GS_RGBAQ_Q_ONE          0x3F800000u
#define GS_FOG_NONE             0xFF
#define GS_Z24_MAX              0xFFFFFF
#define GS_UV_HALF_TEXEL        8
#define GS_XY_SHIFT             4
#define GS_XY_SHIFT_FIELD       3

/* DMA tags / VIF codes the packet bookkeeping still writes (plain memory:
 * the packet buffer keeps its PS2 layout, nothing reads the tags). */
#define DMATAG_NEXT         0x20000000u
#define DMATAG_END          0x70000000u
#define VIF_OFFSET(o)       (0x02000000u | (o))
#define VIF_BASE(b)         (0x03000000u | (b))
#define VIF_STMOD(m)        (0x05000000u | (m))
#define VIF_MSKPATH3        0x06000000u
#define VIF_FLUSHE          0x10000000u
#define VIF_IBIT            0x80000000u
#define STMASK_DEFAULT      0xC0C0C0C0u
#define VU1_BUF_OFFSET      0x1B3

/* The PS2 packet buffer's addresses are plain pointers here: the segment
 * bits of uncached access (gfx_set_packet_access) are not ORed in. */
#define DMA_ADDR(p)         ((u32)(p))

static inline void gs_put_cnt_direct(u32 *p, u32 qwc)
{
    p[0] = DMATAG_CNT(qwc);
    p[1] = 0;
    p[2] = VIF_FLUSH;
    p[3] = VIF_DIRECT(qwc);
}
static inline void gs_put_next_tag(u32 *link, u32 *next)
{
    if (link == NULL)
        return;
    link[0] = DMATAG_NEXT;
    link[1] = (u32)next;
    link[2] = 0;
    link[3] = 0;
}
static inline void gs_put_tag(u32 *p, u32 tag)
{
    if (p == NULL)
        return;
    p[0] = tag;
    p[1] = 0;
    p[2] = 0;
    p[3] = 0;
}

/* The packet headers (gfx_begin_packet / gfx_begin_subpacket). */
typedef struct GsFrameHeader {
    u32 *vif1Chain;         /* 0x00 */
    u32 *gifChain;          /* 0x04 */
    u32 *save0;             /* 0x08 */
    u32 *save1;             /* 0x0C */
} GsFrameHeader;

typedef struct GsSubPacketHeader {
    u32 *vif1Chain;         /* 0x00 */
    u32 *gifChain;          /* 0x04 */
    u64  written;           /* 0x08 the registers the sub-packet emitted */
} GsSubPacketHeader;

/* "A packet is active": packetOpen or subPacketOpen. */
#define GS_PACKET_ACTIVE(ctx) ((ctx)->packetOpen || (ctx)->subPacketOpen)

/* ======================================================================== */
/* EE float helpers                                                         */
/* ======================================================================== */

static inline f32 ee_maxf(f32 a, f32 b) { return a >= b ? a : b; }
static inline f32 ee_minf(f32 a, f32 b) { return a <= b ? a : b; }
static inline f32 ee_clampf(f32 x, f32 lo, f32 hi) { x = ee_maxf(x, lo); x = ee_minf(x, hi); return x; }
static inline u32 pgl_f2u(f32 f) { return pgl::f2u(f); }
static inline f32 pgl_u2f(u32 u) { return pgl::u2f(u); }

/* GsLight.spotCutoff of a light that is not a spot light. */
#define PGL_SPOT_CUTOFF_NONE 180.0f

/* ======================================================================== */
/* VU0 helpers                                                              */
/* ======================================================================== */

/* GsContext.matrixMode */
enum { PGL_MODELVIEW = 0, PGL_PROJECTION = 1, PGL_TEXTURE = 2 };

/* vf<n> as a GsVec4 / float pointer. */
static inline pgl::Vec4 vec4_of(const GsVec4 &v) { return pgl::Vec4{v.x, v.y, v.z, v.w}; }
static inline void vec4_store(GsVec4 &d, const pgl::Vec4 &v) { d.x = v.x; d.y = v.y; d.z = v.z; d.w = v.w; }
static inline pgl::Vec4 vec4_load(const float *p) { return pgl::Vec4{p[0], p[1], p[2], p[3]}; }

/* The modelview wherever it is (vf28..vf31 or parked at VU0 qword 4). */
static inline void pgl_modelview(pgl::Vec4 out[4])
{
    pgl::vu0GetModeMatrix(VU0, g_pglCtx.matrixMode, pgl::MTX_MODELVIEW, out);
}
static inline void pgl_projection(pgl::Vec4 out[4])
{
    pgl::vu0GetModeMatrix(VU0, g_pglCtx.matrixMode, pgl::MTX_PROJECTION, out);
}

/* The VU1 state of a draw (pgl_send_matrix's block): the current one, and
 * the one VU1 holds (what pgl_send_matrix last sent: the draws use it). */
void pgl_vu1_state(GsContext *ctx, PglrVu1 *out);
extern PglrVu1 g_pglVu1Sent;
/* Sends the matrix block when it is due (vu1MatrixSent 0); returns 1 when
 * it was sent (the VU1 entry moves back by one). */
int pgl_vu1_matrix_due(GsContext *ctx);

/* ======================================================================== */
/* The text renderer and effects (font.c, flare.c, particles.c, tex1.cpp)   */
/* ======================================================================== */

typedef struct KanaPage {
    u8         first;       /* 0x00 */
    u8         last;        /* 0x01 */
    u16        unk_02;      /* 0x02 */
    const u16 *map;         /* 0x04 second byte - first -> font code */
} KanaPage;

#define FONT_PARAM_SCALE_X    0x1100
#define FONT_PARAM_SCALE_Y    0x1101
#define FONT_PARAM_SPACING_X  0x1102
#define FONT_PARAM_SPACING_Y  0x1103
#define FONT_KANJI_FIRST      0xA1
#define GS_ALPHA_BLEND        0x44u
#define FONT_SPRITE_REGS      ((u64)(GIF_REG_UV | (GIF_REG_XYZF3 << 4) | (GIF_REG_UV << 8) | (GIF_REG_XYZF2 << 12)))
#define GS_SET_TEX0(tbp, tbw, psm, tw, th, tcc, tfx, cbp, cpsm, csm, csa, cld)       \
    ((u64)(tbp) | ((u64)(tbw) << 14) | ((u64)(psm) << 20) | ((u64)(tw) << 26) |    \
     ((u64)(th) << 30) | ((u64)(tcc) << 34) | ((u64)(tfx) << 35) |                 \
     ((u64)(cbp) << 37) | ((u64)(cpsm) << 51) | ((u64)(csm) << 55) |               \
     ((u64)(csa) << 56) | ((u64)(cld) << 61))
#define GS_SET_CLAMP(wms, wmt, minu, maxu, minv, maxv)                              \
    ((u64)(wms) | ((u64)(wmt) << 2) | ((u64)(minu) << 4) | ((u64)(maxu) << 14) |   \
     ((u64)(minv) << 24) | ((u64)(maxv) << 34))
#define GS_REGION_CLAMP 2

enum { MATSCALE_AMBIENT = 0, MATSCALE_DIFFUSE = 1, MATSCALE_SPECULAR = 2, MATSCALE_EMISSION = 3 };

/* The CLUT animation records of a Tex1 set. */
typedef struct ClutKey {
    f32  time;              /* 0x00 duration; negative = hold on this key */
    u32 *palette;           /* 0x04 nColors RGBA32 colours */
} ClutKey;

typedef struct ClutAnimChannel {
    u16      id;            /* 0x00 */
    u16      mode;          /* 0x02 0 loop, 1 ping-pong, 2 once, else hold */
    u16      nColors;       /* 0x04 */
    u16      nKeys;         /* 0x06 */
    u32     *dest;          /* 0x08 destination CLUT colours */
    ClutKey *keys;          /* 0x0C */
    u16      cur;           /* 0x10 */
    u16      next;          /* 0x12 */
    f32      time;          /* 0x14 */
    f32      lerp;          /* 0x18 weight of keys[cur] */
    s16      hold;          /* 0x1C > 0: frozen */
    s8       dir;           /* 0x1E ping-pong direction */
    u8       dirty;         /* 0x1F */
} ClutAnimChannel;

typedef struct ClutAnimTable {
    u16              count; /* 0x00 */
    u16              pad02;
    ClutAnimChannel *ch;    /* 0x04 */
} ClutAnimTable;

/* Weather particles (particles.c). */
typedef struct WeatherDrop {
    f32 pos[4];         /* 0x00 x, y, z, 1 */
    f32 vel[4];         /* 0x10 x, y, z, 0 */
} WeatherDrop;

typedef struct Weather {
    f32 center[4];      /* 0x00 */
    f32 size;           /* 0x10 */
    f32 spreadXZ;       /* 0x14 */
    f32 spreadY;        /* 0x18 */
    f32 ahead;          /* 0x1C */
    f32 velXZ;          /* 0x20 */
    f32 velY;           /* 0x24 */
    f32 velZ;           /* 0x28 */
    WeatherDrop *drops; /* 0x2C */
    s32 count;          /* 0x30 */
} Weather;

/* Lens flare (flare.c). */
typedef struct FlareRayVert {
    f32 p0[4];
    f32 p1[4];
    s32 rgb[3];
    s32 a;
} FlareRayVert;

typedef struct FlareWork {
    s32  occVram;           /* 0x00 */
    s32  occHeight;         /* 0x04 */
    s32  queries;           /* 0x08 */
    s32  screenVram;        /* 0x0C */
    s32  screenWidth;       /* 0x10 */
    s32  screenHeight;      /* 0x14 */
    s32  width;             /* 0x18 */
    s32  height;            /* 0x1C */
    s32  workVram;          /* 0x20 */
    s32  workWidth;         /* 0x24 */
    s32  workHeight;        /* 0x28 */
    Tex1Image *tex[2];      /* 0x2C */
    f32  aspect;            /* 0x34 */
    u32 *results;           /* 0x38 */
    u32 *reduce[16];        /* 0x3C */
} FlareWork;

/* ======================================================================== */
/* Library globals (defined by the port's pgl files)                        */
/* ======================================================================== */

extern "C" {
extern u8   g_pglAttribStack[0x1000];
extern u8   g_gfxInitialized;
extern KanaPage g_kanaPages[16];
extern u8   g_stripBatchOpen;
extern s32  g_texselTex;
extern s32  g_texselMat;
extern s32  g_texselTexBase2;
extern s32  g_texselMatBase2;
extern u64  g_fmtPow10[19];
}

/* ======================================================================== */
/* Functions G001_flare.h does not declare                                  */
/* ======================================================================== */

extern "C" {
/* driver */
void gfx_init_vu0_constants_spr(void);
void gfx_reset_graph_stored_mode(void);
void gfx_update_display_regs(GfxDriverState *g);
void gfx_dma_channel_init(int ch, u32 chcr);
void gfx_mskpath3(int mask);
u32 *gfx_get_packet_ptr(void);
int  gfx_get_packet_space(void);
void gfx_wait_vblank_event(void);
void gs_build_clear_packet(GsContext *ctx, u64 test, u32 rgba, u32 z, const GsRenderTarget *tgt);
void gs_emit_render_target_regs(GsContext *ctx);
void gs_fill_target_b(GsContext *ctx, u32 blend, float fix);
void gs_copy_target(GsContext *ctx, const GsRenderTarget *dst, const GsRenderTarget *src,
                    u32 blend, int dy, int dv, float fix);
void gs_emit_xyoffset_integer(GsContext *ctx);
u64  gfx_perf_counter1(void);
void gfx_wait_vsync_poll(void);
void gfx_reset_paths(void);
void gfx_context_init(GsContext *ctx);
void gfx_context_init_spr(void);
int  lcg_rand(void);
u64 *gs_emit_masked_regs(u64 *out, u64 mask, const u8 *regNums, const u64 *shadow);
void gs_readback_begin(GfxDriverState *g);
void gs_readback_end(GfxDriverState *g);
u32 *gfx_alloc_direct(s32 qwc);

/* pgl.c */
float float_pow_encoded(float x, int enc);
float pgl_sin_deg(float deg);
void pgl_set_sprite_depth_i(int z);
int  clip_line_homogeneous(float *a, float *b);
int  vu0_clip_code(const float *v);

/* vu1 lighting / primitives */
void vifmodel_setup_lighting(GsContext *ctx, const VifModel *m);
int  vifmodel_send_lighting(GsContext *ctx, u32 mode, int resend);
void pgl_send_lights_diffuse(GsContext *ctx);
void pgl_send_lights_specular4(GsContext *ctx);
void pgl_send_lights_specular2(GsContext *ctx);
void pgl_send_lights_mode1(GsContext *ctx);
void pgl_send_lights_mode4(GsContext *ctx);
void pgl_build_light_list(GsContext *ctx);
void pgl_update_eye_lights(GsContext *ctx);
void pgl_update_light_colors(GsContext *ctx);
void pgl_send_mode5(GsContext *ctx);
void pgl_send_mode2(GsContext *ctx);
void pgl_build_env_matrix(GsContext *ctx);
void pgl_send_mode3(GsContext *ctx);
void pgl_build_envmap_matrix(GsContext *ctx);
void *pgl_gen_colors(GsContext *ctx, const void *xyz, const void *normals, const void *colors, int n);
void *pgl_gen_texcoords(GsContext *ctx, const void *xyz, const void *normals, const void *uvs, int n);
void *pgl_xform_texcoords(GsContext *ctx, const void *uvs, int n);
void pgl_draw_strip_vu1(const void *xyz, const void *normals, const void *colors, const void *uvs, int n);
void gs_flush_all_regs(void);

/* render.c */
u32  vu0_clip_point(u64 x, u64 y, u64 z);
void mat_scale_set_rgb(u32 kind, f32 r, f32 g, f32 b);
void mat_scale_set(u32 kind, f32 r, f32 g, f32 b, f32 a);
void texsel_reset(void);

/* font */
void ring_draw_full(u32 colorInner, u32 colorOuter, f32 width);
void font_set_start(s32 x, s32 y);
void font_set_pen(s32 x, s32 y);
int  font_put_kanji(int code);
int  kana_page_lookup(u32 page, int c);
int  sprintf_putc(int c);
void font_flush(void);
int  font_measure_kanji(int code);
int  font_measure_char(int c);
int  fmt_parse_uint(const u8 **pp);

/* vecmath */
void vec3_normalize(f32 *out, f32 x, f32 y, f32 z);
void vec3_cross(f32 *out, const f32 *a, const f32 *b);

/* tex1 */
void clut_anim_channel_reset(ClutAnimChannel *ch);
void clut_anim_channel_apply(ClutAnimChannel *ch);
void clut_anim_table_relocate(ClutAnimTable *tbl, s32 delta);

/* vu0 */
void vu0_mult_matrix_regs(void);
int  lcg_rand_chance(f32 p);

/* particles / flare */
void weather_drop_init(WeatherDrop *d, f32 spreadXZ, f32 spreadY, f32 velXZ, f32 velY, f32 velZ);
void weather_draw_drop(WeatherDrop *d, Weather *w, f32 period, f32 periodY, f32 size);
}

/* VIF UNPACK formats (the prebuilt VIF packets of flare.c). */
enum { VIF_S_32 = 0x60, VIF_S_8 = 0x62, VIF_V2_32 = 0x64, VIF_V3_32 = 0x68, VIF_V4_32 = 0x6C, VIF_V4_8 = 0x6E };

/* The per-file .cpp names of the library's VU1 programs. */
#define VU1_PROG_BASE           0x320
#define VU1_PROG_BASE_33E       0x33E
#define VU1_PROG_CULL_CW        0x0A
#define VU1_PROG_CULL_CCW       0x14
#define VU1_PRIM_PROG_DEFAULT   0x33E
#define VU1_MAX_VERTS           64

/* GsContext.vu1LightProg: the lighting program (models.md 3.2). */
enum {
    VU1L_DEFAULT        = 0x04E,
    VU1L_SPEC2_S6       = 0x057,
    VU1L_SPEC2_S12      = 0x082,
    VU1L_SPEC2          = 0x0AF,
    VU1L_SPEC4_S6       = 0x0DE,
    VU1L_SPEC4_S12      = 0x115,
    VU1L_SPEC4          = 0x14C,
    VU1L_DIFFUSE        = 0x185,
    VU1L_LOCAL_S6       = 0x1AD,
    VU1L_LOCAL_S12      = 0x1E1,
    VU1L_LOCAL          = 0x21A,
    VU1L_ENVMTX         = 0x257,
    VU1L_ENVMAP         = 0x297,
    VU1L_CONST          = 0x2DB,
    VU1L_FLARE          = 0x2F7
};

#define VIFMODEL_LMODE(m)       ((m) & 0x1F)
#define VIFMODEL_FORMAT(m)      (((m) & 0xE0) >> 5)
enum {
    PGL_LMODE_NONE   = 0,
    PGL_LMODE_LIGHTS = 1,
    PGL_LMODE_ENVMTX = 2,
    PGL_LMODE_ENVMAP = 3,
    PGL_LMODE_LOCAL  = 4,
    PGL_LMODE_CONST  = 5
};
#define VIFMODEL_COLOR_255      0x01
#define VIFMODEL_ALPHA_255      0x02
enum { PGL_PARAM_AF0 = 0, PGL_PARAM_B00 = 2, PGL_PARAM_B10 = 3 };

/* The lighting block of the current model part (what pgl_send_lights_* /
 * pgl_send_mode* sent to VU1 0xC9; the model draw passes it on). */
struct PglLightBlock {
    float q[16][4];
    int   count;
};
extern PglLightBlock g_pglLightBlock;

/* The positions vu1_unpack_xyz handed out for the next model part. */
extern const float *g_pglPreXyz;
extern int          g_pglPreXyzCount;

#endif /* PGL_PORT_H */
