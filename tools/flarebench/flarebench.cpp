/* flarebench: the GPU cost of GT3PC's lens-flare occlusion queries (pgl_exec.cpp
 * exec_flare_query, pgl_d3d.cpp ps_flare), measured without the game on this PC's
 * GPU, and faster ways of taking the same query compared with it texel by texel.
 *
 * The game's sequence per query (pgl_flare.cpp flare_occlusion_query): two small
 * sprites into the 2880x1620 screen with its depth buffer bound (the box's alpha
 * cleared, the shape drawn depth-tested into the alpha), then the native query: a
 * one-texel viewport of the 64 x N result strip, ps_flare looping over the box's
 * GS pixels (up to 64 x 64) in one pixel, reading the depth buffer as a texture.
 *
 * Variants:
 *   V0  the game's (serial loop in one pixel)
 *   V1  one point primitive per GS pixel of the box, each adding its weighted term
 *       into a float scratch texel (additive blend, primitive order = the loop's
 *       order), then a one-pixel pass converting the sum into the strip's texel
 *   V1d V1 with the conversion of every texel in one pass after all queries
 *   V2  one pixel per box row (the row's sum), then the rows summed in the convert
 *   V3  all queries of the frame as one instanced point draw + one convert pass
 *       (what deferring the queries to the strip's first read would allow)
 *
 *   flarebench [queries] [mix]    queries a frame (default 300); mix: 10 % octagons
 */
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>
#include <random>

template <class T> struct Com {
    T *p = nullptr;
    Com() = default;
    Com(const Com &) = delete;
    Com(Com &&o) : p(o.p) { o.p = nullptr; }
    Com &operator=(const Com &) = delete;
    ~Com() { if (p) p->Release(); }
    T **put() { if (p) { p->Release(); p = nullptr; } return &p; }
    T *get() const { return p; }
    T *operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

static const char *kHlsl = R"HLSL(
struct FullOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
FullOut vs_full(uint id : SV_VertexID)
{
    FullOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}

/* ---- the sprites the game draws around a query (alpha only) */
cbuffer CbSprite : register(b2) { float4 rectNdc; float4 zAlpha; };
FullOut vs_sprite(uint id : SV_VertexID)
{
    FullOut o;
    o.pos = float4((id & 1) ? rectNdc.z : rectNdc.x, (id & 2) ? rectNdc.w : rectNdc.y, zAlpha.x, 1);
    o.uv = 0;
    return o;
}
float4 ps_sprite(FullOut i) : SV_Target { return float4(0, 0, 0, zAlpha.y); }

/* ---- a depth buffer of noise */
float ps_depth_fill(FullOut i) : SV_Depth
{
    uint2 p = uint2(i.pos.xy);
    uint h = p.x * 374761393u + p.y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (h & 0xFFFFFFu) / 16777216.0;
}

/* ---- the game's ps_flare, verbatim (pgl_d3d.cpp SEC_FLARE) */
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

/* ---- V1: a point per GS pixel of the box, its term added into the scratch texel */
cbuffer CbQ : register(b1) { int4 qpos; int4 qstrip; };  /* the texel x, y, index; the strip's w, h */
struct PtOut { float4 pos : SV_Position; float term : TEXCOORD0; };
float4 texel_ndc(int2 t, int2 size)
{
    float2 ndc = (float2(t) + 0.5) / float2(size) * float2(2, -2) + float2(-1, 1);
    return float4(ndc, 0, 1);
}
PtOut vs_flare_pt(uint id : SV_VertexID)
{
    int x = (int)id % box.z, y = (int)id / box.z;    /* row by row, as the loop */
    PtOut o;
    o.term = wx4[x >> 2][x & 3] * wy4[y >> 2][y & 3] * shape_alpha(box.x + x, box.y + y);
    o.pos = texel_ndc(qpos.xy, qstrip.xy);
    return o;
}
float4 ps_flare_pt(PtOut i) : SV_Target { return float4(i.term, 0, 0, 0); }

Texture2D<float> sumTex : register(t1);
float4 flare_texel(float sum, int cs)
{
    uint a = (uint)clamp(floor(sum + 0.5), 0, 255);
    uint c = min((uint)cs * a >> 7, 255u);
    return float4(c, c, c, a) / 255.0;
}
float4 ps_flare_out(FullOut i) : SV_Target          /* one texel (the viewport is it) */
{
    return flare_texel(sumTex.Load(int3(qpos.xy, 0)), misc.z);
}
float4 ps_flare_out_all(FullOut i) : SV_Target      /* every texel of the strip (one Cs) */
{
    return flare_texel(sumTex.Load(int3(int2(i.pos.xy), 0)), misc.z);
}

/* ---- V2: a pixel per row */
float4 ps_flare_row(FullOut i) : SV_Target
{
    int y = (int)i.pos.x;
    float wy = wy4[y >> 2][y & 3];
    float sum = 0;
    if (wy != 0)
        for (int x = 0; x < box.z; x++) {
            float wx = wx4[x >> 2][x & 3];
            if (wx != 0)
                sum += wx * wy * shape_alpha(box.x + x, box.y + y);
        }
    return float4(sum, 0, 0, 0);
}
float4 ps_flare_out_rows(FullOut i) : SV_Target
{
    float sum = 0;
    for (int y = 0; y < box.w; y++)
        sum += sumTex.Load(int3(y, qpos.z, 0));
    return flare_texel(sum, misc.z);
}

/* ---- V3: every query of the frame in one instanced point draw (same box size,
 * same weights; per query its origin, shape, Z and texel) */
struct QueryRec { int4 box; int4 rect; float4 zt; };   /* zt: Z, -, texel x, texel y */
StructuredBuffer<QueryRec> qs : register(t2);
float shape_alpha_q(QueryRec q, int gx, int gy)
{
    gx = clamp(gx, 0, scr.x - 1);
    gy = clamp(gy, 0, scr.y - 1);
    if (!(gx >= q.rect.x && gx < q.rect.z && gy >= q.rect.y && gy < q.rect.w))
        return 0;
    float sz = zTex.Load(int3(int((gx + 0.5) * zs.x), int((gy + 0.5) * zs.y), 0)).r;
    bool ok = misc.y == 1 || (misc.y == 2 && q.zt.x >= sz) || (misc.y == 3 && q.zt.x > sz);
    return ok ? 255 : 0;
}
PtOut vs_flare_batch(uint id : SV_VertexID, uint inst : SV_InstanceID)
{
    QueryRec q = qs[inst];
    int x = (int)id % box.z, y = (int)id / box.z;
    PtOut o;
    o.term = wx4[x >> 2][x & 3] * wy4[y >> 2][y & 3] * shape_alpha_q(q, q.box.x + x, q.box.y + y);
    o.pos = texel_ndc(int2(q.zt.zw), qstrip.xy);
    return o;
}
)HLSL";

