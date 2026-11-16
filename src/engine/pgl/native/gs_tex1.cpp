/* gs_tex1.cpp - 'Tex1' texture sets (see the header). */
#include "engine/pgl/native/gs_tex1.h"

#include <algorithm>
#include <cstring>

namespace gstex {

namespace {

inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }
inline float rdf(const uint8_t *p)
{
    uint32_t u = rd32(p);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

/* Resolves the pointer fields of a set: file form (offsets from base,
 * bounds-checked) or memory form (addresses). */
struct Resolver {
    const uint8_t *file = nullptr;
    size_t size = 0;
    size_t base = 0;
    bool memory = false;
    /* (tex1_from_copy) the register blocks and upload records copied */
    const uint8_t *texCopy = nullptr;
    const uint8_t *xferCopy = nullptr;

    /* len bytes at ptr; nullptr for a NULL pointer or out of bounds */
    const uint8_t *at(uint32_t ptr, size_t len) const
    {
        if (ptr == 0)
            return nullptr;
        if (memory)
            return reinterpret_cast<const uint8_t *>((uintptr_t)ptr);
        size_t o = base + ptr;
        if (o < base || o > size || len > size - o)
            return nullptr;
        return file + o;
    }
    /* bytes available from ptr (memory form: unknown, return want) */
    size_t avail(uint32_t ptr, size_t want) const
    {
        if (memory)
            return want;
        size_t o = base + ptr;
        if (ptr == 0 || o >= size)
            return 0;
        return std::min(want, size - o);
    }
};

bool fail(std::string *err, const char *msg)
{
    if (err)
        *err = msg;
    return false;
}

bool parse(const uint8_t *h, const Resolver &r, Tex1Set &out, std::string *err)
{
    if (std::memcmp(h, "Tex1", 4) != 0)
        return fail(err, "no Tex1 magic");
    out.size = rd32(h + 0x0C);
    out.vramBase = rd16(h + 0x10);
    out.vramSize = rd16(h + 0x12);
    uint32_t nTex = rd16(h + 0x14);
    uint32_t nXfer = rd16(h + 0x16);
    uint32_t pTex = rd32(h + 0x18);
    uint32_t pXfer = rd32(h + 0x1C);
    uint32_t pVar = rd32(h + 0x20);
    uint32_t pAnim = rd32(h + 0x24);

    out.tex.clear();
    out.uploads.clear();
    out.variations.clear();
    out.anims.clear();

    if (nTex != 0) {
        const uint8_t *t = r.texCopy != nullptr ? r.texCopy : r.at(pTex, (size_t)nTex * 0x28);
        if (t == nullptr)
            return fail(err, "register blocks out of the buffer");
        for (uint32_t i = 0; i < nTex; i++, t += 0x28) {
            TexRegs regs;
            regs.tex0 = rd64(t + 0x00);
            regs.tex1 = rd64(t + 0x08);
            regs.miptbp1 = rd64(t + 0x10);
            regs.miptbp2 = rd64(t + 0x18);
            regs.clamp = rd64(t + 0x20);
            out.tex.push_back(regs);
        }
    }
    if (nXfer != 0) {
        const uint8_t *x = r.xferCopy != nullptr ? r.xferCopy : r.at(pXfer, (size_t)nXfer * 0xC);
        if (x == nullptr)
            return fail(err, "upload records out of the buffer");
        for (uint32_t i = 0; i < nXfer; i++, x += 0xC) {
            Tex1Upload u;
            uint32_t pData = rd32(x);
            u.t.dbp = rd16(x + 4);
            u.t.dbw = x[6];
            u.t.dpsm = x[7];
            u.t.w = rd16(x + 8);
            u.t.h = rd16(x + 10);
            size_t want = image_bytes(u.t.dpsm, u.t.w, u.t.h);
            u.bytes = r.avail(pData, want);
            u.data = u.bytes ? r.at(pData, u.bytes) : nullptr;
            if (u.data == nullptr)
                u.bytes = 0;
            out.uploads.push_back(u);
        }
    }
    if (pVar != 0) {
        const uint8_t *v = r.at(pVar, 4);
        if (v != nullptr) {
            uint32_t n = rd32(v);
            const uint8_t *tbl = r.at(pVar, 4 + (size_t)n * 4);
            for (uint32_t k = 0; tbl != nullptr && k < n && k < 4096; k++) {
                std::vector<uint32_t> ents;
                uint32_t p = rd32(tbl + 4 + 4 * k);
                const uint8_t *rec = r.at(p, 4);
                if (rec != nullptr) {
                    uint32_t cnt = rd32(rec) & 0xFFFF;
                    const uint8_t *e = r.at(p, 4 + (size_t)cnt * 4);
                    for (uint32_t j = 0; e != nullptr && j < cnt; j++)
                        ents.push_back(rd32(e + 4 + 4 * j));
                }
                out.variations.push_back(std::move(ents));
            }
        }
    }
    if (pAnim != 0) {
        const uint8_t *a = r.at(pAnim, 8);
        if (a != nullptr) {
            uint32_t count = rd16(a);
            uint32_t pCh = rd32(a + 4);
            const uint8_t *ch = r.at(pCh, (size_t)count * 0x20);
            for (uint32_t k = 0; ch != nullptr && k < count; k++, ch += 0x20) {
                Tex1AnimChannel c;
                c.id = rd16(ch);
                c.mode = rd16(ch + 2);
                c.nColors = rd16(ch + 4);
                c.nKeys = rd16(ch + 6);
                c.dest = r.at(rd32(ch + 8), (size_t)c.nColors * 4);
                const uint8_t *keys = r.at(rd32(ch + 12), (size_t)c.nKeys * 8);
                for (uint32_t j = 0; keys != nullptr && j < c.nKeys; j++) {
                    Tex1Key key;
                    key.time = rdf(keys + 8 * j);
                    key.palette = r.at(rd32(keys + 8 * j + 4), (size_t)c.nColors * 4);
                    c.keys.push_back(key);
                }
                out.anims.push_back(std::move(c));
            }
        }
    }
    return true;
}

} /* namespace */

bool tex1_parse(const uint8_t *file, size_t size, size_t offset, Tex1Set &out, std::string *err)
{
    if (offset > size || size - offset < 0x30)
        return fail(err, "header out of the buffer");
    const uint8_t *h = file + offset;
    uint32_t self = rd32(h + 4);
    if (self > offset)
        return fail(err, "self offset before the buffer");
    Resolver r;
    r.file = file;
    r.size = size;
    r.base = offset - self;
    out.offset = offset;
    return parse(h, r, out, err);
}

bool tex1_from_memory(const void *img, Tex1Set &out)
{
    if (sizeof(void *) != 4 || img == nullptr)
        return false;
    Resolver r;
    r.memory = true;
    out.offset = 0;
    return parse(static_cast<const uint8_t *>(img), r, out, nullptr);
}

bool tex1_from_copy(const uint8_t *hdr, const uint8_t *tex, const uint8_t *xfer, Tex1Set &out)
{
    if (sizeof(void *) != 4 || hdr == nullptr)
        return false;
    Resolver r;
    r.memory = true;
    r.texCopy = rd32(hdr + 0x18) != 0 ? tex : nullptr;
    r.xferCopy = rd32(hdr + 0x1C) != 0 ? xfer : nullptr;
    out.offset = 0;
    return parse(hdr, r, out, nullptr);
}

std::vector<Tex1Set> tex1_scan(const uint8_t *file, size_t size)
{
    std::vector<Tex1Set> sets;
    if (size < 0x30)
        return sets;
    for (size_t o = 0; o + 0x30 <= size; o += 4) {
        if (file[o] != 'T' || std::memcmp(file + o, "Tex1", 4) != 0)
            continue;
        Tex1Set s;
        if (!tex1_parse(file, size, o, s))
            continue;
        /* a plausible set: some registers, uploads that fit */
        if (s.tex.empty() && s.uploads.empty())
            continue;
        bool ok = true;
        for (const TexRegs &t : s.tex)
            if (!psm_valid(Tex0::decode(t.tex0).psm))
                ok = false;
        for (const Tex1Upload &u : s.uploads)
            if (!psm_valid(u.t.dpsm) || u.data == nullptr)
                ok = false;
        if (ok)
            sets.push_back(std::move(s));
    }
    return sets;
}

void tex1_layout(const Tex1Set &set, TexSpace &space)
{
    space.reset(set.vramBase, std::max<uint32_t>(set.vramSize, 1));
    for (const Tex1Upload &u : set.uploads)
        if (u.data != nullptr)
            space.upload(u.t, u.data, u.bytes);
}

std::vector<size_t> tex1_texture_sources(const Tex1Set &set, size_t texIndex)
{
    std::vector<size_t> out;
    if (texIndex >= set.tex.size())
        return out;
    const TexRegs &regs = set.tex[texIndex];
    Tex0 t = Tex0::decode(regs.tex0);
    Tex1 t1 = Tex1::decode(regs.tex1);
    Clamp c = Clamp::decode(regs.clamp);
    Miptbp m1 = Miptbp::decode(regs.miptbp1), m2 = Miptbp::decode(regs.miptbp2);
    uint32_t span = std::max<uint32_t>(set.vramSize, 1);

    std::vector<uint8_t> want(span, 0);
    Rect r = decode_region(t, c);
    for (uint32_t level = 0; level <= std::min(t1.mxl, 6u); level++) {
        MipLevel ml = mip_level(t, m1, m2, level);
        image_block_mask(t.psm, ml.tbp, ml.tbw, r.x >> level, r.y >> level,
                         std::max(1u, r.w >> level), std::max(1u, r.h >> level), set.vramBase, want);
    }
    if (psm_indexed(t.psm)) {
        Tex0 t2 = pgl_clut_load(t);
        bool t8 = (t2.psm == PSMT8 || t2.psm == PSMT8H);
        image_block_mask(t2.cpsm == PSMCT24 ? PSMCT32 : t2.cpsm, t2.cbp, 1, 0, 0,
                         t8 ? 16 : 8, t8 ? 16 : 2, set.vramBase, want);
    }
    for (size_t i = 0; i < set.uploads.size(); i++) {
        const Tex1Upload &u = set.uploads[i];
        std::vector<uint8_t> have(span, 0);
        image_block_mask(u.t.dpsm, u.t.dbp, u.t.dbw, 0, 0, u.t.w, u.t.h, set.vramBase, have);
        for (uint32_t b = 0; b < span; b++)
            if (want[b] && have[b]) {
                out.push_back(i);
                break;
            }
    }
    return out;
}

bool tex1_select_variation(Tex1Set &set, int index)
{
    if (index < 0 || (size_t)index >= set.variations.size())
        return false;
    for (uint32_t e : set.variations[index]) {
        uint32_t t = e >> 23;
        if (t >= set.tex.size())
            continue;
        Tex0 t0 = Tex0::decode(set.tex[t].tex0);
        t0.cpsm = (e >> 19) & 0xF;
        t0.cbp = (((e >> 5) & 0x3FFF) + set.vramBase) & 0x3FFF;
        t0.csa = e & 0x1F;
        set.tex[t].tex0 = t0.encode();
    }
    return true;
}

bool tex1_decode(const Tex1Set &set, const TexSpace &space, size_t texIndex, uint32_t level,
                 const Texa &texa, DecodedTexture &out, bool full)
{
    if (texIndex >= set.tex.size())
        return false;
    const TexRegs &regs = set.tex[texIndex];
    Tex0 t = Tex0::decode(regs.tex0);
    Tex1 t1 = Tex1::decode(regs.tex1);
    Clamp c = Clamp::decode(regs.clamp);
    if (!psm_valid(t.psm) || level > std::min(t1.mxl, 6u))
        return false;
    Miptbp m1 = Miptbp::decode(regs.miptbp1), m2 = Miptbp::decode(regs.miptbp2);
    MipLevel ml = mip_level(t, m1, m2, level);

    Rect r;
    if (full) {
        r.w = t.width();
        r.h = t.height();
    } else {
        r = decode_region(t, c);
    }
    r.x >>= level;
    r.y >>= level;
    r.w = std::max(1u, r.w >> level);
    r.h = std::max(1u, r.h >> level);

    ClutBuffer clut;
    if (psm_indexed(t.psm))
        clut.load(space, pgl_clut_load(t));

    out.rect = r;
    out.level = level;
    out.rgba.assign((size_t)r.w * r.h, 0);
    decode_rect(space, t.psm, ml.tbp, ml.tbw, r.x, r.y, r.w, r.h,
                psm_indexed(t.psm) ? &clut : nullptr, t.csa, texa, out.rgba.data(), r.w);
    return true;
}

} /* namespace gstex */
