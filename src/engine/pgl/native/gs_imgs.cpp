/* gs_imgs.cpp - image packs and their images as RGBA pictures (gs_imgs.h). */
#include "engine/pgl/native/gs_imgs.h"

#include "engine/pgl/native/gs_tex1.h"
#include "engine/pgl/native/gs_tex_decode.h"
#include "engine/pgl/native/gs_tex_format.h"
#include "engine/pgl/native/gs_tex_layout.h"
#include "engine/pgl/native/gs_tex_space.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace gstex {
namespace {

constexpr size_t kPackHeader = 16;          /* "IMGS", 0, count, first image's offset */
constexpr size_t kEntrySize = 0x40;         /* name[0x3C], index */
constexpr size_t kTex1Header = 0x30;
constexpr size_t kRegsSize = 0x28;
constexpr size_t kXferSize = 0xC;

uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
void wr16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
void wr32(uint8_t *p, uint32_t v)
{
    wr16(p, v);
    wr16(p + 2, v >> 16);
}
void wr64(uint8_t *p, uint64_t v)
{
    wr32(p, (uint32_t)v);
    wr32(p + 4, (uint32_t)(v >> 32));
}
size_t align16(size_t n) { return (n + 15) & ~(size_t)15; }

bool fail(std::string *why, const std::string &text)
{
    if (why != nullptr)
        *why = text;
    return false;
}

/* log2 of the power of two >= n (n >= 1) */
uint32_t ceil_log2(uint32_t n)
{
    uint32_t l = 0;
    while ((1u << l) < n)
        l++;
    return l;
}

/* The settings every pack image has (gs_imgs.h); "" when it has them, else
 * what differs. */
std::string unkept_settings(const Tex1Set &set)
{
    if (set.tex.size() != 1)
        return std::to_string(set.tex.size()) + " textures";
    if (!set.variations.empty())
        return "CLUT variations";
    if (!set.anims.empty())
        return "a CLUT animation";
    const Tex0 t = Tex0::decode(set.tex[0].tex0);
    const Tex1 t1 = Tex1::decode(set.tex[0].tex1);
    const Clamp c = Clamp::decode(set.tex[0].clamp);
    if (t.tcc != 1 || t.tfx != 0)
        return "TCC " + std::to_string(t.tcc) + ", TFX " + std::to_string(t.tfx);
    if (t1.mxl != 0 || t1.lcm != 0 || t1.mmag != 1 || t1.mmin != 1)
        return "mip levels or other filters";
    if (c.wms != 2 || c.wmt != 2 || c.minu != 0 || c.minv != 0)
        return "a clamp other than a region from 0";
    return "";
}

} // namespace

int imgs_name_compare(const std::string &a, const std::string &b)
{
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; i++) {
        const int x = (signed char)a[i], y = (signed char)b[i];
        if (x != y)
            return x < y ? -1 : 1;
    }
    return a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0;
}