struct CbFlare {
    int32_t box[4];
    int32_t rect[5][4];
    int32_t nRects, ztest, cs, halfY;
    float scale[2], z, pad;
    int32_t screen[4];
    float wx[68], wy[68];
};
static_assert(sizeof(CbFlare) == 688, "CbFlare");
struct QueryRec { int32_t box[4]; int32_t rect[4]; float zt[4]; };
struct Query { CbFlare cb; int q; int gx0, gy0, gx1, gy1; };

/* ---- the game's weights (pgl_exec.cpp flare_half / flare_axis_weights) */
struct FlareStep { int a, b; };
static int flare_half(int n)
{
    int h = n >> 1;
    if (((h << 1) ^ n) != 0)
        h <<= 1;
    return h < 1 ? 1 : h;
}
static void flare_axis_weights(int copied, const FlareStep *steps, int nSteps, float *out)
{
    static float m[64][64], n[64][64];
    for (int k = 0; k < 64; k++)
        for (int c = 0; c < 64; c++)
            m[k][c] = (k == c && k < copied) ? 1.0f : 0.0f;
    for (int s = 0; s < nSteps; s++) {
        const int a = steps[s].a, b = std::min(steps[s].b, 64);
        for (int x = 0; x < b; x++) {
            const float t = 0.5f + (float)x * (float)a / (float)b;
            const int i = (int)std::floor(t);
            const float f = t - (float)i;
            const int i0 = std::min(std::max(i, 0), 63), i1 = std::min(std::max(i + 1, 0), 63);
            for (int c = 0; c < 64; c++)
                n[x][c] = m[i0][c] * (1.0f - f) + m[i1][c] * f;
        }
        for (int x = 0; x < b; x++)
            memcpy(m[x], n[x], sizeof(m[x]));
    }
    for (int c = 0; c < 68; c++)
        out[c] = c < 64 ? m[0][c] : 0.0f;
}
static void octagon_rects(int cx, int cy, float r, float aspect, int32_t (*rect)[4])
{
    int outer = (int)(r * 0.923879f), mid = (int)(r * 0.707107f), inner = (int)(r * 0.382683f);
    int outerY = (int)((float)outer * aspect), midY = (int)((float)mid * aspect), innerY = (int)((float)inner * aspect);
    const int32_t r5[5][4] = {
        {cx - inner, cy - outerY, cx + inner, cy + outerY}, {cx - mid, cy - midY, cx - inner, cy + midY},
        {cx - outer, cy - innerY, cx - mid, cy + innerY},   {cx + inner, cy - midY, cx + mid, cy + midY},
        {cx + mid, cy - innerY, cx + outer, cy + innerY},
    };
    memcpy(rect, r5, sizeof(r5));
}

