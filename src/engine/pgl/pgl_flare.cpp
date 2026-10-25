/* pgl_flare.cpp - the lens-flare / sun-glare part of pgl: the decompiled
 * flare.c (0x002466F0..0x002487B8), ported.  The work area is the start of
 * FlareClass (T159).
 *
 * The models (VIF packets for the VU1 flare program, two prebuilt, two
 * built at start-up), the occlusion query (the screen's alpha in a box
 * cleared, the light's square or octagon drawn with alpha 2.0 depth-tested,
 * the box copied to the work buffer and reduced to one texel by bilinear
 * halving, stored scaled into the result strip, read back) are the
 * decomp's GS work, which the renderer draws natively.  PC: the VU0 asm is
 * pgl_math's; the prebuilt reduction packets (DMA call targets) are
 * recorded command lists of the renderer (pglr_sub_*); vif_model_draw
 * records each part for the flare program (pglr_model_part, the result
 * group read at execution). */
#include "engine/pgl/pgl_port.h"

#define GS_DIRTY_ALL_CTX1_GEN   ((((1ull << GS_GEN_COUNT) - 1) << 32) | ((1ull << GS_REG_COUNT) - 1))
#define FLARE_SAVED_ATTRIBS (PGL_ATTR_SCISSOR | PGL_ATTR_VIEWPORT | PGL_ATTR_TARGET_B | \
                             PGL_ATTR_DEPTH | PGL_ATTR_DRAW_BUFFER)

extern "C" {
f32  flare_project(const f32 *pos, f32 *sx, f32 *sy);
void flare_fill_octagon(f32 cx, f32 cy, f32 r, f32 aspect);
void flare_set_target(u32 fbWordAddr, int w, int h);
void flare_set_texture(u32 texWordAddr, int w, int h);
void flare_copy_visibility(FlareWork *fw, int x0, int y0, int x1, int y1);
void flare_reduce(FlareWork *fw, int w, int h);
u32 *flare_build_reduce(FlareWork *fw, int n);
void flare_run_reduce(FlareWork *fw, int w, int h);
void flare_store_result(FlareWork *fw, f32 scale, int w, int h);
int  flare_occlusion_query(FlareWork *fw, int octagon, f32 sx, f32 sy, f32 size, f32 scale);
void flare_draw_result_disc(FlareWork *fw, VifModel *m, f32 r, f32 g, f32 b);
void flare_draw_disc(FlareWork *fw, VifModel *m, f32 r, f32 g, f32 b, f32 intensity);
int  flare_query_point(FlareWork *fw, f32 scale, f32 size);
extern VifModel *g_003530B0;
extern VifModel g_002F5BD0;
extern VifModel g_002F5D48;
extern const FlareRayVert g_00343D30[72];
extern const u32 g_00344AB0[4];
extern const f32 g_00344AC0[4];
}

/* ======================================================================== */
/* Data                                                                     */
/* ======================================================================== */

/* The prebuilt sun models (flare_sun_model_a / _b): each a VIF1 packet for
 * the VU1 model program (the layout of flare_build_disc's: GIFtag, xyz,
 * strip table, masked ST, colours) in front of its VifModel header.  The
 * packets go out by DMA (vif_model_draw: a ref tag), so they are
 * qword-aligned. */
// DATA_START 0x002F5B40
alignas(16) static u32 g_002F5B40[36] = {    /* 0x002F5B40 */
    VIF_UNPACK(VIF_V4_32, 1, 0xC0),
    0x00008004, 0x302E4000, 0x00000412, 0x00000000,     /* GIFtag NLOOP 4, EOP, PRE, PRIM 0x5C, ST RGBAQ XYZF2 */
    VIF_UNPACK(VIF_V3_32, 4, 0),                    /* xyz (1, 1, 0) (-1, 1, 0) (1, -1, 0) (-1, -1, 0) */
    0x3F800000, 0x3F800000, 0x00000000, 0xBF800000,
    0x3F800000, 0x00000000, 0x3F800000, 0xBF800000,
    0x00000000, 0xBF800000, 0xBF800000, 0x00000000,
    VIF_UNPACK(VIF_S_8, 2, 0x40), 0x00000601,          /* the strip table */
    VIF_UNPACK_M(VIF_V2_32, 4, 0x40),               /* ST (0, 0) (1, 0) (0, 1) (1, 1) */
    0x00000000, 0x00000000, 0x3F800000, 0x00000000,
    0x00000000, 0x3F800000, 0x3F800000, 0x3F800000,
    VIF_UNPACK(VIF_V4_8, 4, 0x80),                  /* RGBA */
    0xFF808080, 0xFF808080, 0xFF808080, 0xFF808080,
    0x00000000, 0x00000000
};
VifModel g_002F5BD0 = {                 /* 0x002F5BD0 */
    NULL, NULL, 0, 0, 1, 4, 2, { { g_002F5B40, 9, 1 } }
};
/* (The array is two words longer than the part's 0x15 qwords: the zeros in
 * front of g_002F5D48.) */
alignas(16) static u32 g_002F5BF0[86] = {    /* 0x002F5BF0 */
    VIF_UNPACK(VIF_V4_32, 1, 0xC0),
    0x0000800C, 0x302E4000, 0x00000412, 0x00000000,     /* GIFtag NLOOP 12, EOP, PRE, PRIM 0x5C, ST RGBAQ XYZF2 */
    VIF_UNPACK(VIF_V3_32, 12, 0),                   /* xyz */
    0x3F800000, 0x3F800000, 0x00000000, 0x00000000,
    0x3F800000, 0x00000000, 0x3F800000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x3F800000, 0xBF800000, 0x00000000, 0x00000000,
    0xBF800000, 0x00000000, 0x00000000, 0x3F800000,
    0x00000000, 0xBF800000, 0x3F800000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0xBF800000,
    0x00000000, 0x00000000, 0x00000000, 0xBF800000,
    0x00000000, 0xBF800000, 0xBF800000, 0x00000000,
    VIF_UNPACK(VIF_S_8, 3, 0x40), 0x000C0C02,          /* the strip table */
    VIF_UNPACK_M(VIF_V2_32, 12, 0x40),              /* ST */
    0x00000000, 0x00000000, 0x3F800000, 0x00000000,
    0x00000000, 0x3F800000, 0x3F800000, 0x3F800000,
    0x00000000, 0x00000000, 0x3F800000, 0x00000000,
    0x3F800000, 0x00000000, 0x00000000, 0x00000000,
    0x3F800000, 0x3F800000, 0x00000000, 0x3F800000,
    0x3F800000, 0x00000000, 0x00000000, 0x00000000,
    VIF_UNPACK(VIF_V4_8, 12, 0x80),                 /* RGBA */
    0x80808080, 0x80808080, 0x80808080, 0x80808080,
    0x80808080, 0x80808080, 0x80808080, 0x80808080,
    0x80808080, 0x80808080, 0x80808080, 0x80808080,
    0x00000000, 0x00000000,
    0x00000000, 0x00000000
};
VifModel g_002F5D48 = {                 /* 0x002F5D48 */
    NULL, NULL, 0, 0, 1, 12, 8, { { g_002F5BF0, 0x15, 2 } }
};
/* The rays: 72 triangles from the centre to p0 and p1 (x, y used, scaled by
 * 1/32 in flare_build_rays) with a grey level. */
