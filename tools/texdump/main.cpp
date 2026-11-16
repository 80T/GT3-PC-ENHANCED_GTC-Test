/* texdump - decodes GT3's textures to PNG with the port's texture library
 * (src/engine/pgl/native/gs_tex*): 'Tex1' sets in any file (loose .img,
 * .imgs packs, .pmb menu layouts with their gzipped textures, fonts, car /
 * course / wheel / tyre models), the kanji font and the memory card icons.
 *
 *   texdump selftest
 *   texdump memtest FILE                    the game (relocated, in-memory) form of the API
 *                                           against the file form (32-bit builds)
 *   texdump scan  FILE                      the Tex1 sets of FILE and their registers
 *   texdump tex1  FILE OUTDIR [options]     every Tex1 set found in FILE
 *   texdump imgs  FILE.imgs OUTDIR [options]  a named image pack
 *   texdump pmb   FILE.pmb OUTDIR [options]  a menu layout's textures (gunzipped)
 *   texdump font  FILE.fnt OUTDIR [options]  a font set: every face, every palette
 *   texdump kanji FILE.kf OUTDIR [N] [--palette P]  the first N glyphs (1024) as a sheet
 *   texdump ico   FILE.ico OUTDIR             a memory card icon's texture
 *   options: --full      the whole 2^TW x 2^TH texture space, not the clamp region
 *            --view      composite the alpha over a grey checkerboard (RGB PNG)
 *            --raw-alpha keep the GS alpha (0x80 = 1.0) in the PNG
 *            --mips      also the mip levels
 *            --vram      also each set's block space read as PSMCT32 (64 wide)
 *            --variations  also every variation (CLUT / picture) of each set
 *            --dry       decode everything, write nothing (a sweep)
 *
 * PNG alpha is the GS alpha doubled (0x80 -> 255) unless --raw-alpha; a
 * texture with TCC 0 (RGB) is written opaque.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "engine/pgl/native/gs_imgs.h"        /* PC (2026-11-08): imgsround */
#include "engine/pgl/native/gs_tex1.h"
#include "engine/pgl/native/gs_tex_decode.h"
#include "engine/pgl/native/gs_tex_format.h"
#include "engine/pgl/native/gs_tex_layout.h"
#include "engine/pgl/native/gs_tex_space.h"
#include "inflate.h"
#include "png_write.h"

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

using namespace gstex;

namespace {

struct Options {
    bool full = false;
    bool view = false;
    bool rawAlpha = false;
    bool mips = false;
    bool vram = false;
    bool variations = false;
    bool dry = false;           /* decode, write nothing (a sweep) */
    int palette = 0;
};

uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool read_file(const std::string &path, std::vector<uint8_t> &out)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr)
        return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? (size_t)n : 0);
    bool ok = out.empty() || std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

std::string base_name(const std::string &path)
{
    size_t s = path.find_last_of("/\\");
    std::string b = (s == std::string::npos) ? path : path.substr(s + 1);
    for (char &c : b)
        if (c == '.' || c == ' ')
            c = '_';
    return b;
}

std::string safe(const std::string &s)
{
    std::string o;
    for (char c : s)
        o += (c >= 32 && c < 127 && c != '/' && c != '\\' && c != ':' && c != '*' && c != '?' &&
              c != '"' && c != '<' && c != '>' && c != '|') ? c : '_';
    return o;
}

/* RGBA (GS alpha) -> the PNG's pixels per the options. */
void write_png(const std::string &path, std::vector<uint32_t> px, uint32_t w, uint32_t h,
               const Options &o, bool opaque)
{
    if (o.dry)
        return;
    for (uint32_t &p : px) {
        if (opaque)
            p |= 0xFF000000u;
        else if (!o.rawAlpha)
            p = alpha_to_unorm(p);
    }
    if (o.view) {
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                uint32_t &p = px[(size_t)y * w + x];
                uint32_t a = p >> 24;
                uint32_t bg = (((x >> 3) ^ (y >> 3)) & 1) ? 0x99 : 0x66;
                uint32_t r = ((p & 0xFF) * a + bg * (255 - a)) / 255;
                uint32_t g = (((p >> 8) & 0xFF) * a + bg * (255 - a)) / 255;
                uint32_t b = (((p >> 16) & 0xFF) * a + bg * (255 - a)) / 255;
                p = r | (g << 8) | (b << 16) | 0xFF000000u;
            }
    }
    if (!png_write(path, px.data(), w, h, !o.view))
        std::fprintf(stderr, "texdump: cannot write %s\n", path.c_str());
}