static const int kGsW = 640, kGsH = 448, kNatW = 2880, kNatH = 1620, kStripW = 64, kStripH = 16;

/* a query as exec_flare_query fills it: box x0, y0 and w x h (the shape's size), the shape */
static Query make_query(int x0, int y0, int w, int h, bool octagon, float z, int q)
{
    Query Q = {};
    CbFlare &cb = Q.cb;
    Q.gx0 = x0; Q.gy0 = y0; Q.gx1 = x0 + w; Q.gy1 = y0 + h; Q.q = q;
    cb.box[0] = x0; cb.box[1] = y0; cb.box[2] = std::min(w + 1, 64); cb.box[3] = std::min(h + 1, 64);
    FlareStep sx[16], sy[16];
    int nSteps = 0;
    if (w >= 2) {
        int a = std::min(w, 64), b = w < 18 ? a : std::min(h, 64);
        while ((a >= 2 || b >= 2) && nSteps < 16) {
            const int na = flare_half(a), nb = flare_half(b);
            sx[nSteps] = FlareStep{a, na};
            sy[nSteps] = FlareStep{b, nb};
            nSteps++;
            a = na;
            b = nb;
        }
    }
    flare_axis_weights(cb.box[2], sx, nSteps, cb.wx);
    flare_axis_weights(cb.box[3], sy, nSteps, cb.wy);
    if (octagon) {
        octagon_rects(x0 + w / 2, y0 + h / 2, (float)w * 0.5f, 1.0f, cb.rect);
        cb.nRects = 5;
    } else {
        cb.rect[0][0] = x0; cb.rect[0][1] = y0; cb.rect[0][2] = x0 + w; cb.rect[0][3] = y0 + h;
        cb.nRects = 1;
    }
    cb.ztest = 2;
    cb.cs = 0x3F;
    cb.halfY = 0;
    cb.scale[0] = (float)kNatW / kGsW;
    cb.scale[1] = (float)kNatH / kGsH;
    cb.z = z;
    cb.screen[0] = kGsW;
    cb.screen[1] = kGsH;
    return Q;
}

struct Gpu {
    Com<ID3D11Device> dev;
    Com<ID3D11DeviceContext> ctx;
    Com<ID3D11Texture2D> screen, depth, strip, stripStaging, scratch, rows;
    Com<ID3D11RenderTargetView> screenRtv, stripRtv, scratchRtv, rowsRtv;
    Com<ID3D11DepthStencilView> dsv;
    Com<ID3D11ShaderResourceView> zSrv, scratchSrv, rowsSrv, recsSrv;
    Com<ID3D11Buffer> cbFlare, cbQ, cbSprite, recs;
    Com<ID3D11VertexShader> vsFull, vsSprite, vsPt, vsBatch;
    Com<ID3D11PixelShader> psSprite, psDepthFill, psFlare, psPt, psOut, psOutAll, psRow, psOutRows;
    Com<ID3D11BlendState> bsWriteA, bsPlain, bsAdd;
    Com<ID3D11DepthStencilState> dsOff, dsTestGE, dsFill;
    Com<ID3D11RasterizerState> rs;
    Com<ID3D11Query> qDisjoint, qT0, qT1;
};

static Com<ID3DBlob> compile(const char *entry, const char *target)
{
    Com<ID3DBlob> code, err;
    HRESULT hr = D3DCompile(kHlsl, strlen(kHlsl), "flarebench.hlsl", nullptr, nullptr, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.put(), err.put());
    if (FAILED(hr)) {
        printf("shader %s: %s\n", entry, err ? (const char *)err->GetBufferPointer() : "?");
        exit(1);
    }
    return code;
}

