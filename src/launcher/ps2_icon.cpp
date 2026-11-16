/* ps2_icon.cpp - a PS2 save's 3D icon as a picture (ps2_icon.h).
 * PC (2026-11-15).  The file layout and the drawing are T077's
 * (src/game/menu/memorycard/T077_memorycard_icon.cpp): icon_model_size's
 * checks, icon_model_build's unpacking, icon_texture_unpack,
 * icon_anim_weights at time 0, icon_light_vertex, icon_model_render's view. */
#include "ps2_icon.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ps2_icon {
namespace {

constexpr int kSamples = 4;                     /* a pixel's samples across (4 x 4) */
constexpr int kTex = 128;                       /* the texture: 128 x 128 16-bit texels */

/* The file read with its size checked at every step. */
struct Reader {
    const uint8_t *p;
    size_t n, at = 0;
    bool ok = true;

    bool need(size_t k)
    {
        if (!ok || k > n || at > n - k)
            ok = false;
        return ok;
    }
    uint32_t u32()
    {
        uint32_t v = 0;
        if (need(4)) {
            memcpy(&v, p + at, 4);
            at += 4;
        }
        return v;
    }
    float f32()
    {
        float v = 0.0f;
        if (need(4)) {
            memcpy(&v, p + at, 4);
            at += 4;
        }
        return v;
    }
    int16_t s16()
    {
        int16_t v = 0;
        if (need(2)) {
            memcpy(&v, p + at, 2);
            at += 2;
        }
        return v;
    }
    void skip(size_t k)
    {
        if (need(k))
            at += k;
    }
};

struct Key {
    float time, value;
};
struct Frame {
    uint32_t shape;
    std::vector<Key> keys;
};

/* icon_anim_key_value */
float key_value(const Frame &f, float t)
{
    if (f.keys.empty())
        return 1.0f;
    for (size_t i = 0; i < f.keys.size(); i++)
        if (t < f.keys[i].time) {
            if (i == 0)
                return f.keys[i].value;
            const Key &a = f.keys[i - 1], &b = f.keys[i];
            const float u = (t - a.time) / (b.time - a.time);
            return a.value * (1.0f - u) + b.value * u;
        }
    return f.keys.back().value;
}

struct Vertex {
    float x, y, z;                              /* the morphed position, the file's units */
    float r, g, b;                              /* lit colour, 0..255 (0x80 = 1.0 under MODULATE) */
    float u, v;                                 /* texels */
};

struct Projected {
    float sx, sy, depth;                        /* samples; depth: the distance from the eye */
};

} // namespace

