/* pgl_d3d.cpp - the Direct3D 11 objects of the pgl renderer: the shaders
 * (HLSL below), input layouts, state-object caches and the dynamic vertex /
 * index streams.
 *
 * Shader conventions (docs/renderer.md, docs/pgl.md 4-5): colours travel in
 * GS units (bytes 0..255, 0x80 = 1.0 for modulation and alpha); the pixel
 * shader computes the GS source colour / alpha (texture function, fog,
 * alpha test) and writes o0 = colour / 255 and, for the blend, o1.a = C /
 * 128 (dual-source: C = As or FIX).  Texture coordinates travel as the GS's
 * S T Q (texels x Q, Q), interpolated linearly in screen space and divided
 * per pixel, as the GS does; the bound native texture holds the GS texels
 * (x0, y0, w, h) of the texture's 2^TW x 2^TH space.
 *
 * PC (CPU time): the VS / PS constants (bind_vs_cb, bind_ps_cb) go into a
 * ring - one large dynamic buffer written forward in 256-byte slots with
 * NO_OVERWRITE (DISCARD when it wraps), bound at their offset (Direct3D
 * 11.1 constant buffer offsets) - instead of renaming one small buffer with
 * DISCARD at each change (a model part's constants nearly every draw: some
 * 3500 renames a race frame).  Without the driver's support: as before. */
#include "engine/pgl/native/pgl_d3d.h"

#include <cstring>
#include <unordered_map>

#include <d3d11_1.h>
#include <d3dcompiler.h>

#include "platform/gpu.h"
#include "platform/log.h"

