/* pgl_d3d.h - the Direct3D 11 objects of the pgl renderer: shaders, input
 * layouts, state-object caches, dynamic buffers (pgl_d3d.cpp). */
#ifndef PGL_NATIVE_PGL_D3D_H
#define PGL_NATIVE_PGL_D3D_H

#include <cstddef>
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
 * colour bytes; the fog coefficient F (0..255); PC: the texel coordinates a
 * sprite's samples are held to (u0, v0, u1, v1: the GS's first and last
 * pixels' - pgl_exec.cpp emit_sprite; the others unbounded). */
struct GsVtx {
    float x, y, z, q;
    float u, v;
    uint32_t rgba;
    float fog;
    float tu0 = -1e30f, tv0 = -1e30f, tu1 = 1e30f, tv1 = 1e30f;
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
    int32_t format;                 /* gt3vif::VertexFormat */
    float flareX, flareY;
    int32_t pad0;
    /* PC (2026-11-07): the hide box (PglrModelPart hideC / hideOn / hideH;
     * vs_vu1's hide): the model-space box whose inside is not drawn - the
     * cockpit camera's driver's head; hideC[3] 0: none.  In the head, so that
     * draws join only with the same box (merge_join's vs_cb_bound). */
    float hideC[4];                 /* centre; w 1 on */
    float hideH[4];                 /* half sizes */
    /* PC (CPU and GPU time, 2026-11-03): the lighting program, its specular
     * exponent and the lighting block are the draw's record's (Vu1Inst
     * lightBase, program, specExp: the blocks in a ring the vertex stage
     * reads, light_block); here only where the device has no such ring
     * (light_supported false) - the constants bound end at kCbVu1Head
     * otherwise, and the parts of a model with materials of their own share
     * them (merge_join) */
    int32_t program;                /* VU1P_* */
    int32_t specExp;                /* 4, 8, 16 */
    int32_t pad1, pad2;
    float L[16][4];                 /* the lighting block */
};
constexpr size_t kCbVu1Head = offsetof(CbVu1, program);
static_assert(kCbVu1Head % 16 == 0, "CbVu1's head");
/* PC (CPU and GPU time, 2026-11-03): what differs from one VU1 draw to the
 * next with the same matrix and lights - its texture's size, PRIM.IIP and the
 * depth from w (vu1_depth: a model's overlay layer) - as the draw's own
 * record, read by vs_vu1 from the instance stream (Vu1In inst0..3;
 * vu1_inst) instead of CbVu1's texW, texH, flatShade, zFromW (left 0 there):
 * 91 % of a race's constant-buffer writes were for these alone.  Likewise
 * the texture's rectangle (CbPs texX0..texInvH, left 0; PSF_INST_RECT),
 * passed on to the pixel stage.  PC (2026-11-04): and its lighting - the
 * block's place in the lights' ring (light_block; -1: CbVu1's L), the
 * program and the specular exponent: a car's parts change materials, and
 * each change was a constant-buffer write that ended the merged draw. */
struct Vu1Inst {
    float texW, texH, flat;         /* 2^TW, 2^TH (1 untextured), flat shading 1 */
    float parts;                    /* PC: a merged draw's parts (CbParts; 0: its own values here) */
    float zFromW[4];                /* CbVu1's zFromW */
    float texRect[4];               /* CbPs's texX0, texY0, texInvW, texInvH (0 untextured) */
    float lightBase, program, specExp, pad;     /* (whole numbers; pad: PC 2026-11-06 - 4 x the mesh's vertices
                                                 * (the doubled index stream, pgl_models.cpp) + the copy's place
                                                 * among the record's copies (vu1_inst): vs_vu1's rep) */
};
/* PC (CPU and GPU time, 2026-11-03): a merged VU1 draw - consecutive batches
 * of a model with the same constants and draw state but their own textures
 * (pgl_exec.cpp MergedDraw): a record of each of its parts, in order of
 * their first vertex (vs_vu1 finds a vertex's by SV_VertexID, reading them
 * from the records' ring as a buffer, t1 of the vertex stage), before the
 * draw's own record (Vu1Inst parts = their count, texRect[0] the first's
 * place in the ring): first vertex, texture slot (0: PS t0 / s0, k: t(k + 1)
 * / s(k)), PS texture flags (kPsTexFlags); 2^TW, 2^TH; the texture's
 * rectangle; PC (2026-11-04): its lighting (Vu1Inst's lightBase, program,
 * specExp). */