void print_set(const Tex1Set &s, const char *name)
{
    std::printf("%s @%zX size %X vramBase %X vramSize %X tex %zu uploads %zu variations %zu anims %zu\n",
                name, s.offset, s.size, s.vramBase, s.vramSize, s.tex.size(), s.uploads.size(),
                s.variations.size(), s.anims.size());
    for (size_t i = 0; i < s.tex.size(); i++) {
        Tex0 t = Tex0::decode(s.tex[i].tex0);
        Tex1 t1 = Tex1::decode(s.tex[i].tex1);
        Clamp c = Clamp::decode(s.tex[i].clamp);
        std::printf("  tex%-3zu TBP %04X TBW %-2u %-5s %4ux%-4u TCC %u TFX %u", i, t.tbp0, t.tbw,
                    psm_name(t.psm), t.width(), t.height(), t.tcc, t.tfx);
        if (psm_indexed(t.psm))
            std::printf(" CBP %04X %s CSM%u CSA %-2u CLD %u", t.cbp, psm_name(t.cpsm), t.csm + 1, t.csa, t.cld);
        std::printf(" | MXL %u MAG %u MIN %u K %d/16 | WMS %u WMT %u U %u..%u V %u..%u", t1.mxl, t1.mmag,
                    t1.mmin, t1.k, c.wms, c.wmt, c.minu, c.maxu, c.minv, c.maxv);
        std::vector<size_t> src = tex1_texture_sources(s, i);
        std::printf(" | uploads");
        for (size_t k : src)
            std::printf(" %zu", k);
        std::printf("\n");
    }
    for (size_t i = 0; i < s.uploads.size(); i++) {
        const Tex1Upload &u = s.uploads[i];
        std::printf("  up%-3zu DBP %04X DBW %-2u %-5s %4ux%-4u %zu bytes\n", i, u.t.dbp, u.t.dbw,
                    psm_name(u.t.dpsm), u.t.w, u.t.h, u.bytes);
    }
    for (size_t k = 0; k < s.anims.size(); k++)
        std::printf("  anim%zu id %u mode %u colours %u keys %u\n", k, s.anims[k].id, s.anims[k].mode,
                    s.anims[k].nColors, s.anims[k].nKeys);
}

/* Every texture of a set (and its mips / variations) as PNG; returns the
 * textures written. */
int dump_set(Tex1Set set, const std::string &outPrefix, const Options &o)
{
    int written = 0;
    Texa texa;                                  /* pgl's TEXA: TA0 0, AEM 0, TA1 0x80 */
    size_t nVar = o.variations ? set.variations.size() : 0;

    for (size_t v = 0; v <= nVar; v++) {
        std::string varTag;
        if (v > 0) {
            tex1_select_variation(set, (int)v - 1);
            varTag = "_var" + std::to_string(v - 1);
        }
        TexSpace space;
        tex1_layout(set, space);
        if (o.vram && v == 0) {
            /* the block space as PSMCT32, 64 pixels wide (a page per 32 rows) */
            uint32_t rows = space.blockCount() * 2;    /* 32 blocks = 64x32: 2 rows a block */
            std::vector<uint32_t> px((size_t)64 * rows);
            decode_rect(space, PSMCT32, space.firstBlock(), 1, 0, 0, 64, rows, nullptr, 0, texa,
                        px.data(), 64);
            write_png(outPrefix + "_vram.png", px, 64, rows, o, true);
        }
        for (size_t i = 0; i < set.tex.size(); i++) {
            Tex0 t = Tex0::decode(set.tex[i].tex0);
            Tex1 t1 = Tex1::decode(set.tex[i].tex1);
            uint32_t levels = o.mips ? std::min(t1.mxl, 6u) : 0;
            for (uint32_t level = 0; level <= levels; level++) {
                DecodedTexture d;
                if (!tex1_decode(set, space, i, level, texa, d, o.full))
                    continue;
                char name[64];
                std::snprintf(name, sizeof(name), "_t%zu_%s%s", i, psm_name(t.psm),
                              level ? ("_mip" + std::to_string(level)).c_str() : "");
                write_png(outPrefix + name + varTag + ".png", d.rgba, d.rect.w, d.rect.h, o, t.tcc == 0);
                written++;
            }
        }
    }
    return written;
}