static bool init(Gpu &g)
{
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                   g.dev.put(), &fl, g.ctx.put());
    if (FAILED(hr)) { printf("no device (%08lX)\n", hr); return false; }
    {
        Com<IDXGIDevice> dd; Com<IDXGIAdapter> ad;
        g.dev->QueryInterface(__uuidof(IDXGIDevice), (void **)dd.put());
        if (dd) dd->GetAdapter(ad.put());
        DXGI_ADAPTER_DESC d = {};
        if (ad) ad->GetDesc(&d);
        printf("adapter: %ls, feature level %X\n", d.Description, (unsigned)fl);
    }
    auto tex2d = [&](int w, int h, DXGI_FORMAT f, UINT bind, Com<ID3D11Texture2D> &t, D3D11_USAGE u = D3D11_USAGE_DEFAULT,
                     UINT cpu = 0) {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = f; d.SampleDesc.Count = 1;
        d.Usage = u; d.BindFlags = bind; d.CPUAccessFlags = cpu;
        return SUCCEEDED(g.dev->CreateTexture2D(&d, nullptr, t.put()));
    };
    bool ok = true;
    ok &= tex2d(kNatW, kNatH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET, g.screen);
    ok &= tex2d(kNatW, kNatH, DXGI_FORMAT_R32_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, g.depth);
    ok &= tex2d(kStripW, kStripH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET, g.strip);
    ok &= tex2d(kStripW, kStripH, DXGI_FORMAT_R8G8B8A8_UNORM, 0, g.stripStaging, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
    ok &= tex2d(kStripW, kStripH, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, g.scratch);
    ok &= tex2d(64, kStripW * kStripH, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, g.rows);
    if (!ok) { printf("textures failed\n"); return false; }
    g.dev->CreateRenderTargetView(g.screen.get(), nullptr, g.screenRtv.put());
    g.dev->CreateRenderTargetView(g.strip.get(), nullptr, g.stripRtv.put());
    g.dev->CreateRenderTargetView(g.scratch.get(), nullptr, g.scratchRtv.put());
    g.dev->CreateRenderTargetView(g.rows.get(), nullptr, g.rowsRtv.put());
    g.dev->CreateShaderResourceView(g.scratch.get(), nullptr, g.scratchSrv.put());
    g.dev->CreateShaderResourceView(g.rows.get(), nullptr, g.rowsSrv.put());
    {
        D3D11_DEPTH_STENCIL_VIEW_DESC dv = {};
        dv.Format = DXGI_FORMAT_D32_FLOAT; dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        g.dev->CreateDepthStencilView(g.depth.get(), &dv, g.dsv.put());
        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = DXGI_FORMAT_R32_FLOAT; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
        g.dev->CreateShaderResourceView(g.depth.get(), &sv, g.zSrv.put());
    }
    auto cb = [&](size_t bytes, Com<ID3D11Buffer> &b) {
        D3D11_BUFFER_DESC d = {};
        d.ByteWidth = (UINT)((bytes + 15) & ~(size_t)15); d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        g.dev->CreateBuffer(&d, nullptr, b.put());
    };
    cb(sizeof(CbFlare), g.cbFlare);
    cb(32, g.cbQ);
    cb(32, g.cbSprite);
    {
        D3D11_BUFFER_DESC d = {};
        d.ByteWidth = sizeof(QueryRec) * kStripW * kStripH; d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        d.StructureByteStride = sizeof(QueryRec);
        g.dev->CreateBuffer(&d, nullptr, g.recs.put());
        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = DXGI_FORMAT_UNKNOWN; sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sv.Buffer.NumElements = kStripW * kStripH;
        g.dev->CreateShaderResourceView(g.recs.get(), &sv, g.recsSrv.put());
    }
    auto vs = [&](const char *entry, const char *target, Com<ID3D11VertexShader> &s) {
        Com<ID3DBlob> b = compile(entry, target);
        g.dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, s.put());
    };
    auto ps = [&](const char *entry, Com<ID3D11PixelShader> &s) {
        Com<ID3DBlob> b = compile(entry, "ps_4_0");
        g.dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, s.put());
    };
    vs("vs_full", "vs_4_0", g.vsFull);
    vs("vs_sprite", "vs_4_0", g.vsSprite);
    vs("vs_flare_pt", "vs_4_0", g.vsPt);
    vs("vs_flare_batch", "vs_5_0", g.vsBatch);
    ps("ps_sprite", g.psSprite);
    ps("ps_depth_fill", g.psDepthFill);
    ps("ps_flare", g.psFlare);
    ps("ps_flare_pt", g.psPt);
    ps("ps_flare_out", g.psOut);
    ps("ps_flare_out_all", g.psOutAll);
    ps("ps_flare_row", g.psRow);
    ps("ps_flare_out_rows", g.psOutRows);
    {
        D3D11_BLEND_DESC d = {};
        D3D11_RENDER_TARGET_BLEND_DESC &r = d.RenderTarget[0];
        r.SrcBlend = D3D11_BLEND_ONE; r.DestBlend = D3D11_BLEND_ZERO; r.BlendOp = D3D11_BLEND_OP_ADD;
        r.SrcBlendAlpha = D3D11_BLEND_ONE; r.DestBlendAlpha = D3D11_BLEND_ZERO; r.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        r.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALPHA;
        g.dev->CreateBlendState(&d, g.bsWriteA.put());
        r.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        g.dev->CreateBlendState(&d, g.bsPlain.put());
        r.BlendEnable = TRUE; r.SrcBlend = D3D11_BLEND_ONE; r.DestBlend = D3D11_BLEND_ONE;
        r.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED;
        g.dev->CreateBlendState(&d, g.bsAdd.put());
    }
    {
        D3D11_DEPTH_STENCIL_DESC d = {};
        d.DepthEnable = FALSE; d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; d.DepthFunc = D3D11_COMPARISON_ALWAYS;
        g.dev->CreateDepthStencilState(&d, g.dsOff.put());
        d.DepthEnable = TRUE; d.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
        g.dev->CreateDepthStencilState(&d, g.dsTestGE.put());
        d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; d.DepthFunc = D3D11_COMPARISON_ALWAYS;
        g.dev->CreateDepthStencilState(&d, g.dsFill.put());
    }
    {
        D3D11_RASTERIZER_DESC d = {};
        d.FillMode = D3D11_FILL_SOLID; d.CullMode = D3D11_CULL_NONE; d.DepthClipEnable = TRUE;
        g.dev->CreateRasterizerState(&d, g.rs.put());
    }
    {
        D3D11_QUERY_DESC d = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        g.dev->CreateQuery(&d, g.qDisjoint.put());
        d.Query = D3D11_QUERY_TIMESTAMP;
        g.dev->CreateQuery(&d, g.qT0.put());
        g.dev->CreateQuery(&d, g.qT1.put());
    }
    g.ctx->RSSetState(g.rs.get());
    return true;
}

