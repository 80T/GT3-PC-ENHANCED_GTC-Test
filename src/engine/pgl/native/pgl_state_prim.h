/* pgl_state_prim.h - the vertices of pgl's immediate primitives, as the GS
 * received them, and their conversion to native window coordinates.
 *
 * pgl's 2D / immediate paths (pgl.c: gs_draw_flat_sprite[_strips],
 * gs_draw_textured_sprite[_f / _strips_f], gs_draw_line[_f],
 * pgl_draw_line_strip_2d, pgl_draw_line_3d; render.c: draw_poly2d) compute
 * GS register values on the CPU - 12.4 fixed-point XY with the draw
 * buffer's XYOFFSET base added, 10.4 UVs with a half-texel bias, the
 * sprite Z / F word - and packed them into GIF packets.  The functions
 * here compute the same values (the same integer truncations, the same
 * 16-bit wrap-arounds) without the packet, so the native renderer draws
 * exactly the pixels the GS would, and convert them to window / native
 * coordinates:
 *
 *   window x = (X - XYOFFSET.OFX) / 16, y = (Y - OFY) / 16   (GS pixels; the
 *     GS samples pixel centres at integer window coordinates)
 *   native  = (window + 0.5) x (native size / GS target size)
 *
 * The VU1 primitive path (pgl_draw_strip, strip_batch_*) sends untransformed
 * vertices; docs/pgl.md describes what its program receives.
 */
#ifndef PGL_NATIVE_PGL_STATE_PRIM_H
#define PGL_NATIVE_PGL_STATE_PRIM_H

#include <cstdint>

#include "pgl_math.h"