int cmd_scan(const std::string &path)
{
    std::vector<uint8_t> f;
    if (!read_file(path, f)) {
        std::fprintf(stderr, "texdump: cannot read %s\n", path.c_str());
        return 1;
    }
    std::vector<Tex1Set> sets = tex1_scan(f.data(), f.size());
    for (const Tex1Set &s : sets)
        print_set(s, path.c_str());
    std::printf("%zu sets\n", sets.size());
    return 0;
}

int cmd_tex1(const std::string &path, const std::string &out, const Options &o)
{
    std::vector<uint8_t> f;
    if (!read_file(path, f)) {
        std::fprintf(stderr, "texdump: cannot read %s\n", path.c_str());
        return 1;
    }
    MKDIR(out.c_str());
    std::vector<Tex1Set> sets = tex1_scan(f.data(), f.size());
    int n = 0;
    for (const Tex1Set &s : sets) {
        char tag[32];
        std::snprintf(tag, sizeof(tag), "_%06zX", s.offset);
        n += dump_set(s, out + "/" + base_name(path) + tag, o);
    }
    std::printf("%s: %zu sets, %d textures\n", path.c_str(), sets.size(), n);
    return 0;
}

/* .imgs: "IMGS", 4 bytes, count, the first image's offset, then count
 * entries {name[0x3C], index} sorted by name; the images follow each other
 * by their size field (T506 image_pack_*). */
int cmd_imgs(const std::string &path, const std::string &out, const Options &o)
{
    std::vector<uint8_t> f;
    if (!read_file(path, f) || f.size() < 16 || std::memcmp(f.data(), "IMGS", 4) != 0) {
        std::fprintf(stderr, "texdump: %s is not an image pack\n", path.c_str());
        return 1;
    }
    MKDIR(out.c_str());
    uint32_t count = rd32(&f[8]);
    uint32_t first = rd32(&f[12]);
    std::vector<size_t> starts;
    for (size_t off = first; off + 0x30 <= f.size();) {
        starts.push_back(off);
        uint32_t size = rd32(&f[off + 0xC]);
        if (size == 0)
            break;
        off += size;
    }
    int n = 0;
    for (uint32_t i = 0; i < count && 16 + (size_t)(i + 1) * 0x40 <= f.size(); i++) {
        const uint8_t *e = &f[16 + (size_t)i * 0x40];
        std::string name((const char *)e, strnlen((const char *)e, 0x3C));
        uint32_t index = rd32(e + 0x3C);
        if (index >= starts.size())
            continue;
        Tex1Set s;
        std::string err;
        if (!tex1_parse(f.data(), f.size(), starts[index], s, &err)) {
            std::fprintf(stderr, "texdump: %s: %s: %s\n", path.c_str(), name.c_str(), err.c_str());
            continue;
        }
        n += dump_set(s, out + "/" + base_name(path) + "_" + safe(name), o);
    }
    std::printf("%s: %u images, %d textures\n", path.c_str(), count, n);
    return 0;
}

/* .pmb: {pageCount, pages, textureCount, textures}; a texture entry {size,
 * data}: a Tex1 image, gzipped (MenuImageHolder, T226) or not (T243). */
int cmd_pmb(const std::string &path, const std::string &out, const Options &o)
{
    std::vector<uint8_t> f;
    if (!read_file(path, f) || f.size() < 16) {
        std::fprintf(stderr, "texdump: cannot read %s\n", path.c_str());
        return 1;
    }
    MKDIR(out.c_str());
    uint32_t texCount = rd32(&f[8]);
    uint32_t texTable = rd32(&f[12]);
    int n = 0;
    for (uint32_t i = 0; i < texCount && texTable + (size_t)(i + 1) * 8 <= f.size(); i++) {
        uint32_t size = rd32(&f[texTable + i * 8]);
        uint32_t data = rd32(&f[texTable + i * 8 + 4]);
        if (data + 4 > f.size())
            continue;
        std::vector<uint8_t> img;
        if (f[data] == 0x1F && f[data + 1] == 0x8B) {
            if (!gunzip(&f[data], f.size() - data, img)) {
                std::fprintf(stderr, "texdump: %s: texture %u: bad gzip\n", path.c_str(), i);
                continue;
            }
        } else {
            img.assign(f.begin() + data, f.begin() + std::min<size_t>(f.size(), data + (size ? size : f.size())));
        }
        Tex1Set s;
        if (!tex1_parse(img.data(), img.size(), 0, s))
            continue;
        char tag[32];
        std::snprintf(tag, sizeof(tag), "_tex%03u", i);
        n += dump_set(s, out + "/" + base_name(path) + tag, o);
    }
    std::printf("%s: %u texture entries, %d textures\n", path.c_str(), texCount, n);
    return 0;
}

