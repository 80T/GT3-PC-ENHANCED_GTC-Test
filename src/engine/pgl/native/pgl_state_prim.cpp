/* pgl_state_prim.cpp - the GS vertices of pgl's immediate primitives (see
 * pgl_state_prim.h).  Each function repeats the integer arithmetic of the
 * routine it names (decomp src/engine/pgl/pgl.c, render.c), down to the
 * truncations and the carries between the X and Y halves of the XY word. */
#include "pgl_state_prim.h"

namespace pgl {
namespace prim {

static inline int yShiftOf(const PrimCtx &c) { return c.halfHeight ? 3 : 4; }

/* The XY word of an XYZF register: X the low half, Y the high half. */
static inline void setXY(GsVertex &v, uint32_t xy)
{
    v.x = (uint16_t)(xy & 0xFFFF);
    v.y = (uint16_t)(xy >> 16);
}

/* The Z / F of an XYZF data word (spriteZF). */
static inline void setZF(GsVertex &v, uint32_t zf)
{
    v.z = zf & 0xFFFFFF;
    v.f = (uint8_t)(zf >> 24);
}

/* The UV register: U bits 0..13, V bits 16..29 of the data word. */
static inline void setUV(GsVertex &v, uint64_t uv)
{
    v.u = (uint16_t)(uv & 0x3FFF);
    v.v = (uint16_t)((uv >> 16) & 0x3FFF);
}

static inline GsVertex blank(uint32_t rgba)
{
    GsVertex v;
    v.x = v.y = 0;
    v.z = 0;
    v.f = 0xFF;
    v.rgba = rgba;
    v.u = v.v = 0;
    v.kick = false;
    return v;
}

static inline uint32_t untextured(uint32_t attr, uint32_t type) { return (uint16_t)((attr | type) & ~PRIM_TME); }

/* gs_draw_flat_sprite (0x00255C90): XY = ((yoff + (y << s)) << 16) |
 * (xoff + (x << 4)) as int, stored as a word (a negative X fills Y). */
Batch flatSprite(const PrimCtx &c, int x0, int y0, int x1, int y1, GsVertex out[2])
{
    const int s = yShiftOf(c);
    int32_t xy0 = (int32_t)(((uint32_t)(c.yoff + (y0 << s))) << 16) | (int32_t)(c.xoff + (x0 << 4));
    int32_t xy1 = (int32_t)(((uint32_t)(c.yoff + (y1 << s))) << 16) | (int32_t)(c.xoff + (x1 << 4));
    out[0] = blank(c.color);
    out[1] = blank(c.color);
    setXY(out[0], (uint32_t)xy0);
    setXY(out[1], (uint32_t)xy1);
    setZF(out[0], c.spriteZF);
    setZF(out[1], c.spriteZF);
    out[1].kick = true;
    return Batch{untextured(c.primAttr, PRIM_SPRITE), 2};
}

int flatSpriteStripCount(int x0, int x1)
{
    if (x1 < x0) {
        int t = x0; x0 = x1; x1 = t;
    }
    return ((x1 - 1) >> 6) - (x0 >> 6) + 1;
}

/* gs_draw_flat_sprite_strips (0x00255DF0): sprites up to each 64-pixel
 * boundary of the window x; xyTop = (xoff | yoff << 16) + (y0 << (s + 16)). */
Batch flatSpriteStrips(const PrimCtx &c, int x0, int y0, int x1, int y1, GsVertex *out)
{
    const int s = yShiftOf(c);
    if (x1 < x0) {
        int t = x0; x0 = x1; x1 = t;
    }
    int nStrips = ((x1 - 1) >> 6) - (x0 >> 6);
    uint64_t x = (uint64_t)(int64_t)(x0 << 4);
    uint64_t xEnd = (uint64_t)(int64_t)(x1 << 4);
    uint32_t base = (uint32_t)c.xoff | ((uint32_t)c.yoff << 16);
    uint64_t xyTop = base + ((uint32_t)y0 << (s + 16));
    uint64_t xyBottom = base + ((uint32_t)y1 << (s + 16));
    int n = 0;
    while (nStrips-- != 0) {
        uint64_t nextX = (x + (64 << 4)) & ~(uint64_t)0x3F0;
        out[n] = blank(c.color);
        setXY(out[n], (uint32_t)(xyTop + x));
        setZF(out[n], c.spriteZF);
        n++;
        out[n] = blank(c.color);
        setXY(out[n], (uint32_t)(xyBottom + nextX));
        setZF(out[n], c.spriteZF);
        out[n].kick = true;
        n++;
        x = nextX;
    }
    out[n] = blank(c.color);
    setXY(out[n], (uint32_t)(xyTop + x));
    setZF(out[n], c.spriteZF);
    n++;
    out[n] = blank(c.color);
    setXY(out[n], (uint32_t)(xyBottom + xEnd));
    setZF(out[n], c.spriteZF);
    out[n].kick = true;
    n++;
    return Batch{untextured(c.primAttr, PRIM_SPRITE), n};
}

/* gs_draw_textured_sprite (0x00256020): window origin and offset added;
 * UV = ((v << 16) | (u & 0xFFFF)) << 4, + 0x80008 (half a texel each). */
Batch texturedSprite(const PrimCtx &c, int x0, int y0, int x1, int y1, int u0, int v0, int u1, int v1,
                     GsVertex out[2])
{
    const int s = yShiftOf(c);
    const int32_t winX = c.winX;
    const int32_t winY = c.winY << 4;
    uint32_t xy0 = ((uint32_t)((y0 << s) + winY + c.yoff) << 16) | (uint32_t)(((x0 + winX) << 4) + c.xoff);
    uint32_t xy1 = ((uint32_t)((y1 << s) + winY + c.yoff) << 16) | (uint32_t)(((x1 + winX) << 4) + c.xoff);
    uint64_t uv0 = (uint64_t)(int64_t)(int32_t)(((uint32_t)v0 << 16) | ((uint32_t)u0 & 0xFFFF)) << 4;
    uint64_t uv1 = (uint64_t)(int64_t)(int32_t)(((uint32_t)v1 << 16) | ((uint32_t)u1 & 0xFFFF)) << 4;
    uv0 += (8 << 16) | 8;
    uv1 += (8 << 16) | 8;
    out[0] = blank(fullColor(c));           /* PC: was colorTex (PRIM_FULL_COLOR) */
    out[1] = blank(fullColor(c));
    setXY(out[0], xy0);
    setXY(out[1], xy1);
    setUV(out[0], uv0);
    setUV(out[1], uv1);
    setZF(out[0], c.spriteZF);
    setZF(out[1], c.spriteZF);
    out[1].kick = true;
    return Batch{(uint32_t)(c.primAttr | PRIM_SPRITE | PRIM_TME | PRIM_FST | PRIM_FULL_COLOR), 2};
}

/* gs_draw_textured_sprite_f (0x00256188). */
Batch texturedSpriteF(const PrimCtx &c, float x0, float y0, float x1, float y1,
                      float u0, float v0, float u1, float v1, GsVertex out[2])
{
    const float yScale = (float)(c.halfHeight ? 8 : 16);
    const float winX = (float)(c.winX << 4);
    const float winY = (float)(c.winY << 4);
    x1 = winX + x1 * 16.0f;
    y1 = winY + y1 * yScale;
    x0 = winX + x0 * 16.0f;
    y0 = winY + y0 * yScale;
    u0 *= 16.0f;
    v0 *= 16.0f;
    u1 *= 16.0f;
    v1 *= 16.0f;
    uint32_t xy0 = ((uint32_t)(c.yoff + vu::ftoi0(y0)) << 16) | ((uint32_t)(c.xoff + vu::ftoi0(x0)) & 0xFFFF);
    uint32_t xy1 = ((uint32_t)(c.yoff + vu::ftoi0(y1)) << 16) | ((uint32_t)(c.xoff + vu::ftoi0(x1)) & 0xFFFF);
    uint32_t uv0 = ((uint32_t)(vu::ftoi0(v0) + 8) << 16) | ((uint32_t)(vu::ftoi0(u0) + 8) & 0xFFFF);
    uint32_t uv1 = ((uint32_t)(vu::ftoi0(v1) + 8) << 16) | ((uint32_t)(vu::ftoi0(u1) + 8) & 0xFFFF);
    out[0] = blank(fullColor(c));           /* PC: was colorTex (PRIM_FULL_COLOR) */
    out[1] = blank(fullColor(c));
    setXY(out[0], xy0);
    setXY(out[1], xy1);
    setUV(out[0], uv0);
    setUV(out[1], uv1);
    setZF(out[0], c.spriteZF);
    setZF(out[1], c.spriteZF);
    out[1].kick = true;
    return Batch{(uint32_t)(c.primAttr | PRIM_SPRITE | PRIM_TME | PRIM_FST | PRIM_FULL_COLOR), 2};
}

/* The 12.4 x of gs_draw_textured_sprite_strips_f. */
static inline int32_t stripX(const PrimCtx &c, float x)
{
    const float winX = (float)c.winX * 16.0f;
    return (int32_t)c.xoff + vu::ftoi0(winX + x * 16.0f);
}

int texturedSpriteStripCount(const PrimCtx &c, float x0, float x1)
{
    int32_t X0 = stripX(c, x0), X1 = stripX(c, x1);
    if (X1 < X0) {
        int32_t t = X0; X0 = X1; X1 = t;
    }
    if (X1 == X0)
        return 0;
    return ((X1 - 1) >> 9) - (X0 >> 9) + 1;
}

/* gs_draw_textured_sprite_strips_f (0x00256360): sprites up to each
 * 32-pixel boundary (512 in 12.4, the low 4 bits kept), u interpolated
 * with an integer division; XY = (Y << 16) + X (a carry crosses over). */
Batch texturedSpriteStripsF(const PrimCtx &c, float x0, float y0, float x1, float y1,
                            float u0, float v0, float u1, float v1, GsVertex *out)
{
    const float yScale = c.halfHeight ? 8.0f : 16.0f;
    const float winY = (float)c.winY * 16.0f;
    const uint32_t prim = (uint16_t)(c.primAttr | PRIM_SPRITE | PRIM_TME | PRIM_FST | PRIM_FULL_COLOR);
    int32_t X0 = stripX(c, x0);
    int32_t X1 = stripX(c, x1);
    int32_t Y0 = (int32_t)c.yoff + vu::ftoi0(winY + y0 * yScale);
    int32_t Y1 = (int32_t)c.yoff + vu::ftoi0(winY + y1 * yScale);
    int32_t U0 = vu::ftoi0(u0 * 16.0f + 8.0f);
    int32_t V0 = vu::ftoi0(v0 * 16.0f + 8.0f);
    int32_t U1 = vu::ftoi0(u1 * 16.0f + 8.0f);
    int32_t V1 = vu::ftoi0(v1 * 16.0f + 8.0f);
    if (X1 < X0) {
        int32_t t = X0; X0 = X1; X1 = t;
        t = U0; U0 = U1; U1 = t;
    }
    int32_t width = X1 - X0;
    int nStrips = ((X1 - 1) >> 9) - (X0 >> 9);
    int32_t du = U1 - U0;
    int64_t curX = X0, xEnd = X1;
    int32_t curU = U0, uEnd = U1;
    if (width == 0)
        return Batch{prim, 0};
    uint64_t y0Hi = (uint64_t)(int64_t)(int32_t)((uint32_t)Y0 << 16);
    uint64_t y1Hi = (uint64_t)(int64_t)(int32_t)((uint32_t)Y1 << 16);
    uint64_t v0Hi = (uint64_t)(int64_t)(int32_t)((uint32_t)V0 << 16);
    uint64_t v1Hi = (uint64_t)(int64_t)(int32_t)((uint32_t)V1 << 16);
    int n = 0;
    auto put = [&](uint64_t uv, uint64_t xy, bool kick) {
        out[n] = blank(fullColor(c));       /* PC: was colorTex (PRIM_FULL_COLOR) */
        setUV(out[n], uv);
        setXY(out[n], (uint32_t)xy);
        setZF(out[n], c.spriteZF);
        out[n].kick = kick;
        n++;
    };
    while (nStrips-- != 0) {
        int64_t nextX = (curX + (32 << 4)) & ~(int64_t)0x1F0;
        int32_t nextU = U0 + (du * ((int32_t)nextX - X0)) / width;
        put(v0Hi + (uint64_t)(int64_t)curU, y0Hi + (uint64_t)curX, false);
        put(v1Hi + (uint64_t)(int64_t)nextU, y1Hi + (uint64_t)nextX, true);
        curX = nextX;
        curU = nextU;
    }
    put(v0Hi + (uint64_t)(int64_t)curU, y0Hi + (uint64_t)curX, false);
    put(v1Hi + (uint64_t)(int64_t)uEnd, y1Hi + (uint64_t)xEnd, true);
    return Batch{prim, n};
}

/* gs_draw_line (0x002566F0): relative to the draw window. */
Batch line(const PrimCtx &c, int x0, int y0, int x1, int y1, GsVertex out[2])
{
    const int s = yShiftOf(c);
    int32_t originX = (int32_t)c.xoff + (c.winX << 4);
    int32_t originY = (int32_t)c.yoff + (c.winY << 4);
    x0 = originX + (x0 << 4);
    y0 = originY + (y0 << s);
    x1 = originX + (x1 << 4);
    y1 = originY + (y1 << s);
    out[0] = blank(c.color);
    out[1] = blank(c.color);
    setXY(out[0], ((uint32_t)y0 << 16) | ((uint32_t)x0 & 0xFFFF));
    setXY(out[1], ((uint32_t)y1 << 16) | ((uint32_t)x1 & 0xFFFF));
    setZF(out[0], c.spriteZF);
    setZF(out[1], c.spriteZF);
    out[1].kick = true;
    return Batch{untextured(c.primAttr, PRIM_LINE), 2};
}

/* gs_draw_line_f (0x00256838). */
Batch lineF(const PrimCtx &c, float x0, float y0, float x1, float y1, GsVertex out[2])
{
    const float yScale = c.halfHeight ? 8.0f : 16.0f;
    int32_t X0 = (int32_t)c.xoff + vu::ftoi0(x0 * 16.0f) + (c.winX << 4);
    int32_t X1 = (int32_t)c.xoff + vu::ftoi0(x1 * 16.0f) + (c.winX << 4);
    int32_t Y1 = (int32_t)c.yoff + vu::ftoi0(y1 * yScale) + (c.winY << 4);
    int32_t Y0 = (int32_t)c.yoff + vu::ftoi0(y0 * yScale) + (c.winY << 4);
    out[0] = blank(c.color);
    out[1] = blank(c.color);
    setXY(out[0], ((uint32_t)Y0 << 16) | ((uint32_t)X0 & 0xFFFF));
    setXY(out[1], ((uint32_t)Y1 << 16) | ((uint32_t)X1 & 0xFFFF));
    setZF(out[0], c.spriteZF);
    setZF(out[1], c.spriteZF);
    out[1].kick = true;
    return Batch{untextured(c.primAttr, PRIM_LINE), 2};
}

static inline void setXyzf(GsVertex &v, const GsXyzf &g)
{
    v.x = g.x;
    v.y = g.y;
    v.z = g.z;
    v.f = g.f;
}

/* pgl_draw_line_strip_2d (0x002569C0). */
Batch lineStrip2d(const PrimCtx &c, const Vu0 &vu, const float *xy, int n, GsVertex *out)
{
    const uint32_t prim = (c.primAttr & ~PRIM_TME) | PRIM_LINE_STRIP;
    if (n < 2)
        return Batch{prim, 0};
    uint32_t prevClip = 0;
    for (int i = 0; i < n; i++) {
        float x = xy[i * 2], y = xy[i * 2 + 1];
        Vec4 p;
        for (int k = 0; k < 4; k++) {               /* vmulax vf24, vmadday vf25, vmaddw vf27 */
            float a = vu.vf[24][k] * x;
            a = a + vu.vf[25][k] * y;
            p[k] = a + vu.vf[27][k];
        }
        float q = vu::div(1.0f, p.w);
        Vec4 s = vu::mini(p, c.vp.limits);
        uint32_t clip = vu::clipFlags(p, p.w);
        if (f2u(p.w) >> 31)
            clip = ((clip >> 1) | 0x2A) ^ ((clip << 1) | 0x15);
        clip &= 0x3F;
        s = vu::scale(s, q);
        s = vu::madd(c.vp.center, s, c.vp.scale);
        if (prevClip & clip)
            s.w = s.w + 2048.0f;                    /* ADC: the segment is not drawn */
        prevClip = clip;
        GsXyzf g = toXyzf2(s);
        out[i] = blank(c.color);
        setXyzf(out[i], g);
        out[i].kick = !g.adc;
    }
    return Batch{prim, n};
}

/* pgl_draw_line_3d (0x00256E60). */
Batch line3d(const PrimCtx &c, const Vu0 &vu, const float *p0, const float *p1, GsVertex out[2])
{
    const uint32_t prim = untextured(c.primAttr, PRIM_LINE);
    Vec4 a = vu::xformW1(&vu.vf[24], Vec4{p0[0], p0[1], p0[2], 1.0f});
    Vec4 b = vu::xformW1(&vu.vf[24], Vec4{p1[0], p1[1], p1[2], 1.0f});
    if (clipLineHomogeneous(b, a) != 0)             /* (p1, p0): the binary's argument order */
        return Batch{prim, 0};
    GsXyzf ga = toXyzf2(projectClip(a, c.vp));
    GsXyzf gb = toXyzf2(projectClip(b, c.vp));
    out[0] = blank(c.color);
    out[1] = blank(c.color);
    setXyzf(out[0], ga);
    setXyzf(out[1], gb);
    out[0].kick = true;                             /* XYZF2 both (the first starts the line) */
    out[1].kick = true;
    return Batch{prim, 2};
}

/* draw_poly2d (0x0024C148). */
Batch polyFan2d(const PrimCtx &c, const float *xy, const float *uv, const uint32_t *rgba, int n,
                GsVertex *out)
{
    const float yScale = c.halfHeight ? 8.0f : 16.0f;
    uint32_t prim = (c.primAttr | (PRIM_FST | PRIM_IIP | PRIM_TRI_FAN)) & 0xFFFF;
    if (uv != nullptr)
        prim |= PRIM_TME;
    if (uv != nullptr && rgba == nullptr)
        prim |= PRIM_FULL_COLOR;            /* PC: one constant colour, whole */
    const float winX16 = (float)c.winX * 16.0f;
    const float winY16 = (float)c.winY * 16.0f;
    const uint32_t constColor = (uv != nullptr) ? fullColor(c) : c.color;
    for (int i = 0; i < n; i++) {
        out[i] = blank(rgba != nullptr ? rgba[i] : constColor);
        if (uv != nullptr) {
            float v = uv[i * 2 + 1] * 16.0f + 8.0f;
            float u = uv[i * 2] * 16.0f + 8.0f;
            setUV(out[i], (uint64_t)(int64_t)(int32_t)((uint32_t)vu::ftoi0(u) | ((uint32_t)vu::ftoi0(v) << 16)));
        }
        float x = winX16 + xy[i * 2] * 16.0f;
        float y = winY16 + xy[i * 2 + 1] * yScale;
        setXY(out[i], (((uint32_t)c.xoff + (uint32_t)vu::ftoi0(x)) & 0xFFFF) |
                          (((uint32_t)c.yoff + (uint32_t)vu::ftoi0(y)) << 16));
        setZF(out[i], c.spriteZF);
        out[i].kick = (i >= 2);                     /* XYZF3 for the first two */
    }
    return Batch{prim, n};
}

float depthOf(uint32_t z, uint8_t zpsm)
{
    switch (zpsm & 0x0F) {
    case 0x0:                                       /* PSMZ32 */
        return (float)((double)z / 4294967296.0);
    case 0x2:
    case 0xA:                                       /* PSMZ16 / 16S */
        if (z > 0xFFFF)
            z = 0xFFFF;
        return (float)z / 65536.0f;
    default:                                        /* PSMZ24 */
        if (z > 0xFFFFFF)
            z = 0xFFFFFF;
        return (float)z / 16777216.0f;
    }
}

}  /* namespace prim */
}  /* namespace pgl */