const FlareRayVert g_00343D30[72] = {   /* 0x00343D30 */
    { { 8.407244f, 30.875853f, 0.017418f, 1.0f }, { 29.874966f, -11.466752f, 0.017418f, 1.0f }, { 87, 87, 87 }, 128 },
    { { -22.53565f, 22.718813f, 0.017418f, 1.0f }, { 24.867983f, 20.139105f, 0.017418f, 1.0f }, { 87, 87, 87 }, 128 },
    { { -30.942894f, -8.15704f, 0.017418f, 1.0f }, { -5.006985f, 31.605856f, 0.017418f, 1.0f }, { 87, 87, 87 }, 128 },
    { { -8.407244f, -30.875853f, 0.017418f, 1.0f }, { -29.874966f, 11.466752f, 0.017418f, 1.0f }, { 87, 87, 87 }, 128 },
    { { 22.53565f, -22.718813f, 0.017418f, 1.0f }, { -24.867983f, -20.139105f, 0.017418f, 1.0f }, { 87, 87, 87 }, 128 },
    { { 30.942894f, 8.15704f, 0.017418f, 1.0f }, { 5.006985f, -31.605856f, 0.017418f, 1.0f }, { 87, 87, 87 }, 128 },
    { { 10.792047f, 30.125267f, 0.030065f, 1.0f }, { 25.534002f, -19.287682f, 0.030065f, 1.0f }, { 118, 118, 118 }, 128 },
    { { -20.693224f, 24.40882f, 0.030065f, 1.0f }, { 29.470625f, 12.469254f, 0.030065f, 1.0f }, { 118, 118, 118 }, 128 },
    { { -31.48527f, -5.716447f, 0.030065f, 1.0f }, { 3.936622f, 31.756935f, 0.030065f, 1.0f }, { 118, 118, 118 }, 128 },
    { { -10.792047f, -30.125267f, 0.030065f, 1.0f }, { -25.534002f, 19.287682f, 0.030065f, 1.0f }, { 118, 118, 118 }, 128 },
    { { 20.693224f, -24.40882f, 0.030065f, 1.0f }, { -29.470625f, -12.469254f, 0.030065f, 1.0f }, { 118, 118, 118 }, 128 },
    { { 31.48527f, 5.716447f, 0.030065f, 1.0f }, { -3.936622f, -31.756935f, 0.030065f, 1.0f }, { 118, 118, 118 }, 128 },
    { { -12.739069f, 29.355001f, 0.02812f, 1.0f }, { 31.819725f, 3.391915f, 0.02812f, 1.0f }, { 86, 86, 86 }, 128 },
    { { -31.791712f, 3.645144f, 0.02812f, 1.0f }, { 12.972378f, 29.252647f, 0.02812f, 1.0f }, { 86, 86, 86 }, 128 },
    { { -19.052643f, -25.709858f, 0.02812f, 1.0f }, { -18.847347f, 25.860733f, 0.02812f, 1.0f }, { 86, 86, 86 }, 128 },
    { { 12.739069f, -29.355001f, 0.02812f, 1.0f }, { -31.819725f, -3.391915f, 0.02812f, 1.0f }, { 86, 86, 86 }, 128 },
    { { 31.791712f, -3.645144f, 0.02812f, 1.0f }, { -12.972378f, -29.252647f, 0.02812f, 1.0f }, { 86, 86, 86 }, 128 },
    { { 19.052643f, 25.709858f, 0.02812f, 1.0f }, { 18.847347f, -25.860733f, 0.02812f, 1.0f }, { 86, 86, 86 }, 128 },
    { { -11.375185f, 29.90995f, 0.030153f, 1.0f }, { 31.984882f, 0.983508f, 0.030153f, 1.0f }, { 65, 65, 65 }, 128 },
    { { -31.59037f, 5.103777f, 0.030153f, 1.0f }, { 15.140698f, 28.191475f, 0.030153f, 1.0f }, { 65, 65, 65 }, 128 },
    { { -20.215185f, -24.806173f, 0.030153f, 1.0f }, { -16.844185f, 27.207968f, 0.030153f, 1.0f }, { 65, 65, 65 }, 128 },
    { { 11.375185f, -29.90995f, 0.030153f, 1.0f }, { -31.984882f, -0.983508f, 0.030153f, 1.0f }, { 65, 65, 65 }, 128 },
    { { 31.59037f, -5.103777f, 0.030153f, 1.0f }, { -15.140698f, -28.191475f, 0.030153f, 1.0f }, { 65, 65, 65 }, 128 },
    { { 20.215185f, 24.806173f, 0.030153f, 1.0f }, { 16.844185f, -27.207968f, 0.030153f, 1.0f }, { 65, 65, 65 }, 128 },
    { { 8.837571f, 30.755444f, 0.025949f, 1.0f }, { 29.663252f, -12.003814f, 0.025949f, 1.0f }, { 76, 76, 76 }, 128 },
    { { -22.21621f, 23.031282f, 0.025949f, 1.0f }, { 25.227234f, 19.687222f, 0.025949f, 1.0f }, { 76, 76, 76 }, 128 },
    { { -31.053782f, -7.724161f, 0.025949f, 1.0f }, { -4.436018f, 31.691036f, 0.025949f, 1.0f }, { 76, 76, 76 }, 128 },
    { { -8.837571f, -30.755444f, 0.025949f, 1.0f }, { -29.663252f, 12.003814f, 0.025949f, 1.0f }, { 76, 76, 76 }, 128 },
    { { 22.21621f, -23.031282f, 0.025949f, 1.0f }, { -25.227234f, -19.687222f, 0.025949f, 1.0f }, { 76, 76, 76 }, 128 },
    { { 31.053782f, 7.724161f, 0.025949f, 1.0f }, { 4.436018f, -31.691036f, 0.025949f, 1.0f }, { 76, 76, 76 }, 128 },
    { { 13.160305f, 29.168585f, 0.024217f, 1.0f }, { 26.983088f, -17.202122f, 0.024217f, 1.0f }, { 122, 122, 122 }, 128 },
    { { -18.680582f, 25.981451f, 0.024217f, 1.0f }, { 28.389017f, 14.766979f, 0.024217f, 1.0f }, { 122, 122, 122 }, 128 },
    { { -31.840887f, -3.187134f, 0.024217f, 1.0f }, { 1.40593f, 31.9691f, 0.024217f, 1.0f }, { 122, 122, 122 }, 128 },
    { { -13.160305f, -29.168585f, 0.024217f, 1.0f }, { -26.983088f, 17.202122f, 0.024217f, 1.0f }, { 122, 122, 122 }, 128 },
    { { 18.680582f, -25.981451f, 0.024217f, 1.0f }, { -28.389017f, -14.766979f, 0.024217f, 1.0f }, { 122, 122, 122 }, 128 },
    { { 31.840887f, 3.187134f, 0.024217f, 1.0f }, { -1.40593f, -31.9691f, 0.024217f, 1.0f }, { 122, 122, 122 }, 128 },
    { { 11.573747f, 29.83368f, 0.019116f, 1.0f }, { 28.788134f, -13.972952f, 0.019116f, 1.0f }, { 73, 73, 73 }, 128 },
    { { -20.04985f, 24.939999f, 0.019116f, 1.0f }, { 26.494999f, 17.944778f, 0.019116f, 1.0f }, { 73, 73, 73 }, 128 },
    { { -31.623596f, -4.89368f, 0.019116f, 1.0f }, { -2.293135f, 31.91773f, 0.019116f, 1.0f }, { 73, 73, 73 }, 128 },
    { { -11.573747f, -29.83368f, 0.019116f, 1.0f }, { -28.788134f, 13.972952f, 0.019116f, 1.0f }, { 73, 73, 73 }, 128 },
    { { 20.04985f, -24.939999f, 0.019116f, 1.0f }, { -26.494999f, -17.944778f, 0.019116f, 1.0f }, { 73, 73, 73 }, 128 },
    { { 31.623596f, 4.89368f, 0.019116f, 1.0f }, { 2.293135f, -31.91773f, 0.019116f, 1.0f }, { 73, 73, 73 }, 128 },
    { { 18.77954f, 25.910015f, 0.024284f, 1.0f }, { 23.39734f, -21.830357f, 0.024284f, 1.0f }, { 120, 120, 120 }, 128 },
    { { -13.048963f, 29.218567f, 0.024284f, 1.0f }, { 30.604313f, 9.347514f, 0.024284f, 1.0f }, { 120, 120, 120 }, 128 },
    { { -31.8285f, 3.308549f, 0.024284f, 1.0f }, { 7.206972f, 31.17787f, 0.024284f, 1.0f }, { 120, 120, 120 }, 128 },
    { { -18.77954f, -25.910015f, 0.024284f, 1.0f }, { -23.39734f, 21.830357f, 0.024284f, 1.0f }, { 120, 120, 120 }, 128 },
    { { 13.048963f, -29.218567f, 0.024284f, 1.0f }, { -30.604313f, -9.347514f, 0.024284f, 1.0f }, { 120, 120, 120 }, 128 },
    { { 31.8285f, -3.308549f, 0.024284f, 1.0f }, { -7.206972f, -31.17787f, 0.024284f, 1.0f }, { 120, 120, 120 }, 128 },
    { { 4.379858f, 31.698847f, 0.029573f, 1.0f }, { 31.100872f, -7.532313f, 0.029573f, 1.0f }, { 107, 107, 107 }, 128 },
    { { -25.262077f, 19.64249f, 0.029573f, 1.0f }, { 22.07361f, 23.16799f, 0.029573f, 1.0f }, { 107, 107, 107 }, 128 },
    { { -29.641935f, -12.056355f, 0.029573f, 1.0f }, { -9.027261f, 30.700302f, 0.029573f, 1.0f }, { 107, 107, 107 }, 128 },
    { { -4.379858f, -31.698847f, 0.029573f, 1.0f }, { -31.100872f, 7.532313f, 0.029573f, 1.0f }, { 107, 107, 107 }, 128 },
    { { 25.262077f, -19.64249f, 0.029573f, 1.0f }, { -22.07361f, -23.16799f, 0.029573f, 1.0f }, { 107, 107, 107 }, 128 },
    { { 29.641935f, 12.056355f, 0.029573f, 1.0f }, { 9.027261f, -30.700302f, 0.029573f, 1.0f }, { 107, 107, 107 }, 128 },
    { { 19.402708f, 25.446709f, 0.015396f, 1.0f }, { 20.802877f, -24.315434f, 0.015396f, 1.0f }, { 110, 110, 110 }, 128 },
    { { -12.336142f, 29.526592f, 0.015396f, 1.0f }, { 31.459223f, 5.858104f, 0.015396f, 1.0f }, { 110, 110, 110 }, 128 },
    { { -31.73885f, 4.079884f, 0.015396f, 1.0f }, { 10.656344f, 30.173536f, 0.015396f, 1.0f }, { 110, 110, 110 }, 128 },
    { { -19.402708f, -25.446709f, 0.015396f, 1.0f }, { -20.802877f, 24.315434f, 0.015396f, 1.0f }, { 110, 110, 110 }, 128 },
    { { 12.336142f, -29.526592f, 0.015396f, 1.0f }, { -31.459223f, -5.858104f, 0.015396f, 1.0f }, { 110, 110, 110 }, 128 },
    { { 31.73885f, -4.079884f, 0.015396f, 1.0f }, { -10.656344f, -30.173536f, 0.015396f, 1.0f }, { 110, 110, 110 }, 128 },
    { { 13.733026f, 28.903357f, 0.019286f, 1.0f }, { 28.449583f, -14.649954f, 0.019286f, 1.0f }, { 114, 114, 114 }, 128 },
    { { -18.164528f, 26.344828f, 0.019286f, 1.0f }, { 26.912025f, 17.313086f, 0.019286f, 1.0f }, { 114, 114, 114 }, 128 },
    { { -31.897554f, -2.558529f, 0.019286f, 1.0f }, { -1.53756f, 31.96304f, 0.019286f, 1.0f }, { 114, 114, 114 }, 128 },
    { { -13.733026f, -28.903357f, 0.019286f, 1.0f }, { -28.449583f, 14.649954f, 0.019286f, 1.0f }, { 114, 114, 114 }, 128 },
    { { 18.164528f, -26.344828f, 0.019286f, 1.0f }, { -26.912025f, -17.313086f, 0.019286f, 1.0f }, { 114, 114, 114 }, 128 },
    { { 31.897554f, 2.558529f, 0.019286f, 1.0f }, { 1.53756f, -31.96304f, 0.019286f, 1.0f }, { 114, 114, 114 }, 128 },
    { { -10.846549f, 30.105686f, 0.021211f, 1.0f }, { 31.987257f, 0.902974f, 0.021211f, 1.0f }, { 72, 72, 72 }, 128 },
    { { -31.495564f, 5.659456f, 0.021211f, 1.0f }, { 15.21163f, 28.153265f, 0.021211f, 1.0f }, { 72, 72, 72 }, 128 },
    { { -20.649015f, -24.446232f, 0.021211f, 1.0f }, { -16.775627f, 27.25029f, 0.021211f, 1.0f }, { 72, 72, 72 }, 128 },
    { { 10.846549f, -30.105686f, 0.021211f, 1.0f }, { -31.987257f, -0.902974f, 0.021211f, 1.0f }, { 72, 72, 72 }, 128 },
    { { 31.495564f, -5.659456f, 0.021211f, 1.0f }, { -15.21163f, -28.153265f, 0.021211f, 1.0f }, { 72, 72, 72 }, 128 },
    { { 20.649015f, 24.446232f, 0.021211f, 1.0f }, { 16.775627f, -27.25029f, 0.021211f, 1.0f }, { 72, 72, 72 }, 128 },
};