/* .fnt: a FontSet (G001_flare.h): count faces at +0x14 (PglFont: name,
 * glyphs, first, last, ...), their Tex1 images at +0x18 (face i's texture
 * is tex[i]: fontset_relocate sets PglFont.tex, 0 in the file).  The face's
 * GsTexRegs index is its palette (Font::begin: &face->tex->tex[palette]). */
int cmd_font(const std::string &path, const std::string &out, const Options &o)
{
    std::vector<uint8_t> f;
    if (!read_file(path, f) || f.size() < 0x1C) {
        std::fprintf(stderr, "texdump: cannot read %s\n", path.c_str());
        return 1;
    }
    MKDIR(out.c_str());
    uint32_t count = rd16(&f[0x10]);
    uint32_t pFonts = rd32(&f[0x14]);
    uint32_t pTexs = rd32(&f[0x18]);
    int n = 0;
    for (uint32_t i = 0; i < count && pFonts + (size_t)(i + 1) * 4 <= f.size() &&
                         pTexs + (size_t)(i + 1) * 4 <= f.size(); i++) {
        uint32_t pf = rd32(&f[pFonts + i * 4]);
        if (pf == 0 || pf + 0x20 > f.size())
            continue;
        uint32_t pName = rd32(&f[pf]);
        std::string name = pName < f.size() ? std::string((const char *)&f[pName], strnlen((const char *)&f[pName], f.size() - pName)) : "face";
        uint32_t pTex = rd32(&f[pTexs + i * 4]);
        Tex1Set s;
        if (pTex == 0 || !tex1_parse(f.data(), f.size(), pTex, s))
            continue;
        std::printf("face %u '%s': first %u last %u width %u line %u kanji %ux%u, %zu palettes\n", i,
                    name.c_str(), rd16(&f[pf + 8]), rd16(&f[pf + 10]), f[pf + 0xC], f[pf + 0xD],
                    f[pf + 0x10], f[pf + 0x11], s.tex.size());
        n += dump_set(s, out + "/" + base_name(path) + "_" + safe(name), o);
    }
    std::printf("%s: %u faces, %d textures\n", path.c_str(), count, n);
    return 0;
}

/* .kf: the kanji font (KanjiFontFile): record 0 {lo, hi, data, glyphBytes,
 * w, h, map}, 16-colour CLUTs at +0x18.  Each glyph goes through the GS
 * path font_put_kanji uses: uploaded as PSMT4 w x h (buffer width 128) into
 * a cell, its CLUT (PSMCT32 8 x 2) 0x1F blocks above, read as a PSMT4
 * texture with CSA 1. */