bool imgs_read(const uint8_t *f, size_t size, std::vector<ImgsImage> &out, std::string *why)
{
    out.clear();
    if (f == nullptr || size < kPackHeader || std::memcmp(f, "IMGS", 4) != 0)
        return fail(why, "not an image pack");
    const uint32_t count = rd32(f + 8), first = rd32(f + 12);
    /* the entries: the game's 0x40 bytes (name[0x3C], index), or an older
     * layout's 0x20 (name[0x1C], index: menu/disp*.imgs, menu/opening.imgs,
     * the arcade.imgs, race/disp.imgs, test.imgs - which the game never
     * loads); the table ends at the first image */
    size_t entrySize = kEntrySize;
    if (count != 0 && first >= kPackHeader && (first - kPackHeader) == (size_t)count * 0x20)
        entrySize = 0x20;
    if (first > size || count > (size - kPackHeader) / entrySize || kPackHeader + (size_t)count * entrySize > first)
        return fail(why, "its table does not fit the file");
    /* the images, each its size field to the next */
    std::vector<size_t> starts;
    for (size_t off = first; off + kTex1Header <= size && starts.size() <= count;) {
        starts.push_back(off);
        const uint32_t step = rd32(f + off + 0xC);
        if (step < kTex1Header)
            break;
        off += step;
    }
    Texa texa;                              /* pgl's: TA0 0, AEM 0, TA1 0x80 */
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = f + kPackHeader + (size_t)i * entrySize;
        ImgsImage img;
        img.name.assign((const char *)e, strnlen((const char *)e, entrySize - 4));
        const uint32_t index = rd32(e + entrySize - 4);
        if (index >= starts.size())
            return fail(why, "the image of " + img.name + " is missing");
        Tex1Set set;
        std::string err;
        if (!tex1_parse(f, size, starts[index], set, &err))
            return fail(why, img.name + ": " + err);
        const std::string unkept = unkept_settings(set);
        if (!unkept.empty())
            return fail(why, img.name + " has " + unkept + ", which a PNG file does not keep");
        TexSpace space;
        tex1_layout(set, space);
        DecodedTexture d;
        if (!tex1_decode(set, space, 0, 0, texa, d))
            return fail(why, img.name + " does not decode");
        if (d.rect.x != 0 || d.rect.y != 0 || d.rect.w == 0 || d.rect.h == 0 || d.rect.w > kImgsSideMax ||
            d.rect.h > kImgsSideMax)
            return fail(why, img.name + ": an unexpected picture rectangle");
        for (uint32_t px : d.rgba)
            if ((px >> 24) > 0x80)
                return fail(why, img.name + " has alpha above 1.0 (0x80), which a PNG file does not keep");
        img.w = d.rect.w;
        img.h = d.rect.h;
        img.rgba.swap(d.rgba);
        /* a draw size the PC kept (tex1_build) */
        const uint8_t *hdr = f + starts[index];
        if (rd32(hdr + 0x2C) == kImgsDrawMagic) {
            img.drawW = (uint32_t)hdr[0x28] | ((uint32_t)hdr[0x29] << 8);
            img.drawH = (uint32_t)hdr[0x2A] | ((uint32_t)hdr[0x2B] << 8);
        }
        out.push_back(std::move(img));
    }
    return true;
}