namespace pgl {
namespace prim {

/* GS PRIM type / attribute bits (GsContext.primAttr holds IIP..AA1). */
enum : uint32_t {
    PRIM_POINT = 0, PRIM_LINE = 1, PRIM_LINE_STRIP = 2, PRIM_TRIANGLE = 3, PRIM_TRI_STRIP = 4,
    PRIM_TRI_FAN = 5, PRIM_SPRITE = 6,
    PRIM_IIP = 0x008, PRIM_TME = 0x010, PRIM_FGE = 0x020, PRIM_ABE = 0x040, PRIM_AA1 = 0x080,
    PRIM_FST = 0x100, PRIM_CTXT = 0x200
};

/* One vertex as the GS registers held it when its XYZ register was written. */
struct GsVertex {
    uint16_t x, y;      /* XYZ: primitive coordinates, 12.4 (16 bits each) */
    uint32_t z;         /* Z (24 bits for XYZF2/3) */
    uint8_t  f;         /* fog coefficient (255: no fog) */
    uint32_t rgba;      /* RGBAQ colour, r | g << 8 | b << 16 | a << 24 */
    uint16_t u, v;      /* UV (FST): 10.4, 14 bits each */
    bool     kick;      /* XYZ2 / XYZF2 (drawing kick) - false: XYZ3 / ADC, no primitive */
};

/* The GsContext members the immediate paths read (G001_flare.h names). */
struct PrimCtx {
    uint16_t xoff, yoff;        /* tgtA.xoff / yoff: the XYOFFSET base (12.4) */
    int32_t  winX, winY;        /* the draw window origin */
    bool     halfHeight;        /* field rendering: y in 12.3 */
    uint32_t color, colorTex;   /* pgl_color: untextured / textured (x 129 / 256) colours */
    uint32_t spriteZF;          /* Z | F << 24 of the 2D primitives */
    uint32_t primAttr;          /* IIP / TME / FGE / ABE / AA1 */
    Viewport vp;                /* viewScale, viewCenter, limits, vBB0 */
};
template <class Ctx> inline PrimCtx primCtxOf(const Ctx &c)
{
    PrimCtx p;
    p.xoff = c.tgtA.xoff;
    p.yoff = c.tgtA.yoff;
    p.winX = c.winX;
    p.winY = c.winY;
    p.halfHeight = c.halfHeight != 0;
    p.color = c.color;
    p.colorTex = c.colorTex;
    p.spriteZF = c.spriteZF;
    p.primAttr = c.primAttr;
    p.vp = viewportOf(c);
    return p;
}

/* A primitive: its PRIM value and n vertices in out[] (the caller's array).
 * For sprites every second vertex kicks (one sprite per pair); line strips
 * and fans as the GS (kick false: the vertex starts / restarts). */
struct Batch {
    uint32_t prim;
    int      n;
};

/* gs_draw_flat_sprite: 2 vertices; window origin NOT added (as retail). */
Batch flatSprite(const PrimCtx &c, int x0, int y0, int x1, int y1, GsVertex out[2]);
/* gs_draw_flat_sprite_strips: 64-pixel strips; out needs flatSpriteStripCount * 2. */
int flatSpriteStripCount(int x0, int x1);
Batch flatSpriteStrips(const PrimCtx &c, int x0, int y0, int x1, int y1, GsVertex *out);
/* gs_draw_textured_sprite (ints: pixels, texels). */
Batch texturedSprite(const PrimCtx &c, int x0, int y0, int x1, int y1, int u0, int v0, int u1, int v1,
                     GsVertex out[2]);
/* gs_draw_textured_sprite_f */
Batch texturedSpriteF(const PrimCtx &c, float x0, float y0, float x1, float y1,
                      float u0, float v0, float u1, float v1, GsVertex out[2]);
/* gs_draw_textured_sprite_strips_f: 32-pixel strips with u interpolated;
 * out needs texturedSpriteStripCount * 2 (0: nothing drawn). */
int texturedSpriteStripCount(const PrimCtx &c, float x0, float x1);
Batch texturedSpriteStripsF(const PrimCtx &c, float x0, float y0, float x1, float y1,
                            float u0, float v0, float u1, float v1, GsVertex *out);
/* gs_draw_line / gs_draw_line_f: 2 vertices (XYZF3, XYZF2). */
Batch line(const PrimCtx &c, int x0, int y0, int x1, int y1, GsVertex out[2]);
Batch lineF(const PrimCtx &c, float x0, float y0, float x1, float y1, GsVertex out[2]);
/* pgl_draw_line_strip_2d: n points (x, y) through the combined matrix
 * vf24..vf27 (sync it first); a segment whose ends share a clip plane gets
 * ADC (no kick).  out needs n. */
Batch lineStrip2d(const PrimCtx &c, const Vu0 &vu, const float *xy, int n, GsVertex *out);
/* pgl_draw_line_3d: the clipped segment (n = 2), or n = 0 when rejected. */
Batch line3d(const PrimCtx &c, const Vu0 &vu, const float *p0, const float *p1, GsVertex out[2]);
/* draw_poly2d: a fan of n window-pixel points, optional texels / colours. */
Batch polyFan2d(const PrimCtx &c, const float *xy, const float *uv, const uint32_t *rgba, int n,
                GsVertex *out);

/* ======================================================================== */
/* Coordinates                                                              */
/* ======================================================================== */

/* GS window pixels of a vertex under XYOFFSET (ofx, ofy, 12.4). */
inline float windowX(uint16_t x, uint16_t ofx) { return (float)((int32_t)x - (int32_t)ofx) * 0.0625f; }
inline float windowY(uint16_t y, uint16_t ofy) { return (float)((int32_t)y - (int32_t)ofy) * 0.0625f; }

/* Native render-target coordinates of a GS window position: the GS pixel
 * centre (integer) is the native centre of the scaled pixel. */
inline float toNative(float window, float scale) { return (window + 0.5f) * scale; }

/* The depth of a GS Z for a native depth buffer: Z / 2^bits of the Z format
 * (PSMZ32 / 24 / 16 / 16S low nibble 0 / 1 / 2 / 0xA), Z clamped to its
 * format's maximum. */
float depthOf(uint32_t z, uint8_t zpsm);

/* Texel coordinates of a UV (FST): u / 16 texels (the +8 half-texel bias of
 * pgl's sprites puts the GS sample at the texel centre). */
inline float texelU(uint16_t u) { return (float)u * 0.0625f; }

}  /* namespace prim */
}  /* namespace pgl */

#endif /* PGL_NATIVE_PGL_STATE_PRIM_H */