int cmd_kanji(const std::string &path, const std::string &out, int count, const Options &o)
{
    std::vector<uint8_t> f;
    if (!read_file(path, f) || f.size() < 0x1C) {
        std::fprintf(stderr, "texdump: cannot read %s\n", path.c_str());
        return 1;
    }
    MKDIR(out.c_str());
    uint32_t pFonts = rd32(&f[0x14]);
    uint32_t pCluts = rd32(&f[0x18]);
    uint32_t pk = rd32(&f[pFonts]);
    if (pk == 0 || pk + 0x18 > f.size()) {
        std::fprintf(stderr, "texdump: %s: no kanji record\n", path.c_str());
        return 1;
    }
    uint32_t lo = rd16(&f[pk]), hi = rd16(&f[pk + 2]);
    uint32_t pData = rd32(&f[pk + 4]);
    uint32_t glyphBytes = rd32(&f[pk + 8]);
    uint32_t gw = rd16(&f[pk + 0xC]), gh = rd16(&f[pk + 0xE]);
    uint32_t total = (uint32_t)((f.size() - pData) / (glyphBytes ? glyphBytes : 1));
    if (count <= 0 || (uint32_t)count > total)
        count = (int)total;
    std::printf("kanji: codes %04X..%04X, %ux%u, %u bytes a glyph, %u glyphs\n", lo, hi, gw, gh, glyphBytes, total);

    const uint32_t cols = 32;
    uint32_t rowsN = ((uint32_t)count + cols - 1) / cols;
    uint32_t cw = gw + 2, ch = gh + 2;
    std::vector<uint32_t> sheet((size_t)cols * cw * rowsN * ch, 0);
    Texa texa;
    const uint8_t *clut = &f[pCluts + (size_t)o.palette * 0x40];
    for (int g = 0; g < count; g++) {
        TexSpace cell(0, 32);                   /* one PSMT4 page: the cell */
        Transfer up;
        up.dbp = 0;
        up.dbw = 2;
        up.dpsm = PSMT4;
        up.w = gw;
        up.h = gh;
        cell.upload(up, &f[pData + (size_t)g * glyphBytes], glyphBytes);
        Transfer cu;
        cu.dbp = 0x1F;
        cu.dbw = 1;
        cu.dpsm = PSMCT32;
        cu.w = 8;
        cu.h = 2;
        cell.upload(cu, clut, 0x40);
        Tex0 t;
        t.tbp0 = 0;
        t.tbw = 2;
        t.psm = PSMT4;
        t.tw = t.th = 7;
        t.tcc = 1;
        t.cbp = 0x1F;
        t.cpsm = PSMCT32;
        t.csa = 1;
        t.cld = 1;
        ClutBuffer cb;
        cb.load(cell, t);
        std::vector<uint32_t> px((size_t)gw * gh);
        decode_tex0(cell, t, &cb, texa, 0, 0, gw, gh, px.data(), gw);
        uint32_t ox = (g % cols) * cw + 1, oy = (g / cols) * ch + 1;
        for (uint32_t y = 0; y < gh; y++)
            for (uint32_t x = 0; x < gw; x++)
                sheet[(size_t)(oy + y) * cols * cw + ox + x] = px[(size_t)y * gw + x];
    }
    write_png(out + "/" + base_name(path) + "_pal" + std::to_string(o.palette) + ".png", sheet,
              cols * cw, rowsN * ch, o, false);
    return 0;
}

/* .ico: the PS2 icon as T077 icon_model_build reads it; its texture is
 * 128 x 128 PSMCT16 (raw or run-length packed), A forced to 1. */
int cmd_ico(const std::string &path, const std::string &out, const Options &o)
{
    std::vector<uint8_t> f;
    if (!read_file(path, f) || f.size() < 20 || rd32(&f[0]) != 0x10000) {
        std::fprintf(stderr, "texdump: %s is not an icon\n", path.c_str());
        return 1;
    }
    MKDIR(out.c_str());
    uint32_t shapes = rd32(&f[4]), texType = rd32(&f[8]), verts = rd32(&f[16]);
    size_t p = 20 + (size_t)(shapes * 8 + 16) * verts;
    if (p + 4 > f.size())
        return 1;
    uint32_t animCount = rd32(&f[p]);
    p += 4;
    for (uint32_t a = 0; a < animCount && p + 16 <= f.size(); a++) {
        p += 12;
        uint32_t frames = rd32(&f[p]);
        p += 4;
        for (uint32_t j = 0; j < frames && p + 8 <= f.size(); j++) {
            uint32_t keys = rd32(&f[p + 4]);
            p += 8 + (size_t)keys * 8;
        }
    }
    std::printf("icon: %u shapes, %u vertices, texture type %X, texture at %zX\n", shapes, verts, texType, p);
    if (!(texType & 4))
        return 0;
    std::vector<uint16_t> tex(128 * 128, 0);
    if (texType & 8) {
        uint32_t size = rd32(&f[p]);
        size_t s = p + 4, end = std::min(f.size(), s + (size & ~1u));
        size_t d = 0;
        while (s + 2 <= end && d < tex.size()) {
            int16_t n = (int16_t)rd16(&f[s]);
            s += 2;
            if (n > 0) {
                uint16_t c = rd16(&f[s]) | 0x8000;
                s += 2;
                for (int i = 0; i < n && d < tex.size(); i++)
                    tex[d++] = c;
            } else if (n < 0) {
                for (int i = 0; i < -n && d < tex.size() && s + 2 <= end; i++, s += 2)
                    tex[d++] = rd16(&f[s]) | 0x8000;
            }
        }
    } else {
        for (size_t i = 0; i < tex.size() && p + 2 * i + 2 <= f.size(); i++)
            tex[i] = rd16(&f[p + 2 * i]) | 0x8000;
    }
    /* VramAllocator::loadImage(PSMCT16, 128, 128): uploaded as is, read as is */
    TexSpace space(0, 32);
    Transfer up;
    up.dbp = 0;
    up.dbw = 2;
    up.dpsm = PSMCT16;
    up.w = up.h = 128;
    space.upload(up, tex.data(), tex.size() * 2);
    std::vector<uint32_t> px(128 * 128);
    decode_rect(space, PSMCT16, 0, 2, 0, 0, 128, 128, nullptr, 0, Texa(), px.data(), 128);
    write_png(out + "/" + base_name(path) + ".png", px, 128, 128, o, false);
    return 0;
}