static void viewport(Gpu &g, float x, float y, float w, float h)
{
    D3D11_VIEWPORT vp = {x, y, w, h, 0, 1};
    g.ctx->RSSetViewports(1, &vp);
}

static void depth_fill(Gpu &g, uint32_t seed)
{
    (void)seed;
    g.ctx->OMSetRenderTargets(0, nullptr, g.dsv.get());
    viewport(g, 0, 0, (float)kNatW, (float)kNatH);
    g.ctx->OMSetDepthStencilState(g.dsFill.get(), 0);
    g.ctx->OMSetBlendState(g.bsPlain.get(), nullptr, 0xFFFFFFFFu);
    g.ctx->VSSetShader(g.vsFull.get(), nullptr, 0);
    g.ctx->PSSetShader(g.psDepthFill.get(), nullptr, 0);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->Draw(3, 0);
    const float black[4] = {0, 0, 0, 0.5f};
    g.ctx->ClearRenderTargetView(g.screenRtv.get(), black);
}

/* the two sprites of a query (the box's alpha cleared, the shape depth-tested) */
static void sprites(Gpu &g, const Query &Q)
{
    ID3D11ShaderResourceView *none[3] = {nullptr, nullptr, nullptr};
    g.ctx->PSSetShaderResources(0, 3, none);
    g.ctx->VSSetShaderResources(0, 3, none);
    ID3D11RenderTargetView *rtv = g.screenRtv.get();
    g.ctx->OMSetRenderTargets(1, &rtv, g.dsv.get());
    viewport(g, 0, 0, (float)kNatW, (float)kNatH);
    g.ctx->OMSetBlendState(g.bsWriteA.get(), nullptr, 0xFFFFFFFFu);
    g.ctx->VSSetShader(g.vsSprite.get(), nullptr, 0);
    g.ctx->PSSetShader(g.psSprite.get(), nullptr, 0);
    ID3D11Buffer *b2 = g.cbSprite.get();
    g.ctx->VSSetConstantBuffers(2, 1, &b2);
    g.ctx->PSSetConstantBuffers(2, 1, &b2);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    auto rect = [&](int x0, int y0, int x1, int y1, float alpha) {
        float c[8] = {(float)x0 / kGsW * 2 - 1, 1 - (float)y0 / kGsH * 2, (float)x1 / kGsW * 2 - 1, 1 - (float)y1 / kGsH * 2,
                      Q.cb.z, alpha, 0, 0};
        g.ctx->UpdateSubresource(g.cbSprite.get(), 0, nullptr, c, 0, 0);
        g.ctx->Draw(4, 0);
    };
    g.ctx->OMSetDepthStencilState(g.dsOff.get(), 0);
    rect(Q.gx0, Q.gy0, Q.gx1 + 2, Q.gy1 + 2, 0.0f);
    g.ctx->OMSetDepthStencilState(g.dsTestGE.get(), 0);
    rect(Q.gx0, Q.gy0, Q.gx1, Q.gy1, 1.0f);
}

static void bind_cb(Gpu &g, const Query &Q)
{
    g.ctx->UpdateSubresource(g.cbFlare.get(), 0, nullptr, &Q.cb, 0, 0);
    int32_t qp[8] = {Q.q & 63, Q.q >> 6, Q.q, 0, kStripW, kStripH, 0, 0};
    g.ctx->UpdateSubresource(g.cbQ.get(), 0, nullptr, qp, 0, 0);
    ID3D11Buffer *bs[2] = {g.cbFlare.get(), g.cbQ.get()};
    g.ctx->VSSetConstantBuffers(0, 2, bs);
    g.ctx->PSSetConstantBuffers(0, 2, bs);
}