/* The occlusion result group of an unoccluded draw (flare_draw_disc): four
 * 'fully visible' texels, sent by DMA (a ref tag in vif_model_draw). */
alignas(16) const u32 g_00344AB0[4] = {    /* 0x00344AB0 */
    0xFF, 0xFF, 0xFF, 0xFF
};
/* The point of flare_query_point. */
const f32 g_00344AC0[4] = {             /* 0x00344AC0 */
    0.0f, 0.0f, 0.0f, 0.0f
};

/* The models built at start-up (FlareClass's constructor): the disc
 * (flare_build_disc) and the rays (flare_build_rays), VIF codes and
 * headers; the rays header has room for 4 parts. */
alignas(16) static u32 g_004A5240[0x118];  /* 0x004A5240 */
static VifModel g_004A56A0;                                 /* 0x004A56A0 */
alignas(16) static u32 g_004A56C0[0x460];  /* 0x004A56C0 */
static u32 g_004A6840[12];                                  /* 0x004A6840 */
VifModel *g_003530B0 = (VifModel *)g_004A6840;              /* 0x003530B0 */

// FUNC 0x002466F0 flare_sun_model_a
VifModel *flare_sun_model_a(void)
{
    return &g_002F5BD0;
}

// FUNC 0x00246700 flare_sun_model_b
VifModel *flare_sun_model_b(void)
{
    return &g_002F5D48;
}