namespace pglr {

ID3D11Device *dev() { return gpu::device(); }
ID3D11DeviceContext *dc() { return gpu::context(); }

namespace {
ID3D11DeviceContext *g_qdc;                 /* (d3d_init: the context never changes) */
}  /* namespace */

ID3D11DeviceContext *qdc() { return g_qdc; }

namespace {

/* One source, compiled per entry point with the SEC_* macro of its section
 * (each section declares its own b0). */
const char *kHlsl = R"HLSL(
struct PsIn {
    float4 pos : SV_Position;
    noperspective float3 stq : TEXCOORD0;   /* S x 2^TW, T x 2^TH, Q */
    noperspective float4 color : COLOR0;    /* GS bytes */
    noperspective float fog : FOG;          /* F, 0..255 */
};
struct PsInFlat {
    float4 pos : SV_Position;
    noperspective float3 stq : TEXCOORD0;
    nointerpolation float4 color : COLOR0;  /* the leading (kicking) vertex's */
    noperspective float fog : FOG;
};
struct FullOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

/* ---- GS-space vertices ---------------------------------------------- */
#ifdef SEC_GS
cbuffer CbGs : register(b0) {
    float4 vp;                      /* native target width, height */
};
struct GsIn {
    float4 pos : POSITION;          /* native x, y, depth, Q */
    float2 tc : TEXCOORD0;          /* texels x Q (FST: texels, Q = 1) */
    uint4 color : COLOR0;
    float fog : FOG;
};
PsIn vs_gs(GsIn i)
{
    PsIn o;
    o.pos = float4(i.pos.x / vp.x * 2.0 - 1.0, 1.0 - i.pos.y / vp.y * 2.0, i.pos.z, 1.0);
    o.stq = float3(i.tc, i.pos.w);
    o.color = float4(i.color);
    o.fog = i.fog;
    return o;
}
#endif

/* ---- VU1 programs (docs/models.md 5) --------------------------------- */
#ifdef SEC_VU1
cbuffer CbVu1 : register(b0) {
    float4 C[4];
    float4 viewScale;
    float4 viewCenter;
    float4 limits;
    float4 ndcMap;
    float morph; float zNorm; float flareO; float flareGpu;
    int program; int format; int specExp; int flatShade;
    float texW; float texH; float flareX; float flareY;
    float4 L[16];
};
/* PC: the flare queries' result strip (flareGpu: the glow's texel read
 * here, at (flareX, flareY), instead of flareO: docs/renderer.md 8) */
Texture2D flareStrip : register(t0);
/* A lit vertex in the VU1's clip space, before the projection: what the
 * microcode's polygon clipper works on. */
struct Vu1Clip {
    float4 pc : CLIPPOS;            /* (P, 1) x C */
    float2 st : TEXCOORD0;          /* S x 2^TW, T x 2^TH (not yet x Q) */
    float4 color : COLOR0;          /* GS bytes */
};
struct Vu1In {
    float3 pos : POSITION0;  float3 pos2 : POSITION1;
    float3 nrm : NORMAL0;    float3 nrm2 : NORMAL1;
    float2 st  : TEXCOORD0;  float2 st2  : TEXCOORD1;
    uint4  rgba : COLOR0;    uint4  rgba2 : COLOR1;
};
float pow_e(float x)
{
    float x2 = x * x;
    float x4 = x2 * x2;
    if (specExp <= 4) return x4;
    float x8 = x4 * x4;
    if (specExp <= 8) return x8;
    return x8 * x8;
}
float4 pow_e4(float4 x)
{
    return float4(pow_e(x.x), pow_e(x.y), pow_e(x.z), pow_e(x.w));
}
Vu1Clip vs_vu1(Vu1In i)
{
    Vu1Clip o;
    float3 P = i.pos;
    float3 N = i.nrm;
    float2 ST = i.st;
    float4 c = float4(i.rgba);
    if (format == 1 || format == 2) {
        P = lerp(i.pos, i.pos2, morph);
        N = lerp(i.nrm, i.nrm2, morph);
        ST = lerp(i.st, i.st2, morph);
        if (format == 1)
            c = trunc(lerp(float4(i.rgba), float4(i.rgba2), morph));
    }
    if (program == 1) {             /* DIFFUSE */
        float4 d = max(L[0] * N.x + L[1] * N.y + L[2] * N.z, 0);
        c = clamp(L[3] + L[4] * d.x + L[5] * d.y + L[6] * d.z + L[7] * d.w, 0, L[8]);
    } else if (program == 2) {      /* SPEC2* */
        float4 t = max(L[0] * N.x + L[1] * N.y + L[2] * N.z, 0);
        c = clamp(L[3] + L[4] * t.x + L[5] * t.y + L[6] * pow_e(t.z) + L[7] * pow_e(t.w), 0, L[8]);
    } else if (program == 3) {      /* SPEC4* */
        float4 dl = max(L[0] * N.x + L[1] * N.y + L[2] * N.z, 0);
        float4 dh = pow_e4(max(L[8] * N.x + L[9] * N.y + L[10] * N.z, 0));
        c = clamp(L[3] + L[4] * dl.x + L[5] * dl.y + L[6] * dl.z + L[7] * dl.w
                  + L[11] * dh.x + L[12] * dh.y + L[13] * dh.z + L[14] * dh.w, 0, L[15]);
    } else if (program == 4) {      /* LOCAL* */
        float3 V = P - L[3].xyz;
        float3 R = V - 2 * dot(V, N) * N;
        float r = max(dot(float3(L[0].x, L[1].x, L[2].x), R) * rsqrt(max(dot(V, V), 1e-30)), 0);
        /* alpha 0x80: the program's vf09 = vf00 + I (127), w = 1 + 127, and
         * FTOI0.xyz replaces only the colour (0x1BF / 0x22C, 0x1D6 / 0x24A) */
        c = float4(trunc(L[4].rgb * pow_e(r)), 128);
    } else if (program == 5) {      /* ENVMTX */
        float3 V = P - L[3].xyz;
        float iv = rsqrt(max(dot(V, V), 1e-30));
        float3 R = (V - 2 * dot(V, N) * N) * iv;
        float3 Rv = R.x * L[0].xyz + R.y * L[1].xyz + R.z * L[2].xyz;
        float m = 0.5 * rsqrt(max(Rv.x * Rv.x + Rv.y * Rv.y + (Rv.z + 1) * (Rv.z + 1), 1e-30));
        ST = float2(0.5 + Rv.x * m, 0.5 - Rv.y * m);
        c = trunc(c * (L[4] + L[5] * min(dot(V, N) * iv, 0)));
    } else if (program == 6) {      /* ENVMAP */
        float3 V = P - L[3].xyz;
        float iv = rsqrt(max(dot(V, V), 1e-30));
        float3 Re = V - 2 * dot(V, N) * N;
        Re = Re.x * L[0].xyz + Re.y * L[1].xyz + Re.z * L[2].xyz;
        float th;
        if (Re.x >= Re.z)
            th = (Re.x >= -Re.z) ? Re.z / Re.x : 6 - Re.x / Re.z;
        else
            th = (Re.x >= -Re.z) ? 2 - Re.x / Re.z : 4 + Re.z / Re.x;
        float y = Re.y * iv;
        ST = float2(L[0].w * th + L[1].w, L[2].w * (y * y * y + 2 * y) + L[3].w);
        c = trunc(c * (L[4] + L[5] * min(dot(V, N) * iv, 0)));
    } else if (program == 7) {      /* CONST */
        c = clamp(c * L[0] + L[2], 0, L[1]);
    } else if (program == 8) {      /* the flare program */
        /* the occlusion texel's R byte: from the strip on the GPU (the
         * queries just before wrote it), or flareO; 0: not drawn (beyond the
         * far plane: gs_vu1 drops the triangle) */
        float occ = flareO;
        if (flareGpu != 0)
            occ = round(flareStrip.Load(int3(flareX, flareY, 0)).r * 255.0);
        c = min(c * float4(L[0].rgb, occ / 255.0), 255);
        if (occ == 0) {
            o.pc = float4(0, 0, 2, 1);
            o.st = float2(0, 0);
            o.color = float4(0, 0, 0, 0);
            return o;
        }
    }
    o.pc = P.x * C[0] + P.y * C[1] + P.z * C[2] + C[3];
    o.st = ST * float2(texW, texH);
    o.color = c;
    return o;
}

/* The transform programs' projection of a clip-space vertex (docs/models.md
 * 3.3): q = 1 / w, STQ = (st q, q), XYZ through viewScale / viewCenter (here
 * to the target's NDC), F = min(w, fog start) q viewScale.w + viewCenter.w. */
PsIn project_vu1(Vu1Clip v)
{
    PsIn o;
    float4 pc = v.pc;
    o.pos.x = ndcMap.x * pc.x + ndcMap.y * pc.w - pc.w;
    o.pos.y = pc.w - (ndcMap.z * pc.y + ndcMap.w * pc.w);
    o.pos.z = (pc.z * viewScale.z + pc.w * viewCenter.z) * zNorm;
    o.pos.w = pc.w;
    float q = 1.0 / pc.w;
    o.fog = clamp(viewCenter.w + viewScale.w * min(pc.w, limits.w) * q, 0, 255);
    o.color = v.color;
    o.stq = float3(v.st * q, q);
    return o;
}

/* The clipper's planes, as distances (inside >= 0): the guard band |x|,
 * |y| <= 2w (x, y x vBB0 = 0.5 against w) and |z| <= w. */
float clip_dist(float4 p, int k)
{
    if (k == 0) return 2 * p.w - p.x;
    if (k == 1) return 2 * p.w + p.x;
    if (k == 2) return 2 * p.w - p.y;
    if (k == 3) return 2 * p.w + p.y;
    if (k == 4) return p.w - p.z;
    return p.w + p.z;
}

Vu1Clip clip_lerp(Vu1Clip a, Vu1Clip b, float t)
{
    Vu1Clip o;
    o.pc = lerp(a.pc, b.pc, t);
    o.st = lerp(a.st, b.st, t);
    o.color = lerp(a.color, b.color, t);
    return o;
}

/* PC: the VU1 microcode's polygon clipper (0x57A..0x762, Sutherland-Hodgman
 * over the six planes, docs/models.md 3.3).  The GS gets STQ = ST x Q and
 * Gouraud colours / fog interpolated linearly on the screen (noperspective
 * here); a triangle reaching past the camera (w <= 0) has no meaningful
 * screen-space values, and Direct3D's own clipping of such already
 * projected attributes gave the road under the bumper camera garbage
 * texture coordinates.  As on the PS2 the clipping is done first, in clip
 * space on the raw ST and colours, and every vertex is projected after it.
 * A triangle inside all six planes is projected unchanged; one outside one
 * plane entirely is dropped (the microcode's ADC). */
[maxvertexcount(21)]
void gs_vu1(triangle Vu1Clip t[3], inout TriangleStream<PsIn> outStream)
{
    bool inside = true;
    [unroll] for (int k = 0; k < 6; k++) {
        float d0 = clip_dist(t[0].pc, k), d1 = clip_dist(t[1].pc, k), d2 = clip_dist(t[2].pc, k);
        if (d0 < 0 && d1 < 0 && d2 < 0)
            return;
        if (d0 < 0 || d1 < 0 || d2 < 0)
            inside = false;
    }
    if (inside) {
        outStream.Append(project_vu1(t[0]));
        outStream.Append(project_vu1(t[1]));
        outStream.Append(project_vu1(t[2]));
        return;
    }

    Vu1Clip poly[9], next[9];
    [unroll] for (int z = 0; z < 9; z++) {
        poly[z] = t[0];
        next[z] = t[0];
    }
    poly[1] = t[1];
    poly[2] = t[2];
    int n = 3;
    [unroll] for (int p = 0; p < 6; p++) {
        int m = 0;
        for (int i = 0; i < n; i++) {
            Vu1Clip a = poly[i];
            Vu1Clip b = poly[i + 1 < n ? i + 1 : 0];
            float da = clip_dist(a.pc, p), db = clip_dist(b.pc, p);
            if (da >= 0)
                next[m++] = a;
            if ((da >= 0) != (db >= 0))
                next[m++] = clip_lerp(a, b, da / (da - db));
        }
        n = m;
        for (int j = 0; j < n; j++)
            poly[j] = next[j];
        if (n < 3)
            return;
    }
    /* flat shading: the triangle's provoking (kicking) colour on every piece */
    if (flatShade != 0)
        for (int f = 0; f < n; f++)
            poly[f].color = t[0].color;
    for (int v = 1; v + 1 < n; v++) {
        outStream.Append(project_vu1(poly[0]));
        outStream.Append(project_vu1(poly[v]));
        outStream.Append(project_vu1(poly[v + 1]));
        outStream.RestartStrip();
    }
}
#endif

/* ---- Full-screen passes ------------------------------------------------- */
#ifdef SEC_FULL
FullOut vs_full(uint id : SV_VertexID)
{
    FullOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}
#endif

/* ---- The GS pixel pipeline ---------------------------------------------- */
#ifdef SEC_PS
Texture2D tex0 : register(t0);
Texture2D dstCopy : register(t1);           /* DATE, C = Ad: the target as the draw state found it */
SamplerState samp0 : register(s0);
cbuffer CbPs : register(b0) {
    float4 texRect;                 /* x0, y0, 1 / w, 1 / h (GS texels) */
    uint flags; float aref; float fix; float rowScale;
    float4 fogTa0;                  /* FOGCOL, TEXA.TA0 */
    float4 ta1;                     /* TEXA.TA1; y: PSF_BLEND_AD's selectors (A | B << 2 | D << 6) */
};
struct PsOut {
    float4 c0 : SV_Target0;
    float4 c1 : SV_Target1;
};
bool compare(float a, float ref, uint f)
{
    if (f == 0) return false;
    if (f == 1) return true;
    if (f == 2) return a < ref;
    if (f == 3) return a <= ref;
    if (f == 4) return a == ref;
    if (f == 5) return a >= ref;
    if (f == 6) return a > ref;
    return a != ref;
}
PsOut ps_body(float4 pos, float3 stq, float4 col, float fog)
{
    PsOut o;
    uint scan = (flags >> 18) & 3;
    if (scan >= 2) {
        uint row = (uint)floor(pos.y / rowScale);
        if ((row & 1) == (scan & 1))
            discard;
    }
    if (flags & (1u << 20)) {
        /* DATE: bit 7 of the destination alpha must equal DATM, else the
         * pixel is not written (colour nor depth) */
        bool set = round(dstCopy.Load(int3(pos.xy, 0)).a * 255.0) >= 128.0;
        if (set != ((flags & (1u << 21)) != 0))
            discard;
    }
    float4 c = floor(col + 0.001);
    if (flags & 1) {
        float2 tc = stq.xy / stq.z;
        float2 uv = (tc - texRect.xy) * texRect.zw;
        float4 t = round(tex0.SampleLevel(samp0, uv, 0) * 255.0);
        uint am = (flags >> 12) & 3;
        if (am == 1) t.a = fogTa0.w;
        else if (am == 2) t.a = t.a >= 128 ? ta1.x : fogTa0.w;
        uint tfx = (flags >> 1) & 3;
        float3 rgb;
        float a;
        if (tfx == 1) rgb = t.rgb;
        else rgb = floor(t.rgb * c.rgb / 128.0);
        if (tfx >= 2) rgb += c.a;
        if ((flags & 8) == 0) a = c.a;
        else if (tfx == 0) a = floor(t.a * c.a / 128.0);
        else if (tfx == 2) a = t.a + c.a;
        else a = t.a;
        c = min(float4(rgb, a), 255.0);
    }
    if (flags & 16) {
        float f = clamp(floor(fog), 0, 255);
        c.rgb = floor((f * c.rgb + (255.0 - f) * fogTa0.rgb) / 255.0);
    }
    c = clamp(c, 0, 255);
    uint gate = (flags >> 10) & 3;
    if (gate != 0) {
        bool ok = compare(c.a, aref, (flags >> 6) & 7);
        if (gate == 1 && !ok) discard;
        if (gate == 2 && ok) discard;
    }
    float cf = (flags & (1u << 17)) ? fix / 128.0 : c.a / 128.0;
    float aOut = c.a;
    if ((flags & (1u << 9)) && aOut < 128) aOut += 128;
    uint premul = (flags >> 14) & 7;
    float3 rgbOut = c.rgb / 255.0;
    if (premul == 1) rgbOut = saturate(c.rgb * (1.0 + min(cf, 1.0)) / 255.0);
    else if (premul == 2) rgbOut = saturate(c.rgb / 128.0);
    else if (premul == 3) rgbOut = min(cf, 1.0).xxx;
    else if (premul == 4) rgbOut = 0;
    else if (premul == 5) rgbOut = min(floor(c.rgb * cf), 255.0) / 255.0;   /* (Cs C) >> 7, C up to 2.0 */
    if (flags & (1u << 22)) {
        /* C = Ad (AdMode::Shader, blending off): the GS's ((A - B) Ad >> 7)
         * + D, clamped, with Cd and Ad from the copy of the target (t1) */
        float4 d = round(dstCopy.Load(int3(pos.xy, 0)) * 255.0);
        uint s = (uint)ta1.y;
        uint sa = s & 3, sb = (s >> 2) & 3, sd = (s >> 6) & 3;
        float3 A = sa == 0 ? c.rgb : (sa == 1 ? d.rgb : 0);
        float3 B = sb == 0 ? c.rgb : (sb == 1 ? d.rgb : 0);
        float3 D = sd == 0 ? c.rgb : (sd == 1 ? d.rgb : 0);
        rgbOut = clamp(floor((A - B) * d.a / 128.0) + D, 0, 255) / 255.0;
    }
    o.c0 = float4(rgbOut, aOut / 255.0);
    o.c1 = float4(0, 0, 0, saturate(cf));
    return o;
}
PsOut ps_main(PsIn i) { return ps_body(i.pos, i.stq, i.color, i.fog); }
PsOut ps_main_flat(PsInFlat i) { return ps_body(i.pos, i.stq, i.color, i.fog); }
/* A Z-format frame drawn from a depth texture (graphics.md 8.3). */
float ps_depth_copy(PsIn i) : SV_Depth
{
    float2 tc = i.stq.xy / i.stq.z;
    float2 uv = (tc - texRect.xy) * texRect.zw;
    return tex0.SampleLevel(samp0, uv, 0).r;
}
#endif

/* ---- Presentation ---------------------------------------------------------- */
#ifdef SEC_PRESENT
Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);
cbuffer CbPresent : register(b0) {
    float4 srcRect;                 /* u0, v0, u1, v1 */
    float4 taps;                    /* the step between taps (u, v), the taps a side (x, y; 0 / 1: one) */
};
float4 ps_present(FullOut i) : SV_Target
{
    float2 uv = lerp(srcRect.xy, srcRect.zw, i.uv);
    int nx = max(1, (int)taps.z), ny = max(1, (int)taps.w);
    if (nx == 1 && ny == 1)
        return float4(tex0.SampleLevel(samp0, uv, 0).rgb, 1);
    /* the screen pixel's part of a larger picture, averaged (supersampling) */
    float2 o = uv - taps.xy * (float2(nx, ny) - 1.0) * 0.5;
    float3 sum = 0;
    [loop] for (int y = 0; y < ny; y++)
        [loop] for (int x = 0; x < nx; x++)
            sum += tex0.SampleLevel(samp0, o + taps.xy * float2(x, y), 0).rgb;
    return float4(sum / (nx * ny), 1);
}
#endif

/* ---- pfx_color_correction (graphics.md 8.5) ----------------------------- */
#ifdef SEC_PFX
Texture2D tex0 : register(t0);
Texture2D curveTex : register(t1);
cbuffer CbPfx : register(b0) {
    float4 colR;                    /* the matrix column of the R input (bytes) */
    float4 colG;
    float4 colB;
    float4 srcScale;                /* source pixels per destination pixel */
};
float3 curve(float i) { return round(curveTex.Load(int3((int)i, 0, 0)).rgb * 255.0); }
float4 ps_pfx_color(FullOut i) : SV_Target
{
    int2 sp = int2(i.pos.xy * srcScale.xy);
    float3 s = round(tex0.Load(int3(sp, 0)).rgb * 255.0);
    /* three sprite passes, each clamped (R, then + B, then + G) */
    float3 p1 = min(floor(curve(s.r) * colR.rgb / 128.0), 255.0);
    float3 p2 = min(p1 + floor(curve(s.b) * colB.rgb / 128.0), 255.0);
    float3 p3 = min(p2 + floor(curve(s.g) * colG.rgb / 128.0), 255.0);
    return float4(p3 / 255.0, 0);
}
#endif

/* ---- A byte plane read through a palette (graphics.md 8.6) -------------- */
#ifdef SEC_PLANE
Texture2D capTex : register(t0);            /* the captured frame (GS bytes) */
Texture2D palTex : register(t1);            /* the palette, n x 1 */
cbuffer CbPlane : register(b0) {
    int4 sel;                               /* channel, shift, mask */
};
float4 ps_plane(FullOut i) : SV_Target
{
    float4 s = capTex.Load(int3(i.pos.xy, 0));
    float b = sel.x == 0 ? s.r : sel.x == 1 ? s.g : s.b;
    uint idx = ((uint)round(b * 255.0) >> (uint)sel.y) & (uint)sel.z;
    return palTex.Load(int3((int)idx, 0, 0));
}
#endif

/* ---- A lens-flare occlusion query (pgl_flare.cpp, docs/renderer.md 8) --- */
#ifdef SEC_FLARE
Texture2D zTex : register(t0);              /* the screen's depth target (Z x zNorm) */
cbuffer CbFlare : register(b0) {
    int4 box;                               /* the copy's origin x0, y0; columns, rows */
    int4 rect[5];                           /* the shape's rectangles, x1 / y1 exclusive */
    int4 misc;                              /* nRects, ztest, Cs, halfY */
    float4 zs;                              /* native per GS pixel x / y, the shape's Z, - */
    int4 scr;                               /* the screen's GS width, height */
    float4 wx4[17];                         /* each copied column's weight in the final texel */
    float4 wy4[17];                         /* each copied row's */
};
/* The shape's alpha at a screen pixel after its depth-tested draw: 255
 * where it passed, 0 where it did not or outside it (the cleared box). */
float shape_alpha(int gx, int gy)
{
    gx = clamp(gx, 0, scr.x - 1);
    gy = clamp(gy, 0, scr.y - 1);
    bool inside = false;
    for (int r = 0; r < misc.x; r++)
        inside = inside || (gx >= rect[r].x && gx < rect[r].z && gy >= rect[r].y && gy < rect[r].w);
    if (!inside)
        return 0;
    int fy = misc.w != 0 ? gy / 2 : gy;
    float sz = zTex.Load(int3(int((gx + 0.5) * zs.x), int((fy + 0.5) * zs.y), 0)).r;
    bool ok = misc.y == 1 || (misc.y == 2 && zs.z >= sz) || (misc.y == 3 && zs.z > sz);
    return ok ? 255 : 0;
}
/* PC: the query as the GS computed it, without its emulation.  On the PS2 the
 * box's alpha is cleared, the shape drawn depth-tested at alpha 2.0 (0xFF),
 * the box copied 1:1 into the work buffer, halved in place to one texel by
 * bilinear sprites, and Cs x As >> 7 stored into the result texel.  The
 * halvings are linear: the final alpha is a weighted sum of the copied
 * pixels, the weights (exec_flare_query, the GS's own sequence, odd sizes and
 * in-place reads included) per axis.  Here each pixel's alpha is the depth
 * test at its GS pixel centre in the native depth, whatever the output
 * scale.  RGB = (Cs A) >> 7, alpha A, the texel the game reads back. */
float4 ps_flare(FullOut i) : SV_Target
{
    float sum = 0;
    for (int y = 0; y < box.w; y++) {
        float wy = wy4[y >> 2][y & 3];
        if (wy == 0)
            continue;
        for (int x = 0; x < box.z; x++) {
            float wx = wx4[x >> 2][x & 3];
            if (wx != 0)
                sum += wx * wy * shape_alpha(box.x + x, box.y + y);
        }
    }
    uint a = (uint)clamp(floor(sum + 0.5), 0, 255);
    uint c = min((uint)misc.z * a >> 7, 255u);
    return float4(c, c, c, a) / 255.0;
}
#endif

/* ---- A multisampled depth target's resolve (docs/renderer.md "Anti-aliasing") */
#ifdef SEC_DRESOLVE
Texture2DMS<float> zMs : register(t0);
float ps_depth_resolve(FullOut i) : SV_Depth
{
    return zMs.Load(int2(i.pos.xy), 0);     /* sample 0 */
}
#endif

/* ---- A multisampled colour target's resolve where the scissor lets it (the
 * part written since its copy: target_copy_srv): the samples' mean, as
 * ResolveSubresource gives it */
#ifdef SEC_CRESOLVE
Texture2DMS<float4> cMs : register(t0);
float4 ps_color_resolve(FullOut i) : SV_Target
{
    uint w, h, n;
    cMs.GetDimensions(w, h, n);
    float4 s = 0;
    for (uint k = 0; k < n; k++)
        s += cMs.Load(int2(i.pos.xy), k);
    return s / (float)n;
}
#endif

/* ---- FXAA over a race view's 3D picture (docs/renderer.md "Anti-aliasing") --
 * FXAA 3.11's quality algorithm (Timothy Lottes, NVIDIA): luma edges found
 * from the 3 x 3 neighbourhood, the edge's span searched both ways along it
 * (steps 1, 1.5, 2, 2, 2, 2, 4, 8 pixels), the pixel resampled across the
 * edge by its place in the span; sub-pixel aliasing blended by 1/2 (FXAA's
 * default 3/4 softens textures more).  Luma from the colour (the alpha byte
 * is the GS's own: kept). */
#ifdef SEC_FXAA
Texture2D tex0 : register(t0);              /* a copy of the picture */
SamplerState samp0 : register(s0);          /* linear, clamped */
cbuffer CbFxaa : register(b0) {
    float4 rcpFrame;                        /* 1 / width, 1 / height (native pixels) */
};
static const float3 kLuma = float3(0.299, 0.587, 0.114);
float fxaa_luma(float2 uv) { return dot(tex0.SampleLevel(samp0, uv, 0).rgb, kLuma); }
float4 ps_fxaa(FullOut i) : SV_Target
{
    const float subpix = 0.5, edgeThreshold = 0.125, edgeThresholdMin = 0.0625;
    float2 posM = i.uv;
    float4 rgbaM = tex0.SampleLevel(samp0, posM, 0);
    float lumaM = dot(rgbaM.rgb, kLuma);
    float lumaS = dot(tex0.SampleLevel(samp0, posM, 0, int2(0, 1)).rgb, kLuma);
    float lumaE = dot(tex0.SampleLevel(samp0, posM, 0, int2(1, 0)).rgb, kLuma);
    float lumaN = dot(tex0.SampleLevel(samp0, posM, 0, int2(0, -1)).rgb, kLuma);
    float lumaW = dot(tex0.SampleLevel(samp0, posM, 0, int2(-1, 0)).rgb, kLuma);
    float rangeMax = max(max(lumaN, lumaW), max(lumaE, max(lumaS, lumaM)));
    float rangeMin = min(min(lumaN, lumaW), min(lumaE, min(lumaS, lumaM)));
    float range = rangeMax - rangeMin;
    if (range < max(edgeThresholdMin, rangeMax * edgeThreshold))
        return rgbaM;
    float lumaNW = dot(tex0.SampleLevel(samp0, posM, 0, int2(-1, -1)).rgb, kLuma);
    float lumaSE = dot(tex0.SampleLevel(samp0, posM, 0, int2(1, 1)).rgb, kLuma);
    float lumaNE = dot(tex0.SampleLevel(samp0, posM, 0, int2(1, -1)).rgb, kLuma);
    float lumaSW = dot(tex0.SampleLevel(samp0, posM, 0, int2(-1, 1)).rgb, kLuma);
    float lumaNS = lumaN + lumaS, lumaWE = lumaW + lumaE;
    float subpixRcpRange = 1.0 / range;
    float subpixNSWE = lumaNS + lumaWE;
    float edgeHorz1 = -2.0 * lumaM + lumaNS, edgeVert1 = -2.0 * lumaM + lumaWE;
    float lumaNESE = lumaNE + lumaSE, lumaNWNE = lumaNW + lumaNE;
    float edgeHorz2 = -2.0 * lumaE + lumaNESE, edgeVert2 = -2.0 * lumaN + lumaNWNE;
    float lumaNWSW = lumaNW + lumaSW, lumaSWSE = lumaSW + lumaSE;
    float edgeHorz4 = abs(edgeHorz1) * 2.0 + abs(edgeHorz2);
    float edgeVert4 = abs(edgeVert1) * 2.0 + abs(edgeVert2);
    float edgeHorz3 = -2.0 * lumaW + lumaNWSW, edgeVert3 = -2.0 * lumaS + lumaSWSE;
    float edgeHorz = abs(edgeHorz3) + edgeHorz4, edgeVert = abs(edgeVert3) + edgeVert4;
    float subpixNWSWNESE = lumaNWSW + lumaNESE;
    float lengthSign = rcpFrame.x;
    bool horzSpan = edgeHorz >= edgeVert;
    float subpixA = subpixNSWE * 2.0 + subpixNWSWNESE;
    if (!horzSpan) lumaN = lumaW;
    if (!horzSpan) lumaS = lumaE;
    if (horzSpan) lengthSign = rcpFrame.y;
    float subpixB = subpixA * (1.0 / 12.0) - lumaM;
    float gradientN = lumaN - lumaM, gradientS = lumaS - lumaM;
    float lumaNN = lumaN + lumaM, lumaSS = lumaS + lumaM;
    bool pairN = abs(gradientN) >= abs(gradientS);
    float gradient = max(abs(gradientN), abs(gradientS));
    if (pairN) lengthSign = -lengthSign;
    float subpixC = saturate(abs(subpixB) * subpixRcpRange);
    float2 posB = posM;
    float2 offNP = horzSpan ? float2(rcpFrame.x, 0.0) : float2(0.0, rcpFrame.y);
    if (!horzSpan) posB.x += lengthSign * 0.5;
    if (horzSpan) posB.y += lengthSign * 0.5;
    float2 posN = posB - offNP, posP = posB + offNP;
    float subpixD = -2.0 * subpixC + 3.0;
    float subpixE = subpixC * subpixC;
    if (!pairN) lumaNN = lumaSS;
    float gradientScaled = gradient * 0.25;
    float lumaMM = lumaM - lumaNN * 0.5;
    float subpixF = subpixD * subpixE;
    bool lumaMLTZero = lumaMM < 0.0;
    float lumaEndN = fxaa_luma(posN) - lumaNN * 0.5;
    float lumaEndP = fxaa_luma(posP) - lumaNN * 0.5;
    bool doneN = abs(lumaEndN) >= gradientScaled;
    bool doneP = abs(lumaEndP) >= gradientScaled;
    const float steps[7] = {1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0};
    [loop] for (int s = 0; s < 7 && !(doneN && doneP); s++) {
        if (!doneN) {
            posN -= offNP * steps[s];
            lumaEndN = fxaa_luma(posN) - lumaNN * 0.5;
            doneN = abs(lumaEndN) >= gradientScaled;
        }
        if (!doneP) {
            posP += offNP * steps[s];
            lumaEndP = fxaa_luma(posP) - lumaNN * 0.5;
            doneP = abs(lumaEndP) >= gradientScaled;
        }
    }
    float dstN = horzSpan ? posM.x - posN.x : posM.y - posN.y;
    float dstP = horzSpan ? posP.x - posM.x : posP.y - posM.y;
    bool goodSpanN = (lumaEndN < 0.0) != lumaMLTZero;
    bool goodSpanP = (lumaEndP < 0.0) != lumaMLTZero;
    float spanLength = dstP + dstN;
    bool directionN = dstN < dstP;
    float dst = min(dstN, dstP);
    bool goodSpan = directionN ? goodSpanN : goodSpanP;
    float subpixG = subpixF * subpixF;
    float pixelOffset = dst * (-1.0 / spanLength) + 0.5;
    float subpixH = subpixG * subpix;
    float pixelOffsetGood = goodSpan ? pixelOffset : 0.0;
    float pixelOffsetSubpix = max(pixelOffsetGood, subpixH);
    if (!horzSpan) posM.x += pixelOffsetSubpix * lengthSign;
    if (horzSpan) posM.y += pixelOffsetSubpix * lengthSign;
    return float4(tex0.SampleLevel(samp0, posM, 0).rgb, rgbaM.a);
}
#endif
)HLSL";