bool render(const std::vector<uint8_t> &ico, const std::vector<uint8_t> *iconSys, int size,
            std::vector<uint32_t> *bgra)
{
    if (size <= 0 || bgra == nullptr)
        return false;
    Reader in{ico.data(), ico.size()};

    /* icon_model_size's checks */
    if (in.u32() != 0x10000)
        return false;
    const uint32_t shapes = in.u32();
    if (!(shapes == 1 || shapes == 2 || shapes == 4 || shapes == 6 || shapes == 8))
        return false;
    const uint32_t texType = in.u32();
    in.u32();                                   /* (unused) */
    const uint32_t verts = in.u32();
    if (!in.ok || verts < 3 || verts % 3 != 0)
        return false;
    const uint32_t tris = verts / 3;
    const uint32_t maxTris = shapes == 1 ? 600 : shapes == 2 ? 550 : shapes == 4 ? 500 : shapes == 6 ? 450 : 400;
    if (tris > maxTris)
        return false;

    /* the vertices: the shapes' positions, the normal, (u, v), the colour */
    std::vector<int16_t> pos((size_t)verts * shapes * 3);
    std::vector<float> normal((size_t)verts * 3);
    std::vector<int16_t> uv((size_t)verts * 2);
    std::vector<uint32_t> rgba(verts);
    for (uint32_t i = 0; i < verts && in.ok; i++) {
        for (uint32_t j = 0; j < shapes; j++) {
            int16_t *q = &pos[((size_t)i * shapes + j) * 3];
            q[0] = in.s16();
            q[1] = in.s16();
            q[2] = in.s16();
            in.s16();                           /* w */
        }
        normal[(size_t)i * 3 + 0] = in.s16() / 4096.0f;
        normal[(size_t)i * 3 + 1] = in.s16() / 4096.0f;
        normal[(size_t)i * 3 + 2] = in.s16() / 4096.0f;
        in.s16();
        uv[(size_t)i * 2 + 0] = in.s16();
        uv[(size_t)i * 2 + 1] = in.s16();
        rgba[i] = in.u32();
    }

    /* the animations: the first one kept */
    const uint32_t animCount = in.u32();
    if (!in.ok || animCount == 0 || animCount > 0x100)
        return false;
    uint32_t frameLength = 0;
    float speed = 0.0f, offset = 0.0f;
    std::vector<Frame> frames;
    for (uint32_t a = 0; a < animCount && in.ok; a++) {
        const uint32_t len = in.u32();
        const float sp = in.f32();
        const int32_t off = (int32_t)in.u32();
        const uint32_t count = in.u32();
        if (count > 8)
            return false;
        for (uint32_t f = 0; f < count && in.ok; f++) {
            Frame fr;
            fr.shape = in.u32();
            const uint32_t keys = in.u32();
            if (keys > 10)
                return false;
            for (uint32_t k = 0; k < keys && in.ok; k++) {
                Key key;
                key.time = in.f32();
                key.value = in.f32();
                fr.keys.push_back(key);
            }
            if (a == 0)
                frames.push_back(fr);
        }
        if (a == 0) {
            frameLength = len;
            speed = sp;
            offset = (float)off;
        }
    }
    (void)speed;                                /* (time 0: the speed does not count) */

    /* the texture: raw, or RLE (icon_texture_unpack) */
    std::vector<uint16_t> tex;
    const bool textured = (texType & 4) != 0;
    if (textured) {
        tex.assign((size_t)kTex * kTex, 0);
        if (texType & 8) {
            const uint32_t packed = in.u32();
            if (!in.ok || packed > 0x10004 || !in.need(packed & ~1u))
                return false;
            const uint8_t *src = in.p + in.at, *end = src + (packed & ~1u);
            size_t d = 0;
            auto get = [&](int16_t *v) {
                if (end - src < 2)
                    return false;
                memcpy(v, src, 2);
                src += 2;
                return true;
            };
            int16_t n = 0, c = 0;
            while (d < tex.size() && get(&n)) {
                if (n > 0) {
                    if (!get(&c))
                        break;
                    for (int k = 0; k < n && d < tex.size(); k++)
                        tex[d++] = (uint16_t)c | 0x8000;
                } else if (n < 0) {
                    for (int k = 0; k < -n && d < tex.size() && get(&c); k++)
                        tex[d++] = (uint16_t)c | 0x8000;
                }
            }
        } else {
            if (!in.need(tex.size() * 2))
                return false;
            memcpy(tex.data(), in.p + in.at, tex.size() * 2);
        }
    }

    /* the shapes' weights at time 0 (icon_anim_weights / icon_anim_time) */
    float weight[8] = {};
    {
        float t = offset;
        if (frameLength > 0)
            t -= (float)((int)(t / (float)frameLength) * (int)frameLength);
        float sum = 0.0f;
        for (const Frame &f : frames) {
            const float v = key_value(f, t);
            sum += v;
            if (f.shape < 8)
                weight[f.shape] = v;
        }
        if (sum <= 0.0f) {
            std::fill(weight, weight + 8, 0.0f);
            weight[0] = 1.0f;
        } else {
            for (float &w : weight)
                w /= sum;
        }
        if (shapes == 1) {
            std::fill(weight, weight + 8, 0.0f);
            weight[0] = 1.0f;
        }
    }

    /* the lights (icon_light_init, icon_light_vertex) */
    bool lit = iconSys != nullptr && iconSys->size() >= 0xC0;
    float dir[3][3] = {}, col[3][3] = {}, amb[3] = {};
    if (lit) {
        const uint8_t *s = iconSys->data();
        for (int i = 0; i < 3; i++) {
            float d[3], c[3];
            memcpy(d, s + 0x50 + i * 16, 12);
            memcpy(c, s + 0x80 + i * 16, 12);
            const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            for (int k = 0; k < 3; k++) {
                dir[i][k] = len > 0.0f ? d[k] / len : 0.0f;
                col[i][k] = c[k];
            }
        }
        memcpy(amb, s + 0xB0, 12);
        for (float v : {amb[0], amb[1], amb[2], dir[0][0], col[0][0]})
            if (!std::isfinite(v))
                lit = false;
    }

    std::vector<Vertex> vx(verts);
    for (uint32_t i = 0; i < verts; i++) {
        Vertex &v = vx[i];
        float p[3] = {};
        for (uint32_t j = 0; j < shapes && j < 8; j++)
            for (int k = 0; k < 3; k++)
                p[k] += weight[j] * pos[((size_t)i * shapes + j) * 3 + k];
        v.x = p[0];
        v.y = p[1];
        v.z = p[2];
        const float cr = (float)(rgba[i] & 0xFF), cg = (float)((rgba[i] >> 8) & 0xFF),
                    cb = (float)((rgba[i] >> 16) & 0xFF);
        if (lit) {
            float r = amb[0], g = amb[1], b = amb[2];
            const float *n = &normal[(size_t)i * 3];
            for (int l = 0; l < 3; l++) {
                const float facing = -(n[0] * dir[l][0] + n[1] * dir[l][1] + n[2] * dir[l][2]);
                if (facing > 0.0f) {
                    r += facing * col[l][0];
                    g += facing * col[l][1];
                    b += facing * col[l][2];
                }
            }
            v.r = std::min(255.0f, std::max(0.0f, r * cr));
            v.g = std::min(255.0f, std::max(0.0f, g * cg));
            v.b = std::min(255.0f, std::max(0.0f, b * cb));
        } else {
            v.r = cr;
            v.g = cg;
            v.b = cb;
        }
        v.u = uv[(size_t)i * 2 + 0] / 4096.0f * kTex;
        v.v = uv[(size_t)i * 2 + 1] / 4096.0f * kTex;
    }

    /* the view (icon_model_render, icon_model_draw): eye = (x / 1024,
     * -y / 1024 - 10, -z / 1024 - 50); +-11 seen at 50 */
    const int R = size * kSamples;
    std::vector<Projected> pr(verts);
    for (uint32_t i = 0; i < verts; i++) {
        const float ex = vx[i].x / 1024.0f, ey = -vx[i].y / 1024.0f - 10.0f, ez = -vx[i].z / 1024.0f - 50.0f;
        const float d = std::max(0.01f, -ez);
        const float nx = ex * 50.0f / d / 11.0f, ny = ey * 50.0f / d / 11.0f;
        pr[i].sx = (nx + 1.0f) * 0.5f * R;
        pr[i].sy = (1.0f - ny) * 0.5f * R;
        pr[i].depth = d;
    }

    /* the samples: colour (premultiplied, alpha 255 where drawn) and depth */
    std::vector<uint32_t> sample((size_t)R * R, 0);
    std::vector<float> depth((size_t)R * R, 1e30f);
    for (uint32_t t = 0; t < tris; t++) {
        const uint32_t i0 = t * 3, i1 = i0 + 1, i2 = i0 + 2;
        const Projected &a = pr[i0], &b = pr[i1], &c = pr[i2];
        const float area = (b.sx - a.sx) * (c.sy - a.sy) - (b.sy - a.sy) * (c.sx - a.sx);
        if (std::fabs(area) < 1e-6f)
            continue;
        const int x0 = std::max(0, (int)std::floor(std::min({a.sx, b.sx, c.sx})));
        const int x1 = std::min(R - 1, (int)std::ceil(std::max({a.sx, b.sx, c.sx})));
        const int y0 = std::max(0, (int)std::floor(std::min({a.sy, b.sy, c.sy})));
        const int y1 = std::min(R - 1, (int)std::ceil(std::max({a.sy, b.sy, c.sy})));
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                const float px = x + 0.5f, py = y + 0.5f;
                float w0 = ((b.sx - px) * (c.sy - py) - (b.sy - py) * (c.sx - px)) / area;
                float w1 = ((c.sx - px) * (a.sy - py) - (c.sy - py) * (a.sx - px)) / area;
                float w2 = 1.0f - w0 - w1;
                if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
                    continue;
                const float z = w0 * a.depth + w1 * b.depth + w2 * c.depth;
                float &dz = depth[(size_t)y * R + x];
                if (z >= dz)
                    continue;
                dz = z;
                const Vertex &A = vx[i0], &B = vx[i1], &C = vx[i2];
                float r = w0 * A.r + w1 * B.r + w2 * C.r;
                float g = w0 * A.g + w1 * B.g + w2 * C.g;
                float bl = w0 * A.b + w1 * B.b + w2 * C.b;
                if (textured) {
                    const float u = w0 * A.u + w1 * B.u + w2 * C.u, v = w0 * A.v + w1 * B.v + w2 * C.v;
                    const int s = std::min(kTex - 1, std::max(0, (int)std::floor(u)));
                    const int q = std::min(kTex - 1, std::max(0, (int)std::floor(v)));
                    const uint16_t texel = tex[(size_t)q * kTex + s];
                    /* MODULATE: texel x colour / 128 */
                    r = (float)((texel & 31) << 3) * r / 128.0f;
                    g = (float)(((texel >> 5) & 31) << 3) * g / 128.0f;
                    bl = (float)(((texel >> 10) & 31) << 3) * bl / 128.0f;
                }
                const uint32_t R8 = (uint32_t)std::min(255.0f, std::max(0.0f, r));
                const uint32_t G8 = (uint32_t)std::min(255.0f, std::max(0.0f, g));
                const uint32_t B8 = (uint32_t)std::min(255.0f, std::max(0.0f, bl));
                sample[(size_t)y * R + x] = 0xFF000000u | R8 << 16 | G8 << 8 | B8;
            }
    }

    /* each pixel: its samples averaged (premultiplied BGRA) */
    bgra->assign((size_t)size * size, 0);
    const int n = kSamples * kSamples;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            uint32_t sa = 0, sr = 0, sg = 0, sb = 0;
            for (int j = 0; j < kSamples; j++)
                for (int i = 0; i < kSamples; i++) {
                    const uint32_t c = sample[(size_t)(y * kSamples + j) * R + (x * kSamples + i)];
                    sa += c >> 24;
                    sr += (c >> 16) & 0xFF;
                    sg += (c >> 8) & 0xFF;
                    sb += c & 0xFF;
                }
            (*bgra)[(size_t)y * size + x] = (sa / n) << 24 | (sr / n) << 16 | (sg / n) << 8 | (sb / n);
        }
    return true;
}

} // namespace ps2_icon