static void query_v0(Gpu &g, const Query &Q)
{
    ID3D11ShaderResourceView *none[2] = {nullptr, nullptr};
    g.ctx->PSSetShaderResources(0, 2, none);
    ID3D11RenderTargetView *rtv = g.stripRtv.get();
    g.ctx->OMSetRenderTargets(1, &rtv, nullptr);
    viewport(g, (float)(Q.q & 63), (float)(Q.q >> 6), 1, 1);
    g.ctx->OMSetBlendState(g.bsPlain.get(), nullptr, 0xFFFFFFFFu);
    g.ctx->OMSetDepthStencilState(g.dsOff.get(), 0);
    g.ctx->VSSetShader(g.vsFull.get(), nullptr, 0);
    g.ctx->PSSetShader(g.psFlare.get(), nullptr, 0);
    bind_cb(g, Q);
    ID3D11ShaderResourceView *z = g.zSrv.get();
    g.ctx->PSSetShaderResources(0, 1, &z);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->Draw(3, 0);
    g.ctx->PSSetShaderResources(0, 2, none);
}

static void convert_one(Gpu &g, const Query &Q, ID3D11ShaderResourceView *sums, ID3D11PixelShader *ps)
{
    ID3D11ShaderResourceView *none[2] = {nullptr, nullptr};
    ID3D11RenderTargetView *rtv = g.stripRtv.get();
    g.ctx->OMSetRenderTargets(1, &rtv, nullptr);
    viewport(g, (float)(Q.q & 63), (float)(Q.q >> 6), 1, 1);
    g.ctx->OMSetBlendState(g.bsPlain.get(), nullptr, 0xFFFFFFFFu);
    g.ctx->VSSetShader(g.vsFull.get(), nullptr, 0);
    g.ctx->PSSetShader(ps, nullptr, 0);
    g.ctx->PSSetShaderResources(1, 1, &sums);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->Draw(3, 0);
    g.ctx->PSSetShaderResources(0, 2, none);
}

static void convert_all(Gpu &g, const Query &Q)
{
    ID3D11ShaderResourceView *none[2] = {nullptr, nullptr};
    ID3D11RenderTargetView *rtv = g.stripRtv.get();
    g.ctx->OMSetRenderTargets(1, &rtv, nullptr);
    viewport(g, 0, 0, (float)kStripW, (float)kStripH);
    g.ctx->OMSetBlendState(g.bsPlain.get(), nullptr, 0xFFFFFFFFu);
    g.ctx->OMSetDepthStencilState(g.dsOff.get(), 0);
    g.ctx->VSSetShader(g.vsFull.get(), nullptr, 0);
    g.ctx->PSSetShader(g.psOutAll.get(), nullptr, 0);
    bind_cb(g, Q);
    ID3D11ShaderResourceView *s = g.scratchSrv.get();
    g.ctx->PSSetShaderResources(1, 1, &s);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->Draw(3, 0);
    g.ctx->PSSetShaderResources(0, 2, none);
}

/* V1: the points into the scratch (cleared once a frame), then (unless deferred) the texel */
static void query_v1(Gpu &g, const Query &Q, bool convert)
{
    ID3D11ShaderResourceView *none[2] = {nullptr, nullptr};
    g.ctx->PSSetShaderResources(0, 2, none);
    ID3D11RenderTargetView *rtv = g.scratchRtv.get();
    g.ctx->OMSetRenderTargets(1, &rtv, nullptr);
    viewport(g, 0, 0, (float)kStripW, (float)kStripH);
    g.ctx->OMSetBlendState(g.bsAdd.get(), nullptr, 0xFFFFFFFFu);
    g.ctx->OMSetDepthStencilState(g.dsOff.get(), 0);
    g.ctx->VSSetShader(g.vsPt.get(), nullptr, 0);
    g.ctx->PSSetShader(g.psPt.get(), nullptr, 0);
    bind_cb(g, Q);
    ID3D11ShaderResourceView *z = g.zSrv.get();
    g.ctx->VSSetShaderResources(0, 1, &z);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    g.ctx->Draw((UINT)(Q.cb.box[2] * Q.cb.box[3]), 0);
    g.ctx->VSSetShaderResources(0, 2, none);
    if (convert)
        convert_one(g, Q, g.scratchSrv.get(), g.psOut.get());
}