// FUNC 0x00246710 flare_build_disc
/* The disc model (14 triangles, white centre, black rim) into g_004A5240 /
 * g_004A56A0. */
void flare_build_disc(void)
{
    u32 *p = g_004A5240;
    f32 *xyz;
    u8 *strips;
    int i, j, k;
    int words;
    int n = 28;

    *p++ = VIF_UNPACK(VIF_V4_32, 1, 0xC0);
    *p++ = 0x801C;
    *p++ = 0x30264000;
    *p++ = 0x412;
    *p++ = 0;
    *p++ = VIF_UNPACK(VIF_S_8, 8, 0x40);
    strips = (u8 *)p;
    strips[0] = 7;
    strips += 7;
    for (k = 6; k >= 0; k--)
        *strips-- = 6;
    p += 2;
    *p = VIF_UNPACK(VIF_V3_32, n, 0);
    xyz = (f32 *)(p + 1);
    for (i = 0; i < 7; i++) {
        int rim = i * 4;

        for (j = 0; j < 3; j++, rim += 2) {
            f32 a;

            if (i * 2 + j != 14)
                a = (f32)rim * 3.1415927f / 14.0f;
            else
                a = 0.0f;
            *xyz++ = (f32)cos((f64)a);
            *xyz++ = (f32)sin((f64)a);
            *xyz++ = 0.0f;
            if (j == 0) {
                *xyz++ = 0.0f;
                *xyz++ = 0.0f;
                *xyz++ = 0.0f;
            }
        }
    }
    p = (u32 *)xyz;
    *p++ = VIF_UNPACK(VIF_V4_8, n, 0x80);
    for (k = 0; k < 28; k++)
        *p++ = ((k & 3) ^ 1) != 0 ? 0 : 0xFFFFFFFFu;
    while ((p - g_004A5240) & 3)
        *p++ = 0;
    words = (int)(p - g_004A5240);
    g_004A56A0.nParts = 1;
    g_004A56A0.nVerts = 0x1C;
    g_004A56A0.nPrims = 0xE;
    g_004A56A0.part[0].qwc = (u16)(words / 4);
    g_004A56A0.part[0].tex = 0;
    g_004A56A0.mode = 0;
    g_004A56A0.scaleFlags = 0;
    g_004A56A0.part[0].data = g_004A5240;
}

// FUNC 0x002469E0 flare_disc_model
VifModel *flare_disc_model(void)
{
    return &g_004A56A0;
}

// FUNC 0x002469F0 flare_build_rays
/* The rays model (72 triangles in parts of at most 21) into g_004A56C0 /
 * *g_003530B0. */
void flare_build_rays(void)
{
    u32 *p = g_004A56C0;
    int part = 0;
    int first = 0;

    do {
        int n = 0x48 - first;
        u32 *start;
        u8 *strips;
        f32 *xyz;
        int k;
        int words;
        int nv;

        if (!(n < 0x16))
            n = 0x15;
        nv = n * 3;
        start = p;
        start[0] = VIF_UNPACK(VIF_V4_32, 1, 0xC0);
        start[1] = nv | 0x8000;
        start[2] = 0x30264000;
        start[3] = 0x412;
        start[5] = VIF_UNPACK(VIF_S_8, n + 1, 0x40);
        start[4] = 0;
        strips = (u8 *)&start[6];
        strips[0] = (u8)n;
        for (k = 0; k < n; k++)
            strips[k + 1] = 3;
        p = (u32 *)strips + (n + 4) / 4;
        *p = VIF_UNPACK(VIF_V3_32, nv, 0);
        xyz = (f32 *)(p + 1);
        for (k = 0; k < n; k++) {
            *xyz++ = 0.0f;
            *xyz++ = 0.0f;
            *xyz++ = 0.0f;
            *xyz++ = g_00343D30[first + k].p0[0] * 0.03125f;
            *xyz++ = g_00343D30[first + k].p0[1] * 0.03125f;
            *xyz++ = 0.0f;
            *xyz++ = g_00343D30[first + k].p1[0] * 0.03125f;
            *xyz++ = g_00343D30[first + k].p1[1] * 0.03125f;
            *xyz++ = 0.0f;
        }
        p = (u32 *)xyz;
        *p++ = VIF_UNPACK(VIF_V4_8, nv, 0x80);
        for (k = 0; k < n; k++) {
            *p++ = (u32)g_00343D30[first + k].rgb[0] | ((u32)g_00343D30[first + k].rgb[1] << 8) |
                   0xFF000000 | ((u32)g_00343D30[first + k].rgb[2] << 16);
            *p++ = 0;
            *p++ = 0;
        }
        while ((p - start) & 3)
            *p++ = 0;
        words = (int)(p - start);
        g_003530B0->part[part].data = start;
        g_003530B0->part[part].qwc = (u16)(words / 4);
        g_003530B0->part[part].tex = 0;
        part++;
        first += 21;
    } while (first < 0x48);
    g_003530B0->mode = 0;
    g_003530B0->scaleFlags = 0;
    g_003530B0->nParts = (u16)part;
    g_003530B0->nVerts = 0xD8;
    g_003530B0->nPrims = 0x48;
}

// FUNC 0x00246D28 flare_rays_model
VifModel *flare_rays_model(void)
{
    return g_003530B0;
}

// FUNC 0x00246D30 flare_project
/* pos through vf24..vf27, divided, clamped, through the viewport (vftoi4);
 * returns the projected z; *sx / *sy the window position (y / 8 when
 * rendering fields); spriteZF = F << 24 | Z of the point. */