int cmd_selftest()
{
    int r = layout_self_test();
    if (r != 0) {
        std::printf("layout self test FAILED (%d)\n", r);
        return 1;
    }
    /* upload / read round trips: a format written and read in itself is
     * the identity, whatever its layout */
    const uint32_t psms[] = { PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMT8, PSMT4, PSMZ32, PSMZ16 };
    for (uint32_t psm : psms) {
        uint32_t w = 200, h = 100, bw = 4;
        std::vector<uint8_t> img(image_bytes(psm, w, h));
        for (size_t i = 0; i < img.size(); i++)
            img[i] = (uint8_t)(i * 2654435761u >> 13);
        TexSpace s(0x123, 0x400);
        Transfer t;
        t.dbp = 0x123;
        t.dbw = bw;
        t.dpsm = psm;
        t.w = w;
        t.h = h;
        s.upload(t, img.data(), img.size());
        uint64_t bitPos = 0;
        uint32_t bits = psm_transfer_bits(psm);
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++, bitPos += bits) {
                uint32_t v = 0;
                for (uint32_t b = 0; b < bits; b++)
                    v |= (uint32_t)((img[(size_t)((bitPos + b) >> 3)] >> (uint32_t)((bitPos + b) & 7)) & 1) << b;
                if (s.read(psm, 0x123, bw, x, y) != v) {
                    std::printf("round trip FAILED: %s at %u,%u\n", psm_name(psm), x, y);
                    return 1;
                }
            }
    }
    /* a T8 image uploaded as PSMCT32 with half the width and height (the
     * known equivalence of the two layouts per page) reads back */
    {
        uint32_t w = 128, h = 64;
        std::vector<uint8_t> t8(w * h);
        for (size_t i = 0; i < t8.size(); i++)
            t8[i] = (uint8_t)i;
        TexSpace a(0, 32), b(0, 32);
        Transfer t;
        t.dbp = 0;
        t.dbw = 2;
        t.dpsm = PSMT8;
        t.w = w;
        t.h = h;
        a.upload(t, t8.data(), t8.size());
        /* the same page read as CT32 64 x 32 and written back as CT32 */
        std::vector<uint32_t> words(64 * 32);
        for (uint32_t y = 0; y < 32; y++)
            for (uint32_t x = 0; x < 64; x++)
                words[y * 64 + x] = a.read(PSMCT32, 0, 1, x, y);
        Transfer c;
        c.dbp = 0;
        c.dbw = 1;
        c.dpsm = PSMCT32;
        c.w = 64;
        c.h = 32;
        b.upload(c, words.data(), words.size() * 4);
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
                if (b.read(PSMT8, 0, 2, x, y) != t8[y * w + x]) {
                    std::printf("T8 via CT32 FAILED at %u,%u\n", x, y);
                    return 1;
                }
    }
    std::printf("self test passed\n");
    return 0;
}

/* memtest: the game form of the API.  Each set of FILE is copied, relocated
 * in place the way tex1_relocate (0x00248DA8) does it (pointers = address,
 * on a 32-bit build), read back with tex1_from_memory and decoded; the
 * textures must equal the file form's. */
