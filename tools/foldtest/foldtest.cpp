/* foldtest: two questions about this GPU's Direct3D 11 behaviour that the folded
 * mirror pass (GT3PC pgl_exec.cpp mirror_fold_ok) depends on.
 *   1. Is SV_Position.z in the pixel shader the very depth the depth unit wrote
 *      for the same fragment (a draw with depth writes, no SV_Depth)?  Random
 *      triangles with perspective (random w), each: depth cleared, drawn with
 *      depth writes, then drawn again by a shader that reads the depth texture
 *      and paints red where pos.z != stored; the red pixels counted.
 *      And: the same triangle drawn with the depth function EQUAL and
 *      SV_Depth = stored passes everywhere it covers?
 *   2. In an instanced draw, do all primitives of instance 0 come before those
 *      of instance 1 (instance-major), or do the instances of a primitive follow
 *      each other?  Two overlapping triangles in one draw of two instances with
 *      an order-dependent blend (over, alpha 0.5; instance 0 red, 1 green); the
 *      overlap pixel's colour tells.
 */
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
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
struct VsIn { float4 pos : POSITION; float4 color : COLOR0; uint inst : SV_InstanceID; };
struct PsIn { float4 pos : SV_Position; float4 color : COLOR0; nointerpolation uint inst : TEXCOORD1; };
PsIn vs_main(VsIn i)
{
    PsIn o;
    o.pos = i.pos;              /* clip space: (x w, y w, z w, w) */
    o.color = i.color;
    o.inst = i.inst;
    return o;
}
/* depth writes only */
float4 ps_depth(PsIn i) : SV_Target { return float4(0, 0, 0, 0); }
/* red where the pixel shader's z is not the stored depth */
Texture2D<float> zBuf : register(t0);
float4 ps_check(PsIn i) : SV_Target
{
    float stored = zBuf.Load(int3(i.pos.xy, 0));
    return i.pos.z == stored ? float4(0, 0, 0, 1) : float4(1, 0, 0, 1);
}
/* SV_Depth = the stored depth (the fixed test EQUAL); paints blue where it runs */
struct PsOutD { float4 c : SV_Target; float depth : SV_Depth; };
PsOutD ps_equal(PsIn i)
{
    PsOutD o;
    o.c = float4(0, 0, 1, 1);
    o.depth = zBuf.Load(int3(i.pos.xy, 0));
    return o;
}
/* the instance order: instance 0 red, 1 green, alpha 0.5 (blended over) */
float4 ps_order(PsIn i) : SV_Target
{
    return i.inst == 0 ? float4(1, 0, 0, 0.5) : float4(0, 1, 0, 0.5);
}
)HLSL";

struct Vtx { float x, y, z, w; float r, g, b, a; };

static Com<ID3DBlob> compile(const char *entry, const char *target)
{
    Com<ID3DBlob> code, err;
    if (FAILED(D3DCompile(kHlsl, strlen(kHlsl), "foldtest.hlsl", nullptr, nullptr, entry, target,
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.put(), err.put()))) {
        printf("shader %s: %s\n", entry, err ? (const char *)err->GetBufferPointer() : "?");
        exit(1);
    }
    return code;
}