struct Shaders {
    Com<ID3D11VertexShader> vsGs, vsVu1, vsFull;
    Com<ID3D11GeometryShader> gsVu1;        /* the VU1 clipper */
    Com<ID3D11PixelShader> psMain, psMainFlat, psDepthCopy, psPresent, psPfxColor, psPlane, psFlare;
    Com<ID3D11PixelShader> psDepthResolve, psFxaa, psColorResolve;
    Com<ID3D11InputLayout> ilGs, ilVu1;
    Com<ID3D11Buffer> cbVs, cbPs;
    Com<ID3D11Buffer> cbFlare;              /* ps_flare's (larger than cbPs) */
    size_t cbVsSize = 0, cbPsSize = 0;
} g_sh;

Vs g_curVs = Vs::Full;
bool g_vsBound = false;

/* What is bound (d3d_invalidate forgets it). */
struct Bound {
    bool valid = false;
    ID3D11PixelShader *ps = nullptr;
    ID3D11RenderTargetView *rtv = nullptr;
    ID3D11DepthStencilView *dsv = nullptr;
    int vpW = 0, vpH = 0;
    D3D11_RECT scissor = {-1, -1, -1, -1};
    ID3D11RasterizerState *raster = nullptr;
    ID3D11BlendState *blend = nullptr;
    ID3D11DepthStencilState *depth = nullptr;
    ID3D11ShaderResourceView *srv0 = nullptr;
    ID3D11ShaderResourceView *srv1 = nullptr;
    ID3D11SamplerState *samp0 = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11Buffer *vb = nullptr;
    UINT vbStride = 0;
    ID3D11Buffer *ib = nullptr;
    uint8_t vsCb[sizeof(CbVu1)];
    size_t vsCbBytes = 0;
    uint8_t psCb[256];
    size_t psCbBytes = 0;
} g_bound;