int cmd_memtest(const std::string &path)
{
    if (sizeof(void *) != 4) {
        std::printf("memtest needs a 32-bit build\n");
        return 1;
    }
    std::vector<uint8_t> f;
    if (!read_file(path, f))
        return 1;
    std::vector<Tex1Set> sets = tex1_scan(f.data(), f.size());
    std::vector<uint8_t> mem(f);                /* the "loaded" file */
    int checked = 0;
    for (const Tex1Set &s : sets) {
        /* only complete sets: the memory form trusts the sizes */
        bool complete = true;
        for (const Tex1Upload &u : s.uploads)
            if (u.bytes < image_bytes(u.t.dpsm, u.t.w, u.t.h))
                complete = false;
        if (!complete) {
            std::printf("@%zX: truncated in the file, skipped\n", s.offset);
            continue;
        }
        uint8_t *img = mem.data() + s.offset;
        uint32_t self = rd32(img + 4);
        uint32_t delta = (uint32_t)(uintptr_t)img - self;  /* tex1_relocate: img - img->self */
        auto reloc = [&](uint8_t *field) {
            uint32_t v = rd32(field);
            if (v != 0) {
                v += delta;
                std::memcpy(field, &v, 4);
            }
        };
        uint32_t newSelf = (uint32_t)(uintptr_t)img;
        std::memcpy(img + 4, &newSelf, 4);
        reloc(img + 0x08);
        reloc(img + 0x18);
        reloc(img + 0x1C);
        reloc(img + 0x20);
        uint32_t nXfer = rd16(img + 0x16);
        uint8_t *xfer = reinterpret_cast<uint8_t *>((uintptr_t)rd32(img + 0x1C));
        for (uint32_t i = 0; xfer != nullptr && i < nXfer; i++)
            reloc(xfer + i * 0xC);
        uint8_t *tbl = reinterpret_cast<uint8_t *>((uintptr_t)rd32(img + 0x20));
        for (uint32_t i = 0; tbl != nullptr && i < rd32(tbl); i++)
            reloc(tbl + 4 + 4 * i);
        reloc(img + 0x24);
        uint8_t *anim = reinterpret_cast<uint8_t *>((uintptr_t)rd32(img + 0x24));
        if (anim != nullptr) {
            reloc(anim + 4);
            uint8_t *ch = reinterpret_cast<uint8_t *>((uintptr_t)rd32(anim + 4));
            for (uint32_t i = 0; ch != nullptr && i < rd16(anim); i++, ch += 0x20) {
                reloc(ch + 8);
                reloc(ch + 12);
                uint8_t *keys = reinterpret_cast<uint8_t *>((uintptr_t)rd32(ch + 12));
                for (uint32_t k = 0; keys != nullptr && k < rd16(ch + 6); k++)
                    reloc(keys + 8 * k + 4);
            }
        }
        Tex1Set m;
        if (!tex1_from_memory(img, m)) {
            std::printf("@%zX: tex1_from_memory failed\n", s.offset);
            return 1;
        }
        TexSpace a, b;
        tex1_layout(s, a);
        tex1_layout(m, b);
        for (size_t i = 0; i < s.tex.size(); i++) {
            DecodedTexture da, db;
            bool oa = tex1_decode(s, a, i, 0, Texa(), da);
            bool ob = tex1_decode(m, b, i, 0, Texa(), db);
            if (oa != ob || da.rgba != db.rgba) {
                std::printf("@%zX tex %zu: memory form differs\n", s.offset, i);
                return 1;
            }
            checked++;
        }
    }
    std::printf("%s: %zu sets, %d textures identical in memory form\n", path.c_str(), sets.size(), checked);
    return 0;
}

/* PC (2026-11-08): the image packs' PNG form (gs_imgs.h): each pack's images
 * read, the pack built again from them as the game builds it from its PNG
 * files, that read back - every image the same size and pixels; and the
 * alpha's PNG round trip for 0 .. 0x80.  Prints a line a pack. */