/* V2: a pixel per row into the rows scratch (row y of query q at texel (y, q)) */
static void query_v2(Gpu &g, const Query &Q)
{
    ID3D11ShaderResourceView *none[2] = {nullptr, nullptr};
    g.ctx->PSSetShaderResources(0, 2, none);
    ID3D11RenderTargetView *rtv = g.rowsRtv.get();
    g.ctx->OMSetRenderTargets(1, &rtv, nullptr);
    viewport(g, 0, (float)Q.q, (float)Q.cb.box[3], 1);
    g.ctx->OMSetBlendState(g.bsPlain.get(), nullptr, 0xFFFFFFFFu);
    g.ctx->OMSetDepthStencilState(g.dsOff.get(), 0);
    g.ctx->VSSetShader(g.vsFull.get(), nullptr, 0);
    g.ctx->PSSetShader(g.psRow.get(), nullptr, 0);
    bind_cb(g, Q);
    ID3D11ShaderResourceView *z = g.zSrv.get();
    g.ctx->PSSetShaderResources(0, 1, &z);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->Draw(3, 0);
    g.ctx->PSSetShaderResources(0, 2, none);
    convert_one(g, Q, g.rowsSrv.get(), g.psOutRows.get());
}

/* V3: every query in one instanced draw (all of one box size), then one convert */
static void queries_v3(Gpu &g, const std::vector<Query> &qs)
{
    std::vector<QueryRec> recs(qs.size());
    for (size_t i = 0; i < qs.size(); i++) {
        const CbFlare &cb = qs[i].cb;
        memcpy(recs[i].box, cb.box, sizeof(recs[i].box));
        memcpy(recs[i].rect, cb.rect[0], sizeof(recs[i].rect));
        recs[i].zt[0] = cb.z; recs[i].zt[1] = 0; recs[i].zt[2] = (float)(qs[i].q & 63); recs[i].zt[3] = (float)(qs[i].q >> 6);
    }
    D3D11_BOX b = {0, 0, 0, (UINT)(recs.size() * sizeof(QueryRec)), 1, 1};
    g.ctx->UpdateSubresource(g.recs.get(), 0, &b, recs.data(), 0, 0);
    ID3D11ShaderResourceView *none[3] = {nullptr, nullptr, nullptr};
    g.ctx->PSSetShaderResources(0, 3, none);
    ID3D11RenderTargetView *rtv = g.scratchRtv.get();
    g.ctx->OMSetRenderTargets(1, &rtv, nullptr);
    viewport(g, 0, 0, (float)kStripW, (float)kStripH);
    g.ctx->OMSetBlendState(g.bsAdd.get(), nullptr, 0xFFFFFFFFu);
    g.ctx->OMSetDepthStencilState(g.dsOff.get(), 0);
    g.ctx->VSSetShader(g.vsBatch.get(), nullptr, 0);
    g.ctx->PSSetShader(g.psPt.get(), nullptr, 0);
    bind_cb(g, qs[0]);
    ID3D11ShaderResourceView *srvs[3] = {g.zSrv.get(), nullptr, g.recsSrv.get()};
    g.ctx->VSSetShaderResources(0, 3, srvs);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    g.ctx->DrawInstanced((UINT)(qs[0].cb.box[2] * qs[0].cb.box[3]), (UINT)qs.size(), 0, 0);
    g.ctx->VSSetShaderResources(0, 3, none);
    convert_all(g, qs[0]);
}

static void clear_scratch(Gpu &g)
{
    const float zero[4] = {0, 0, 0, 0};
    g.ctx->ClearRenderTargetView(g.scratchRtv.get(), zero);
    g.ctx->ClearRenderTargetView(g.stripRtv.get(), zero);
}

/* a frame of queries by variant; GPU ms */
static double frame(Gpu &g, int variant, const std::vector<Query> &qs, bool timed)
{
    if (timed) {
        g.ctx->Begin(g.qDisjoint.get());
        g.ctx->End(g.qT0.get());
    }
    clear_scratch(g);
    for (const Query &Q : qs) {
        sprites(g, Q);
        switch (variant) {
        case 0: query_v0(g, Q); break;
        case 1: query_v1(g, Q, true); break;
        case 2: query_v1(g, Q, false); break;
        case 3: query_v2(g, Q); break;
        case 4: break;
        case 5: break;
        }
    }
    if (variant == 2)
        convert_all(g, qs[0]);
    if (variant == 4)
        queries_v3(g, qs);
    if (!timed)
        return 0;
    g.ctx->End(g.qT1.get());
    g.ctx->End(g.qDisjoint.get());
    g.ctx->Flush();
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
    while (g.ctx->GetData(g.qDisjoint.get(), &dj, sizeof(dj), 0) != S_OK)
        Sleep(0);
    UINT64 t0 = 0, t1 = 0;
    while (g.ctx->GetData(g.qT0.get(), &t0, sizeof(t0), 0) != S_OK) Sleep(0);
    while (g.ctx->GetData(g.qT1.get(), &t1, sizeof(t1), 0) != S_OK) Sleep(0);
    if (dj.Disjoint || dj.Frequency == 0)
        return -1;
    return (double)(t1 - t0) * 1000.0 / (double)dj.Frequency;
}

