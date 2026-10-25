/* pgl_d3d.h - the Direct3D 11 objects of the pgl renderer: shaders, input
 * layouts, state-object caches, dynamic buffers (pgl_d3d.cpp). */
#ifndef PGL_NATIVE_PGL_D3D_H
#define PGL_NATIVE_PGL_D3D_H

#include <cstdint>

#include <d3d11.h>

#include "engine/pgl/native/pgl_render.h"
#include "engine/pgl/native/pgl_state_gs.h"

namespace pglr {

ID3D11Device *dev();
/* The immediate context: everything queued for the D3D thread is run first
 * (pgl_queue.h; nothing to wait for on the D3D thread itself). */
ID3D11DeviceContext *dc();
/* The immediate context as it is: for the calls queued (q::run's closures,
 * on the D3D thread) only. */
ID3D11DeviceContext *qdc();

/* The vertex of the GS-space draws (2D primitives, GIF blocks): native pixel
 * position, depth (0..1), Q; texel coordinates (already divided by Q); the
 * colour bytes; the fog coefficient F (0..255). */
struct GsVtx {
    float x, y, z, q;
    float u, v;
    uint32_t rgba;
    float fog;
};

/* Constant buffers (layouts of the HLSL in pgl_d3d.cpp). */
struct CbGs {                       /* b0 of vs_gs */
    float vpW, vpH, pad0, pad1;
};
struct CbVu1 {                      /* b0 of vs_vu1 */
    float C[4][4];                  /* the combined matrix, rows (v x C) */
    float viewScale[4];
    float viewCenter[4];
    float limits[4];
    /* GS window -> target NDC: x_ndc = (a_x pc.x + b_x pc.w) - pc.w,
     * y_ndc = pc.w - (a_y pc.y + b_y pc.w) (a = 2 viewScale s / native size,
     * b = 2 ((viewCenter - offset) s + 0.5) / native size) */
    float ndcMap[4];                /* a_x, b_x, a_y, b_y */
    /* flareO: the flare program's occlusion texel (0..255); flareGpu 1: read
     * instead from the result strip bound to VS t0, at native (flareX, flareY) */
    float morph, zNorm, flareO, flareGpu;
    int32_t program;                /* VU1P_* */
    int32_t format;                 /* gt3vif::VertexFormat */
    int32_t specExp;                /* 4, 8, 16 */
    int32_t flatShade;              /* PRIM.IIP 0: gs_vu1 keeps the provoking colour on clipped pieces */
    float texW, texH, flareX, flareY;   /* 2^TW, 2^TH */
    float L[16][4];                 /* the lighting block */
};
struct CbPs {                       /* b0 of the GS pixel shaders */
    float texX0, texY0, texInvW, texInvH;
    uint32_t flags;                 /* PSF_* */
    float aref;
    float fix;
    float rowScale;                 /* native rows per GS row (SCANMSK) */
    float fogR, fogG, fogB, ta0;
    float ta1;
    float adSel;                    /* PSF_BLEND_AD: ALPHA's A | B << 2 | D << 6 (an integer) */
    float pad1, pad2;
};
struct CbPresent {
    float u0, v0, u1, v1;
    /* a picture larger than the screen (the resolution scale): tapsX x
     * tapsY bilinear taps over each screen pixel's part of it, (stepU,
     * stepV) apart - a box filter; 0 / 1: one tap */
    float stepU, stepV, tapsX, tapsY;
};
struct CbPfx {
    float colR[4], colG[4], colB[4];
    float srcScale[4];              /* source pixels per destination pixel */
};
/* b0 of ps_flare: one lens-flare occlusion query (pgl_flare.cpp
 * flare_occlusion_query, the native pass of exec_flare_query). */
struct CbFlare {
    int32_t box[4];                 /* x0, y0: the copy's origin on the screen; columns, rows copied */
    int32_t rect[5][4];             /* the shape: x0, y0, x1, y1 (exclusive) per rectangle */
    int32_t nRects;                 /* 1 (square) or 5 (octagon) */
    int32_t ztest;                  /* 0 never, 1 always, 2 GEQUAL, 3 GREATER (the shape's ZTST; 1 with ZTE off) */
    int32_t cs;                     /* the store's colour Cs (GS units, at most 0x80) */
    int32_t halfY;                  /* 1: the frame holds every other row (field rendering) */
    float   scale[2];               /* native depth pixels per GS pixel */
    float   z;                      /* the shape's Z, as the depth target holds it (Z x zNorm) */
    float   pad;
    int32_t screen[4];              /* the screen's GS width, height (the copy's texture clamp) */
    float   wx[68];                 /* the weight of each copied column in the final texel (64 used) */
    float   wy[68];                 /* ... of each copied row */
};
struct CbPlane {                    /* b0 of ps_plane (a byte plane through a palette) */
    int32_t channel;                /* the captured frame's channel holding the plane's bytes (0 R, 1 G, 2 B) */
    int32_t shift, mask;            /* index = (byte >> shift) & mask: T8H 0 / 0xFF, T4HL 0 / 0xF, T4HH 4 / 0xF */
    int32_t pad;
};

/* CbPs.flags */
enum : uint32_t {
    PSF_TME        = 1u << 0,
    PSF_TFX_SHIFT  = 1,             /* 2 bits */
    PSF_TCC        = 1u << 3,
    PSF_FGE        = 1u << 4,
    PSF_FBA        = 1u << 9,
    PSF_ATST_SHIFT = 6,             /* 3 bits: pgl::gs::Compare */
    PSF_GATE_SHIFT = 10,            /* 2 bits: pgl::gs::AlphaGate */
    PSF_AMODE_SHIFT = 12,           /* 2 bits: TexBinding::alphaMode */
    PSF_PREMUL_SHIFT = 14,          /* 3 bits: pgl::gs::ColorPremul */
    PSF_CFIX       = 1u << 17,      /* C = FIX (else As) */
    PSF_SCAN_SHIFT = 18,            /* 2 bits: SCANMSK.MSK */
    PSF_DATE       = 1u << 20,      /* TEST.DATE: the destination alpha (t1) decides */
    PSF_DATM       = 1u << 21,      /* TEST.DATM: pass where its bit 7 is 1 (else 0) */
    PSF_BLEND_AD   = 1u << 22       /* C = Ad (AdMode::Shader): the GS blend from the destination (t1) */
};

/* VU1 lighting programs of vs_vu1. */
enum : int32_t {
    VU1P_DEFAULT = 0, VU1P_DIFFUSE, VU1P_SPEC2, VU1P_SPEC4, VU1P_LOCAL, VU1P_ENVMTX, VU1P_ENVMAP,
    VU1P_CONST, VU1P_FLARE
};

void d3d_init();

/* Shaders and layouts. */
enum class Vs { Gs, Vu1, Full };
enum class Ps { Main, MainFlat, DepthCopy, Present, PfxColor, Plane, Flare, DepthResolve, Fxaa, ColorResolve };
void bind_vs(Vs vs);
void bind_ps(Ps ps);
/* Constant buffer contents (b0); skipped when equal to what is bound. */
void bind_vs_cb(const void *data, size_t bytes);
/* ps_flare's constants, in their own buffer (bound to slot b0 of the pixel
 * stage; the next bind_ps / bind_ps_cb puts cbPs back). */
void bind_flare_cb(const CbFlare &cb);
void bind_ps_cb(const void *data, size_t bytes);
/* Forgets the bindings (another module bound its own). */
void d3d_invalidate();
/* The same without the geometry stage's reset queued: after a module that
 * used the context directly (and left no geometry shader bound). */
void d3d_forget();

/* Pipeline bindings through a cache of what is bound (redundant calls
 * skipped).  set_targets also unbinds PS slot 0 when the targets change
 * (D3D11 drops an SRV of a resource bound as output). */
void set_targets(ID3D11RenderTargetView *rtv, ID3D11DepthStencilView *dsv, int w, int h);
void set_scissor(const D3D11_RECT &r);
void set_raster(ID3D11RasterizerState *s);
void set_blend(ID3D11BlendState *s);
void set_depth(ID3D11DepthStencilState *s);
void set_srv0(ID3D11ShaderResourceView *srv);
/* PS slot 1: the copy of the target a DATE draw reads its destination alpha
 * from (never a resource bound as output). */
void set_srv1(ID3D11ShaderResourceView *srv);
void set_sampler0(ID3D11SamplerState *s);
void set_topology(D3D11_PRIMITIVE_TOPOLOGY t);
void set_vb(ID3D11Buffer *vb, UINT stride);
void set_ib(ID3D11Buffer *ib);

/* State objects. */
ID3D11BlendState *blend_state(const pgl::gs::Blend &b, uint8_t writeMask);
ID3D11BlendState *blend_plain(uint8_t writeMask);
ID3D11DepthStencilState *depth_state(bool test, pgl::gs::Compare func, bool write);
ID3D11RasterizerState *raster_state(int cull /* 0 none, 1 back, 2 front */, bool scissor);
ID3D11SamplerState *sampler_state(bool magLinear, bool minLinear, int wrapU, int wrapV);

/* GT3PC_FLICKER: the state the binding cache believes is bound for a draw
 * against what the context holds (texture: the draw samples slot 0, date:
 * slot 1; indexed: it reads the index buffer).  The bits that differ
 * (D3DB_*), described into what. */
enum : uint32_t {
    D3DB_RTV = 1u << 0, D3DB_DSV = 1u << 1, D3DB_VIEWPORT = 1u << 2, D3DB_SCISSOR = 1u << 3,
    D3DB_RASTER = 1u << 4, D3DB_BLEND = 1u << 5, D3DB_DEPTH = 1u << 6, D3DB_PS = 1u << 7, D3DB_VS = 1u << 8,
    D3DB_TOPO = 1u << 9, D3DB_VB = 1u << 10, D3DB_IB = 1u << 11, D3DB_SRV0 = 1u << 12, D3DB_SAMP0 = 1u << 13,
    D3DB_SRV1 = 1u << 14, D3DB_PSCB = 1u << 15, D3DB_VSCB = 1u << 16
};
uint32_t d3d_check_bound(bool texture, bool date, bool indexed, char *what, size_t n);
/* GT3PC_FLICKER: right after a draw, copies of what the GPU holds for it
 * (the constant buffers; the vertex range, an indexed draw's first `count'
 * vertices from baseVertex, and its index range) into slot `slot' of
 * staging set `set' (0..3, one per frame in flight), with hashes of what
 * was written.  Resolved once the GPU is done with that frame: bits per
 * slot, 1 VS constants, 2 PS constants, 4 vertices, 8 indices differ. */
void d3d_verify_copy(int set, uint32_t slot, bool indexed, uint32_t first, uint32_t count, int32_t baseVertex);
void d3d_verify_resolve(int set, uint32_t used, uint32_t *bits);

/* Dynamic vertex / index streams: copies data in, returns the first
 * element's index in the buffer. */
uint32_t stream_vertices(const void *data, uint32_t stride, uint32_t count, ID3D11Buffer **vb);
uint32_t stream_indices(const uint32_t *data, uint32_t count, ID3D11Buffer **ib);

/* A small RGBA8 texture with initial data (the colour filter's curve). */
bool make_texture(int w, int h, const uint32_t *rgba, Com<ID3D11Texture2D> &tex,
                  Com<ID3D11ShaderResourceView> &srv);

}  /* namespace pglr */

#endif /* PGL_NATIVE_PGL_D3D_H */
