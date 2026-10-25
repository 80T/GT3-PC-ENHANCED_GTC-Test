/* overlay.cpp - drawing over the picture for GT3 PC's own panels
 * (overlay.h).  The frame time monitor's drawing, shared. */
#include "platform/overlay.h"
#include "platform/gpu.h"
#include "platform/log.h"

#include <windows.h>

#include <string.h>

namespace overlay {
namespace {

constexpr int kMaxVertices = 6 * 16384;

bool g_ready, g_failed;
ID3D11VertexShader *g_vs;
ID3D11PixelShader *g_ps;
ID3D11InputLayout *g_layout;
ID3D11Buffer *g_vb;
ID3D11BlendState *g_blend;
ID3D11RasterizerState *g_raster;
ID3D11DepthStencilState *g_depth;

struct Vertex {
    float x, y;                                 /* clip space */
    uint32_t color;
};
Vertex *g_v;                                    /* the mapped buffer while building */
int g_n;
int g_bw, g_bh;
float g_toX, g_toY;                             /* pixels to clip space */

const char kShader[] = R"(
struct VsOut { float4 pos : SV_Position; float4 color : COLOR; };
VsOut vs_main(float2 pos : POSITION, float4 color : COLOR)
{
    VsOut o;
    o.pos = float4(pos, 0.0, 1.0);
    o.color = color;
    return o;
}
float4 ps_main(VsOut i) : SV_Target { return i.color; }
)";

bool init()
{
    ID3D11Device *dev = gpu::device();
    ID3DBlob *code = nullptr;
    g_vs = gpu::compile_vs("overlay", kShader, "vs_main", &code);
    g_ps = gpu::compile_ps("overlay", kShader, "ps_main");
    if (g_vs == nullptr || g_ps == nullptr || code == nullptr)
        return false;
    const D3D11_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    dev->CreateInputLayout(il, 2, code->GetBufferPointer(), code->GetBufferSize(), &g_layout);
    code->Release();

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = (UINT)(sizeof(Vertex) * kMaxVertices);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    dev->CreateBuffer(&bd, nullptr, &g_vb);

    D3D11_BLEND_DESC b = {};
    b.RenderTarget[0].BlendEnable = TRUE;
    b.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    b.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    b.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    b.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    b.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    b.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    b.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&b, &g_blend);

    D3D11_RASTERIZER_DESC r = {};
    r.FillMode = D3D11_FILL_SOLID;
    r.CullMode = D3D11_CULL_NONE;
    r.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&r, &g_raster);

    D3D11_DEPTH_STENCIL_DESC d = {};
    d.DepthEnable = FALSE;
    d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc = D3D11_COMPARISON_ALWAYS;
    dev->CreateDepthStencilState(&d, &g_depth);

    return g_layout && g_vb && g_blend && g_raster && g_depth;
}

/* ---- a 5 x 7 font (rows top down, bit 4 the left column) */

const char kGlyphChars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ.:-/%()=<>+,'!?;_";
const uint8_t kGlyphs[][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},     /* 0 */
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},     /* 1 */
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},     /* 2 */
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},     /* 3 */
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},     /* 4 */
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},     /* 5 */
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},     /* 6 */
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},     /* 7 */
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},     /* 8 */
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},     /* 9 */
    {0x0E, 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11},     /* A */
    {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},     /* B */
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E},     /* C */
    {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C},     /* D */
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F},     /* E */
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},     /* F */
    {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F},     /* G */
    {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},     /* H */
    {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},     /* I */
    {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C},     /* J */
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11},     /* K */
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},     /* L */
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11},     /* M */
    {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11},     /* N */
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},     /* O */
    {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},     /* P */
    {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D},     /* Q */
    {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},     /* R */
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E},     /* S */
    {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},     /* T */
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},     /* U */
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04},     /* V */
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A},     /* W */
    {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11},     /* X */
    {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04},     /* Y */
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F},     /* Z */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C},     /* . */
    {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00},     /* : */
    {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00},     /* - */
    {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00},     /* / */
    {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03},     /* % */
    {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02},     /* ( */
    {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08},     /* ) */
    {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00},     /* = */
    {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02},     /* < */
    {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08},     /* > */
    {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00},     /* + */
    {0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08},     /* , */
    {0x04, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00},     /* ' */
    {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04},     /* ! */
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04},     /* ? */
    {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x04, 0x08},     /* ; */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F},     /* _ */
};
static_assert(sizeof(kGlyphs) / sizeof(kGlyphs[0]) == sizeof(kGlyphChars) - 1, "one glyph per character");

} // namespace