static std::vector<uint32_t> read_strip(Gpu &g)
{
    g.ctx->CopyResource(g.stripStaging.get(), g.strip.get());
    D3D11_MAPPED_SUBRESOURCE m;
    std::vector<uint32_t> out(kStripW * kStripH);
    if (SUCCEEDED(g.ctx->Map(g.stripStaging.get(), 0, D3D11_MAP_READ, 0, &m))) {
        for (int y = 0; y < kStripH; y++)
            memcpy(&out[y * kStripW], (const uint8_t *)m.pData + y * m.RowPitch, kStripW * 4);
        g.ctx->Unmap(g.stripStaging.get(), 0);
    }
    return out;
}

static std::vector<Query> make_frame(std::mt19937 &rng, int n, bool mix, bool allLamps)
{
    std::vector<Query> qs;
    std::uniform_real_distribution<float> uz(0.0f, 1.0f);
    for (int i = 0; i < n; i++) {
        const bool oct = mix && !allLamps && (i % 10 == 9);
        int w = oct ? 24 + (int)(rng() % 25) : 10;
        int x0 = 12 + (int)(rng() % (kGsW - 24 - w)), y0 = 12 + (int)(rng() % (kGsH - 24 - w));
        qs.push_back(make_query(x0, y0, w, w, oct, uz(rng), i));
    }
    return qs;
}

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 300;
    bool mix = argc > 2 && strcmp(argv[2], "mix") == 0;
    if (n < 1 || n > kStripW * kStripH) n = 300;
    Gpu g;
    if (!init(g))
        return 1;
    std::mt19937 rng(12345);
    depth_fill(g, 1);
    const char *names[] = {"V0 game (serial loop, one pixel)", "V1 points + texel each", "V1d points, one convert",
                           "V2 row pixels + texel each", "V3 one instanced draw", "sprites only"};
    const int nVariants = mix ? 4 : 5;     /* V3 needs one box size */

    /* ---- exactness: the strips of V1.. against V0 over several frames */
    printf("exactness over 20 frames of %d queries%s (texels differing from V0; the largest alpha difference):\n", n,
           mix ? " (10 %% octagons)" : "");
    for (int v = 1; v < nVariants; v++) {
        std::mt19937 r2(777);
        int diff = 0, maxd = 0, total = 0;
        for (int f = 0; f < 20; f++) {
            std::vector<Query> qs = make_frame(r2, n, mix, false);
            frame(g, 0, qs, false);
            std::vector<uint32_t> a = read_strip(g);
            frame(g, v, qs, false);
            std::vector<uint32_t> b = read_strip(g);
            for (int i = 0; i < n; i++) {
                total++;
                if (a[i] != b[i]) {
                    diff++;
                    int da = abs((int)(a[i] >> 24) - (int)(b[i] >> 24));
                    maxd = std::max(maxd, da);
                    if (diff <= 3)
                        printf("   %s: frame %d query %d: V0 %08X, this %08X\n", names[v], f, i, a[i], b[i]);
                }
            }
        }
        printf("  %-34s %d of %d texels differ, max alpha difference %d\n", names[v], diff, total, maxd);
    }

    /* ---- time: the best of 7 frames per variant */
    printf("\nGPU time of a frame of %d queries (the two sprites of each included), best of 7:\n", n);
    std::vector<Query> qs = make_frame(rng, n, mix, false);
    for (int v = 0; v <= 5; v++) {
        if (v == 4 && mix) continue;
        double best = 1e9;
        for (int k = 0; k < 7; k++) {
            double ms = frame(g, v, qs, true);
            if (ms >= 0) best = std::min(best, ms);
        }
        printf("  %-34s %8.3f ms  %7.2f us a query\n", names[v], best, best * 1000.0 / n);
    }
    /* the loop's cost by box size: one query of each size, V0 and V1 */
    printf("\none query by box size (GS pixels of the shape), V0 against V1, best of 7, us:\n");
    for (int s : {4, 10, 16, 24, 32, 48, 63}) {
        std::vector<Query> one = {make_query(100, 100, s, s, s > 16, 0.5f, 0)};
        double b0 = 1e9, b1 = 1e9;
        for (int k = 0; k < 7; k++) {
            b0 = std::min(b0, frame(g, 0, one, true));
            b1 = std::min(b1, frame(g, 1, one, true));
        }
        printf("  %2dx%-2d  V0 %8.2f   V1 %8.2f\n", s, s, b0 * 1000, b1 * 1000);
    }
    return 0;
}