bool tex1_build(const ImgsImage &img, std::vector<uint8_t> &out, std::string *why)
{
    const uint32_t w = img.w, h = img.h;
    if (w == 0 || h == 0)
        return fail(why, img.name + ": no pixels");
    if (w > kImgsSideMax || h > kImgsSideMax)
        return fail(why, img.name + ": " + std::to_string(w) + " x " + std::to_string(h) + " is larger than " +
                             std::to_string(kImgsSideMax) + " pixels a side");
    if (img.rgba.size() != (size_t)w * h)
        return fail(why, img.name + ": its pixels do not match its size");
    if (img.drawW > kImgsDrawMax || img.drawH > kImgsDrawMax)
        return fail(why, img.name + ": its draw size is larger than " + std::to_string(kImgsDrawMax));

    /* the colours, in their first appearance's order, while 256 or fewer */
    std::vector<uint32_t> palette;
    std::unordered_map<uint32_t, uint32_t> index;
    for (uint32_t c : img.rgba) {
        if (index.count(c) != 0)
            continue;
        if (palette.size() == 256) {
            palette.push_back(c);           /* (257: no palette) */
            break;
        }
        index.emplace(c, (uint32_t)palette.size());
        palette.push_back(c);
    }
    const uint32_t psm = palette.size() <= 16 ? PSMT4 : palette.size() <= 256 ? PSMT8 : PSMCT32;
    const bool indexed = psm != PSMCT32;

    Tex0 t;
    t.psm = psm;
    t.tw = ceil_log2(w);
    t.th = ceil_log2(h);
    uint32_t tbw = ((1u << t.tw) + 63u) / 64u;
    if (indexed)
        tbw = std::max(2u, (tbw + 1u) & ~1u); /* (T4 / T8 buffers: 128 pixels a step) */
    else
        tbw = std::max(1u, tbw);
    t.tbw = tbw;
    t.tcc = 1;
    t.cpsm = PSMCT32;
    t.cld = indexed ? 1 : 0;
    /* the upload: rows of whole 32-bit words (padding outside the clamp
     * region, never read) */
    const uint32_t uw = psm == PSMT4 ? (w + 7u) & ~7u : psm == PSMT8 ? (w + 3u) & ~3u : (w + 3u) & ~3u;
    /* the CLUT after the texture's blocks (the whole 2^TW x 2^TH, as the
     * originals have it) and the upload's */
    const BlockRange tb = texture_blocks(t);
    const BlockRange ub = image_blocks(psm, 0, tbw, 0, 0, uw, h);
    uint32_t last = std::max(tb.last, ub.last);
    if (indexed) {
        uint32_t cbp = last + 1;
        if (psm == PSMT8)
            cbp = (cbp + 3u) & ~3u;         /* (the 16 x 16 CLUT: a square of 4 blocks) */
        t.cbp = cbp;
        last = std::max(last, clut_blocks(t, false).last);
    }
    const uint32_t vramSize = last + 1;
    if (vramSize > GS_MEM_BLOCKS)
        return fail(why, img.name + ": too big for the GS memory");

    Tex1 t1;
    t1.mmag = 1;
    t1.mmin = 1;
    Clamp c;
    c.wms = 2;
    c.wmt = 2;
    c.maxu = w - 1;
    c.maxv = h - 1;

    /* the texels (host order: T4 two a byte, the left one in the low half) */
    std::vector<uint8_t> texels(image_bytes(psm, uw, h), 0);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            const uint32_t px = img.rgba[(size_t)y * w + x];
            if (psm == PSMT4) {
                const uint8_t v = (uint8_t)index[px];
                texels[((size_t)y * uw + x) / 2] |= (x & 1) ? (uint8_t)(v << 4) : v;
            } else if (psm == PSMT8) {
                texels[(size_t)y * uw + x] = (uint8_t)index[px];
            } else {
                wr32(&texels[((size_t)y * uw + x) * 4], px);
            }
        }
    /* the CLUT: T4 an 8 x 2 CT32 image in index order, T8 16 x 16 with entry
     * i at clut_csm1_position(i) */
    std::vector<uint8_t> clut;
    uint32_t clutW = 0, clutH = 0;
    if (indexed) {
        clutW = psm == PSMT4 ? 8 : 16;
        clutH = psm == PSMT4 ? 2 : 16;
        clut.assign((size_t)clutW * clutH * 4, 0);
        for (size_t i = 0; i < palette.size(); i++) {
            const uint32_t p = psm == PSMT8 ? clut_csm1_position((uint32_t)i) : (uint32_t)i;
            wr32(&clut[(size_t)p * 4], palette[i]);
        }
    }

    const uint32_t nXfer = indexed ? 2 : 1;
    const size_t regsAt = kTex1Header;
    const size_t xferAt = regsAt + kRegsSize;
    const size_t texelsAt = align16(xferAt + nXfer * kXferSize);
    const size_t clutAt = align16(texelsAt + texels.size());
    const size_t total = align16(indexed ? clutAt + clut.size() : texelsAt + texels.size());

    const size_t base = align16(out.size());
    out.resize(base + total, 0);
    uint8_t *s = out.data() + base;
    std::memcpy(s, "Tex1", 4);
    wr32(s + 0x0C, (uint32_t)total);
    wr16(s + 0x10, 0);
    wr16(s + 0x12, vramSize);
    wr16(s + 0x14, 1);
    wr16(s + 0x16, nXfer);
    wr32(s + 0x18, (uint32_t)regsAt);
    wr32(s + 0x1C, (uint32_t)xferAt);
    /* the draw size (gs_imgs.h kImgsDrawMagic) */
    wr16(s + 0x28, img.drawW != 0 ? img.drawW : w);
    wr16(s + 0x2A, img.drawH != 0 ? img.drawH : h);
    wr32(s + 0x2C, kImgsDrawMagic);
    uint8_t *r = s + regsAt;
    wr64(r + 0x00, t.encode());
    wr64(r + 0x08, t1.encode());
    wr64(r + 0x20, c.encode());
    uint8_t *x = s + xferAt;
    wr32(x + 0, (uint32_t)texelsAt);
    wr16(x + 4, 0);
    x[6] = (uint8_t)tbw;
    x[7] = (uint8_t)psm;
    wr16(x + 8, uw);
    wr16(x + 10, h);
    std::memcpy(s + texelsAt, texels.data(), texels.size());
    if (indexed) {
        x += kXferSize;
        wr32(x + 0, (uint32_t)clutAt);
        wr16(x + 4, t.cbp);
        x[6] = 1;
        x[7] = (uint8_t)PSMCT32;
        wr16(x + 8, clutW);
        wr16(x + 10, clutH);
        std::memcpy(s + clutAt, clut.data(), clut.size());
    }
    return true;
}