/* The constants' ring (see the file comment); vs / ps: where the bound
 * constants lie in it (first constant, count: 16 bytes each). */
struct CbRing {
    Com<ID3D11Buffer> buf;
    Com<ID3D11DeviceContext1> dc1;
    uint32_t size = 0, pos = 0;
    UINT vsFirst = 0, vsNum = 0, psFirst = 0, psNum = 0;
    bool on = false;
};
CbRing g_ring;
constexpr uint32_t kRingBytes = 4u << 20;

Com<ID3DBlob> compile(const char *section, const char *entry, const char *target)
{
    Com<ID3DBlob> code, errors;
    D3D_SHADER_MACRO defs[] = {{section, "1"}, {nullptr, nullptr}};
    HRESULT hr = D3DCompile(kHlsl, strlen(kHlsl), "pgl_shaders.hlsl", defs, nullptr, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.put(), errors.put());
    if (FAILED(hr))
        pclog::fatal("pgl: shader %s does not compile:\n%s", entry,
                     errors ? (const char *)errors->GetBufferPointer() : "?");
    return code;
}

Com<ID3D11Buffer> make_cb(size_t bytes)
{
    D3D11_BUFFER_DESC d = {};
    d.ByteWidth = (UINT)((bytes + 15) & ~(size_t)15);
    d.Usage = D3D11_USAGE_DYNAMIC;
    d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    Com<ID3D11Buffer> b;
    dev()->CreateBuffer(&d, nullptr, b.put());
    return b;
}

void update_cb(ID3D11Buffer *b, const void *data, size_t bytes)
{
    /* (queued: the D3D thread) */
    q::run_with(data, bytes, [b, bytes](const uint8_t *p) {
        D3D11_MAPPED_SUBRESOURCE m;
        const HRESULT hr = qdc()->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
        if (SUCCEEDED(hr)) {
            memcpy(m.pData, p, bytes);
            qdc()->Unmap(b, 0);
        } else {
            log_once("pgl-cb-map", "pgl: a constant buffer could not be mapped (0x%08lX): the draw used its old"
                     " contents", (unsigned long)hr);
        }
    });
}

/* GT3PC_FLICKER: the CPU's copy of what the streams hold (stream_put),
 * for d3d_verify_copy. */
std::vector<uint8_t> g_vshadow, g_ishadow;

/* State caches */
std::unordered_map<uint64_t, Com<ID3D11BlendState>> g_blend;
std::unordered_map<uint32_t, Com<ID3D11DepthStencilState>> g_depth;
std::unordered_map<uint32_t, Com<ID3D11RasterizerState>> g_raster;
std::unordered_map<uint32_t, Com<ID3D11SamplerState>> g_samplers;

/* Dynamic streams: one buffer each, written forward with NO_OVERWRITE and
 * restarted with DISCARD when full. */
struct Stream {
    Com<ID3D11Buffer> buf;
    uint32_t size = 0;
    uint32_t pos = 0;
    bool index = false;
};
Stream g_vstream, g_istream;

void stream_grow(Stream &s, uint32_t bytes)
{
    uint32_t size = s.size ? s.size : (s.index ? (1u << 20) : (4u << 20));
    while (size < bytes)
        size *= 2;
    D3D11_BUFFER_DESC d = {};
    d.ByteWidth = size;
    d.Usage = D3D11_USAGE_DYNAMIC;
    d.BindFlags = s.index ? D3D11_BIND_INDEX_BUFFER : D3D11_BIND_VERTEX_BUFFER;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    s.buf.reset();
    dev()->CreateBuffer(&d, nullptr, s.buf.put());
    s.size = size;
    s.pos = 0;
}