int main()
{
    const int W = 640, H = 448;
    Com<ID3D11Device> dev;
    Com<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                 dev.put(), &fl, ctx.put()))) {
        printf("no device\n");
        return 1;
    }
    {
        Com<IDXGIDevice> dd; Com<IDXGIAdapter> ad;
        dev->QueryInterface(__uuidof(IDXGIDevice), (void **)dd.put());
        if (dd) dd->GetAdapter(ad.put());
        DXGI_ADAPTER_DESC d = {};
        if (ad) ad->GetDesc(&d);
        printf("adapter: %ls, feature level %X\n", d.Description, (unsigned)fl);
    }
    /* resources */
    Com<ID3D11Texture2D> rt, depth, staging;
    Com<ID3D11RenderTargetView> rtv;
    Com<ID3D11DepthStencilView> dsv, dsvRead;
    Com<ID3D11ShaderResourceView> zSrv;
    {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = W; d.Height = H; d.MipLevels = 1; d.ArraySize = 1; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.BindFlags = D3D11_BIND_RENDER_TARGET;
        dev->CreateTexture2D(&d, nullptr, rt.put());
        dev->CreateRenderTargetView(rt.get(), nullptr, rtv.put());
        d.BindFlags = 0; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        dev->CreateTexture2D(&d, nullptr, staging.put());
        d.Usage = D3D11_USAGE_DEFAULT; d.CPUAccessFlags = 0;
        d.Format = DXGI_FORMAT_R32G8X24_TYPELESS; d.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        dev->CreateTexture2D(&d, nullptr, depth.put());
        D3D11_DEPTH_STENCIL_VIEW_DESC dv = {};
        dv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        dev->CreateDepthStencilView(depth.get(), &dv, dsv.put());
        dv.Flags = D3D11_DSV_READ_ONLY_DEPTH;
        dev->CreateDepthStencilView(depth.get(), &dv, dsvRead.put());
        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(depth.get(), &sv, zSrv.put());
    }
    Com<ID3D11VertexShader> vs;
    Com<ID3D11PixelShader> psDepth, psCheck, psEqual, psOrder;
    Com<ID3D11InputLayout> il;
    {
        Com<ID3DBlob> b = compile("vs_main", "vs_4_0");
        dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, vs.put());
        D3D11_INPUT_ELEMENT_DESC le[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        dev->CreateInputLayout(le, 2, b->GetBufferPointer(), b->GetBufferSize(), il.put());
        auto ps = [&](const char *e, Com<ID3D11PixelShader> &s) {
            Com<ID3DBlob> c = compile(e, "ps_4_0");
            dev->CreatePixelShader(c->GetBufferPointer(), c->GetBufferSize(), nullptr, s.put());
        };
        ps("ps_depth", psDepth); ps("ps_check", psCheck); ps("ps_equal", psEqual); ps("ps_order", psOrder);
    }
    Com<ID3D11Buffer> vb;
    {
        D3D11_BUFFER_DESC d = {};
        d.ByteWidth = sizeof(Vtx) * 6; d.Usage = D3D11_USAGE_DYNAMIC; d.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        dev->CreateBuffer(&d, nullptr, vb.put());
    }
    Com<ID3D11DepthStencilState> dsWrite, dsEqual, dsOff;
    {
        D3D11_DEPTH_STENCIL_DESC d = {};
        d.DepthEnable = TRUE; d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; d.DepthFunc = D3D11_COMPARISON_ALWAYS;
        dev->CreateDepthStencilState(&d, dsWrite.put());
        d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; d.DepthFunc = D3D11_COMPARISON_EQUAL;
        dev->CreateDepthStencilState(&d, dsEqual.put());
        d.DepthEnable = FALSE;
        dev->CreateDepthStencilState(&d, dsOff.put());
    }
    Com<ID3D11BlendState> bsPlain, bsAdd, bsOver;
    {
        D3D11_BLEND_DESC d = {};
        D3D11_RENDER_TARGET_BLEND_DESC &r = d.RenderTarget[0];
        r.SrcBlend = D3D11_BLEND_ONE; r.DestBlend = D3D11_BLEND_ZERO; r.BlendOp = D3D11_BLEND_OP_ADD;
        r.SrcBlendAlpha = D3D11_BLEND_ONE; r.DestBlendAlpha = D3D11_BLEND_ZERO; r.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        r.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        dev->CreateBlendState(&d, bsPlain.put());
        r.BlendEnable = TRUE; r.DestBlend = D3D11_BLEND_ONE;
        dev->CreateBlendState(&d, bsAdd.put());
        r.SrcBlend = D3D11_BLEND_SRC_ALPHA; r.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        dev->CreateBlendState(&d, bsOver.put());
    }
    Com<ID3D11RasterizerState> rs;
    {
        D3D11_RASTERIZER_DESC d = {};
        d.FillMode = D3D11_FILL_SOLID; d.CullMode = D3D11_CULL_NONE; d.DepthClipEnable = TRUE;
        dev->CreateRasterizerState(&d, rs.put());
    }
    ctx->RSSetState(rs.get());
    const D3D11_VIEWPORT vp = {0, 0, (float)W, (float)H, 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->IASetInputLayout(il.get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs.get(), nullptr, 0);
    UINT stride = sizeof(Vtx), off = 0;
    ID3D11Buffer *vbp = vb.get();
    ctx->IASetVertexBuffers(0, 1, &vbp, &stride, &off);
    auto put = [&](const Vtx *v, int n) {
        D3D11_MAPPED_SUBRESOURCE m;
        ctx->Map(vb.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
        memcpy(m.pData, v, sizeof(Vtx) * n);
        ctx->Unmap(vb.get(), 0);
    };
    auto read = [&](std::vector<uint8_t> &out) {
        ctx->CopyResource(staging.get(), rt.get());
        D3D11_MAPPED_SUBRESOURCE m;
        out.resize((size_t)W * H * 4);
        ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m);
        for (int y = 0; y < H; y++)
            memcpy(&out[(size_t)y * W * 4], (const uint8_t *)m.pData + (size_t)y * m.RowPitch, (size_t)W * 4);
        ctx->Unmap(staging.get(), 0);
    };

    /* ---- 1. pos.z against the stored depth, and EQUAL with SV_Depth = stored */
    std::mt19937 rng(4242);
    std::uniform_real_distribution<float> u01(0.0f, 1.0f);
    long covered = 0, red = 0, blueMissing = 0, blueExtra = 0;
    ID3D11ShaderResourceView *none = nullptr;
    for (int t = 0; t < 400; t++) {
        Vtx v[3];
        const bool flat = (t % 4) == 0;             /* every fourth: one z at every vertex (a car part over its base pass) */
        const float zc = u01(rng);
        const float scale = (t % 3 == 0) ? 0.05f : (t % 3 == 1) ? 0.4f : 1.5f;
        const float cx = u01(rng) * 2 - 1, cy = u01(rng) * 2 - 1;
        for (int k = 0; k < 3; k++) {
            const float w = 0.5f + 3.0f * u01(rng);  /* perspective: random w */
            const float x = cx + (u01(rng) * 2 - 1) * scale, y = cy + (u01(rng) * 2 - 1) * scale;
            const float z = flat ? zc : u01(rng);
            v[k] = {x * w, y * w, z * w, w, 0, 0, 0, 0};
        }
        put(v, 3);
        const float clear[4] = {0, 0, 0, 0};
        /* the depth written by the triangle (depth cleared first) */
        ctx->PSSetShaderResources(0, 1, &none);
        ctx->ClearDepthStencilView(dsv.get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
        ctx->ClearRenderTargetView(rtv.get(), clear);
        ID3D11RenderTargetView *r = rtv.get();
        ctx->OMSetRenderTargets(1, &r, dsv.get());
        ctx->OMSetDepthStencilState(dsWrite.get(), 0);
        ctx->OMSetBlendState(bsPlain.get(), nullptr, 0xFFFFFFFFu);
        ctx->PSSetShader(psDepth.get(), nullptr, 0);
        ctx->Draw(3, 0);
        /* the check: red where pos.z != stored (depth test off) */
        ctx->OMSetRenderTargets(1, &r, dsvRead.get());
        ctx->OMSetDepthStencilState(dsOff.get(), 0);
        ID3D11ShaderResourceView *z = zSrv.get();
        ctx->PSSetShaderResources(0, 1, &z);
        ctx->PSSetShader(psCheck.get(), nullptr, 0);
        ctx->Draw(3, 0);
        std::vector<uint8_t> a;
        read(a);
        long cov = 0, rd = 0;
        for (size_t i = 0; i < a.size(); i += 4) {
            if (a[i + 3] != 0) {
                cov++;
                if (a[i] != 0) rd++;
            }
        }
        /* EQUAL with SV_Depth = stored: blue where it passes (must be everywhere covered) */
        ctx->ClearRenderTargetView(rtv.get(), clear);
        ctx->OMSetDepthStencilState(dsEqual.get(), 0);
        ctx->PSSetShader(psEqual.get(), nullptr, 0);
        ctx->Draw(3, 0);
        std::vector<uint8_t> b;
        read(b);
        long miss = 0, extra = 0;
        for (size_t i = 0; i < a.size(); i += 4) {
            const bool was = a[i + 3] != 0, now = b[i + 2] != 0;
            if (was && !now) miss++;
            if (!was && now) extra++;
        }
        covered += cov; red += rd; blueMissing += miss; blueExtra += extra;
        if ((rd != 0 || miss != 0 || extra != 0) && t < 400)
            printf("  triangle %d (%s, scale %.2f): %ld pixels, %ld with pos.z != stored, EQUAL missed %ld, extra %ld\n",
                   t, flat ? "flat z" : "sloped z", scale, cov, rd, miss, extra);
    }
    printf("1. %ld pixels of 400 triangles: pos.z != stored depth at %ld; EQUAL with SV_Depth = stored missed %ld,"
           " passed extra %ld\n", covered, red, blueMissing, blueExtra);

    /* ---- 2. the instance order */
    {
        Vtx v[6] = {
            {-0.5f, -0.5f, 0.5f, 1, 0, 0, 0, 0}, {0.5f, -0.5f, 0.5f, 1, 0, 0, 0, 0}, {0.0f, 0.5f, 0.5f, 1, 0, 0, 0, 0},
            {-0.5f, 0.5f, 0.5f, 1, 0, 0, 0, 0},  {0.5f, 0.5f, 0.5f, 1, 0, 0, 0, 0},  {0.0f, -0.5f, 0.5f, 1, 0, 0, 0, 0},
        };
        put(v, 6);
        const float clear[4] = {0, 0, 0, 0};
        ctx->PSSetShaderResources(0, 1, &none);
        ctx->ClearRenderTargetView(rtv.get(), clear);
        ID3D11RenderTargetView *r = rtv.get();
        ctx->OMSetRenderTargets(1, &r, nullptr);
        ctx->OMSetDepthStencilState(dsOff.get(), 0);
        ctx->OMSetBlendState(bsOver.get(), nullptr, 0xFFFFFFFFu);
        ctx->PSSetShader(psOrder.get(), nullptr, 0);
        ctx->DrawInstanced(6, 2, 0, 0);
        std::vector<uint8_t> a;
        read(a);
        const size_t i = ((size_t)(H / 2) * W + W / 2) * 4;    /* the centre: both triangles */
        printf("2. the overlap pixel: R %d G %d (instance-major: R 48 G 191; primitive-major: R 80 G 159)\n",
               a[i], a[i + 1]);
    }
    return 0;
}
