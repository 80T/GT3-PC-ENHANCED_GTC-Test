/* pgl_state_gs.cpp - GS register shadows -> native render-state description
 * (see pgl_state_gs.h and docs/pgl.md "State mapping"). */
#include "pgl_state_gs.h"

#include <cmath>

namespace pgl {
namespace gs {

static inline uint32_t bits(uint64_t v, int shift, int width)
{
    return (uint32_t)((v >> shift) & ((1ull << width) - 1));
}

/* ======================================================================== */
/* Register fields                                                          */
/* ======================================================================== */

Prim decodePrim(uint32_t prim)
{
    Prim p;
    p.type = (PrimType)(prim & 7);
    p.iip = (prim & 0x008) != 0;
    p.tme = (prim & 0x010) != 0;
    p.fge = (prim & 0x020) != 0;
    p.abe = (prim & 0x040) != 0;
    p.aa1 = (prim & 0x080) != 0;
    p.fst = (prim & 0x100) != 0;
    p.ctxt = (prim & 0x200) != 0;
    return p;
}

uint32_t encodePrim(const Prim &p)
{
    return (uint32_t)p.type | (p.iip ? 0x008 : 0) | (p.tme ? 0x010 : 0) | (p.fge ? 0x020 : 0) |
           (p.abe ? 0x040 : 0) | (p.aa1 ? 0x080 : 0) | (p.fst ? 0x100 : 0) | (p.ctxt ? 0x200 : 0);
}

Alpha decodeAlpha(uint64_t v)
{
    Alpha a;
    a.a = (uint8_t)bits(v, 0, 2);
    a.b = (uint8_t)bits(v, 2, 2);
    a.c = (uint8_t)bits(v, 4, 2);
    a.d = (uint8_t)bits(v, 6, 2);
    a.fix = (uint8_t)bits(v, 32, 8);
    return a;
}

Test decodeTest(uint64_t v)
{
    Test t;
    t.ate = bits(v, 0, 1) != 0;
    t.atst = (Atst)bits(v, 1, 3);
    t.aref = (uint8_t)bits(v, 4, 8);
    t.afail = (Afail)bits(v, 12, 2);
    t.date = bits(v, 14, 1) != 0;
    t.datm = bits(v, 15, 1) != 0;
    t.zte = bits(v, 16, 1) != 0;
    t.ztst = (Ztst)bits(v, 17, 2);
    return t;
}

Frame decodeFrame(uint64_t v)
{
    Frame f;
    f.fbp = (uint16_t)bits(v, 0, 9);
    f.fbw = (uint8_t)bits(v, 16, 6);
    f.psm = (uint8_t)bits(v, 24, 6);
    f.fbmsk = (uint32_t)(v >> 32);
    return f;
}

Zbuf decodeZbuf(uint64_t v)
{
    Zbuf z;
    z.zbp = (uint16_t)bits(v, 0, 9);
    z.psm = (uint8_t)bits(v, 24, 4);
    z.zmsk = bits(v, 32, 1) != 0;
    return z;
}

Scissor decodeScissor(uint64_t v)
{
    Scissor s;
    s.x0 = (uint16_t)bits(v, 0, 11);
    s.x1 = (uint16_t)bits(v, 16, 11);
    s.y0 = (uint16_t)bits(v, 32, 11);
    s.y1 = (uint16_t)bits(v, 48, 11);
    return s;
}

XyOffset decodeXyOffset(uint64_t v)
{
    XyOffset o;
    o.ofx = (uint16_t)bits(v, 0, 16);
    o.ofy = (uint16_t)bits(v, 32, 16);
    return o;
}

FogCol decodeFogCol(uint64_t v)
{
    FogCol f;
    f.r = (uint8_t)bits(v, 0, 8);
    f.g = (uint8_t)bits(v, 8, 8);
    f.b = (uint8_t)bits(v, 16, 8);
    return f;
}

Tex0 decodeTex0(uint64_t v)
{
    Tex0 t;
    t.tbp0 = (uint16_t)bits(v, 0, 14);
    t.tbw = (uint8_t)bits(v, 14, 6);
    t.psm = (uint8_t)bits(v, 20, 6);
    t.tw = (uint8_t)bits(v, 26, 4);
    t.th = (uint8_t)bits(v, 30, 4);
    t.tcc = bits(v, 34, 1) != 0;
    t.tfx = (uint8_t)bits(v, 35, 2);
    t.cbp = (uint16_t)bits(v, 37, 14);
    t.cpsm = (uint8_t)bits(v, 51, 4);
    t.csm = bits(v, 55, 1) != 0;
    t.csa = (uint8_t)bits(v, 56, 5);
    t.cld = (uint8_t)bits(v, 61, 3);
    return t;
}

Tex1 decodeTex1(uint64_t v)
{
    Tex1 t;
    t.lcm = bits(v, 0, 1) != 0;
    t.mxl = (uint8_t)bits(v, 2, 3);
    t.mmag = bits(v, 5, 1) != 0;
    t.mmin = (uint8_t)bits(v, 6, 3);
    t.mtba = bits(v, 9, 1) != 0;
    t.l = (uint8_t)bits(v, 19, 2);
    t.k = (int16_t)((int32_t)(bits(v, 32, 12) << 20) >> 20);
    return t;
}

Clamp decodeClamp(uint64_t v)
{
    Clamp c;
    c.wms = (uint8_t)bits(v, 0, 2);
    c.wmt = (uint8_t)bits(v, 2, 2);
    c.minu = (uint16_t)bits(v, 4, 10);
    c.maxu = (uint16_t)bits(v, 14, 10);
    c.minv = (uint16_t)bits(v, 24, 10);
    c.maxv = (uint16_t)bits(v, 34, 10);
    return c;
}

Texa decodeTexa(uint64_t v)
{
    Texa t;
    t.ta0 = (uint8_t)bits(v, 0, 8);
    t.aem = bits(v, 15, 1) != 0;
    t.ta1 = (uint8_t)bits(v, 32, 8);
    return t;
}

/* ======================================================================== */
/* Blend                                                                    */
/* ======================================================================== */

namespace {

/* The coefficient of one colour (Cs or Cd) in (A - B) C + D: c C + one. */
struct Coef { int c; int one; };

inline Coef coefOf(int sel, const Alpha &al)
{
    Coef k;
    k.c = (al.a == sel) - (al.b == sel);
    k.one = (al.d == sel);
    return k;
}

inline bool isZero(Coef k) { return k.c == 0 && k.one == 0; }
inline bool isNeg(Coef k) { return k.c < 0 && k.one == 0; }     /* -C */

/* A non-negative coefficient as a factor; OnePlusC (1 + C) has none. */
inline bool factorOf(Coef k, BlendFactor &f)
{
    if (k.c == 0 && k.one == 0) { f = BlendFactor::Zero; return true; }
    if (k.c == 0 && k.one == 1) { f = BlendFactor::One; return true; }
    if (k.c == 1 && k.one == 0) { f = BlendFactor::SrcC; return true; }
    if (k.c == -1 && k.one == 1) { f = BlendFactor::InvSrcC; return true; }
    return false;                       /* 1 + C */
}

}  /* namespace */

Blend translateBlend(const Alpha &alIn, bool abe, bool ct24Frame)
{
    Blend b;
    b.enable = false;
    b.src = BlendFactor::One;
    b.dst = BlendFactor::Zero;
    b.op = BlendOp::Add;
    b.c = CSource::As;
    b.premul = ColorPremul::None;
    b.fix = alIn.fix;
    b.selectors = alphaSelectors(alIn);
    b.exact = true;
    b.ad = AdMode::None;
    b.adSel = 0;
    if (!abe)
        return b;

    /* Reserved selectors (3): taken as zero / FIX (not exact). */
    Alpha al = alIn;
    if (al.a == 3) { al.a = 2; b.exact = false; }
    if (al.b == 3) { al.b = 2; b.exact = false; }
    if (al.d == 3) { al.d = 2; b.exact = false; }
    if (al.c == 3) { al.c = 2; b.exact = false; }
    /* A 24-bit frame has no alpha: the GS uses 1.0 for Ad (it skips the
     * multiplication), i.e. FIX 0x80. */
    if (al.c == 1 && ct24Frame) {
        al.c = 2;
        al.fix = 0x80;
        b.fix = 0x80;
    }
    b.c = (CSource)al.c;

    Coef ks = coefOf(0, al);            /* Cs */
    Coef kd = coefOf(1, al);            /* Cd */

    /* A == B: the result is D (C does not matter). */
    if (ks.c == 0 && kd.c == 0) {
        if (ks.one) {
            b.enable = false;           /* Cs: as without blending */
            return b;
        }
        b.enable = true;
        b.src = BlendFactor::Zero;
        b.dst = kd.one ? BlendFactor::One : BlendFactor::Zero;
        return b;
    }

    /* C = Ad (see AdMode): Cs Ad + Cd and Cs Ad as two DEST_ALPHA passes,
     * every other form in the shader from a copy of the target. */
    if (b.c == CSource::Ad) {
        b.adSel = (uint8_t)(al.a | (al.b << 2) | (al.d << 6));
        if (ks.c == 1 && ks.one == 0 && kd.c == 0) {
            b.enable = true;
            b.ad = AdMode::TwoPass;
            b.src = BlendFactor::DstAlpha;
            b.dst = kd.one ? BlendFactor::One : BlendFactor::Zero;
            b.op = BlendOp::Add;
        } else {
            b.enable = false;           /* the shader writes the result */
            b.ad = AdMode::Shader;
        }
        return b;
    }
    b.enable = true;

    BlendFactor fs = BlendFactor::Zero, fd = BlendFactor::Zero;
    if (isNeg(ks)) {
        /* dst kd - src C */
        b.op = BlendOp::RevSubtract;
        fs = BlendFactor::SrcC;
        if (!factorOf(kd, fd)) {
            fd = BlendFactor::One;      /* Cd (1 + C) - Cs C: the + Cd C is lost */
            b.exact = false;
        }
        if (isZero(kd)) {
            /* -Cs C alone: always <= 0, the clamp gives 0 */
            b.op = BlendOp::Add;
            fs = BlendFactor::Zero;
            fd = BlendFactor::Zero;
        }
    } else if (isNeg(kd)) {
        /* src ks - dst C */
        b.op = BlendOp::Subtract;
        fd = BlendFactor::SrcC;
        if (!factorOf(ks, fs)) {
            fs = BlendFactor::One;      /* Cs (1 + C) - Cd C: o0 saturates before the subtraction */
            b.premul = ColorPremul::OnePlusC;
            b.exact = false;
        }
        if (isZero(ks)) {
            b.op = BlendOp::Add;        /* -Cd C alone: 0 after the clamp */
            fs = BlendFactor::Zero;
            fd = BlendFactor::Zero;
        }
    } else {
        b.op = BlendOp::Add;
        if (!factorOf(ks, fs)) {
            fs = BlendFactor::One;      /* Cs (1 + C) [+ Cd ...] */
            b.premul = ColorPremul::OnePlusC;
        }
        if (!factorOf(kd, fd)) {
            /* Cd (1 + C): only with no Cs term, as C x DEST_COLOR + Cd */
            if (isZero(ks)) {
                fs = BlendFactor::DstColor;
                fd = BlendFactor::One;
                b.premul = ColorPremul::CAsColor;
            } else {
                fd = BlendFactor::One;
                b.exact = false;
            }
        }
    }

    /* Cs C [+ Cd]: the shader multiplies, as the GS does ((Cs C) >> 7, the
     * sum clamped), and the blend adds (src ONE).  C reaches 2.0 on the GS
     * (As up to 0xFF, FIX up to 0xFF), a blend factor only 1.0: the flare
     * queries' result texels (Cs As with the shape's alpha 2.0) came out at
     * half and flat above half visibility. */
    if (b.op == BlendOp::Add && fs == BlendFactor::SrcC && b.premul == ColorPremul::None &&
        (fd == BlendFactor::Zero || fd == BlendFactor::One)) {
        fs = BlendFactor::One;
        b.premul = ColorPremul::TimesC;
    } else if (b.c == CSource::Fix && al.fix > 0x80) {
        b.exact = false;
    }

    b.src = fs;
    b.dst = fd;
    return b;
}

Compare alphaCompare(Atst atst)
{
    switch (atst) {
    case Atst::Never: return Compare::Never;
    case Atst::Always: return Compare::Always;
    case Atst::Less: return Compare::Less;
    case Atst::LEqual: return Compare::LessEqual;
    case Atst::Equal: return Compare::Equal;
    case Atst::GEqual: return Compare::GreaterEqual;
    case Atst::Greater: return Compare::Greater;
    default: return Compare::NotEqual;
    }
}

Compare depthCompare(Ztst ztst)
{
    switch (ztst) {
    case Ztst::Never: return Compare::Never;
    case Ztst::Always: return Compare::Always;
    case Ztst::GEqual: return Compare::GreaterEqual;
    default: return Compare::Greater;
    }
}

/* ======================================================================== */
/* The draw state                                                           */
/* ======================================================================== */

static uint8_t writeMaskOf(uint32_t fbmsk, uint8_t fpsm, uint16_t &inexact)
{
    uint8_t m = 0;
    for (int ch = 0; ch < 4; ch++) {
        uint32_t byte = (fbmsk >> (8 * ch)) & 0xFF;
        if (byte == 0)
            m |= (uint8_t)(1 << ch);
        else if (byte != 0xFF) {
            m |= (uint8_t)(1 << ch);    /* partly written: drawn, flagged */
            inexact |= INEXACT_FBMSK;
        }
    }
    if ((fpsm & 0x0F) == PSMCT24)
        m &= WRITE_RGB;                 /* no alpha stored */
    if ((fpsm & 0x0F) == PSMCT16 || (fpsm & 0x0F) == PSMCT16S)
        inexact |= INEXACT_FRAME_16;
    return m;
}

DrawState translate(const Regs &r, uint32_t primBits)
{
    DrawState s;
    s.inexact = 0;
    s.prim = decodePrim(primBits);

    const Frame frame = decodeFrame(r.reg[REG_FRAME]);
    const Zbuf zbuf = decodeZbuf(r.reg[REG_ZBUF]);
    const Test test = decodeTest(r.reg[REG_TEST]);
    const Alpha alpha = decodeAlpha(r.reg[REG_ALPHA]);

    /* blend */
    s.blend = translateBlend(alpha, s.prim.abe, (frame.psm & 0x0F) == PSMCT24);
    if (!s.blend.exact) {
        if (alpha.a == 3 || alpha.b == 3 || alpha.c == 3 || alpha.d == 3)
            s.inexact |= INEXACT_BLEND_SEL3;
        else if (alpha.c == 2 && alpha.fix > 0x80)
            s.inexact |= INEXACT_BLEND_FIX;
        else
            s.inexact |= INEXACT_BLEND_FORM;
    }
    if (s.prim.abe && (r.gen[GEN_PABE] & 1))
        s.inexact |= INEXACT_PABE;
    s.colclamp = (r.gen[GEN_COLCLAMP] & 1) != 0;
    if (!s.colclamp)
        s.inexact |= INEXACT_COLCLAMP;
    if (r.gen[GEN_DTHE] & 1)
        s.inexact |= INEXACT_DITHER;
    if (s.prim.aa1)
        s.inexact |= INEXACT_AA1;
    if (!(r.gen[GEN_PRMODECONT] & 1))
        s.inexact |= INEXACT_PRMODE;

    /* depth: ZTE 0 (pgl's colour-only clears) = no depth access */
    s.zpsm = (uint8_t)(0x30 | zbuf.psm);
    s.depthTest = test.zte;
    s.depthFunc = test.zte ? depthCompare(test.ztst) : Compare::Always;
    s.depthWrite = test.zte && !zbuf.zmsk;
    if (test.zte && test.ztst == Ztst::Never)
        s.depthWrite = false;

    /* destination alpha test */
    s.date = test.date && (frame.psm & 0x0F) != PSMCT24;
    s.datm = test.datm;

    /* frame */
    s.fbmsk = frame.fbmsk;
    s.fpsm = frame.psm;
    s.frameWriteMask = writeMaskOf(frame.fbmsk, frame.psm, s.inexact);
    s.fba = (r.reg[REG_FBA] & 1) != 0 && (frame.psm & 0x0F) == PSMCT32;

    /* alpha test and AFAIL: the passes */
    s.alphaFunc = test.ate ? alphaCompare(test.atst) : Compare::Always;
    s.aref = test.aref;
    s.afail = test.afail;
    const uint8_t fullMask = s.frameWriteMask;
    const bool fullZ = s.depthWrite;
    uint8_t failMask = 0;
    bool failZ = false;
    switch (test.afail) {
    case Afail::Keep: break;
    case Afail::FbOnly: failMask = fullMask; break;
    case Afail::ZbOnly: failZ = fullZ; break;
    case Afail::RgbOnly: failMask = (uint8_t)(fullMask & WRITE_RGB); break;
    }
    s.pass[0] = Pass{AlphaGate::None, fullMask, fullZ};
    s.pass[1] = Pass{AlphaGate::None, 0, false};
    if (s.alphaFunc == Compare::Always) {
        s.passCount = 1;
    } else if (s.alphaFunc == Compare::Never) {
        s.pass[0] = Pass{AlphaGate::None, failMask, failZ};
        s.passCount = (failMask != 0 || failZ) ? 1 : 0;
    } else if (failMask == 0 && !failZ) {
        s.pass[0].gate = AlphaGate::Pass;
        s.passCount = 1;
    } else if (failMask == fullMask && failZ == fullZ) {
        s.passCount = 1;                /* both kinds write the same: no test needed */
    } else {
        s.pass[0].gate = AlphaGate::Pass;
        s.pass[1] = Pass{AlphaGate::Fail, failMask, failZ};
        s.passCount = 2;
    }
    if (s.depthTest && s.depthFunc == Compare::Never)
        s.passCount = 0;

    /* raster */
    s.scissor = decodeScissor(r.reg[REG_SCISSOR]);
    s.offset = decodeXyOffset(r.reg[REG_XYOFFSET]);
    s.scanmask = (uint8_t)(r.gen[GEN_SCANMSK] & 3);

    /* fog */
    s.fog = s.prim.fge;
    s.fogColor = decodeFogCol(r.gen[GEN_FOGCOL]);
    return s;
}

uint32_t packBlendKey(const Blend &b, uint8_t writeMask)
{
    if (!b.enable)
        return 0x80000000u | (uint32_t)(writeMask & 15);
    return (uint32_t)(writeMask & 15) | ((uint32_t)b.src << 4) | ((uint32_t)b.dst << 8) |
           ((uint32_t)b.op << 12) | (1u << 16);
}

uint32_t packDepthKey(const DrawState &s, const Pass &p)
{
    return (uint32_t)(s.depthTest ? 1 : 0) | ((uint32_t)s.depthFunc << 1) | ((p.depthWrite ? 1u : 0u) << 4) |
           ((s.date ? 1u : 0u) << 5) | ((s.datm ? 1u : 0u) << 6);
}

uint32_t packShaderKey(const DrawState &s, const Pass &p, uint64_t tex0)
{
    const Tex0 t = decodeTex0(tex0);
    uint32_t k = 0;
    k |= s.prim.tme ? 1u : 0u;
    k |= (uint32_t)(s.prim.tme ? t.tfx : 0) << 1;
    k |= (uint32_t)(s.prim.tme && t.tcc ? 1 : 0) << 3;
    k |= (uint32_t)(s.prim.fst ? 1 : 0) << 4;
    k |= (uint32_t)(s.fog ? 1 : 0) << 5;
    k |= (uint32_t)(s.fba ? 1 : 0) << 6;
    k |= (uint32_t)p.gate << 7;
    k |= (uint32_t)(p.gate != AlphaGate::None ? s.alphaFunc : Compare::Always) << 9;
    k |= (uint32_t)s.blend.premul << 12;
    k |= (uint32_t)s.blend.c << 15;
    k |= (uint32_t)s.scanmask << 17;
    k |= (uint32_t)(s.prim.iip ? 1 : 0) << 19;
    return k;
}

/* ======================================================================== */
/* Reference evaluation                                                     */
/* ======================================================================== */

int gsBlendChannel(const Alpha &al, int cs, int cd, int as, int ad, bool colclamp)
{
    auto pick = [&](int sel) { return sel == 0 ? cs : sel == 1 ? cd : 0; };
    int a = pick(al.a), b = pick(al.b), d = pick(al.d);
    int c = al.c == 0 ? as : al.c == 1 ? ad : al.fix;
    int v = (((a - b) * c) >> 7) + d;
    if (colclamp)
        return v < 0 ? 0 : v > 255 ? 255 : v;
    return v & 0xFF;
}

float nativeBlendChannel(const Blend &b, int cs, int cd, int as, int ad)
{
    auto sat = [](float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; };
    float src = cs / 255.0f;
    float dst = cd / 255.0f;
    float dstA = ad / 255.0f;
    if (b.ad == AdMode::Shader) {
        /* the shader's ((A - B) Ad >> 7) + D, clamped */
        auto pick = [&](int sel) { return sel == 0 ? cs : sel == 1 ? cd : 0; };
        const int v = (((pick(b.adSel & 3) - pick((b.adSel >> 2) & 3)) * ad) >> 7) + pick((b.adSel >> 6) & 3);
        return (float)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
    if (b.ad == AdMode::TwoPass) {
        /* two passes of Cs x DEST_ALPHA, each result stored as a UNORM byte */
        const float add = src * dstA;
        const float d1 = b.dst == BlendFactor::One ? dst : 0.0f;
        const float v1 = std::floor(sat(d1 + add) * 255.0f + 0.5f) / 255.0f;
        return std::floor(sat(v1 + add) * 255.0f + 0.5f);
    }
    if (!b.enable)
        return sat(src) * 255.0f;
    /* The shader knows As and FIX, not Ad: where a form needs C = Ad in the
     * shader (OnePlusC / CAsColor) it assumes 0x80, GT3's usual frame alpha. */
    float c = b.c == CSource::As ? as / 128.0f : b.c == CSource::Fix ? b.fix / 128.0f : 1.0f;
    float o1 = sat(c);                  /* the dual-source output (UNORM: saturated) */
    switch (b.premul) {
    case ColorPremul::None: break;
    case ColorPremul::OnePlusC: src = src * (1.0f + c); break;
    case ColorPremul::DstAlphaX2: src = src * (255.0f / 128.0f); break;
    case ColorPremul::CAsColor: src = c; break;
    case ColorPremul::Zero: src = 0.0f; break;
    case ColorPremul::TimesC: src = std::floor(cs * c) / 255.0f; break;
    }
    src = sat(src);
    auto factor = [&](BlendFactor f) {
        switch (f) {
        case BlendFactor::Zero: return 0.0f;
        case BlendFactor::One: return 1.0f;
        case BlendFactor::SrcC: return o1;
        case BlendFactor::InvSrcC: return 1.0f - o1;
        case BlendFactor::DstAlpha: return dstA;
        case BlendFactor::InvDstAlpha: return 1.0f - dstA;
        case BlendFactor::DstColor: return dst;
        }
        return 0.0f;
    };
    float s = src * factor(b.src), d = dst * factor(b.dst);
    float v = b.op == BlendOp::Add ? s + d : b.op == BlendOp::Subtract ? s - d : d - s;
    return sat(v) * 255.0f;
}

int texFuncColor(uint8_t tfx, int ct, int cf, int af)
{
    int v;
    switch (tfx & 3) {
    case 0: v = (ct * cf) >> 7; break;              /* MODULATE */
    case 1: v = ct; break;                          /* DECAL */
    default: v = ((ct * cf) >> 7) + af; break;      /* HIGHLIGHT, HIGHLIGHT2 */
    }
    return v > 255 ? 255 : v;
}

int texFuncAlpha(uint8_t tfx, bool tcc, int at, int af)
{
    if (!tcc)
        return af;
    int v;
    switch (tfx & 3) {
    case 0: v = (at * af) >> 7; break;              /* MODULATE */
    case 2: v = at + af; break;                     /* HIGHLIGHT */
    default: v = at; break;                         /* DECAL, HIGHLIGHT2 */
    }
    return v > 255 ? 255 : v;
}

int fogChannel(int c, int f, int fogc)
{
    float v = (float)fogc + ((float)c - (float)fogc) * ((float)f / 255.0f);
    return (int)std::trunc(v);
}

}  /* namespace gs */
}  /* namespace pgl */