f32 flare_project(const f32 *pos, f32 *sx, f32 *sy)
{
    GsContext *ctx = GS_SPR_CTX;
    f32 div = 16.0f;
    pgl::Vec4 v;
    s32 scr[4];
    f32 z;

    if (ctx->halfHeight != 0)
        div = 8.0f;
    vu0_load_screen_regs();
    v = pgl::vu::xformW1(&VU0.vf[24], pgl::Vec4{pos[0], pos[1], pos[2], 1.0f});
    v = pgl::vu::scale(pgl::vu::mini(v, VU0.vf[21]), pgl::vu::div(1.0f, v.w));
    z = v.z;
    v = pgl::vu::madd(VU0.vf[22], v, VU0.vf[23]);
    VU0.vf[16] = v;
    scr[0] = pgl::vu::ftoi4(v.x);
    scr[1] = pgl::vu::ftoi4(v.y);
    scr[2] = pgl::vu::ftoi4(v.z);
    scr[3] = pgl::vu::ftoi4(v.w);
    *sx = (f32)(scr[0] - ctx->tgtA.xoff) * 0.0625f;
    *sy = (f32)(scr[1] - ctx->tgtA.yoff) / div;
    scr[2] >>= 4;
    scr[3] >>= 4;
    ctx->spriteZF = ((u32)scr[3] << 24) | (u32)scr[2];
    return z;
}

// FUNC 0x00246E60 flare_fill_octagon
/* A filled octagon of radius r around (cx, cy) as 5 rectangles. */
void flare_fill_octagon(f32 cx, f32 cy, f32 r, f32 aspect)
{
    int outer = (int)(r * 0.923879f);
    int mid = (int)(r * 0.707107f);
    int inner = (int)(r * 0.382683f);
    int x = (int)cx;
    int y = (int)cy;
    int outerY = (int)((f32)outer * aspect);
    int midY = (int)((f32)mid * aspect);
    int innerY = (int)((f32)inner * aspect);

    gs_draw_flat_sprite(x - inner, y - outerY, x + inner, y + outerY);
    gs_draw_flat_sprite(x - mid, y - midY, x - inner, y + midY);
    gs_draw_flat_sprite(x - outer, y - innerY, x - mid, y + innerY);
    gs_draw_flat_sprite(x + inner, y - midY, x + mid, y + midY);
    gs_draw_flat_sprite(x + mid, y - innerY, x + outer, y + innerY);
}

// FUNC 0x00246FC0 flare_set_target
void flare_set_target(u32 fbWordAddr, int w, int h)
{
    gfx_set_draw_buffer(fbWordAddr, 0, w, h);
    gfx_set_draw_area(0, 0, w, h);
    gfx_set_field_mode(0);
    gs_set_scanmask_mode(0);
}

// FUNC 0x00247020 flare_set_texture
void flare_set_texture(u32 texWordAddr, int w, int h)
{
    gfx_texflush();
    gfx_set_texture_buffer(0, texWordAddr, 0, (w + 0x3F) & ~0x3F);
    gfx_set_texture_size(w, h);
    gfx_set_texture_region(0, 0, w);
    gfx_set_texture_region(1, 0, h);
    gfx_tex_param(PGL_TEXPARAM_WMS, PGL_WRAP_CLAMP);
    gfx_tex_param(PGL_TEXPARAM_WMT, PGL_WRAP_CLAMP);
}

// FUNC 0x002470C0 flare_copy_visibility
/* The screen's box (x0, y0)-(x1, y1) to the top left of the work buffer
 * (at most its size), one textured sprite, point sampled; the caller's
 * colour mask writes only alpha. */
void flare_copy_visibility(FlareWork *fw, int x0, int y0, int x1, int y1)
{
    int w = x1 - x0;
    int h = y1 - y0;
    int workW = fw->workWidth;
    int workH = fw->workHeight;

    if (workW < w)
        w = workW;
    if (workH < h)
        h = workH;
    if (GS_SPR_CTX->halfHeight != 0) {
        y0 = y0 / 2;
        y1 = y1 / 2;
    }
    flare_set_target(fw->workVram, workW, workH);
    flare_set_texture(fw->screenVram, fw->screenWidth, fw->screenHeight);
    gfx_disable(PGL_CAP_DEPTH_TEST);
    pgl_color4f(1.0f, 1.0f, 1.0f, 1.0f);
    gfx_tex_param(PGL_TEXPARAM_MMAG, PGL_FILTER_NEAREST);
    gfx_tex_param(PGL_TEXPARAM_MMIN, PGL_FILTER_NEAREST);
    gs_draw_textured_sprite(0, 0, w, h, x0, y0, x1, y1);
}

// FUNC 0x002471D8 flare_reduce
/* The w x h box of the work buffer reduced in place to one texel by
 * bilinear halving sprites (boxes of 18 or more). */
void flare_reduce(FlareWork *fw, int w, int h)
{
    int workW = fw->workWidth;
    int workH = fw->workHeight;

    w = (workW < w) ? workW : w;
    h = (workH < h) ? workH : h;
    flare_set_target(fw->workVram, workW, workH);
    flare_set_texture(fw->workVram, workW, workH);
    pgl_color4f(1.0f, 1.0f, 1.0f, 1.0f);
    gfx_tex_param(PGL_TEXPARAM_MMAG, PGL_FILTER_LINEAR);
    gfx_tex_param(PGL_TEXPARAM_MMIN, PGL_FILTER_LINEAR);
    while (w >= 2 || h >= 2) {
        int nw = w >> 1;
        int nh = h >> 1;

        if (((nw << 1) ^ w) != 0)
            nw = nw << 1;
        if (((nh << 1) ^ h) != 0)
            nh = nh << 1;
        if (!(0 < nw))
            nw = 1;
        if (!(0 < nh))
            nh = 1;
        gfx_texflush();
        gs_draw_textured_sprite_f(0.0f, 0.0f, (f32)nw, (f32)nh, 0.5f, 0.5f, (f32)w + 0.5f, (f32)h + 0.5f);
        w = nw;
        h = nh;
    }
}

// FUNC 0x00247340 flare_build_reduce
/* Records the square reduction of an n x n box once (a DMA call target in
 * a block from the packet end); the state the recording changed is due
 * again.  PC: a recorded command list of the renderer, keyed by the
 * block. */
u32 *flare_build_reduce(FlareWork *fw, int n)
{
    GsContext *ctx = GS_SPR_CTX;
    int workW = fw->workWidth;
    int workH = fw->workHeight;
    u32 *key;
    u8 savedPacketOpen = ctx->packetOpen;

    n = (workW < n) ? workW : n;
    key = (u32 *)gfx_alloc_tail(1);
    key[0] = DMATAG_RET;
    key[1] = key[2] = key[3] = 0;
    pglr_sub_begin(key);
    /* the recording flushes registers as a packet would (an active
     * packet); the PS2 recorded into the frame packet's buffer */
    ctx->packetOpen = 1;
    flare_set_target(fw->workVram, workW, workH);
    flare_set_texture(fw->workVram, workW, workH);
    gs_flush_dirty_regs();
    pgl_color4f(1.0f, 1.0f, 1.0f, 1.0f);
    gfx_tex_param(PGL_TEXPARAM_MMAG, PGL_FILTER_LINEAR);
    gfx_tex_param(PGL_TEXPARAM_MMIN, PGL_FILTER_LINEAR);
    if (!(n < 2)) {
        do {
            int half = n >> 1;

            if (((half << 1) ^ n) != 0)
                half = half << 1;
            gfx_texflush();
            gs_draw_textured_sprite_f(0.0f, 0.0f, (f32)half, (f32)half, 0.5f, 0.5f,
                                      (f32)n + 0.5f, (f32)n + 0.5f);
            n = half;
        } while (!(n < 2));
    }
    ctx->packetOpen = savedPacketOpen;
    pglr_sub_end();
    ctx->dirty |= GS_DIRTY_REG_BOTH(GS_REG_FRAME) | GS_DIRTY_REG_BOTH(GS_REG_XYOFFSET);
    ctx->vu1MatrixSent = 0;
    return key;
}