uint32_t stream_put(Stream &s, const void *data, uint32_t bytes, uint32_t align)
{
    uint32_t at = (s.pos + align - 1) / align * align;
    bool discard = false;
    if (!s.buf || bytes > s.size) {
        stream_grow(s, bytes);
        at = 0;
        discard = true;
        if (flicker_on())
            flicker_event("the %s stream made: %u bytes", s.index ? "index" : "vertex", s.size);
    } else if (at + bytes > s.size) {
        if (flicker_on())
            flicker_event("the %s stream restarted (DISCARD) at %u of %u bytes, for %u", s.index ? "index" : "vertex",
                          at, s.size, bytes);
        at = 0;
        discard = true;
    }
    /* (queued: the D3D thread maps the stream and copies the data along) */
    const D3D11_MAP mode = discard ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE;
    ID3D11Buffer *buf = s.buf.get();
    const bool index = s.index;
    q::run_with(data, bytes, [buf, mode, at, bytes, index](const uint8_t *p) {
        D3D11_MAPPED_SUBRESOURCE m;
        const HRESULT hr = qdc()->Map(buf, 0, mode, 0, &m);
        if (FAILED(hr)) {
            log_once("pgl-stream-map", "pgl: a %s stream could not be mapped (0x%08lX)", index ? "index" : "vertex",
                     (unsigned long)hr);
            return;
        }
        memcpy((uint8_t *)m.pData + at, p, bytes);
        qdc()->Unmap(buf, 0);
    });
    if (flicker_on()) {
        std::vector<uint8_t> &sh = s.index ? g_ishadow : g_vshadow;
        if (sh.size() != s.size)
            sh.assign(s.size, 0);
        memcpy(sh.data() + at, data, bytes);
    }
    s.pos = at + bytes;
    return at;
}

}  /* namespace */

void d3d_init()
{
    Com<ID3DBlob> vsGs = compile("SEC_GS", "vs_gs", "vs_4_0");
    Com<ID3DBlob> vsVu1 = compile("SEC_VU1", "vs_vu1", "vs_4_0");
    Com<ID3DBlob> vsFull = compile("SEC_FULL", "vs_full", "vs_4_0");
    Com<ID3DBlob> gsVu1 = compile("SEC_VU1", "gs_vu1", "gs_4_0");
    dev()->CreateVertexShader(vsGs->GetBufferPointer(), vsGs->GetBufferSize(), nullptr, g_sh.vsGs.put());
    dev()->CreateVertexShader(vsVu1->GetBufferPointer(), vsVu1->GetBufferSize(), nullptr, g_sh.vsVu1.put());
    dev()->CreateVertexShader(vsFull->GetBufferPointer(), vsFull->GetBufferSize(), nullptr, g_sh.vsFull.put());
    dev()->CreateGeometryShader(gsVu1->GetBufferPointer(), gsVu1->GetBufferSize(), nullptr, g_sh.gsVu1.put());
    struct { const char *section; const char *entry; Com<ID3D11PixelShader> *out; const char *target; } ps[] = {
        {"SEC_PS", "ps_main", &g_sh.psMain, "ps_4_0"},
        {"SEC_PS", "ps_main_flat", &g_sh.psMainFlat, "ps_4_0"},
        {"SEC_PS", "ps_depth_copy", &g_sh.psDepthCopy, "ps_4_0"},
        {"SEC_PRESENT", "ps_present", &g_sh.psPresent, "ps_4_0"},
        {"SEC_PFX", "ps_pfx_color", &g_sh.psPfxColor, "ps_4_0"},
        {"SEC_PLANE", "ps_plane", &g_sh.psPlane, "ps_4_0"},
        {"SEC_FLARE", "ps_flare", &g_sh.psFlare, "ps_4_0"},
        {"SEC_DRESOLVE", "ps_depth_resolve", &g_sh.psDepthResolve, "ps_4_1"},    /* (a Texture2DMS of any count) */
        {"SEC_FXAA", "ps_fxaa", &g_sh.psFxaa, "ps_4_0"},
        {"SEC_CRESOLVE", "ps_color_resolve", &g_sh.psColorResolve, "ps_4_1"},    /* (a Texture2DMS of any count) */
    };
    for (auto &p : ps) {
        Com<ID3DBlob> b = compile(p.section, p.entry, p.target);
        dev()->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, p.out->put());
    }
    D3D11_INPUT_ELEMENT_DESC gsLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"FOG", 0, DXGI_FORMAT_R32_FLOAT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    dev()->CreateInputLayout(gsLayout, 4, vsGs->GetBufferPointer(), vsGs->GetBufferSize(), g_sh.ilGs.put());
    D3D11_INPUT_ELEMENT_DESC vu1Layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"POSITION", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, 36, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 56, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, 64, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 1, DXGI_FORMAT_R8G8B8A8_UINT, 0, 68, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    dev()->CreateInputLayout(vu1Layout, 8, vsVu1->GetBufferPointer(), vsVu1->GetBufferSize(), g_sh.ilVu1.put());
    g_sh.cbVsSize = sizeof(CbVu1);
    g_sh.cbPsSize = 256;
    g_sh.cbVs = make_cb(g_sh.cbVsSize);
    g_sh.cbPs = make_cb(g_sh.cbPsSize);
    g_sh.cbFlare = make_cb(sizeof(CbFlare));
    static_assert(sizeof(CbFlare) % 16 == 0, "CbFlare");
    static_assert(sizeof(GsVtx) == 32, "GsVtx");
    static_assert(sizeof(gt3vif::Vertex) == 72, "Vertex");
    static_assert(sizeof(CbVu1) % 16 == 0 && sizeof(CbPs) % 16 == 0, "constant buffers");
    if (!g_sh.ilGs || !g_sh.ilVu1 || !g_sh.cbVs || !g_sh.cbPs || !g_sh.gsVu1)
        pclog::fatal("pgl: the renderer's Direct3D 11 objects could not be created");

    /* PC: the constants' ring, where the driver takes constant buffer
     * offsets and NO_OVERWRITE maps of constant buffers (Direct3D 11.1) */
    D3D11_FEATURE_DATA_D3D11_OPTIONS opt = {};
    if (SUCCEEDED(dev()->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &opt, sizeof(opt))) &&
        opt.ConstantBufferOffsetting && opt.MapNoOverwriteOnDynamicConstantBuffer &&
        SUCCEEDED(dc()->QueryInterface(__uuidof(ID3D11DeviceContext1),
                                       reinterpret_cast<void **>(g_ring.dc1.put())))) {
        g_ring.buf = make_cb(kRingBytes);
        g_ring.size = kRingBytes;
        g_ring.pos = kRingBytes;            /* (the first write discards) */
        g_ring.on = (bool)g_ring.buf;
    }
    pclog::write("pgl: shader constants %s", g_ring.on ? "in a 4 MB ring (Direct3D 11.1 constant buffer offsets)"
                                                       : "in their own buffers (no constant buffer offsets)");
    g_qdc = dc();
}

/* (Below: the binding cache and the ring's positions are the executor's -
 * what has been queued; the calls themselves are queued for the D3D thread,
 * pgl_queue.h, with the values they need.) */
namespace {

/* The ring's bindings: the VS constants (and the VU1 clipper's, the same:
 * gs) and the PS constants at their offsets (queued). */
void ring_bind_vs()
{
    ID3D11Buffer *b = g_ring.buf.get();
    ID3D11DeviceContext1 *c1 = g_ring.dc1.get();
    const UINT first = g_ring.vsFirst, num = g_ring.vsNum;
    const bool gs = g_curVs == Vs::Vu1;
    q::run([b, c1, first, num, gs] {
        c1->VSSetConstantBuffers1(0, 1, &b, &first, &num);
        if (gs)
            c1->GSSetConstantBuffers1(0, 1, &b, &first, &num);
    });
}

void ring_bind_ps()
{
    ID3D11Buffer *b = g_ring.buf.get();
    ID3D11DeviceContext1 *c1 = g_ring.dc1.get();
    const UINT first = g_ring.psFirst, num = g_ring.psNum;
    q::run([b, c1, first, num] { c1->PSSetConstantBuffers1(0, 1, &b, &first, &num); });
}

/* bytes of data, a stage's constants (vs, else ps), into the ring's next
 * slots and bound there.  At a wrap the buffer is discarded: the other
 * stage's bound constants are written into it again first and bound anew
 * (a draw may still use them). */
bool ring_write(bool vs, const void *data, size_t bytes)
{
    const uint32_t need = (uint32_t)((bytes + 255) & ~(size_t)255);
    const uint32_t other = vs ? (uint32_t)g_bound.psCbBytes : (g_vsBound ? (uint32_t)g_bound.vsCbBytes : 0u);
    const uint32_t otherNeed = (other + 255) & ~255u;
    const bool wrap = g_ring.pos + need > g_ring.size;
    const uint32_t keep = wrap ? other : 0;  /* the other stage's constants written again */
    uint32_t at = g_ring.pos;
    if (wrap) {
        at = 0;
        if (keep != 0) {
            (vs ? g_ring.psFirst : g_ring.vsFirst) = 0;
            (vs ? g_ring.psNum : g_ring.vsNum) = otherNeed / 16;
            at = otherNeed;
        }
    }
    (vs ? g_ring.vsFirst : g_ring.psFirst) = at / 16;
    (vs ? g_ring.vsNum : g_ring.psNum) = need / 16;
    g_ring.pos = at + need;
    /* the data along: the kept constants first, then the new */
    uint8_t blob[2 * 512];
    if (keep != 0)
        memcpy(blob, vs ? g_bound.psCb : g_bound.vsCb, keep);
    memcpy(blob + keep, data, bytes);
    ID3D11Buffer *buf = g_ring.buf.get();
    const uint32_t n = (uint32_t)bytes;
    q::run_with(blob, keep + bytes, [buf, wrap, keep, at, n](const uint8_t *p) {
        D3D11_MAPPED_SUBRESOURCE m;
        const HRESULT hr = qdc()->Map(buf, 0, wrap ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0, &m);
        if (FAILED(hr)) {
            log_once("pgl-ring-map", "pgl: the constants' ring could not be mapped (0x%08lX): draws used old"
                     " constants", (unsigned long)hr);
            return;
        }
        uint8_t *dst = static_cast<uint8_t *>(m.pData);
        if (keep != 0)
            memcpy(dst, p, keep);
        memcpy(dst + at, p + keep, n);
        qdc()->Unmap(buf, 0);
    });
    if (vs || keep != 0)
        ring_bind_vs();
    if (!vs || keep != 0)
        ring_bind_ps();
    return true;
}

}  /* namespace */

