/* pgl_state_gs.h - the GS register shadows of pgl as a native render-state
 * description.
 *
 * pgl keeps the GS drawing state as register shadows in GsContext (reg[] /
 * gen[], G001_flare.h; the gfx_* setters write them, gs_flush_dirty_regs
 * sent the dirty ones to the GS).  The port reads them at draw time: this
 * file decodes the register fields and turns them into a compact,
 * API-neutral description - the blend equation, depth / alpha / destination
 * alpha tests split into draw passes, write masks, scissor, offsets, fog -
 * that a Direct3D 11 backend hashes into its state objects and shader
 * permutations (packBlendKey / packDepthKey / packShaderKey).  No D3D types,
 * no Windows, no game dependency.
 *
 * Conventions the description assumes (docs/pgl.md "State mapping"):
 *   - colour targets UNORM, a channel byte b stored as b / 255 (the alpha
 *     byte too: frame alpha is data - readbacks, DATE, colour filters);
 *   - the pixel shader computes the GS source colour Cs / As in 0..255 and
 *     writes o0 = (Cs, As) / 255 and o1.a = C / 128 for the blend (dual
 *     source; C = As or FIX, saturated at 1.0 = 0x80);
 *   - depth = Z / 2^bits of the Z format, compared GREATER / GREATER_EQUAL
 *     (GS Z grows towards the viewer), cleared to 0 for far.
 */
#ifndef PGL_NATIVE_PGL_STATE_GS_H
#define PGL_NATIVE_PGL_STATE_GS_H

#include <cstdint>