// FUNC 0x00247518 flare_run_reduce
/* w < 2: nothing; w >= 18: flare_reduce; else the work buffer as texture
 * and the prebuilt reduction reduce[w - 2] (square). */
void flare_run_reduce(FlareWork *fw, int w, int h)
{
    if (w < 2)
        return;
    if (!(w < 0x12)) {
        flare_reduce(fw, w, h);
        return;
    }
    flare_set_texture(fw->workVram, fw->workWidth, fw->workHeight);
    gs_flush_dirty_regs();
    pglr_sub_call(fw->reduce[w - 2]);
}

// FUNC 0x002475C8 flare_store_result
/* The reduced texel (0, 0) of the work buffer, colour scale * w * h / 4,
 * blend Cs * As, into texel (q % 64, q / 64) of the result strip. */
void flare_store_result(FlareWork *fw, f32 scale, int w, int h)
{
    f32 c = scale * (f32)(w * h) * 0.25f;
    int q;
    int qx, qy;

    flare_set_target(fw->occVram, 0x40, fw->occHeight);
    q = fw->queries;
    qy = q / 64;
    qx = q & 0x3F;
    gfx_enable(PGL_CAP_BLEND);
    gfx_blend_func(PGL_BLEND_SRC_ALPHA, 0.0f);
    pgl_color4f(c, c, c, 1.0f);
    gs_draw_textured_sprite(qx, qy, qx + 1, qy + 1, 0, 0, 0, 0);
}

// FUNC 0x00247690 vif_model_draw
/* Model m for the VU1 flare program (nothing when vu1Prog is 0): per part
 * its texture (fw->tex[tex - 1]) when tex != 0, the result group, the index
 * and (r, g, b); matrices and dirty registers as due. */
void vif_model_draw(FlareWork *fw, VifModel *m, const void *results, u32 index, f32 r, f32 g, f32 b)
{
    GsContext *ctx = GS_SPR_CTX;
    u32 i;

    ctx->stmask = STMASK_DEFAULT;
    if (ctx->matrixDirty != 0)
        pgl_sync_matrices(ctx);
    if (ctx->vu1Prog == 0)
        return;
    ctx->nVerts += m->nVerts;
    ctx->nPrims += m->nPrims;
    ctx->nParts += m->nParts;
    for (i = 0; i < m->nParts; i++) {
        PglrModelPart part;

        if (m->part[i].tex != 0)
            gfx_set_texture_regs(fw->tex[m->part[i].tex - 1]->tex);
        pgl_vu1_matrix_due(ctx);
        if (ctx->dirty & GS_DIRTY_ALL_CTX1_GEN)
            gs_collect_dirty_regs(ctx, NULL);
        memset(&part, 0, sizeof(part));
        part.model = m;
        part.part = (int)i;
        part.lightProg = VU1L_FLARE;
        part.lightOfs = 0;
        part.light[0][0] = r;
        part.light[0][1] = g;
        part.light[0][2] = b;
        part.light[1][0] = (f32)index;
        part.lightQwords = 2;
        part.colorScale = 128.0f;
        part.alphaScale = 128.0f;
        part.morphWeight = ctx->morphWeight;
        part.primMask = 0;
        part.primAttr = ctx->primAttr;
        part.flareResults = (const u32 *)results;
        part.flareIndex = index;
        /* PC: a result disc (its texel in the readback array): the GPU
         * reads texel q of the result strip itself (flare_readback no longer
         * waits for it, docs/renderer.md 8) */
        if (fw->results != NULL && (const u32 *)results >= fw->results &&
            (const u32 *)results < fw->results + 0x40 * fw->occHeight) {
            part.flareStripBlock = (u32)fw->occVram >> 6;
            part.flareStripH = fw->occHeight;
            part.flareQ = (s32)((const u32 *)results - fw->results) + (s32)index;
        }
        g_pglVu1Sent.prog = ctx->vu1Prog;
        pglr_model_part(&g_pglVu1Sent, &part, ctx->halfHeight ? PGLR_HALF_HEIGHT : 0);
    }
}

// FUNC 0x00247948 flare_init
/* Per frame set-up (FlareClass::begin): the buffers and textures, the 16
 * reduction packets (n = 2..17), texel (0, 0) of the work buffer cleared
 * to red (1, 0, 0, 0). */
void flare_init(FlareWork *fw, f32 aspect, int screenVram, int screenW, int screenH, int occVram,
                int occH, int workVram, int workW, int workH, Tex1Image *tex0, Tex1Image *tex1)
{
    int i;

    fw->width = screenW;
    fw->height = screenH;
    fw->workVram = workVram;
    fw->workWidth = workW;
    fw->workHeight = workH;
    fw->queries = 0;
    fw->aspect = aspect;
    fw->screenVram = screenVram;
    fw->screenWidth = screenW;
    fw->screenHeight = screenH;
    fw->occVram = occVram;
    fw->occHeight = occH;
    fw->tex[0] = tex0;
    fw->tex[1] = tex1;
    pgl_push_attrib(FLARE_SAVED_ATTRIBS);
    for (i = 0; i < 0x10; i++)
        fw->reduce[i] = flare_build_reduce(fw, i + 2);
    gfx_disable(PGL_CAP_ALPHA_TEST);
    gfx_disable(PGL_CAP_BLEND);
    gfx_tex_param(PGL_TEXPARAM_MMAG, PGL_FILTER_LINEAR);
    gfx_tex_param(PGL_TEXPARAM_MMIN, PGL_FILTER_LINEAR);
    gfx_tex_param(PGL_TEXPARAM_TFX, PGL_TFX_MODULATE);
    gfx_disable(PGL_CAP_DEPTH_TEST);
    gfx_depth_mask(0);
    flare_set_target(workVram, workW, workH);
    pgl_color4f(1.0f, 0.0f, 0.0f, 0.0f);
    gs_draw_flat_sprite(0, 0, 1, 1);
    gfx_depth_mask(1);
    gfx_enable(PGL_CAP_DEPTH_TEST);
    pgl_pop_attrib();
}

/* PC: flare_fill_octagon's five rectangles (x0, y0, x1, y1), for the native
 * query. */