void d3d_invalidate()
{
    /* the VU1 clipper only in front of vs_vu1: the other modules' draws run
     * without a geometry stage */
    q::run([] { qdc()->GSSetShader(nullptr, nullptr, 0); });
    g_vsBound = false;
    g_bound = Bound();
}

void d3d_forget()
{
    g_vsBound = false;
    g_bound = Bound();
}

void set_targets(ID3D11RenderTargetView *rtv, ID3D11DepthStencilView *dsv, int w, int h)
{
    if (!g_bound.valid || rtv != g_bound.rtv || dsv != g_bound.dsv) {
        q::run([rtv, dsv] {
            ID3D11ShaderResourceView *none = nullptr;
            qdc()->PSSetShaderResources(0, 1, &none);
            qdc()->OMSetRenderTargets(rtv ? 1 : 0, rtv ? &rtv : nullptr, dsv);
        });
        g_bound.srv0 = nullptr;
        g_bound.rtv = rtv;
        g_bound.dsv = dsv;
        g_bound.valid = true;
    }
    if (w != g_bound.vpW || h != g_bound.vpH) {
        const D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1};
        q::run([vp] { qdc()->RSSetViewports(1, &vp); });
        g_bound.vpW = w;
        g_bound.vpH = h;
    }
}

void set_scissor(const D3D11_RECT &r)
{
    if (memcmp(&r, &g_bound.scissor, sizeof(r)) != 0) {
        const D3D11_RECT c = r;
        q::run([c] { qdc()->RSSetScissorRects(1, &c); });
        g_bound.scissor = r;
    }
}

void set_raster(ID3D11RasterizerState *s)
{
    if (s != g_bound.raster) {
        q::run([s] { qdc()->RSSetState(s); });
        g_bound.raster = s;
    }
}

void set_blend(ID3D11BlendState *s)
{
    if (s != g_bound.blend) {
        q::run([s] { qdc()->OMSetBlendState(s, nullptr, 0xFFFFFFFFu); });
        g_bound.blend = s;
    }
}

void set_depth(ID3D11DepthStencilState *s)
{
    if (s != g_bound.depth) {
        q::run([s] { qdc()->OMSetDepthStencilState(s, 0); });
        g_bound.depth = s;
    }
}

void set_srv0(ID3D11ShaderResourceView *srv)
{
    if (srv != g_bound.srv0) {
        q::run([srv] { qdc()->PSSetShaderResources(0, 1, &srv); });
        g_bound.srv0 = srv;
    }
}

void set_srv1(ID3D11ShaderResourceView *srv)
{
    if (srv != g_bound.srv1) {
        q::run([srv] { qdc()->PSSetShaderResources(1, 1, &srv); });
        g_bound.srv1 = srv;
    }
}

void set_sampler0(ID3D11SamplerState *s)
{
    if (s != g_bound.samp0) {
        q::run([s] { qdc()->PSSetSamplers(0, 1, &s); });
        g_bound.samp0 = s;
    }
}

void set_topology(D3D11_PRIMITIVE_TOPOLOGY t)
{
    if (t != g_bound.topo) {
        q::run([t] { qdc()->IASetPrimitiveTopology(t); });
        g_bound.topo = t;
    }
}

void set_vb(ID3D11Buffer *vb, UINT stride)
{
    if (vb != g_bound.vb || stride != g_bound.vbStride) {
        q::run([vb, stride] {
            const UINT offset = 0;
            qdc()->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        });
        g_bound.vb = vb;
        g_bound.vbStride = stride;
    }
}

void set_ib(ID3D11Buffer *ib)
{
    if (ib != g_bound.ib) {
        q::run([ib] { qdc()->IASetIndexBuffer(ib, DXGI_FORMAT_R32_UINT, 0); });
        g_bound.ib = ib;
    }
}

uint32_t d3d_check_bound(bool texture, bool date, bool indexed, char *what, size_t n)
{
    uint32_t bits = 0;
    size_t used = 0;
    if (n > 0)
        what[0] = 0;
    auto note = [&](uint32_t bit, const char *name, const void *actual, const void *cached) {
        bits |= bit;
        if (used < n) {
            const int k = snprintf(what + used, n - used, "%s%s %p (cached %p)", used ? ", " : "", name, actual,
                                   cached);
            used = k > 0 ? std::min(n - 1, used + (size_t)k) : used;
        }
    };
    auto release = [](IUnknown *p) {
        if (p != nullptr)
            p->Release();
    };
    ID3D11RenderTargetView *rtv = nullptr;
    ID3D11DepthStencilView *dsv = nullptr;
    dc()->OMGetRenderTargets(1, &rtv, &dsv);
    if (rtv != g_bound.rtv)
        note(D3DB_RTV, "render target", rtv, g_bound.rtv);
    if (dsv != g_bound.dsv)
        note(D3DB_DSV, "depth target", dsv, g_bound.dsv);
    release(rtv);
    release(dsv);
    UINT nv = 1;
    D3D11_VIEWPORT vp = {};
    dc()->RSGetViewports(&nv, &vp);
    if (nv < 1 || (int)vp.Width != g_bound.vpW || (int)vp.Height != g_bound.vpH || vp.TopLeftX != 0 ||
        vp.TopLeftY != 0)
        note(D3DB_VIEWPORT, "viewport", (const void *)(uintptr_t)(int)vp.Width, (const void *)(uintptr_t)g_bound.vpW);
    UINT nr = 1;
    D3D11_RECT sr = {};
    dc()->RSGetScissorRects(&nr, &sr);
    if (nr < 1 || memcmp(&sr, &g_bound.scissor, sizeof(sr)) != 0)
        note(D3DB_SCISSOR, "scissor (left)", (const void *)(uintptr_t)sr.left,
             (const void *)(uintptr_t)g_bound.scissor.left);
    ID3D11RasterizerState *rs = nullptr;
    dc()->RSGetState(&rs);
    if (rs != g_bound.raster)
        note(D3DB_RASTER, "raster state", rs, g_bound.raster);
    release(rs);
    ID3D11BlendState *bs = nullptr;
    FLOAT factor[4];
    UINT mask = 0;
    dc()->OMGetBlendState(&bs, factor, &mask);
    if (bs != g_bound.blend || mask != 0xFFFFFFFFu)
        note(D3DB_BLEND, "blend state", bs, g_bound.blend);
    release(bs);
    ID3D11DepthStencilState *ds = nullptr;
    UINT ref = 0;
    dc()->OMGetDepthStencilState(&ds, &ref);
    if (ds != g_bound.depth)
        note(D3DB_DEPTH, "depth state", ds, g_bound.depth);
    release(ds);
    ID3D11PixelShader *ps = nullptr;
    dc()->PSGetShader(&ps, nullptr, nullptr);
    if (ps != g_bound.ps)
        note(D3DB_PS, "pixel shader", ps, g_bound.ps);
    release(ps);
    if (g_vsBound) {
        ID3D11VertexShader *vs = nullptr;
        dc()->VSGetShader(&vs, nullptr, nullptr);
        ID3D11VertexShader *want = g_curVs == Vs::Gs ? g_sh.vsGs.get() : g_curVs == Vs::Vu1 ? g_sh.vsVu1.get()
                                                                                            : g_sh.vsFull.get();
        if (vs != want)
            note(D3DB_VS, "vertex shader", vs, want);
        release(vs);
        ID3D11Buffer *cb = nullptr;
        ID3D11Buffer *wantCb = g_ring.on ? g_ring.buf.get() : g_sh.cbVs.get();
        dc()->VSGetConstantBuffers(0, 1, &cb);
        if (cb != wantCb)
            note(D3DB_VSCB, "VS constants", cb, wantCb);
        release(cb);
    }
    if (g_bound.psCbBytes != 0) {
        ID3D11Buffer *cb = nullptr;
        ID3D11Buffer *want = g_ring.on ? g_ring.buf.get() : g_sh.cbPs.get();
        dc()->PSGetConstantBuffers(0, 1, &cb);
        if (cb != want)
            note(D3DB_PSCB, "PS constants", cb, want);
        release(cb);
    }
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    dc()->IAGetPrimitiveTopology(&topo);
    if (topo != g_bound.topo)
        note(D3DB_TOPO, "topology", (const void *)(uintptr_t)topo, (const void *)(uintptr_t)g_bound.topo);
    ID3D11Buffer *vb = nullptr;
    UINT stride = 0, offset = 0;
    dc()->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
    if (vb != g_bound.vb || stride != g_bound.vbStride || offset != 0)
        note(D3DB_VB, "vertex buffer", vb, g_bound.vb);
    release(vb);
    if (indexed) {
        ID3D11Buffer *ib = nullptr;
        DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
        dc()->IAGetIndexBuffer(&ib, &fmt, &offset);
        if (ib != g_bound.ib || fmt != DXGI_FORMAT_R32_UINT || offset != 0)
            note(D3DB_IB, "index buffer", ib, g_bound.ib);
        release(ib);
    }
    if (texture) {
        ID3D11ShaderResourceView *srv = nullptr;
        dc()->PSGetShaderResources(0, 1, &srv);
        if (srv != g_bound.srv0)
            note(D3DB_SRV0, "texture 0", srv, g_bound.srv0);
        release(srv);
        ID3D11SamplerState *smp = nullptr;
        dc()->PSGetSamplers(0, 1, &smp);
        if (smp != g_bound.samp0)
            note(D3DB_SAMP0, "sampler 0", smp, g_bound.samp0);
        release(smp);
    }
    if (date) {
        ID3D11ShaderResourceView *srv = nullptr;
        dc()->PSGetShaderResources(1, 1, &srv);
        if (srv != g_bound.srv1)
            note(D3DB_SRV1, "texture 1", srv, g_bound.srv1);
        release(srv);
    }
    return bits;
}

/* GT3PC_FLICKER: what the GPU holds for a draw - its constant buffers and
 * its vertex / index ranges, copied into staging memory right after it -
 * against what was written (the binding cache's copy of the constants, the
 * streams' CPU shadows).  A set per frame in flight, a slot per draw. */