constexpr uint32_t kMergeParts = 32, kMergeSlots = 16;
struct Vu1Part {
    uint32_t firstVertex, slot, texFlags, pad0;
    float texW, texH, pad1, pad2;
    float texRect[4];
    float lightBase, program, specExp, pad3;
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
    /* PC (2026-11-10): PSF_ANISO's - pad1 1: the centre's colour alone (the
     * check GT3PC_PGL_ANISO_ZERO); pad2: the most taps (g_aniso; 0 off) */
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
/* PC (2026-11-05): a frame's flare queries drawn as one batch (pgl_exec.cpp
 * flare_queries_flush, flare_batch_draw below; docs/renderer.md 8): a query's
 * record - 8 float4 the vertex stage reads as a buffer (vs_flare_pt,
 * vs_flare_conv) - and a set of the halvings' weights (32 float4), shared by
 * the queries of one box size. */
struct FlareRec {
    int32_t box[4];                 /* x0, y0: the copy's origin on the screen; columns, rows copied */
    int32_t rect[5][4];             /* the shape: x0, y0, x1, y1 (exclusive) per rectangle */
    int32_t nRects, ztest, cs, weights;     /* the rectangles, the test (CbFlare), Cs, the weights set */
    float   z, texelX, texelY, pad; /* the shape's Z (Z x zNorm), the query's texel of the strip */
};
struct FlareWeights {
    float wx[64], wy[64];           /* CbFlare's wx / wy */
};
struct FlareBatch {                 /* what the batch's queries share */
    float    scale[2];              /* native depth pixels per GS pixel */
    int32_t  screen[2];             /* the screen's GS width, height */
    int32_t  halfY;                 /* 1: the frame holds every other row */
    int32_t  stripW, stripH;        /* the result strip (at GS resolution) */
    uint32_t nRecs, nWeights;
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
    /* PC (2026-11-10): [Graphics] AnisotropicFiltering - the texture's colour
     * over the pixel's footprint: up to CbPs::pad2 taps along its long axis,
     * each the GS's bilinear sample, weighted by its alpha; the alpha the
     * centre's (pgl_d3d.cpp aniso_rgb).  A 3D draw's bilinear texture
     * decoded from the GS's memory (pgl_exec.cpp state_texture: aniso_ok) */
    PSF_ANISO      = 1u << 5,
    PSF_FBA        = 1u << 9,
    PSF_ATST_SHIFT = 6,             /* 3 bits: pgl::gs::Compare */
    PSF_GATE_SHIFT = 10,            /* 2 bits: pgl::gs::AlphaGate */
    PSF_AMODE_SHIFT = 12,           /* 2 bits: TexBinding::alphaMode */
    PSF_PREMUL_SHIFT = 14,          /* 3 bits: pgl::gs::ColorPremul */
    PSF_CFIX       = 1u << 17,      /* C = FIX (else As) */
    PSF_SCAN_SHIFT = 18,            /* 2 bits: SCANMSK.MSK */
    PSF_DATE       = 1u << 20,      /* TEST.DATE: the destination alpha (t1) decides */
    PSF_DATM       = 1u << 21,      /* TEST.DATM: pass where its bit 7 is 1 (else 0) */
    PSF_BLEND_AD   = 1u << 22,      /* C = Ad (AdMode::Shader): the GS blend from the destination (t1) */
    PSF_FULL_COLOR = 1u << 23,      /* PC: the colour modulates with 0xFF = 1.0 (PRIM_FULL_COLOR) */
    PSF_INST_RECT  = 1u << 24,      /* PC: the texture's rectangle from the VU1 draw's record (Vu1Inst), not texX0.. */
    PSF_AD_INST    = 1u << 25,      /* PC (2026-11-05): a C = Ad draw (AdMode::TwoPass) as two instances of one draw
                                     * (below): the first writes no alpha (o0.a = 0, o1.a = 0), the second As (o1.a = 1) */
    /* PC (2026-11-05): the alpha mirror pass folded into the draw as its last
     * instance (the FOLD_PS variants, pgl_exec.cpp draw_passes; docs/
     * renderer.md 3, "DATE"): the pixel shader repeats the depth test from the
     * depth buffer's texture (t17: PSF_ZTEST on, the function PSF_ZFUNC_SHIFT)
     * and writes SV_Depth - the depth buffer's own value for the colour
     * instances (the fixed test is EQUAL: they pass) and another for the
     * mirror instance's fragments whose alpha bit is not the one held
     * (PSF_MIRROR_KEEP1), so those fail it and the depth-fail stencil op
     * marks the mirror (StencilOp::FoldReplace / FoldDate); the rest discard */
    PSF_MIRROR       = 1u << 26,
    PSF_MIRROR_KEEP1 = 1u << 27,
    PSF_ZFUNC_SHIFT  = 28,          /* 3 bits: pgl::gs::Compare, the draw's depth function */
    PSF_ZTEST        = 1u << 31     /* the draw tests depth (else every fragment passes) */
};
/* The texture's own PS flags (a merged draw's part has its own: ps_body) */
constexpr uint32_t kPsTexFlags =
    PSF_TME | (3u << PSF_TFX_SHIFT) | PSF_TCC | PSF_ANISO | (3u << PSF_AMODE_SHIFT) | PSF_FULL_COLOR;
static_assert(kPsTexFlags == 0x80302Fu, "ps_body's mask");

/* PC (2026-11-05): C = Ad as two instances.  Cs Ad + Cd (ALPHA 0x58: the
 * cars' env passes, the course's glint) needs Cs x DEST_ALPHA twice - Ad /
 * 255 each, Ad / 128 together, up to 2.0 for Ad 0xFF, which the GS allows
 * and GT3 uses (the course's env view holds alphas up to 0xFF, read back as
 * weights) - with the alpha left as it is between the two (the second
 * reads it too) and written by the second.  One draw of two instances does
 * both (DrawIndexedInstanced with 2, the VU1 draw's record twice in the
 * records' ring: vu1_inst's copies): the blend state's alpha equation is
 * o0.a x ONE + Ad x INV_SRC1_ALPHA, and ps_body (PSF_AD_INST) writes o0.a =
 * 0, o1.a = 0 for instance 0 - the alpha stays Ad - and o0.a = As, o1.a =
 * 1 for instance 1 (blend_state's adInst).  One submission and one state
 * where there were two of each with the binding changes between; the GPU's
 * work is the same.  Such a draw is one part's and is never extended by the
 * next part nor joined (the GS draws each part whole, in order: the next
 * part's first pass reads the alpha this part's second wrote where they
 * overlap).  Not when the form's dst is ZERO (Cs Ad, 0x98: the first
 * instance's result is the second's Cd), nor with depth writes (the second
 * instance's test), nor when the draw flips the alpha mirror's bit
 * (pgl_exec.cpp mirror_plan DateInvert: the first instance would, before
 * the second's DATE test); a mirror pass after the draw is as it was.
 * Those stay two draws.
 * (A target's alpha byte holding twice the GS alpha, so that one DEST_ALPHA
 * pass would do, was tried the same day: it cannot hold the weights above
 * 1.0, which GT3's env view uses, and was taken out.) */

/* VU1 lighting programs of vs_vu1. */
enum : int32_t {
    VU1P_DEFAULT = 0, VU1P_DIFFUSE, VU1P_SPEC2, VU1P_SPEC4, VU1P_LOCAL, VU1P_ENVMTX, VU1P_ENVMAP,
    VU1P_CONST, VU1P_FLARE
};

void d3d_init();

/* Shaders and layouts. */
enum class Vs { Gs, Vu1, Full, Vu1Direct };    /* PC: Vu1Direct - vs_vu1 without the clipper (vu1_draw) */
enum class Ps { Main, MainFlat, DepthCopy, Present, PfxColor, Plane, Flare, DepthResolve, Fxaa, ColorResolve,
                MirrorSync,                 /* PC: MirrorSync - the alpha mirror from a copy (mirror_sync) */
                MainFold, MainFlatFold,     /* PC (2026-11-05): ps_main / ps_main_flat with the mirror pass folded in (FOLD_PS) */
                /* PC (2026-11-15): ps_main / ps_main_flat shaded at every
                 * sample (SAMPLE_PS: AGSS, settings.h - a multisampled
                 * target's 3D draws, draw_passes) */
                MainSs, MainFlatSs };
inline bool ps_per_sample(Ps ps)
{
    return ps == Ps::MainSs || ps == Ps::MainFlatSs;
}
void bind_vs(Vs vs);
void bind_ps(Ps ps);
/* PC (2026-11-04): the mirror pass's pixel shader (docs/renderer.md 3,
 * "DATE"): ps_main / ps_main_flat (merged: the merged draws' variant) that
 * discards where the alpha it would write has the bit the mirror holds
 * (CbMirror keep, mirror_cb: 0 / 1) - the stencil op marks the rest; the
 * shader bound, the depth state and the blend state as the executor's cache
 * has them (the pass puts them back after itself).  PC (2026-11-15):
 * perSample, the variant shaded at every sample (a per-sample draw's pass:
 * the samples it marks are the ones the draw wrote). */
ID3D11PixelShader *mirror_ps(bool flat, bool merged, bool perSample = false);
/* PC (2026-11-15): the per-sample shaders were made (ps_4_1: feature level
 * 10.1, which MSAA needs too). */
bool per_sample_shaders();
ID3D11Buffer *mirror_cb(bool keep1);
ID3D11PixelShader *bound_ps();
const char *ps_name(ID3D11PixelShader *ps);     /* PC (2026-11-06): its name (the frame capture's notes) */
ID3D11DepthStencilState *bound_depth(uint8_t *stencilRef);
ID3D11BlendState *bound_blend();
/* Constant buffer contents (b0); skipped when equal to what is bound. */
void bind_vs_cb(const void *data, size_t bytes);
/* ps_flare's constants, in their own buffer (bound to slot b0 of the pixel
 * stage; the next bind_ps / bind_ps_cb puts cbPs back). */
void bind_flare_cb(const CbFlare &cb);
/* PC (2026-11-05): a batch of flare queries drawn (FlareRec; on the D3D
 * thread, in order with the draws, the records and weights copied along):
 * zSrv the screen's depth (null: no depth test), stripRtv the result strip's
 * view.  The caller forgets its bindings around it (d3d_invalidate). */
void flare_batch_draw(const FlareBatch &b, const FlareRec *recs, const FlareWeights *weights,
                      ID3D11ShaderResourceView *zSrv, ID3D11RenderTargetView *stripRtv, uint32_t capId);
void bind_ps_cb(const void *data, size_t bytes);
/* Forgets the bindings (another module bound its own). */
void d3d_invalidate();
/* The same without the geometry stage's reset queued: after a module that
 * used the context directly (and left no geometry shader bound). */
void d3d_forget();

/* Pipeline bindings through a cache of what is bound (redundant calls
 * skipped).  set_targets also unbinds PS slot 0 when the targets change
 * (D3D11 drops an SRV of a resource bound as output).
 * PC (2026-11-05): the cache is the submit stage's (pgl_queue.h sq; docs/
 * renderer.md 3, "The submit thread"): these, bind_vs .. bind_flare_cb and
 * d3d_invalidate / d3d_forget, called from the executor's thread while a
 * frame pipelines (q::t_forward: the passes - target copies and resolves,
 * the alpha mirror's sync, the effect recipes), go through the submit
 * thread's ring, in order with the draws, and take effect there.  What reads
 * the cache (vs_cb_bound, bound_*, d3d_check_bound) and the records' and
 * lighting rings (vu1_inst*, light_*) are the submit stage's alone. */
void set_targets(ID3D11RenderTargetView *rtv, ID3D11DepthStencilView *dsv, int w, int h);
void set_scissor(const D3D11_RECT &r);
void set_raster(ID3D11RasterizerState *s);
void set_blend(ID3D11BlendState *s);
void set_depth(ID3D11DepthStencilState *s, uint8_t stencilRef = 0);
void set_srv0(ID3D11ShaderResourceView *srv);
/* PS slot 1: the copy of the target a DATE draw reads its destination alpha
 * from (never a resource bound as output). */
void set_srv1(ID3D11ShaderResourceView *srv);
/* PC (2026-11-05): PS slot 17, the depth buffer's texture for a draw with its
 * mirror pass folded in (PSF_MIRROR; bound read-only as the depth-stencil
 * view then: Target::dsvRead); null for every other draw. */
void set_srv_z(ID3D11ShaderResourceView *srv);
void set_sampler0(ID3D11SamplerState *s);
void set_topology(D3D11_PRIMITIVE_TOPOLOGY t);
void set_vb(ID3D11Buffer *vb, UINT stride);
void set_ib(ID3D11Buffer *ib);
/* PC (2026-11-03): input slot 2, the VU1 vertices' batches (pgl_models.cpp
 * make_batch_buffer; null: streamed vertices - a buffer of zeros at stride
 * 0, read by no merged draw). */
void set_batch_vb(ID3D11Buffer *bvb);

/* PC (2026-11-03): the VU1 draws' records (Vu1Inst).  vu1_inst: the record's
 * number (the last one's again when equal), written to the executor's ring
 * of kVu1Insts (it waits for the D3D thread when that is full) and the
 * stream bound to input slot 1.  vu1_inst_upload (in the draw's call, on the
 * D3D thread): the records written so far copied into the GPU's ring - in
 * one map, the first draw of each run that needs one (NO_OVERWRITE, DISCARD
 * at the start of each cycle of the ring) - then the draw takes its record
 * as its first instance (vu1_inst_slot). */
constexpr uint32_t kVu1Insts = 1u << 16;
/* PC (2026-11-05): the rings a submitting thread writes - the records', the
 * lighting blocks', the constants' (pgl_d3d.cpp Rings) - are the thread's
 * own (the executor's when it submits itself, the submit thread's, the
 * submit workers'): made as the thread starts (d3d_thread_init), named to
 * the D3D thread by the draws' calls (thread_rings). */
struct Rings;
Rings *thread_rings();
void d3d_thread_init();
/* PC (2026-11-05): copies - the record `copies' times in consecutive places
 * (2: a C = Ad draw's two instances read places seq and seq + 1) */
uint32_t vu1_inst(const Vu1Inst &rec, uint32_t copies = 1);
void vu1_inst_upload(Rings *rg, uint32_t seq);
inline uint32_t vu1_inst_slot(uint32_t seq) { return seq & (kVu1Insts - 1); }
/* PC (2026-11-03): a merged draw's n part records and its own record rec
 * (Vu1Part, its texRect[0] set here), one after the other in one cycle of
 * the ring: rec's number (PC, 2026-11-05: rec `copies' times, as vu1_inst). */
uint32_t vu1_inst_parts(const Vu1Part *parts, uint32_t n, Vu1Inst rec, uint32_t copies = 1);
/* PC (2026-11-03): draws may be merged (the records' ring read as a buffer:
 * Direct3D 11.1's NO_OVERWRITE maps of a buffer a shader reads). */
bool merge_supported();
/* PC (2026-11-03): the VS constants bound are these bytes (a merged draw's
 * part needs its batch's to be the draw's). */
bool vs_cb_bound(const void *data, size_t bytes);
/* PC (2026-11-03): a merged VU1 draw queued: slots 1.. of its textures and
 * samplers (slot 0 bound as ever; the others where they changed), the
 * merged variant of the pixel shader bound (ps: the one bound, Main or
 * MainFlat) for the draw with record inst (vu1_inst_parts), the bound one
 * back after it; instances: 1, or 2 for a C = Ad draw (PSF_AD_INST). */
void draw_merged(ID3D11ShaderResourceView *const *srv, ID3D11SamplerState *const *samp, uint32_t nSlots, Ps ps,
                 uint32_t count, uint32_t first, int32_t base, uint32_t inst, uint32_t lights, uint32_t instances,
                 uint32_t capId);           /* (PC, 2026-11-06: capId - the frame capture's record, ~0u none) */
/* PC (2026-11-04): the lighting blocks' ring (pgl_d3d.cpp LightRing; docs/
 * renderer.md 4, "Lighting blocks"): a VU1 draw's block (CbVu1's L, 16
 * float4) goes in when it is not the last one or one still in the ring
 * (light_block: its number - the draw's record's lightBase; -1 without the
 * ring), read by vs_vu1 as a buffer (t2 of the vertex stage, lightBlocks);
 * the D3D thread copies the blocks published to the GPU's ring as draws
 * need them (light_upload, up to the number light_mark gave when the draw
 * was queued: a draw's blocks are all before it, and a new cycle of the
 * ring - a DISCARD map - takes nothing from the one before).  Without the
 * ring (light_supported false: no NO_OVERWRITE maps of a buffer a shader
 * reads) the block stays in CbVu1 and the constants are written whole. */
constexpr uint32_t kLightBlocks = 1u << 14;
bool light_supported();
int32_t light_block(const float (*L)[4]);
uint32_t light_mark();
void light_upload(Rings *rg, uint32_t upTo);

/* State objects.  PC (2026-11-05): adInst - the alpha equation of a C = Ad
 * draw of two instances (above): o0.a + Ad (1 - o1.a). */
ID3D11BlendState *blend_state(const pgl::gs::Blend &b, uint8_t writeMask, bool adInst = false);
ID3D11BlendState *blend_plain(uint8_t writeMask);
/* PC (2026-11-04): the stencil's part of a depth state (the alpha mirror of
 * the DATE draws, docs/renderer.md 3): None - off; Replace - every fragment
 * drawn writes ref (an alpha write whose bit 7 is known, or the mirror
 * pass's 1, or 0 before a mirror pass); DateKeep - the DATE test (equal to
 * ref, DATM), the mirror kept; DateInvert - the DATE test, the bit flipped
 * where drawn (an alpha write of the other value, or a DATE draw's mirror
 * pass).  PC (2026-11-05): the mirror
 * pass folded into its draw (PSF_MIRROR, the depth function EQUAL): the
 * colour instances pass the depth test and FoldReplace writes 0 where they
 * draw / FoldDate (the DATE test) keeps; the mirror instance's fragments fail
 * it and the depth-fail op writes ref 1 / flips the bit.
 * PC (2026-11-06): the mirror is stencil bit 1 (kMirrorBit); a ref of 1 in
 * the plans means that bit (stencil_ref_byte).  DatePair - a C = Ad draw of
 * two instances that flips the bit (the cars' env passes): the DATE test on
 * bit 1 and the op INCREMENT through bits 0..1.  The first instance passes
 * on a clear bit and counts 0 -> 1 (bit 1 still clear: the second instance
 * passes too) and the second counts 1 -> 2 (the bit set) - the keep of the
 * first draw and the invert of the second of the pair this replaces, in one
 * draw (pgl_exec.cpp draw_passes).  Both instances pass or fail together
 * (the same fragments, no depth writes between), so bit 0 is 0 after every
 * pair; Replace writes bits 0..1 (ref 0 / 2: bit 0 cleared as well), the
 * inverts and the fold's ops bit 1 alone. */
enum class StencilOp : uint8_t { None, Replace, DateKeep, DateInvert, FoldReplace, FoldDate, DatePair };
constexpr uint8_t kMirrorBit = 0x02;
inline uint8_t stencil_ref_byte(uint8_t ref) { return ref != 0 ? kMirrorBit : 0; }
ID3D11DepthStencilState *depth_state(bool test, pgl::gs::Compare func, bool write, StencilOp op = StencilOp::None,
                                     uint8_t ref = 0);
ID3D11RasterizerState *raster_state(int cull /* 0 none, 1 back, 2 front */, bool scissor);
ID3D11SamplerState *sampler_state(bool magLinear, bool minLinear, int wrapU, int wrapV);
/* PC (2026-11-13): a replacement texture's (pgl_texrepl.h): through its mip
 * levels - trilinear (aniso 1: the draws' use; the anisotropic taps are the
 * shader's, aniso_rgb), or the hardware's anisotropic filter at level aniso. */
ID3D11SamplerState *sampler_state_repl(int wrapU, int wrapV, int aniso);

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