std::string imgs_sizes_text(const std::vector<ImgsImage> &images)
{
    std::string s =
        "# The size each picture of this folder is drawn at, in the game's 640 x 448 screen pixels: its\r\n"
        "# original's.  A PNG of another size is drawn at this size (a sharper or coarser picture, the HUD\r\n"
        "# the same); a picture not listed is drawn at its PNG's size.  <picture> <width> <height>\r\n";
    for (const ImgsImage &img : images)
        s += img.name + " " + std::to_string(img.drawW != 0 ? img.drawW : img.w) + " " +
             std::to_string(img.drawH != 0 ? img.drawH : img.h) + "\r\n";
    return s;
}

std::vector<ImgsSize> imgs_sizes_parse(const std::string &text)
{
    std::vector<ImgsSize> out;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        const size_t hash = line.find('#');
        if (hash != std::string::npos)
            line.resize(hash);
        /* name, width, height: the last two numbers, the name what is before
         * them (names have no spaces on the discs, but may) */
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        uint32_t nums[2] = {0, 0};
        bool ok = true;
        for (int k = 1; k >= 0 && ok; k--) {
            size_t sp = line.find_last_of(" \t");
            if (sp == std::string::npos) {
                ok = false;
                break;
            }
            const std::string num = line.substr(sp + 1);
            ok = !num.empty() && num.size() <= 5 && num.find_first_not_of("0123456789") == std::string::npos;
            if (ok)
                nums[k] = (uint32_t)std::stoul(num);
            line.resize(sp);
            while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
                line.pop_back();
        }
        size_t lead = line.find_first_not_of(" \t");
        if (!ok || lead == std::string::npos || nums[0] == 0 || nums[1] == 0 || nums[0] > kImgsDrawMax ||
            nums[1] > kImgsDrawMax)
            continue;
        ImgsSize s;
        s.name = line.substr(lead);
        s.w = nums[0];
        s.h = nums[1];
        out.push_back(std::move(s));
    }
    return out;
}

bool imgs_build(std::vector<ImgsImage> images, std::vector<uint8_t> &out, std::string *why)
{
    out.clear();
    std::sort(images.begin(), images.end(),
              [](const ImgsImage &a, const ImgsImage &b) { return imgs_name_compare(a.name, b.name) < 0; });
    for (size_t i = 0; i < images.size(); i++) {
        const std::string &n = images[i].name;
        if (n.empty() || n.size() > kImgsNameMax || n.find('\0') != std::string::npos)
            return fail(why, "the name \"" + n + "\" is empty or longer than " + std::to_string(kImgsNameMax) +
                                 " characters");
        if (i > 0 && imgs_name_compare(images[i - 1].name, n) == 0)
            return fail(why, "two images are named " + n);
    }
    const size_t first = kPackHeader + images.size() * kEntrySize;
    out.resize(first, 0);
    std::memcpy(out.data(), "IMGS", 4);
    wr32(&out[8], (uint32_t)images.size());
    wr32(&out[12], (uint32_t)first);
    for (size_t i = 0; i < images.size(); i++) {
        uint8_t *e = &out[kPackHeader + i * kEntrySize];
        std::memcpy(e, images[i].name.data(), images[i].name.size());
        wr32(e + 0x3C, (uint32_t)i);
        if (!tex1_build(images[i], out, why))
            return false;
    }
    return true;
}

} /* namespace gstex */