namespace pgl {
namespace gs {

/* ======================================================================== */
/* The shadows (GsContext.reg[] / gen[] order)                             */
/* ======================================================================== */

enum : int {
    REG_FRAME = 0, REG_XYOFFSET, REG_FBA, REG_ZBUF, REG_SCISSOR, REG_ALPHA, REG_TEST,
    REG_CLAMP, REG_TEX2, REG_TEX1, REG_TEX0, REG_MIPTBP1, REG_MIPTBP2, REG_COUNT
};
enum : int {
    GEN_COLCLAMP = 0, GEN_DIMX, GEN_DTHE, GEN_FOGCOL, GEN_PABE, GEN_PRMODECONT, GEN_SCANMSK,
    GEN_TEXA, GEN_TEXCLUT, GEN_TEXFLUSH, GEN_COUNT
};

/* A copy of the shadows (64-bit register images). */
struct Regs {
    uint64_t reg[REG_COUNT];
    uint64_t gen[GEN_COUNT];
};
template <class Ctx> inline Regs regsOf(const Ctx &ctx)
{
    Regs r;
    for (int i = 0; i < REG_COUNT; i++)
        r.reg[i] = ctx.reg[i];
    for (int i = 0; i < GEN_COUNT; i++)
        r.gen[i] = ctx.gen[i];
    return r;
}

/* ======================================================================== */
/* Register fields                                                          */
/* ======================================================================== */

enum class PrimType : uint8_t { Point, Line, LineStrip, Triangle, TriStrip, TriFan, Sprite, Reserved };
/* TEST.ATST, also the alpha-test functions of the native passes. */
enum class Atst : uint8_t { Never, Always, Less, LEqual, Equal, GEqual, Greater, NotEqual };
enum class Afail : uint8_t { Keep, FbOnly, ZbOnly, RgbOnly };
enum class Ztst : uint8_t { Never, Always, GEqual, Greater };

/* PRIM (with GsContext.primAttr's bits). */
struct Prim {
    PrimType type;
    bool iip;       /* Gouraud (else flat: the colour of the primitive's last vertex) */
    bool tme;       /* textured */
    bool fge;       /* fog */
    bool abe;       /* alpha blending */
    bool aa1;       /* antialiasing (coverage alpha) */
    bool fst;       /* UV (texel, 10.4) coordinates instead of STQ */
    bool ctxt;      /* GS context 2 */
};
Prim decodePrim(uint32_t prim);
uint32_t encodePrim(const Prim &p);

/* ALPHA: (A - B) * C / 128 + D; A, B, D: 0 Cs, 1 Cd, 2 zero (3 reserved);
 * C: 0 As, 1 Ad, 2 FIX (3 reserved). */
struct Alpha { uint8_t a, b, c, d, fix; };
Alpha decodeAlpha(uint64_t alpha);
inline uint8_t alphaSelectors(const Alpha &al) { return (uint8_t)(al.a | al.b << 2 | al.c << 4 | al.d << 6); }

struct Test {
    bool ate;       /* alpha test */
    Atst atst;
    uint8_t aref;
    Afail afail;
    bool date;      /* destination alpha test */
    bool datm;      /* 1: pass where the destination alpha bit 7 is 1, 0: where it is 0 */
    bool zte;       /* depth test (pgl keeps it 1; 0 in its colour-only clears) */
    Ztst ztst;
};
Test decodeTest(uint64_t test);

struct Frame { uint16_t fbp; uint8_t fbw; uint8_t psm; uint32_t fbmsk; };   /* fbp in 2048-word pages, fbw in 64 px */
Frame decodeFrame(uint64_t frame);
struct Zbuf { uint16_t zbp; uint8_t psm; bool zmsk; };                      /* psm: the low 4 bits (0 Z32, 1 Z24, 2 Z16, 0xA Z16S) */
Zbuf decodeZbuf(uint64_t zbuf);
struct Scissor { uint16_t x0, x1, y0, y1; };                                /* window pixels, inclusive */
Scissor decodeScissor(uint64_t scissor);
struct XyOffset { uint16_t ofx, ofy; };                                     /* 12.4 */
XyOffset decodeXyOffset(uint64_t xyoffset);
struct FogCol { uint8_t r, g, b; };
FogCol decodeFogCol(uint64_t fogcol);

/* Texture registers (state side; the texture cache resolves TBP / CBP). */
struct Tex0 {
    uint16_t tbp0; uint8_t tbw, psm, tw, th; bool tcc; uint8_t tfx;
    uint16_t cbp; uint8_t cpsm; bool csm; uint8_t csa, cld;
};
Tex0 decodeTex0(uint64_t tex0);
struct Tex1 { bool lcm; uint8_t mxl; bool mmag; uint8_t mmin; bool mtba; uint8_t l; int16_t k; };  /* k: signed 7.4 */
Tex1 decodeTex1(uint64_t tex1);
struct Clamp { uint8_t wms, wmt; uint16_t minu, maxu, minv, maxv; };
Clamp decodeClamp(uint64_t clamp);
struct Texa { uint8_t ta0; bool aem; uint8_t ta1; };
Texa decodeTexa(uint64_t texa);

/* PSMs of the frame / Z formats. */
enum : uint8_t {
    PSMCT32 = 0x00, PSMCT24 = 0x01, PSMCT16 = 0x02, PSMCT16S = 0x0A,
    PSMZ32 = 0x30, PSMZ24 = 0x31, PSMZ16 = 0x32, PSMZ16S = 0x3A
};

/* ======================================================================== */
/* The native description                                                   */
/* ======================================================================== */

/* Blend factors of the fixed-function equation (D3D11 names in brackets). */
enum class BlendFactor : uint8_t {
    Zero,           /* ZERO */
    One,            /* ONE */
    SrcC,           /* C from the shader: SRC1_ALPHA (o1.a = As / 128 or FIX / 128), or BLEND_FACTOR for FIX */
    InvSrcC,        /* INV_SRC1_ALPHA / INV_BLEND_FACTOR */
    DstAlpha,       /* DEST_ALPHA (C = Ad: the stored Ad / 255, see AdMode::TwoPass) */
    InvDstAlpha,    /* INV_DEST_ALPHA (unused since AdMode: 1 - Ad / 255) */
    DstColor        /* DEST_COLOR (the "C as colour" form) */
};
enum class BlendOp : uint8_t { Add, Subtract, RevSubtract };   /* src - dst, dst - src */
enum class CSource : uint8_t { As, Ad, Fix };
/* What the shader does to its colour output o0.rgb before the blend. */
enum class ColorPremul : uint8_t {
    None,
    OnePlusC,       /* o0.rgb = Cs (1 + C)            ((Cs - Cd) C + Cs, Cs C + Cs) */
    DstAlphaX2,     /* o0.rgb = Cs 255 / 128          (no longer produced: AdMode) */
    CAsColor,       /* o0.rgb = C                     (Cd C + Cd: Cd (1 + C) as o0 x DEST_COLOR + Cd) */
    Zero,           /* o0.rgb = 0 */
    TimesC          /* o0.rgb = (Cs C) >> 7, clamped  (Cs C, Cs C + Cd with src ONE: exact for any C,
                       As up to 0xFF and FIX > 0x80 included - a blend factor stops at 1.0) */
};
/* C = Ad, the frame's alpha (0x80 = 1.0): DEST_ALPHA holds Ad / 255 and a
 * UNORM target clamps o0 to 1 before blending, so no single fixed-function
 * pass gives Ad / 128.  (A 24-bit frame stores no alpha: the GS takes Ad as
 * 0x80 there, translated as FIX 0x80.) */
enum class AdMode : uint8_t {
    None,
    TwoPass,        /* Cs Ad + Cd, Cs Ad: the draw twice, o0 = Cs, src DEST_ALPHA - Ad / 255 each, Ad / 128
                     * together; the first pass without the alpha write (Ad stays) and with the form's dst
                     * (ONE / ZERO), the second with dst ONE.  Exact within one unit (rounding) */
    Shader          /* the other forms: blending off, the shader computes the GS's ((A - B) Ad >> 7) + D
                     * from a copy of the target (t1, made fresh for the draw).  Exact; a draw's own
                     * overlapping primitives read the target as it was before the draw */
};

struct Blend {
    bool        enable;     /* off: the colour is Cs (or AdMode::Shader's) */
    BlendFactor src, dst;
    BlendOp     op;
    CSource     c;
    ColorPremul premul;
    uint8_t     fix;        /* ALPHA.FIX (constant buffer, not part of the state key) */
    uint8_t     selectors;  /* the GS ALPHA low byte, for logs */
    bool        exact;      /* false: no fixed-function form (see docs/pgl.md) */
    AdMode      ad;         /* C = Ad: how it is drawn */
    uint8_t     adSel;      /* AdMode::Shader: A | B << 2 | D << 6 (the selectors used) */
};
/* The blend of an ALPHA register for a primitive with PRIM.ABE abe into a
 * frame of 24-bit format (ct24Frame: Ad reads as 0x80) or not. */
Blend translateBlend(const Alpha &al, bool abe, bool ct24Frame = false);

/* Comparison functions of the native tests. */
enum class Compare : uint8_t { Never, Always, Less, LessEqual, Equal, GreaterEqual, Greater, NotEqual };
Compare alphaCompare(Atst atst);                    /* the shader's alpha test: As cmp AREF */
Compare depthCompare(Ztst ztst);                    /* GS Z: GEQUAL -> GreaterEqual, GREATER -> Greater */

/* Channel bits of a write mask. */
enum : uint8_t { WRITE_R = 1, WRITE_G = 2, WRITE_B = 4, WRITE_A = 8, WRITE_RGB = 7, WRITE_RGBA = 15 };

/* How a pass treats the alpha test. */
enum class AlphaGate : uint8_t {
    None,           /* no test */
    Pass,           /* discard the pixels that fail AREF */
    Fail            /* discard the pixels that pass (the AFAIL pass) */
};
struct Pass {
    AlphaGate gate;
    uint8_t   writeMask;    /* WRITE_* */
    bool      depthWrite;
};

/* Inexactness flags: GS behaviour the description cannot reproduce with
 * the fixed-function pipeline (none of them used by GT3's own code paths,
 * except where docs/pgl.md says so). */
enum : uint16_t {
    INEXACT_BLEND_FORM   = 0x0001,  /* (Cd - Cs) C + Cd and the like: no fixed-function form */
    INEXACT_BLEND_FIX    = 0x0002,  /* FIX > 0x80: the C factor saturates at 1 */
    INEXACT_BLEND_AD     = 0x0004,  /* C = Ad (no longer set: AdMode) */
    INEXACT_BLEND_SEL3   = 0x0008,  /* a reserved selector (3) */
    INEXACT_PABE         = 0x0010,  /* PABE on (per-pixel blend switch) */
    INEXACT_COLCLAMP     = 0x0020,  /* COLCLAMP off (colours wrap) */
    INEXACT_FBMSK        = 0x0040,  /* FBMSK not whole bytes */
    INEXACT_DITHER       = 0x0080,  /* DTHE on (ignored) */
    INEXACT_AA1          = 0x0100,  /* PRIM.AA1 (coverage alpha ignored) */
    INEXACT_PRMODE       = 0x0200,  /* PRMODECONT.AC = 0 (attributes from PRMODE) */
    INEXACT_FRAME_16     = 0x0400   /* a 16-bit frame (5:5:5:1 conversion not modelled) */
};

struct DrawState {
    Prim      prim;
    Blend     blend;
    /* depth */
    bool      depthTest;            /* off when ZTE = 0 */
    Compare   depthFunc;
    bool      depthWrite;           /* !ZMSK (and ZTE) */
    uint8_t   zpsm;                 /* the Z format (PSMZ*) */
    /* destination alpha test: the pixel shader against bit 7 of the frame's
     * alpha (read from a copy of the target, pgl_exec.cpp) */
    bool      date;
    bool      datm;
    /* alpha test: the shader compares As (0..255) with aref */
    Compare   alphaFunc;
    uint8_t   aref;
    Afail     afail;
    /* the passes the alpha test / AFAIL split the draw into (0: nothing drawn) */
    uint8_t   passCount;
    Pass      pass[2];
    /* frame */
    uint8_t   frameWriteMask;       /* WRITE_* from FBMSK and the frame PSM */
    uint32_t  fbmsk;
    uint8_t   fpsm;
    bool      fba;                  /* the shader ORs 0x80 into As (32-bit frames) */
    /* raster */
    Scissor   scissor;
    XyOffset  offset;
    uint8_t   scanmask;             /* SCANMSK.MSK: 2 even rows, 3 odd rows not drawn */
    /* fog */
    bool      fog;                  /* PRIM.FGE: C = Cs F / 255 + FOGCOL (1 - F / 255) */
    FogCol    fogColor;
    /* everything else */
    bool      colclamp;
    uint16_t  inexact;
};

/* The state of a primitive drawn with PRIM prim (the value the path sends:
 * primAttr | type bits, see docs/pgl.md) under the shadows r. */
DrawState translate(const Regs &r, uint32_t prim);

/* Keys for state-object caches.  Blend: enable, factors, op, write mask.
 * Depth / stencil: test, func, write, DATE / DATM.  Shader: everything the
 * pixel shader branches on (texture function and the texture registers are
 * the texture path's: pass tex0 / prim.tme along). */
uint32_t packBlendKey(const Blend &b, uint8_t writeMask);
uint32_t packDepthKey(const DrawState &s, const Pass &p);
uint32_t packShaderKey(const DrawState &s, const Pass &p, uint64_t tex0);

/* ======================================================================== */
/* Reference evaluation (tests, debugging)                                  */
/* ======================================================================== */

/* The GS blend of one channel: ((A - B) * C >> 7) + D, clamped to 0..255
 * (COLCLAMP) or wrapped (& 0xFF). */
int gsBlendChannel(const Alpha &al, int cs, int cd, int as, int ad, bool colclamp);
/* The native fixed-function evaluation of a Blend (UNORM: the shader
 * outputs saturated to [0, 1], the result saturated), one channel in 0..255. */
float nativeBlendChannel(const Blend &b, int cs, int cd, int as, int ad);
/* The texture function (TEX0.TFX / TCC) on 0..255 values: MODULATE Ct Cf >>
 * 7 (0x80 = 1.0), DECAL Ct, HIGHLIGHT Ct Cf >> 7 + Af, HIGHLIGHT2 the same
 * with At; returns the colour channel or the alpha. */
int texFuncColor(uint8_t tfx, int ct, int cf, int af);
int texFuncAlpha(uint8_t tfx, bool tcc, int at, int af);
/* GS fog: (F C + (255 - F) FOGCOL) / 255 per channel (the GS uses F / 256
 * steps; within one unit). */
int fogChannel(int c, int f, int fogc);

}  /* namespace gs */
}  /* namespace pgl */

#endif /* PGL_NATIVE_PGL_STATE_GS_H */