namespace {

constexpr uint32_t kVerifyVb = 2048, kVerifyIb = 512, kVerifySlots = 512;

uint32_t verify_hash(const void *p, size_t n)
{
    const uint8_t *s = (const uint8_t *)p;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++)
        h = (h ^ s[i]) * 16777619u;
    return h;
}

struct VerifySlot {
    Com<ID3D11Buffer> vsCb, psCb;           /* staging, the constant buffers' sizes */
    uint32_t vsBytes = 0, psBytes = 0, vsHash = 0, psHash = 0;
    uint32_t vbBytes = 0, vbHash = 0, ibBytes = 0, ibHash = 0;
};
struct VerifySet {
    std::vector<VerifySlot> slots;
    Com<ID3D11Buffer> vb, ib;               /* staging: kVerifyVb / kVerifyIb bytes a slot */
    bool failed = false;
};
VerifySet g_verify[4];

Com<ID3D11Buffer> staging_like(ID3D11Buffer *src, UINT bytes)
{
    D3D11_BUFFER_DESC d = {};
    if (src != nullptr)
        src->GetDesc(&d);
    d.ByteWidth = src != nullptr ? d.ByteWidth : bytes;
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    d.StructureByteStride = 0;
    Com<ID3D11Buffer> b;
    dev()->CreateBuffer(&d, nullptr, b.put());
    return b;
}

}  /* namespace */

void d3d_verify_copy(int set, uint32_t slot, bool indexed, uint32_t first, uint32_t count, int32_t baseVertex)
{
    VerifySet &vs = g_verify[set & 3];
    if (vs.failed || slot >= kVerifySlots)
        return;
    if (!vs.vb) {
        vs.vb = staging_like(nullptr, kVerifyVb * kVerifySlots);
        vs.ib = staging_like(nullptr, kVerifyIb * kVerifySlots);
        if (!vs.vb || !vs.ib) {
            vs.failed = true;
            return;
        }
    }
    if (vs.slots.size() <= slot)
        vs.slots.resize(slot + 1);
    VerifySlot &s = vs.slots[slot];
    if (!s.vsCb) {
        s.vsCb = staging_like(g_sh.cbVs.get(), 0);
        s.psCb = staging_like(g_sh.cbPs.get(), 0);
    }
    s.vsBytes = s.psBytes = s.vbBytes = s.ibBytes = 0;
    /* (the constants: their buffer, or their slots of the ring) */
    auto copy_cb = [](ID3D11Buffer *dst, ID3D11Buffer *own, UINT first, size_t bytes) {
        if (!g_ring.on) {
            dc()->CopyResource(dst, own);
            return;
        }
        const D3D11_BOX box = {first * 16, 0, 0, first * 16 + (UINT)bytes, 1, 1};
        dc()->CopySubresourceRegion(dst, 0, 0, 0, 0, g_ring.buf.get(), 0, &box);
    };
    if (s.vsCb && g_vsBound && g_bound.vsCbBytes != 0) {
        copy_cb(s.vsCb.get(), g_sh.cbVs.get(), g_ring.vsFirst, g_bound.vsCbBytes);
        s.vsBytes = (uint32_t)g_bound.vsCbBytes;
        s.vsHash = verify_hash(g_bound.vsCb, s.vsBytes);
    }
    if (s.psCb && g_bound.psCbBytes != 0) {
        copy_cb(s.psCb.get(), g_sh.cbPs.get(), g_ring.psFirst, g_bound.psCbBytes);
        s.psBytes = (uint32_t)g_bound.psCbBytes;
        s.psHash = verify_hash(g_bound.psCb, s.psBytes);
    }
    /* the vertex range (an indexed draw's: its first `count' vertices) */
    const uint32_t stride = g_bound.vbStride;
    const uint64_t v0 = (uint64_t)(indexed ? (uint32_t)std::max(0, baseVertex) : first) * stride;
    uint64_t vbytes = std::min<uint64_t>((uint64_t)count * stride, kVerifyVb);
    if (g_bound.vb == g_vstream.buf.get() && v0 + vbytes <= g_vshadow.size() && vbytes > 0) {
        const D3D11_BOX box = {(UINT)v0, 0, 0, (UINT)(v0 + vbytes), 1, 1};
        dc()->CopySubresourceRegion(vs.vb.get(), 0, slot * kVerifyVb, 0, 0, g_vstream.buf.get(), 0, &box);
        s.vbBytes = (uint32_t)vbytes;
        s.vbHash = verify_hash(g_vshadow.data() + v0, (size_t)vbytes);
    }
    if (indexed) {
        const uint64_t i0 = (uint64_t)first * 4;
        const uint64_t ibytes = std::min<uint64_t>((uint64_t)count * 4, kVerifyIb);
        if (g_bound.ib == g_istream.buf.get() && i0 + ibytes <= g_ishadow.size() && ibytes > 0) {
            const D3D11_BOX box = {(UINT)i0, 0, 0, (UINT)(i0 + ibytes), 1, 1};
            dc()->CopySubresourceRegion(vs.ib.get(), 0, slot * kVerifyIb, 0, 0, g_istream.buf.get(), 0, &box);
            s.ibBytes = (uint32_t)ibytes;
            s.ibHash = verify_hash(g_ishadow.data() + i0, (size_t)ibytes);
        }
    }
}

void d3d_verify_resolve(int set, uint32_t used, uint32_t *bits)
{
    VerifySet &vs = g_verify[set & 3];
    for (uint32_t i = 0; i < used; i++)
        bits[i] = 0;
    if (vs.failed || !vs.vb)
        return;
    D3D11_MAPPED_SUBRESOURCE mv = {}, mi = {};
    const bool okV = SUCCEEDED(dc()->Map(vs.vb.get(), 0, D3D11_MAP_READ, 0, &mv));
    const bool okI = SUCCEEDED(dc()->Map(vs.ib.get(), 0, D3D11_MAP_READ, 0, &mi));
    for (uint32_t i = 0; i < used && i < vs.slots.size() && i < kVerifySlots; i++) {
        VerifySlot &s = vs.slots[i];
        uint32_t b = 0;
        D3D11_MAPPED_SUBRESOURCE m;
        if (s.vsBytes != 0 && SUCCEEDED(dc()->Map(s.vsCb.get(), 0, D3D11_MAP_READ, 0, &m))) {
            if (verify_hash(m.pData, s.vsBytes) != s.vsHash)
                b |= 1;
            dc()->Unmap(s.vsCb.get(), 0);
        }
        if (s.psBytes != 0 && SUCCEEDED(dc()->Map(s.psCb.get(), 0, D3D11_MAP_READ, 0, &m))) {
            if (verify_hash(m.pData, s.psBytes) != s.psHash)
                b |= 2;
            dc()->Unmap(s.psCb.get(), 0);
        }
        if (okV && s.vbBytes != 0 &&
            verify_hash((const uint8_t *)mv.pData + (size_t)i * kVerifyVb, s.vbBytes) != s.vbHash)
            b |= 4;
        if (okI && s.ibBytes != 0 &&
            verify_hash((const uint8_t *)mi.pData + (size_t)i * kVerifyIb, s.ibBytes) != s.ibHash)
            b |= 8;
        bits[i] = b;
    }
    if (okV)
        dc()->Unmap(vs.vb.get(), 0);
    if (okI)
        dc()->Unmap(vs.ib.get(), 0);
}

void bind_vs(Vs vs)
{
    if (g_vsBound && vs == g_curVs)
        return;
    g_vsBound = true;
    g_curVs = vs;
    ID3D11Buffer *cb = g_ring.on ? nullptr : g_sh.cbVs.get();     /* (the ring: ring_bind_vs) */
    ID3D11VertexShader *v = vs == Vs::Gs ? g_sh.vsGs.get() : vs == Vs::Vu1 ? g_sh.vsVu1.get() : g_sh.vsFull.get();
    ID3D11GeometryShader *g = vs == Vs::Vu1 ? g_sh.gsVu1.get() : nullptr;     /* the VU1 clipper */
    ID3D11InputLayout *il = vs == Vs::Gs ? g_sh.ilGs.get() : vs == Vs::Vu1 ? g_sh.ilVu1.get() : nullptr;
    q::run([v, g, il, cb] {
        qdc()->VSSetShader(v, nullptr, 0);
        qdc()->GSSetShader(g, nullptr, 0);
        qdc()->IASetInputLayout(il);
        if (cb != nullptr) {
            qdc()->VSSetConstantBuffers(0, 1, &cb);
            if (g != nullptr)
                qdc()->GSSetConstantBuffers(0, 1, &cb);
        }
    });
    /* (the ring's constants where they are; bind_vs_cb binds new ones) */
    if (g_ring.on && g_bound.vsCbBytes != 0)
        ring_bind_vs();
}

void bind_ps(Ps ps)
{
    ID3D11PixelShader *s = nullptr;
    switch (ps) {
    case Ps::Main: s = g_sh.psMain.get(); break;
    case Ps::MainFlat: s = g_sh.psMainFlat.get(); break;
    case Ps::DepthCopy: s = g_sh.psDepthCopy.get(); break;
    case Ps::Present: s = g_sh.psPresent.get(); break;
    case Ps::PfxColor: s = g_sh.psPfxColor.get(); break;
    case Ps::Plane: s = g_sh.psPlane.get(); break;
    case Ps::Flare: s = g_sh.psFlare.get(); break;
    case Ps::DepthResolve: s = g_sh.psDepthResolve.get(); break;
    case Ps::Fxaa: s = g_sh.psFxaa.get(); break;
    case Ps::ColorResolve: s = g_sh.psColorResolve.get(); break;
    }
    if (s == g_bound.ps)
        return;
    const bool first = g_bound.ps == nullptr;
    ID3D11Buffer *cb = first && !g_ring.on ? g_sh.cbPs.get() : nullptr;
    q::run([s, cb] {
        qdc()->PSSetShader(s, nullptr, 0);
        if (cb != nullptr)
            qdc()->PSSetConstantBuffers(0, 1, &cb);
    });
    g_bound.ps = s;
}