int cmd_imgsround(const std::vector<std::string> &paths)
{
    for (uint32_t a = 0; a <= 0x80; a++)
        if (imgs_alpha_from_png(imgs_alpha_to_png(a)) != a) {
            std::printf("alpha %u does not survive the PNG round trip\n", a);
            return 1;
        }
    int bad = 0;
    for (const std::string &path : paths) {
        std::vector<uint8_t> f, built;
        std::vector<ImgsImage> images, back;
        std::string why;
        if (!read_file(path, f)) {
            std::printf("%s: cannot read\n", path.c_str());
            bad++;
            continue;
        }
        if (!imgs_read(f.data(), f.size(), images, &why)) {
            std::printf("%s: KEPT AS IT IS: %s\n", path.c_str(), why.c_str());
            bad++;
            continue;
        }
        if (!imgs_build(images, built, &why) || !imgs_read(built.data(), built.size(), back, &why)) {
            std::printf("%s: rebuild failed: %s\n", path.c_str(), why.c_str());
            bad++;
            continue;
        }
        int counts[3] = {0, 0, 0};          /* T4, T8, CT32 */
        const std::vector<Tex1Set> sets = tex1_scan(built.data(), built.size());
        for (const Tex1Set &s : sets) {
            const uint32_t psm = Tex0::decode(s.tex[0].tex0).psm;
            counts[psm == PSMT4 ? 0 : psm == PSMT8 ? 1 : 2]++;
        }
        size_t same = 0;
        std::string firstBad;
        for (const ImgsImage &img : images) {
            const ImgsImage *b = nullptr;
            for (const ImgsImage &x : back)
                if (x.name == img.name)
                    b = &x;
            if (b != nullptr && b->w == img.w && b->h == img.h && b->rgba == img.rgba)
                same++;
            else if (firstBad.empty())
                firstBad = img.name;
        }
        std::printf("%s: %zu images, %zu identical after the rebuild (T4 %d, T8 %d, CT32 %d), %zu -> %zu bytes%s%s\n",
                    path.c_str(), images.size(), same, counts[0], counts[1], counts[2], f.size(), built.size(),
                    firstBad.empty() ? "" : "; FIRST DIFFERENT: ", firstBad.c_str());
        if (same != images.size())
            bad++;
    }
    return bad != 0 ? 1 : 0;
}

void usage()
{
    std::fprintf(stderr,
                 "usage: texdump selftest | scan FILE | tex1 FILE OUTDIR | imgs FILE OUTDIR |\n"
                 "       pmb FILE OUTDIR | font FILE OUTDIR | kanji FILE OUTDIR [N] | ico FILE OUTDIR |\n"
                 "       imgsround FILE.imgs...\n"
                 "options: --full --view --raw-alpha --mips --vram --variations --palette P\n");
}

} /* namespace */

int main(int argc, char **argv)
{
    Options o;
    std::vector<std::string> args;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--full") o.full = true;
        else if (a == "--view") o.view = true;
        else if (a == "--raw-alpha") o.rawAlpha = true;
        else if (a == "--mips") o.mips = true;
        else if (a == "--vram") o.vram = true;
        else if (a == "--variations") o.variations = true;
        else if (a == "--dry") o.dry = true;
        else if (a == "--palette" && i + 1 < argc) o.palette = std::atoi(argv[++i]);
        else args.push_back(a);
    }
    if (args.empty()) {
        usage();
        return 2;
    }
    const std::string &cmd = args[0];
    if (cmd == "selftest")
        return cmd_selftest();
    if (cmd == "scan" && args.size() >= 2)
        return cmd_scan(args[1]);
    if (cmd == "memtest" && args.size() >= 2)
        return cmd_memtest(args[1]);
    if (cmd == "imgsround" && args.size() >= 2)
        return cmd_imgsround(std::vector<std::string>(args.begin() + 1, args.end()));
    if (args.size() < 3) {
        usage();
        return 2;
    }
    if (cmd == "tex1") return cmd_tex1(args[1], args[2], o);
    if (cmd == "imgs") return cmd_imgs(args[1], args[2], o);
    if (cmd == "pmb") return cmd_pmb(args[1], args[2], o);
    if (cmd == "font") return cmd_font(args[1], args[2], o);
    if (cmd == "kanji") return cmd_kanji(args[1], args[2], args.size() > 3 ? std::atoi(args[3].c_str()) : 1024, o);
    if (cmd == "ico") return cmd_ico(args[1], args[2], o);
    usage();
    return 2;
}