bool begin()
{
    if (g_failed)
        return false;
    if (!g_ready) {
        if (!init()) {
            g_failed = true;
            pclog::write("overlay: its D3D objects could not be made (no frame monitor or settings menu)");
            return false;
        }
        g_ready = true;
    }
    gpu::backbuffer_size(&g_bw, &g_bh);
    if (g_bw <= 0 || g_bh <= 0)
        return false;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(gpu::context()->Map(g_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return false;
    g_v = (Vertex *)m.pData;
    g_n = 0;
    g_toX = 2.0f / (float)g_bw;
    g_toY = 2.0f / (float)g_bh;
    return true;
}

void size(int *w, int *h)
{
    *w = g_bw;
    *h = g_bh;
}

void rect(float x, float y, float w, float h, uint32_t c)
{
    if (g_v == nullptr || w <= 0.0f || h <= 0.0f || g_n + 6 > kMaxVertices)
        return;
    const float x0 = x * g_toX - 1.0f, x1 = (x + w) * g_toX - 1.0f;
    const float y0 = 1.0f - y * g_toY, y1 = 1.0f - (y + h) * g_toY;
    Vertex *v = g_v + g_n;
    v[0] = {x0, y0, c};
    v[1] = {x1, y0, c};
    v[2] = {x0, y1, c};
    v[3] = {x0, y1, c};
    v[4] = {x1, y0, c};
    v[5] = {x1, y1, c};
    g_n += 6;
}

float text_width(const char *str, int s)
{
    return (float)(strlen(str) * 6 * s);
}

/* each row's runs of lit pixels as one rectangle */
float text(float x, float y, int s, const char *str, uint32_t c)
{
    for (; *str; str++, x += 6.0f * s) {
        char ch = *str;
        if (ch >= 'a' && ch <= 'z')
            ch = (char)(ch - 'a' + 'A');
        const char *p = strchr(kGlyphChars, ch);
        if (ch == 0 || p == nullptr)
            continue;
        const uint8_t *g = kGlyphs[p - kGlyphChars];
        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5;) {
                if (!(g[row] & (0x10 >> col))) {
                    col++;
                    continue;
                }
                int end = col;
                while (end < 5 && (g[row] & (0x10 >> end)))
                    end++;
                rect(x + (float)(col * s), y + (float)(row * s), (float)((end - col) * s), (float)s, c);
                col = end;
            }
        }
    }
    return x;
}

void end(ID3D11RenderTargetView *bb)
{
    ID3D11DeviceContext *dc = gpu::context();
    if (g_v == nullptr)
        return;
    dc->Unmap(g_vb, 0);
    g_v = nullptr;
    if (bb == nullptr || g_n == 0)
        return;
    const D3D11_VIEWPORT vp = {0.0f, 0.0f, (float)g_bw, (float)g_bh, 0.0f, 1.0f};
    dc->OMSetRenderTargets(1, &bb, nullptr);
    dc->RSSetViewports(1, &vp);
    dc->RSSetState(g_raster);
    dc->OMSetBlendState(g_blend, nullptr, 0xFFFFFFFFu);
    dc->OMSetDepthStencilState(g_depth, 0);
    dc->IASetInputLayout(g_layout);
    dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const UINT stride = sizeof(Vertex), offset = 0;
    dc->IASetVertexBuffers(0, 1, &g_vb, &stride, &offset);
    dc->VSSetShader(g_vs, nullptr, 0);
    dc->GSSetShader(nullptr, nullptr, 0);
    dc->PSSetShader(g_ps, nullptr, 0);
    dc->Draw((UINT)g_n, 0);
}

} // namespace overlay