static void flare_octagon_rects(f32 cx, f32 cy, f32 r, f32 aspect, s32 (*rect)[4])
{
    int outer = (int)(r * 0.923879f);
    int mid = (int)(r * 0.707107f);
    int inner = (int)(r * 0.382683f);
    int x = (int)cx;
    int y = (int)cy;
    int outerY = (int)((f32)outer * aspect);
    int midY = (int)((f32)mid * aspect);
    int innerY = (int)((f32)inner * aspect);
    const s32 r5[5][4] = {
        { x - inner, y - outerY, x + inner, y + outerY },
        { x - mid, y - midY, x - inner, y + midY },
        { x - outer, y - innerY, x - mid, y + innerY },
        { x + inner, y - midY, x + mid, y + midY },
        { x + mid, y - innerY, x + outer, y + innerY },
    };
    memcpy(rect, r5, sizeof(r5));
}

// FUNC 0x00247AA0 flare_occlusion_query
/* One occlusion query at window point (sx, sy) for a size x size box: 0
 * when off the screen; else the box's alpha cleared, the square (or the
 * octagon, scale x 4 / pi) of alpha 2.0 drawn depth-tested, the box copied
 * to the work buffer, reduced, stored with scale; queries++; 1.
 * PC: the box clear and the shape are drawn as on the PS2 (they leave the
 * frame's alpha as the game expects); the copy, the bilinear halvings and
 * the store - GS tricks for a mean the PS2 had no other way to take - are
 * one native query (pglr_flare_query: the share of the box the shape
 * passes the depth test in, tested per GS pixel against the screen's Z
 * buffer, stored as Cs x A >> 7 into the query's result texel).  Emulated
 * at the output scale they gave no usable result (every texel 0). */
int flare_occlusion_query(FlareWork *fw, int octagon, f32 sx, f32 sy, f32 size, f32 scale)
{
    int isize = (int)size;
    f32 fsize = (f32)isize;
    f32 half;
    f32 cx, cy;
    int x0, y0, x1, y1;
    int w, h;
    int isOctagon;

    sy = sy + 0.5f;
    sx = sx + 0.5f;
    half = fsize * 0.5f;
    cy = (f32)(int)sy;
    cx = (f32)(int)sx;
    isOctagon = octagon & 0xFF;
    y0 = (int)(cy - half);
    x0 = (int)(cx - half);
    x1 = x0 + isize;
    y1 = (int)((f32)y0 + fsize);
    if (x1 < 0 || fw->width < x0 || y1 < 0 || fw->height < y0)
        return 0;
    if (!(2 < x1 - x0))
        x1 = x0 + 2;
    if (!(2 < y1 - y0))
        y1 = y0 + 2;
    gfx_color_mask(0, 0, 0, 1);
    gfx_depth_mask(0);
    pgl_color(0);
    gfx_disable(PGL_CAP_DEPTH_TEST);
    gfx_disable(PGL_CAP_BLEND);
    gs_draw_flat_sprite(x0, y0, x1 + 2, y1 + 2);
    gfx_enable(PGL_CAP_DEPTH_TEST);
    pgl_color4f(1.0f, 1.0f, 1.0f, 2.0f);
    PglrFlareQuery query;
    memset(&query, 0, sizeof(query));
    if (isOctagon != 0) {
        flare_fill_octagon(cx, cy, half, fw->aspect);
        flare_octagon_rects(cx, cy, half, fw->aspect, query.rect);
        query.nRects = 5;
        scale = scale * 1.2732406f;
    } else {
        gs_draw_flat_sprite(x0, y0, x1, y1);
        query.rect[0][0] = x0;
        query.rect[0][1] = y0;
        query.rect[0][2] = x1;
        query.rect[0][3] = y1;
        query.nRects = 1;
    }
    if (!(-1 < x0))
        x0 = 0;
    if (fw->width - 1 < x1)
        x1 = fw->width - 1;
    if (fw->height - 1 < y1)
        y1 = fw->height - 1;
    if (!(-1 < y0))
        y0 = 0;
    w = x1 - x0;
    h = y1 - y0;
    {
        /* PC: the native query (see above), in the shape draw's state */
        GsContext *ctx = GS_SPR_CTX;
        const GsTest *test = (const GsTest *)&ctx->reg[GS_REG_TEST];
        const GsZbuf *zbuf = (const GsZbuf *)&ctx->reg[GS_REG_ZBUF];
        f32 c = scale * (f32)(w * h) * 0.25f;
        u32 packed = pack_rgba_floats_to_u32(c, c, c, 1.0f, 255.0f) & 0xFF;
        query.frameBlock = (u32)SHADOW_U16(ctx->reg[GS_REG_FRAME])[0] * 32;
        query.fbw = SHADOW_U8(ctx->reg[GS_REG_FRAME])[2];
        query.fpsm = SHADOW_U8(ctx->reg[GS_REG_FRAME])[3];
        query.frameW = fw->width;
        query.frameH = fw->height;
        query.zBlock = (u32)zbuf->ZBP * 32;
        query.zpsm = zbuf->PSM;
        query.ztest = test->ZTE ? (s32)test->ZTST : 1;
        query.z = ctx->spriteZF & 0xFFFFFF;
        query.halfY = ctx->halfHeight != 0;
        query.box[0] = x0;
        query.box[1] = y0;
        query.box[2] = w;
        query.box[3] = h;
        query.cs = (packed * 129) >> 8;                 /* pgl_color's scale to GS units */
        query.resultBlock = (u32)fw->occVram >> 6;
        query.resultH = fw->occHeight;
        query.q = fw->queries;
        gs_flush_dirty_regs();
        pglr_flare_query(&query);
        /* the state the PS2's store left */
        gfx_color_mask(1, 1, 1, 1);
        gfx_enable(PGL_CAP_BLEND);
        gfx_blend_func(PGL_BLEND_SRC_ALPHA, 0.0f);
        pgl_color4f(c, c, c, 1.0f);
    }
    gfx_depth_mask(1);
    fw->queries = fw->queries + 1;
    return 1;
}

// FUNC 0x00247D50 flare_readback
/* The 64 x occHeight result strip (PSMCT32) read back into results (kept
 * for the draws); the query count restarts.
 * PC: the readback is only recorded (pglr_readback_flare): the draws read
 * the strip on the GPU (vif_model_draw), and the renderer does it only for
 * GT3PC_FLARE_LOG - the CPU no longer waits for the GPU here every race
 * frame.  No game code reads results. */
void flare_readback(FlareWork *fw, u32 *results)
{
    fw->results = results;
    pglr_readback_flare();
    gs_download_image(fw->occVram, 0, 0x40, 0, 0, 0x40, fw->occHeight, results);
    fw->queries = 0;
}

// FUNC 0x00247DA0 flare_draw_result_disc
void flare_draw_result_disc(FlareWork *fw, VifModel *m, f32 r, f32 g, f32 b)
{
    s32 q = fw->queries;

    vif_model_draw(fw, m, fw->results + (q & ~3), (u32)(q & 3), r, g, b);
    fw->queries = fw->queries + 1;
}

// FUNC 0x00247DF0 flare_draw_disc
void flare_draw_disc(FlareWork *fw, VifModel *m, f32 r, f32 g, f32 b, f32 intensity)
{
    intensity = intensity * 0.5f;
    vif_model_draw(fw, m, g_00344AB0, 0, r * intensity, g * intensity, b * intensity);
}