void bind_flare_cb(const CbFlare &cb)
{
    update_cb(g_sh.cbFlare.get(), &cb, sizeof(cb));
    ID3D11Buffer *b = g_sh.cbFlare.get();
    q::run([b] { qdc()->PSSetConstantBuffers(0, 1, &b); });
    g_bound.psCbBytes = 0;          /* slot 0 no longer holds cbPs's contents */
}

void bind_vs_cb(const void *data, size_t bytes)
{
    if (bytes > g_sh.cbVsSize)
        bytes = g_sh.cbVsSize;
    if (g_vsBound && bytes == g_bound.vsCbBytes && memcmp(data, g_bound.vsCb, bytes) == 0)
        return;
    ProfScope prof(PROF_CB);                /* (the verbose monitor's profile) */
    g_profCount[PCOUNT_CBS]++;
    if (g_ring.on) {
        if (ring_write(true, data, bytes)) {
            memcpy(g_bound.vsCb, data, bytes);
            g_bound.vsCbBytes = bytes;
        }
        return;
    }
    update_cb(g_sh.cbVs.get(), data, bytes);
    memcpy(g_bound.vsCb, data, bytes);
    g_bound.vsCbBytes = bytes;
    ID3D11Buffer *cb = g_sh.cbVs.get();
    q::run([cb] { qdc()->VSSetConstantBuffers(0, 1, &cb); });
}

void bind_ps_cb(const void *data, size_t bytes)
{
    if (bytes > g_sh.cbPsSize)
        bytes = g_sh.cbPsSize;
    if (g_bound.ps != nullptr && bytes == g_bound.psCbBytes && memcmp(data, g_bound.psCb, bytes) == 0)
        return;
    ProfScope prof(PROF_CB);
    g_profCount[PCOUNT_CBS]++;
    if (g_ring.on) {
        if (ring_write(false, data, bytes)) {
            memcpy(g_bound.psCb, data, bytes);
            g_bound.psCbBytes = bytes;
        }
        return;
    }
    update_cb(g_sh.cbPs.get(), data, bytes);
    memcpy(g_bound.psCb, data, bytes);
    g_bound.psCbBytes = bytes;
    ID3D11Buffer *cb = g_sh.cbPs.get();
    q::run([cb] { qdc()->PSSetConstantBuffers(0, 1, &cb); });
}

static D3D11_BLEND d3d_factor(pgl::gs::BlendFactor f)
{
    using pgl::gs::BlendFactor;
    switch (f) {
    case BlendFactor::Zero: return D3D11_BLEND_ZERO;
    case BlendFactor::One: return D3D11_BLEND_ONE;
    case BlendFactor::SrcC: return D3D11_BLEND_SRC1_ALPHA;
    case BlendFactor::InvSrcC: return D3D11_BLEND_INV_SRC1_ALPHA;
    case BlendFactor::DstAlpha: return D3D11_BLEND_DEST_ALPHA;
    case BlendFactor::InvDstAlpha: return D3D11_BLEND_INV_DEST_ALPHA;
    case BlendFactor::DstColor: return D3D11_BLEND_DEST_COLOR;
    }
    return D3D11_BLEND_ONE;
}

ID3D11BlendState *blend_state(const pgl::gs::Blend &b, uint8_t writeMask)
{
    const uint64_t key = (uint64_t)pgl::gs::packBlendKey(b, writeMask);
    auto it = g_blend.find(key);
    if (it != g_blend.end())
        return it->second.get();
    D3D11_BLEND_DESC d = {};
    D3D11_RENDER_TARGET_BLEND_DESC &r = d.RenderTarget[0];
    r.BlendEnable = b.enable ? TRUE : FALSE;
    r.SrcBlend = b.enable ? d3d_factor(b.src) : D3D11_BLEND_ONE;
    r.DestBlend = b.enable ? d3d_factor(b.dst) : D3D11_BLEND_ZERO;
    r.BlendOp = !b.enable ? D3D11_BLEND_OP_ADD
              : b.op == pgl::gs::BlendOp::Add ? D3D11_BLEND_OP_ADD
              : b.op == pgl::gs::BlendOp::Subtract ? D3D11_BLEND_OP_SUBTRACT : D3D11_BLEND_OP_REV_SUBTRACT;
    /* the GS writes As: the alpha channel never blends */
    r.SrcBlendAlpha = D3D11_BLEND_ONE;
    r.DestBlendAlpha = D3D11_BLEND_ZERO;
    r.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    r.RenderTargetWriteMask = writeMask & 15;
    Com<ID3D11BlendState> s;
    dev()->CreateBlendState(&d, s.put());
    ID3D11BlendState *p = s.get();
    g_blend[key] = std::move(s);
    return p;
}

ID3D11BlendState *blend_plain(uint8_t writeMask)
{
    pgl::gs::Blend b = {};
    b.enable = false;
    return blend_state(b, writeMask);
}

static D3D11_COMPARISON_FUNC d3d_compare(pgl::gs::Compare c)
{
    using pgl::gs::Compare;
    switch (c) {
    case Compare::Never: return D3D11_COMPARISON_NEVER;
    case Compare::Always: return D3D11_COMPARISON_ALWAYS;
    case Compare::Less: return D3D11_COMPARISON_LESS;
    case Compare::LessEqual: return D3D11_COMPARISON_LESS_EQUAL;
    case Compare::Equal: return D3D11_COMPARISON_EQUAL;
    case Compare::GreaterEqual: return D3D11_COMPARISON_GREATER_EQUAL;
    case Compare::Greater: return D3D11_COMPARISON_GREATER;
    case Compare::NotEqual: return D3D11_COMPARISON_NOT_EQUAL;
    }
    return D3D11_COMPARISON_ALWAYS;
}

ID3D11DepthStencilState *depth_state(bool test, pgl::gs::Compare func, bool write)
{
    uint32_t key = (test ? 1u : 0u) | ((uint32_t)func << 1) | (write ? 0x10u : 0u);
    auto it = g_depth.find(key);
    if (it != g_depth.end())
        return it->second.get();
    D3D11_DEPTH_STENCIL_DESC d = {};
    d.DepthEnable = (test || write) ? TRUE : FALSE;
    d.DepthFunc = test ? d3d_compare(func) : D3D11_COMPARISON_ALWAYS;
    d.DepthWriteMask = write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    d.StencilEnable = FALSE;
    d.StencilReadMask = 0xFF;
    d.StencilWriteMask = 0xFF;
    d.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
    d.FrontFace.StencilPassOp = d.FrontFace.StencilFailOp = d.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
    d.BackFace = d.FrontFace;
    Com<ID3D11DepthStencilState> s;
    dev()->CreateDepthStencilState(&d, s.put());
    ID3D11DepthStencilState *p = s.get();
    g_depth[key] = std::move(s);
    return p;
}

ID3D11RasterizerState *raster_state(int cull, bool scissor)
{
    uint32_t key = (uint32_t)cull | (scissor ? 4u : 0u);
    auto it = g_raster.find(key);
    if (it != g_raster.end())
        return it->second.get();
    D3D11_RASTERIZER_DESC d = {};
    d.FillMode = D3D11_FILL_SOLID;
    d.CullMode = cull == 1 ? D3D11_CULL_BACK : cull == 2 ? D3D11_CULL_FRONT : D3D11_CULL_NONE;
    /* the VU1 programs' front faces are counter-clockwise in GL's y-up NDC
     * (docs/models.md 4.6) */
    d.FrontCounterClockwise = TRUE;
    d.DepthClipEnable = TRUE;
    d.ScissorEnable = scissor ? TRUE : FALSE;
    Com<ID3D11RasterizerState> s;
    dev()->CreateRasterizerState(&d, s.put());
    ID3D11RasterizerState *p = s.get();
    g_raster[key] = std::move(s);
    return p;
}

ID3D11SamplerState *sampler_state(bool magLinear, bool minLinear, int wrapU, int wrapV)
{
    uint32_t key = (magLinear ? 1u : 0u) | (minLinear ? 2u : 0u) | ((uint32_t)wrapU << 2) | ((uint32_t)wrapV << 4);
    auto it = g_samplers.find(key);
    if (it != g_samplers.end())
        return it->second.get();
    D3D11_SAMPLER_DESC d = {};
    if (magLinear && minLinear)
        d.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    else if (magLinear)
        d.Filter = D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT;
    else if (minLinear)
        d.Filter = D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT;
    else
        d.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    d.AddressU = wrapU == 0 ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
    d.AddressV = wrapV == 0 ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
    d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    d.MaxLOD = D3D11_FLOAT32_MAX;
    d.ComparisonFunc = D3D11_COMPARISON_NEVER;
    Com<ID3D11SamplerState> s;
    dev()->CreateSamplerState(&d, s.put());
    ID3D11SamplerState *p = s.get();
    g_samplers[key] = std::move(s);
    return p;
}

uint32_t stream_vertices(const void *data, uint32_t stride, uint32_t count, ID3D11Buffer **vb)
{
    ProfScope prof(PROF_STREAM);            /* (the verbose monitor's profile) */
    g_vstream.index = false;
    uint32_t at = stream_put(g_vstream, data, stride * count, stride);
    *vb = g_vstream.buf.get();
    return at / stride;
}

uint32_t stream_indices(const uint32_t *data, uint32_t count, ID3D11Buffer **ib)
{
    ProfScope prof(PROF_STREAM);
    g_istream.index = true;
    uint32_t at = stream_put(g_istream, data, 4 * count, 4);
    *ib = g_istream.buf.get();
    return at / 4;
}

bool make_texture(int w, int h, const uint32_t *rgba, Com<ID3D11Texture2D> &tex,
                  Com<ID3D11ShaderResourceView> &srv)
{
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = (UINT)w;
    d.Height = (UINT)h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = rgba;
    init.SysMemPitch = (UINT)w * 4;
    tex.reset();
    srv.reset();
    if (FAILED(dev()->CreateTexture2D(&d, &init, tex.put())))
        return false;
    return SUCCEEDED(dev()->CreateShaderResourceView(tex.get(), nullptr, srv.put()));
}

}  /* namespace pglr */