// FUNC 0x00247E18 flare_query_point
int flare_query_point(FlareWork *fw, f32 scale, f32 size)
{
    f32 sx, sy;
    f32 z;
    int isize;

    z = flare_project(g_00344AC0, &sx, &sy);
    if (z < -1.0f)
        return 0;
    if (1.0f < z)
        return 0;
    if (size < 2.0f)
        size = 2.0f;
    gfx_enable(PGL_CAP_DEPTH_TEST);
    isize = (int)size;
    return flare_occlusion_query(fw, 0, sx, sy, size, scale / (f32)(isize * isize));
}

// FUNC 0x00247EF0 flare_query_screen
int flare_query_screen(FlareWork *fw, const f32 *screen, f32 scale, f32 size)
{
    f32 x, y, z;
    int isize;

    x = (screen[0] * 0.5f + 0.5f) * (f32)fw->screenWidth;
    y = (0.5f - screen[1] * 0.5f) * (f32)fw->screenHeight;
    z = screen[2] - 0.2f;
    if (z < 0.0f)
        z = 0.0f;
    pgl_set_sprite_depth(z);
    if (size < 2.0f)
        size = 2.0f;
    isize = (int)size;
    return flare_occlusion_query(fw, 0, x, y, size, scale / (f32)(isize * isize));
}

// FUNC 0x00247FE8 flare_draw_glow
void flare_draw_glow(FlareWork *fw, int flags, const f32 *pos, VifModel *m, f32 near, f32 r, f32 g,
                     f32 b, f32 a, f32 scale, f32 haloA, f32 haloScale)
{
    int halo = 0;
    int drawModel = flags & 1;
    int unoccluded;
    f32 d;
    f32 k;

    d = -pos[2];
    if (0.0f < haloA && 0.0f < haloScale && 1.0f < d)
        halo = 1;
    k = d - near;
    unoccluded = flags & 2;
    k = k / d;
    gfx_depth_mask(0);
    gfx_enable(PGL_CAP_BLEND);
    gfx_blend_func(PGL_BLEND_ADD, 0.0f);
    vu0_load_identity_regs();
    pgl_translate(pos[0] * k, pos[1] * k, pos[2] * k);
    if (drawModel != 0) {
        pgl_push_matrix();
        vu0_scale(scale, scale, scale);
        if (unoccluded != 0) {
            gfx_enable(PGL_CAP_DEPTH_TEST);
            flare_draw_disc(fw, m, r, g, b, a);
        } else {
            gfx_disable(PGL_CAP_DEPTH_TEST);
            flare_draw_result_disc(fw, m, r * a, g * a, b * a);
        }
        pgl_pop_matrix();
    }
    if (halo != 0) {
        vu0_scale(haloScale, haloScale, haloScale);
        gfx_enable(PGL_CAP_DEPTH_TEST);
        flare_draw_disc(fw, flare_disc_model(), r, g, b, haloA);
    }
    gfx_depth_mask(1);
}

// FUNC 0x002481F0 flare_draw_streak
/* An elongated glow; the early exit for d < 0 skips the pop and the
 * depth-mask reset (a push / pop imbalance, as in the binary). */
void flare_draw_streak(FlareWork *fw, const f32 *pos, const f32 *axes, VifModel *unused, f32 near,
                       f32 r, f32 g, f32 b, f32 a)
{
    alignas(16) f32 mtx[4][4];
    f32 pz, d, k;

    (void)unused;
    pgl_push_matrix();
    if (0.0f < a) {
        gfx_depth_mask(0);
        gfx_enable(PGL_CAP_BLEND);
        gfx_enable(PGL_CAP_DEPTH_TEST);
        gfx_blend_func(PGL_BLEND_ADD, 0.0f);
        pz = pos[2];
        d = -pz;
        if (d < 0.0f)
            return;
        k = (d - near) / d;
        mtx[0][0] = axes[0];
        mtx[0][1] = axes[1];
        mtx[0][2] = 0.0f;
        mtx[0][3] = 0.0f;
        mtx[1][0] = axes[2];
        mtx[1][1] = axes[3];
        mtx[1][2] = 0.0f;
        mtx[1][3] = 0.0f;
        mtx[2][0] = 0.0f;
        mtx[2][1] = 0.0f;
        mtx[2][2] = 1.0f;
        mtx[2][3] = 0.0f;
        mtx[3][0] = pos[0] * k;
        mtx[3][1] = pos[1] * k;
        mtx[3][2] = pz * k;
        mtx[3][3] = 1.0f;
        pgl_load_matrix(&mtx[0][0]);
        flare_draw_disc(fw, flare_disc_model(), r, g, b, a);
        gfx_depth_mask(1);
    }
    pgl_pop_matrix();
}

// FUNC 0x00248378 flare_query_sun
int flare_query_sun(FlareWork *fw, const f32 *pos, f32 scale, f32 size)
{
    f32 sx, sy;
    int isize = (int)size;
    f32 z;

    pgl_push_matrix();
    vu0_reset_translation();
    z = flare_project(pos, &sx, &sy);
    pgl_pop_matrix();
    if (z < -1.0f)
        return 0;
    if (1.0f < z)
        return 0;
    gs_set_sprite_zf(0, 0);
    return flare_occlusion_query(fw, 0, sx, sy, (f32)isize, scale / (f32)(isize * isize));
}

// FUNC 0x00248458 flare_draw_sun
void flare_draw_sun(FlareWork *fw, const f32 *pos, VifModel *m, f32 scale, f32 r, f32 g, f32 b)
{
    gfx_depth_mask(0);
    pgl_push_matrix();
    vu0_reset_translation();
    pgl_translate(pos[0], pos[1], pos[2]);
    vu0_reset_rotation();
    gfx_blend_func(PGL_BLEND_ADD, 0.0f);
    vu0_scale(scale, scale, scale);
    gfx_disable(PGL_CAP_DEPTH_TEST);
    flare_draw_result_disc(fw, m, r, g, b);
    pgl_pop_matrix();
    gfx_depth_mask(1);
}

// FUNC 0x00248538 flare_draw_sun_disc
void flare_draw_sun_disc(FlareWork *fw, const f32 *pos, f32 r, f32 g, f32 b, f32 a, f32 scale)
{
    if (scale <= 0.0f)
        return;
    gfx_depth_mask(0);
    gfx_disable(PGL_CAP_DEPTH_TEST);
    gfx_disable(PGL_CAP_CULL_FACE);
    gfx_disable(PGL_CAP_ALPHA_TEST);
    gfx_blend_func(PGL_BLEND_ADD, 0.0f);
    pgl_push_matrix();
    vu0_reset_translation();
    pgl_translate(pos[0], pos[1], pos[2]);
    vu0_reset_rotation();
    vu0_scale(scale, scale, scale);
    flare_draw_disc(fw, flare_disc_model(), r, g, b, a);
    pgl_pop_matrix();
    gfx_depth_mask(1);
    gfx_enable(PGL_CAP_CULL_FACE);
}
