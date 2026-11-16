/* menu_layout.cpp - the menu layout files as source text (menu_layout.h). */
#include "menu_layout.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace menu_layout {
namespace {

/* ======================================================================== */
/* The records (T229)                                                       */
/* ======================================================================== */

constexpr size_t kNameSize = 0x40;          /* page / layer / item names (with the NUL) */
constexpr size_t kItemSize = 0x6C;
constexpr size_t kListRecSize = 0x50;       /* a page or a layer */
constexpr size_t kUvSize = 0x24;
constexpr size_t kTextSize = 0x10;
constexpr int kMaxDepth = 16;               /* active items in active items */
constexpr uint32_t kMaxCount = 1u << 20;

enum : uint32_t {
    F_TEXTURE = 0x1, F_COLORS = 0x2, F_TEXT = 0x4, F_ACTIVE = 0x8,
    F_BLEND = 0xF0, F_FRAME = 0xF00, F_CURSOR = 0xF000, F_KIND = 0x30000,
    F_GZIP = 0x40000, F_LINEAR = 0x80000, F_FADE_IN = 0xF00000, F_FADE_OUT = 0xF000000,
    F_KNOWN = 0x0FFFFFFF
};
enum { KIND_PLAIN = 0, KIND_HOOK = 1, KIND_BUTTON = 2, KIND_HIGHLIGHT = 3 };

struct Text {
    uint16_t unk0 = 1;                      /* MenuText.unk0 (not read by the game; 1 in every file) */
    uint16_t flags = 0;                     /* anchor 0x0F, mode 0xF0, the rest kept */
    uint32_t color = 0;
    std::string text;                       /* the bytes */
    bool hasFont = false;                   /* a font pointer (NULL: none) */
    std::string font;
};

struct Item {
    std::string name;
    uint32_t flags = 0;
    float rect[5] = {0, 0, 0, 0, 0};        /* x, y, z, w, h */
    std::string hook;                       /* kind 1's type record name */
    std::unique_ptr<Item> active;
    int32_t texture = 0;
    float uv[8] = {0, 0, 1, 0, 1, 1, 0, 1};
    uint8_t colors[16] = {};
    Text text;
};

/* keepOrder: its lists as written (the source's keep_order: a file whose
 * list was not in the game's name order, the EU prototype disc's
 * gt_mode4 hm/hm_credit) */
struct Layer {
    std::string name;
    std::vector<Item> items, objects;
    bool keepOrder = false;
};

struct Page {
    std::string name;
    std::vector<Item> items;
    std::vector<Layer> layers;
    bool keepOrder = false;
};

struct Layout {
    std::vector<Page> pages;
    bool keepOrder = false;
};

/* ---- reading the file ---- */

struct Reader {
    const uint8_t *f;
    size_t n;
    std::string err;

    bool fail(const std::string &e)
    {
        if (err.empty())
            err = e;
        return false;
    }
    bool has(size_t o, size_t len) const { return o <= n && len <= n - o; }
    uint32_t u32(size_t o) const
    {
        return (uint32_t)f[o] | ((uint32_t)f[o + 1] << 8) | ((uint32_t)f[o + 2] << 16) | ((uint32_t)f[o + 3] << 24);
    }
    float f32(size_t o) const
    {
        uint32_t u = u32(o);
        float v;
        std::memcpy(&v, &u, 4);
        return v;
    }
    bool cstr(size_t o, size_t max, std::string *out)
    {
        if (o >= n)
            return fail("a string out of the file");
        const size_t lim = std::min(max, n - o);
        const void *z = std::memchr(f + o, 0, lim);
        if (z == nullptr)
            return fail("a string without its end");
        out->assign((const char *)f + o, (const uint8_t *)z - (f + o));
        return true;
    }

    bool item(size_t o, int depth, Item *it)
    {
        if (depth > kMaxDepth)
            return fail("active items too deep");
        if (!has(o, kItemSize))
            return fail("an item out of the file");
        if (!cstr(o, kNameSize, &it->name))
            return false;
        it->flags = u32(o + 0x40);
        for (int k = 0; k < 5; k++)
            it->rect[k] = f32(o + 0x44 + 4 * k);
        const uint32_t typ = u32(o + 0x58), act = u32(o + 0x5C), uv = u32(o + 0x60), col = u32(o + 0x64),
                       txt = u32(o + 0x68);
        if ((it->flags & F_KIND) == (KIND_HOOK << 16)) {
            if (!has(typ, 4) || !cstr(u32(typ), 0x10000, &it->hook))
                return fail("a hook record out of the file");
        }
        if (it->flags & F_ACTIVE) {
            it->active.reset(new Item);
            if (!item(act, depth + 1, it->active.get()))
                return false;
        }
        if (it->flags & F_TEXTURE) {
            if (!has(uv, kUvSize))
                return fail("a texture record out of the file");
            it->texture = (int32_t)u32(uv);
            for (int k = 0; k < 8; k++)
                it->uv[k] = f32(uv + 4 + 4 * k);
        }
        if (it->flags & F_COLORS) {
            if (!has(col, 16))
                return fail("colours out of the file");
            std::memcpy(it->colors, f + col, 16);
        }
        if (it->flags & F_TEXT) {
            if (!has(txt, kTextSize))
                return fail("a text out of the file");
            Text &t = it->text;
            t.unk0 = (uint16_t)(f[txt] | (f[txt + 1] << 8));
            t.flags = (uint16_t)(f[txt + 2] | (f[txt + 3] << 8));
            t.color = u32(txt + 4);
            if (!cstr(u32(txt + 8), 0x100000, &t.text))
                return false;
            const uint32_t fp = u32(txt + 12);
            t.hasFont = fp != 0;
            if (t.hasFont && !cstr(fp, 0x10000, &t.font))
                return false;
        }
        return true;
    }

    bool items(size_t list, uint32_t count, std::vector<Item> *out)
    {
        if (count > kMaxCount || !has(list, (size_t)count * 4))
            return fail("an item list out of the file");
        out->resize(count);
        for (uint32_t i = 0; i < count; i++)
            if (!item(u32(list + 4 * i), 0, &(*out)[i]))
                return false;
        return true;
    }

    bool layout(Layout *l, uint32_t *texCount, uint32_t *texTable)
    {
        if (n < 16)
            return fail("shorter than its header");
        const uint32_t pc = u32(0), pages = u32(4);
        *texCount = u32(8);
        *texTable = u32(12);
        if (pc > kMaxCount || !has(pages, (size_t)pc * 4))
            return fail("its page list is out of the file");
        l->pages.resize(pc);
        for (uint32_t i = 0; i < pc; i++) {
            const uint32_t p = u32(pages + 4 * i);
            Page &pg = l->pages[i];
            if (!has(p, kListRecSize) || !cstr(p, kNameSize, &pg.name))
                return fail("a page out of the file");
            if (!items(u32(p + 0x44), u32(p + 0x40), &pg.items))
                return false;
            const uint32_t lc = u32(p + 0x48), ll = u32(p + 0x4C);
            if (lc > kMaxCount || !has(ll, (size_t)lc * 4))
                return fail("a layer list out of the file");
            pg.layers.resize(lc);
            for (uint32_t k = 0; k < lc; k++) {
                const uint32_t lo = u32(ll + 4 * k);
                Layer &ly = pg.layers[k];
                if (!has(lo, kListRecSize) || !cstr(lo, kNameSize, &ly.name))
                    return fail("a layer out of the file");
                if (!items(u32(lo + 0x44), u32(lo + 0x40), &ly.items) ||
                    !items(u32(lo + 0x4C), u32(lo + 0x48), &ly.objects))
                    return false;
            }
        }
        return true;
    }
};

/* ---- writing the file (the original tool's order: a page, its item
 * list, its items, its layer list, each layer, its item list, items,
 * object list, objects; an item, then its active item, hook record, UVs,
 * colours, text; strings with their NUL to a 4-byte boundary) ---- */

struct Writer {
    std::vector<uint8_t> b;

    size_t at() const { return b.size(); }
    size_t reserve(size_t len)
    {
        const size_t o = b.size();
        b.resize(o + len, 0);
        return o;
    }
    void w32(size_t o, uint32_t v)
    {
        b[o] = (uint8_t)v;
        b[o + 1] = (uint8_t)(v >> 8);
        b[o + 2] = (uint8_t)(v >> 16);
        b[o + 3] = (uint8_t)(v >> 24);
    }
    void wf(size_t o, float v)
    {
        uint32_t u;
        std::memcpy(&u, &v, 4);
        w32(o, u);
    }
    void name(size_t o, const std::string &s) { std::memcpy(&b[o], s.data(), std::min(s.size(), kNameSize - 1)); }
    size_t string(const std::string &s)
    {
        const size_t o = reserve(s.size() + 1);
        std::memcpy(&b[o], s.data(), s.size());
        while (b.size() % 4)
            b.push_back(0);
        return o;
    }

    size_t item(const Item &it)
    {
        const size_t o = reserve(kItemSize);
        name(o, it.name);
        w32(o + 0x40, it.flags);
        for (int k = 0; k < 5; k++)
            wf(o + 0x44 + 4 * k, it.rect[k]);
        if ((it.flags & F_ACTIVE) && it.active)
            w32(o + 0x5C, (uint32_t)item(*it.active));
        if ((it.flags & F_KIND) == (KIND_HOOK << 16)) {
            const size_t t = reserve(4);
            w32(o + 0x58, (uint32_t)t);
            w32(t, (uint32_t)string(it.hook));
        }
        if (it.flags & F_TEXTURE) {
            const size_t u = reserve(kUvSize);
            w32(o + 0x60, (uint32_t)u);
            w32(u, (uint32_t)it.texture);
            for (int k = 0; k < 8; k++)
                wf(u + 4 + 4 * k, it.uv[k]);
        }
        if (it.flags & F_COLORS) {
            const size_t c = reserve(16);
            w32(o + 0x64, (uint32_t)c);
            std::memcpy(&b[c], it.colors, 16);
        }
        if (it.flags & F_TEXT) {
            const size_t t = reserve(kTextSize);
            w32(o + 0x68, (uint32_t)t);
            b[t] = (uint8_t)it.text.unk0;
            b[t + 1] = (uint8_t)(it.text.unk0 >> 8);
            b[t + 2] = (uint8_t)it.text.flags;
            b[t + 3] = (uint8_t)(it.text.flags >> 8);
            w32(t + 4, it.text.color);
            w32(t + 8, (uint32_t)string(it.text.text));
            if (it.text.hasFont)
                w32(t + 12, (uint32_t)string(it.text.font));
        }
        return o;
    }

    size_t items(const std::vector<Item> &list)
    {
        const size_t l = reserve(4 * list.size());
        for (size_t i = 0; i < list.size(); i++)
            w32(l + 4 * i, (uint32_t)item(list[i]));
        return l;
    }

    void layout(const Layout &lay)
    {
        reserve(16);
        const size_t pl = reserve(4 * lay.pages.size());
        w32(0, (uint32_t)lay.pages.size());
        w32(4, (uint32_t)pl);
        for (size_t i = 0; i < lay.pages.size(); i++) {
            const Page &pg = lay.pages[i];
            const size_t p = reserve(kListRecSize);
            w32(pl + 4 * i, (uint32_t)p);
            name(p, pg.name);
            w32(p + 0x40, (uint32_t)pg.items.size());
            w32(p + 0x44, (uint32_t)items(pg.items));
            w32(p + 0x48, (uint32_t)pg.layers.size());
            const size_t ll = reserve(4 * pg.layers.size());
            w32(p + 0x4C, (uint32_t)ll);
            for (size_t k = 0; k < pg.layers.size(); k++) {
                const Layer &ly = pg.layers[k];
                const size_t lo = reserve(kListRecSize);
                w32(ll + 4 * k, (uint32_t)lo);
                name(lo, ly.name);
                w32(lo + 0x40, (uint32_t)ly.items.size());
                w32(lo + 0x44, (uint32_t)items(ly.items));
                w32(lo + 0x48, (uint32_t)ly.objects.size());
                w32(lo + 0x4C, (uint32_t)items(ly.objects));
            }
        }
    }
};

/* the game's order of names (its binary searches: signed characters) */
bool name_less(const std::string &a, const std::string &b)
{
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; i++) {
        const int x = (signed char)a[i], y = (signed char)b[i];
        if (x != y)
            return x < y;
    }
    return a.size() < b.size();
}

template <class T> void sort_by_name(std::vector<T> &v)
{
    std::stable_sort(v.begin(), v.end(), [](const T &a, const T &b) { return name_less(a.name, b.name); });
}

template <class T> bool in_order(const std::vector<T> &v)
{
    for (size_t i = 1; i < v.size(); i++)
        if (name_less(v[i].name, v[i - 1].name))
            return false;
    return true;
}

/* ======================================================================== */
/* Texts: the file's bytes <-> the source's characters                      */
/* ======================================================================== */

/* Windows-1252's 0x80..0x9F (0: none) */
const uint16_t kCp1252[32] = {0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                              0x2039, 0x0152, 0, 0x017D, 0, 0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013,
                              0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178};

constexpr uint32_t kRawByte = 0x110000;     /* a code point for an escaped byte: kRawByte + the byte */
constexpr UINT kEucJp = 20932;

/* The two bytes of code point c in the European layouts' texts (false:
 * none): a Latin-1 letter after 0x81 ("\x81\xe9": é), Windows-1252's
 * punctuation after 0x80 ("\x80\x92": ’). */
bool gt3_letter(uint32_t c, std::string *out)
{
    if (c >= 0xA0 && c <= 0xFF) {
        *out = std::string("\x81") + (char)c;
        return true;
    }
    for (int i = 0; i < 32; i++)
        if (kCp1252[i] != 0 && kCp1252[i] == c) {
            *out = std::string("\x80") + (char)(0x80 + i);
            return true;
        }
    return false;
}

/* Code point c as EUC-JP bytes (false: none). */
bool euc_encode(uint32_t c, std::string *out)
{
    if (c > 0xFFFF)
        return false;
    wchar_t w = (wchar_t)c;
    char buf[8];
    BOOL used = FALSE;
    const int n = WideCharToMultiByte(kEucJp, 0, &w, 1, buf, sizeof(buf), nullptr, &used);
    if (n <= 0 || used)
        return false;
    out->assign(buf, (size_t)n);
    return true;
}

/* Code point c as the file's bytes in this encoding (false: none). */
bool encode_char(uint32_t c, Encoding e, std::string *out)
{
    if (c >= kRawByte && c < kRawByte + 0x100) {
        *out = std::string(1, (char)(c - kRawByte));
        return true;
    }
    if (c < 0x80) {
        *out = std::string(1, (char)c);
        return true;
    }
    if (e == Encoding::euc_jp && euc_encode(c, out))
        return true;
    return gt3_letter(c, out);
}

void append_utf8(std::string *s, uint32_t c)
{
    if (c < 0x80) {
        *s += (char)c;
    } else if (c < 0x800) {
        *s += (char)(0xC0 | (c >> 6));
        *s += (char)(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        *s += (char)(0xE0 | (c >> 12));
        *s += (char)(0x80 | ((c >> 6) & 0x3F));
        *s += (char)(0x80 | (c & 0x3F));
    } else {
        *s += (char)(0xF0 | (c >> 18));
        *s += (char)(0x80 | ((c >> 12) & 0x3F));
        *s += (char)(0x80 | ((c >> 6) & 0x3F));
        *s += (char)(0x80 | (c & 0x3F));
    }
}

void append_escape(std::string *s, uint8_t b)
{
    char buf[8];
    std::snprintf(buf, sizeof(buf), "\\x%02X", b);
    *s += buf;
}

/* The file's bytes as a quoted source string: each character that encodes
 * back to the same bytes as itself, the rest as escapes. */
std::string quote(const std::string &bytes, Encoding e)
{
    std::string s = "\"";
    const uint8_t *p = (const uint8_t *)bytes.data();
    const size_t n = bytes.size();
    for (size_t i = 0; i < n;) {
        const uint8_t b = p[i];
        if (b == '"' || b == '\\') {
            s += '\\';
            s += (char)b;
            i++;
            continue;
        }
        if (b == '\n') { s += "\\n"; i++; continue; }
        if (b == '\r') { s += "\\r"; i++; continue; }
        if (b == '\t') { s += "\\t"; i++; continue; }
        if (b >= 0x20 && b < 0x7F) {
            s += (char)b;
            i++;
            continue;
        }
        /* a multi-byte character: the longest sequence (3, 2) that decodes
         * to one code point which encodes back to it */
        bool done = false;
        for (size_t len = 3; len >= 2 && !done; len--) {
            if (i + len > n)
                continue;
            uint32_t c = 0;
            if (len == 2 && b == 0x81 && p[i + 1] >= 0xA0) {
                c = p[i + 1];
            } else if (len == 2 && b == 0x80 && p[i + 1] >= 0x80 && p[i + 1] < 0xA0) {
                c = kCp1252[p[i + 1] - 0x80];
            } else if (e == Encoding::euc_jp && b >= 0x8E) {
                wchar_t w[4];
                const int nw = MultiByteToWideChar(kEucJp, MB_ERR_INVALID_CHARS, (const char *)p + i, (int)len, w, 4);
                if (nw == 1)
                    c = (uint32_t)w[0];
            }
            std::string back;
            if (c != 0 && c != 0xFFFD && encode_char(c, e, &back) && back.size() == len &&
                std::memcmp(back.data(), p + i, len) == 0) {
                append_utf8(&s, c);
                i += len;
                done = true;
            }
        }
        if (!done) {
            append_escape(&s, b);
            i++;
        }
    }
    s += '"';
    return s;
}

/* ======================================================================== */
/* Numbers and colours                                                      */
/* ======================================================================== */

/* The shortest decimal that reads back as the same float. */
std::string fmt_float(float v)
{
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    if (!std::isfinite(v)) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "f32:0x%08X", bits);
        return buf;
    }
    if (bits == 0x80000000u)
        return "-0";
    if (v == std::floor(v) && std::fabs(v) < 1e7f) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%.0f", (double)v);
        return buf;
    }
    char buf[40];
    for (int prec = 1; prec <= 9; prec++) {
        std::snprintf(buf, sizeof(buf), "%.*g", prec, (double)v);
        if (std::strtof(buf, nullptr) == v)
            break;
    }
    return buf;
}

std::string fmt_color(const uint8_t *rgba)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X", rgba[0], rgba[1], rgba[2], rgba[3]);
    return buf;
}

std::string fmt_hex(uint32_t v)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%X", v);
    return buf;
}

const char *const kAlignNames[9] = {"top-left", "top", "top-right", "left", "center", "right",
                                    "bottom-left", "bottom", "bottom-right"};
const char *const kBlendNames[3] = {"alpha", "add", "special"};
const char *const kFrameNames[4] = {"none", "shadow", "raised", "sunken"};
const char *const kCursorNames[3] = {"none", "frame", "blink"};
const char *const kModeNames[3] = {"none", "scroll", "typewriter"};

/* ======================================================================== */
/* The source, written                                                      */
/* ======================================================================== */

struct Emitter {
    Encoding enc;
    std::string out;

    void line(int depth, const std::string &s)
    {
        out.append((size_t)depth * 4, ' ');
        out += s;
        out += '\n';
    }

    void item(const char *kw, const Item &it, int depth)
    {
        line(depth, std::string(kw) + " " + quote(it.name, enc) + " {");
        const int d = depth + 1;
        const uint32_t f = it.flags;
        line(d, "rect " + fmt_float(it.rect[0]) + " " + fmt_float(it.rect[1]) + " " + fmt_float(it.rect[2]) + " " +
                    fmt_float(it.rect[3]) + " " + fmt_float(it.rect[4]));
        switch ((f & F_KIND) >> 16) {
        case KIND_HOOK: line(d, "hook " + quote(it.hook, enc)); break;
        case KIND_BUTTON: line(d, "button"); break;
        case KIND_HIGHLIGHT: line(d, "highlight"); break;
        }
        if (f & F_TEXTURE) {
            const float *u = it.uv;
            std::string s = "image " + std::to_string(it.texture);
            /* the corners clockwise from the top left: a rectangle */
            uint32_t b[8];
            std::memcpy(b, u, 32);
            if (b[2] == b[4] && b[1] == b[3] && b[5] == b[7] && b[0] == b[6])
                s += " uv " + fmt_float(u[0]) + " " + fmt_float(u[1]) + " " + fmt_float(u[4]) + " " + fmt_float(u[5]);
            else {
                s += " uv4";
                for (int k = 0; k < 8; k++)
                    s += " " + fmt_float(u[k]);
            }
            line(d, s);
        }
        if (f & F_COLORS) {
            if (std::memcmp(it.colors, it.colors + 4, 4) == 0 && std::memcmp(it.colors, it.colors + 8, 4) == 0 &&
                std::memcmp(it.colors, it.colors + 12, 4) == 0)
                line(d, "color " + fmt_color(it.colors));
            else
                line(d, "colors " + fmt_color(it.colors) + " " + fmt_color(it.colors + 4) + " " +
                            fmt_color(it.colors + 8) + " " + fmt_color(it.colors + 12));
        }
        if (f & F_TEXT) {
            const Text &t = it.text;
            std::string s = "text " + quote(t.text, enc) + " {";
            if (t.hasFont)
                s += " font " + quote(t.font, enc);
            uint8_t c[4] = {(uint8_t)t.color, (uint8_t)(t.color >> 8), (uint8_t)(t.color >> 16),
                            (uint8_t)(t.color >> 24)};
            s += " color " + fmt_color(c);
            const unsigned anchor = t.flags & 0x0F, mode = (t.flags >> 4) & 0x0F;
            s += " align " + (anchor < 9 ? std::string(kAlignNames[anchor]) : std::to_string(anchor));
            if (mode != 0)
                s += " mode " + (mode < 3 ? std::string(kModeNames[mode]) : std::to_string(mode));
            if (t.flags & 0xFF00)
                s += " flags " + fmt_hex(t.flags & 0xFF00);
            if (t.unk0 != 1)
                s += " unk0 " + std::to_string(t.unk0);
            line(d, s + " }");
        }
        const unsigned cursor = (f & F_CURSOR) >> 12, blend = (f & F_BLEND) >> 4, frame = (f & F_FRAME) >> 8;
        if (cursor != 0)
            line(d, "cursor " + (cursor < 3 ? std::string(kCursorNames[cursor]) : std::to_string(cursor)));
        if (blend != 0)
            line(d, "blend " + (blend < 3 ? std::string(kBlendNames[blend]) : std::to_string(blend)));
        if (frame != 0)
            line(d, "frame " + (frame < 4 ? std::string(kFrameNames[frame]) : std::to_string(frame)));
        std::string misc;
        if (f & F_GZIP)
            misc += " gzip";
        if (f & F_LINEAR)
            misc += " linear";
        if (f & F_FADE_IN)
            misc += " fade_in " + std::to_string((f & F_FADE_IN) >> 20);
        if (f & F_FADE_OUT)
            misc += " fade_out " + std::to_string((f & F_FADE_OUT) >> 24);
        if (f & ~F_KNOWN)
            misc += " flags " + fmt_hex(f & ~F_KNOWN);
        if (!misc.empty())
            line(d, misc.substr(1));
        if ((f & F_ACTIVE) && it.active)
            item("active", *it.active, d);
        line(depth, "}");
    }

    void layout(const Layout &l, const std::string &name, uint32_t textures)
    {
        out += "// Gran Turismo 3 menu layout " + name + " (written from " + name +
               ".pmb by tools/menulayout); its\n// textures are in the .tex file of its name (" +
               std::to_string(textures) +
               (textures == 1 ? " texture" : " textures") +
               "). menulayout compile makes the .pmb back:\n// docs/menu_layouts.md has the "
               "syntax. y counts up from the bottom of the 640 x 448 screen.\n";
        line(0, "format 1");
        line(0, std::string("encoding ") + encoding_name(enc));
        if (!in_order(l.pages))
            line(0, "keep_order");
        for (const Page &pg : l.pages) {
            out += '\n';
            line(0, "page " + quote(pg.name, enc) + " {");
            if (!in_order(pg.items) || !in_order(pg.layers))
                line(1, "keep_order");
            for (const Item &it : pg.items)
                item("item", it, 1);
            for (const Layer &ly : pg.layers) {
                line(1, "layer " + quote(ly.name, enc) + " {");
                if (!in_order(ly.items) || !in_order(ly.objects))
                    line(2, "keep_order");
                for (const Item &it : ly.items)
                    item("item", it, 2);
                for (const Item &it : ly.objects)
                    item("object", it, 2);
                line(1, "}");
            }
            line(0, "}");
        }
    }
};

/* ======================================================================== */
/* The source, read                                                         */
/* ======================================================================== */

enum class Tok { end, word, number, string, color, open, close };

struct Token {
    Tok kind = Tok::end;
    std::string text;                       /* word / number / colour text */
    std::u32string chars;                   /* a string's characters (kRawByte + b: an escaped byte) */
    int line = 0;
};

struct Parser {
    const char *p, *e;
    int line = 1;
    Token tok;
    std::string err;
    Encoding enc = Encoding::gt3;
    uint32_t textures = 0;

    bool fail(const std::string &msg)
    {
        if (err.empty())
            err = "line " + std::to_string(tok.line != 0 ? tok.line : line) + ": " + msg;
        return false;
    }

    /* the next UTF-8 code point (0xFFFD for bad bytes) */
    uint32_t utf8()
    {
        const uint8_t b = (uint8_t)*p++;
        if (b < 0x80)
            return b;
        int extra = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : -1;
        if (extra < 0)
            return 0xFFFD;
        uint32_t c = b & (0x3F >> extra);
        for (int k = 0; k < extra; k++) {
            if (p >= e || ((uint8_t)*p & 0xC0) != 0x80)
                return 0xFFFD;
            c = (c << 6) | ((uint8_t)*p++ & 0x3F);
        }
        return c;
    }

    bool next()
    {
        tok = Token();
        for (;;) {
            while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
                if (*p == '\n')
                    line++;
                p++;
            }
            if (p + 1 < e && p[0] == '/' && p[1] == '/') {
                while (p < e && *p != '\n')
                    p++;
                continue;
            }
            break;
        }
        tok.line = line;
        if (p >= e)
            return true;
        const char c = *p;
        if (c == '{' || c == '}') {
            tok.kind = c == '{' ? Tok::open : Tok::close;
            p++;
            return true;
        }
        if (c == '"') {
            tok.kind = Tok::string;
            p++;
            for (;;) {
                if (p >= e || *p == '\n')
                    return fail("a string without its closing quote");
                if (*p == '"') {
                    p++;
                    return true;
                }
                if (*p == '\\') {
                    p++;
                    if (p >= e)
                        return fail("a string without its closing quote");
                    const char x = *p++;
                    if (x == 'n') tok.chars += U'\n';
                    else if (x == 'r') tok.chars += U'\r';
                    else if (x == 't') tok.chars += U'\t';
                    else if (x == '"' || x == '\\') tok.chars += (char32_t)x;
                    else if (x == 'x') {
                        if (e - p < 2 || !std::isxdigit((unsigned char)p[0]) || !std::isxdigit((unsigned char)p[1]))
                            return fail("\\x takes two hex digits");
                        const char hex[3] = {p[0], p[1], 0};
                        tok.chars += (char32_t)(kRawByte + std::strtoul(hex, nullptr, 16));
                        p += 2;
                    } else
                        return fail(std::string("an unknown escape \\") + x);
                    continue;
                }
                const uint32_t cp = utf8();
                if (cp == 0xFFFD)
                    return fail("a string that is not UTF-8");
                tok.chars += (char32_t)cp;
            }
        }
        if (c == '#') {
            tok.kind = Tok::color;
            const char *s = ++p;
            while (p < e && std::isxdigit((unsigned char)*p))
                p++;
            tok.text.assign(s, p);
            if (tok.text.size() != 8)
                return fail("a colour is #RRGGBBAA (8 hex digits)");
            return true;
        }
        if (c == '-' || c == '+' || c == '.' || std::isdigit((unsigned char)c)) {
            tok.kind = Tok::number;
            const char *s = p;
            while (p < e && (std::isalnum((unsigned char)*p) || *p == '.' || *p == '-' || *p == '+' || *p == ':'))
                p++;
            tok.text.assign(s, p);
            return true;
        }
        if (std::isalpha((unsigned char)c) || c == '_') {
            tok.kind = Tok::word;
            const char *s = p;
            while (p < e && (std::isalnum((unsigned char)*p) || *p == '_' || *p == '-' || *p == ':' || *p == '.'))
                p++;
            tok.text.assign(s, p);
            /* f32:0x...: a number */
            if (tok.text.compare(0, 4, "f32:") == 0)
                tok.kind = Tok::number;
            return true;
        }
        return fail(std::string("unexpected '") + c + "'");
    }

    bool word(std::string *out)
    {
        if (tok.kind != Tok::word)
            return fail("a word expected");
        *out = tok.text;
        return next();
    }
    bool expect(Tok k, const char *what)
    {
        if (tok.kind != k)
            return fail(std::string(what) + " expected");
        return next();
    }
    bool number(double *out, double lo, double hi)
    {
        if (tok.kind != Tok::number)
            return fail("a number expected");
        char *end = nullptr;
        const double v = tok.text.compare(0, 2, "0x") == 0 || tok.text.compare(0, 2, "0X") == 0
                             ? (double)std::strtoull(tok.text.c_str() + 2, &end, 16)
                             : std::strtod(tok.text.c_str(), &end);
        if (end == nullptr || *end != '\0' || !(v >= lo && v <= hi) || v != std::floor(v))
            return fail("a whole number from " + std::to_string((long long)lo) + " to " + std::to_string((long long)hi) +
                        " expected");
        *out = v;
        return next();
    }
    bool uint(uint32_t *out, uint32_t max)
    {
        double v;
        if (!number(&v, 0, max))
            return false;
        *out = (uint32_t)v;
        return true;
    }
    bool real(float *out)
    {
        if (tok.kind != Tok::number)
            return fail("a number expected");
        if (tok.text.compare(0, 4, "f32:") == 0) {
            const uint32_t bits = (uint32_t)std::strtoul(tok.text.c_str() + 4, nullptr, 16);
            std::memcpy(out, &bits, 4);
            return next();
        }
        char *end = nullptr;
        const float v = std::strtof(tok.text.c_str(), &end);
        if (end == nullptr || *end != '\0')
            return fail("a number expected, not " + tok.text);
        *out = v;
        return next();
    }
    bool color(uint8_t *rgba)
    {
        if (tok.kind != Tok::color)
            return fail("a colour (#RRGGBBAA) expected");
        for (int k = 0; k < 4; k++) {
            const char hex[3] = {tok.text[2 * k], tok.text[2 * k + 1], 0};
            rgba[k] = (uint8_t)std::strtoul(hex, nullptr, 16);
        }
        return next();
    }
    bool str(std::string *bytes, size_t max, const char *what)
    {
        if (tok.kind != Tok::string)
            return fail(std::string(what) + " (a string in quotes) expected");
        bytes->clear();
        for (char32_t c : tok.chars) {
            std::string b;
            if (!encode_char((uint32_t)c, enc, &b)) {
                std::string ch;
                append_utf8(&ch, (uint32_t)c);
                return fail("the character '" + ch + "' has no " + encoding_name(enc) +
                            " bytes (an escape \\xNN writes a byte)");
            }
            *bytes += b;
        }
        if (bytes->find('\0') != std::string::npos)
            return fail(std::string(what) + " with a \\x00 in it");
        if (bytes->size() > max)
            return fail(std::string(what) + " longer than " + std::to_string(max) + " bytes");
        return next();
    }
    /* a name or a number for a 4-bit field */
    bool named(const char *const *names, int count, uint32_t *out, uint32_t max)
    {
        if (tok.kind == Tok::word) {
            for (int i = 0; i < count; i++)
                if (tok.text == names[i]) {
                    *out = (uint32_t)i;
                    return next();
                }
            std::string list;
            for (int i = 0; i < count; i++)
                list += (i ? ", " : "") + std::string(names[i]);
            return fail("'" + tok.text + "': one of " + list + " (or a number) expected");
        }
        return uint(out, max);
    }

    bool text(Text *t)
    {
        if (!str(&t->text, 0xFFFF, "the text"))
            return false;
        t->unk0 = 1;
        t->color = 0x7F000000u;
        t->hasFont = false;
        t->flags = 0;
        if (tok.kind != Tok::open)
            return true;
        next();
        uint32_t anchor = 0, mode = 0, extra = 0, unk0 = 1;
        while (tok.kind == Tok::word) {
            std::string kw = tok.text;
            next();
            if (kw == "font") {
                t->hasFont = true;
                if (!str(&t->font, 0xFFFF, "the font"))
                    return false;
            } else if (kw == "color") {
                uint8_t c[4];
                if (!color(c))
                    return false;
                t->color = c[0] | (c[1] << 8) | (c[2] << 16) | ((uint32_t)c[3] << 24);
            } else if (kw == "align") {
                if (!named(kAlignNames, 9, &anchor, 15))
                    return false;
            } else if (kw == "mode") {
                if (!named(kModeNames, 3, &mode, 15))
                    return false;
            } else if (kw == "flags") {
                if (!uint(&extra, 0xFFFF))
                    return false;
                if (extra & 0xFF)
                    return fail("a text's flags are its bits from 0x100 (align and mode say the others)");
            } else if (kw == "unk0") {
                if (!uint(&unk0, 0xFFFF))
                    return false;
            } else {
                return fail("'" + kw + "' is not a text's (font, color, align, mode, flags, unk0)");
            }
        }
        t->flags = (uint16_t)(anchor | (mode << 4) | extra);
        t->unk0 = (uint16_t)unk0;
        return expect(Tok::close, "'}' (the text's end)");
    }

    bool item(Item *it, int depth)
    {
        if (depth > kMaxDepth)
            return fail("active items too deep");
        if (!str(&it->name, kNameSize - 1, "the item's name") || !expect(Tok::open, "'{'"))
            return false;
        uint32_t kind = 0, cursor = 0, blend = 0, frame = 0, fadeIn = 0, fadeOut = 0, extra = 0;
        bool gzip = false, linear = false, hasImage = false, hasUv = false, hasColors = false, hasText = false;
        bool hasRect = false, kindSet = false;
        while (tok.kind == Tok::word) {
            const std::string kw = tok.text;
            const int kwLine = tok.line;
            next();
            auto set_kind = [&](uint32_t k) {
                if (kindSet)
                    return fail("an item is one of hook, button, highlight");
                kindSet = true;
                kind = k;
                return true;
            };
            if (kw == "rect") {
                for (int k = 0; k < 5; k++)
                    if (!real(&it->rect[k]))
                        return false;
                hasRect = true;
            } else if (kw == "hook") {
                if (!set_kind(KIND_HOOK) || !str(&it->hook, 0xFFFF, "the hook's name"))
                    return false;
            } else if (kw == "button") {
                if (!set_kind(KIND_BUTTON))
                    return false;
            } else if (kw == "highlight") {
                if (!set_kind(KIND_HIGHLIGHT))
                    return false;
            } else if (kw == "image") {
                double v;
                if (!number(&v, 0, 0x7FFFFFFF))
                    return false;
                if (v >= textures) {
                    tok.line = kwLine;
                    return fail("image " + std::to_string((long long)v) + ": the .tex file has " +
                                std::to_string(textures) + " textures (0 .. " + std::to_string((long long)textures - 1) +
                                ")");
                }
                it->texture = (int32_t)v;
                hasImage = true;
            } else if (kw == "uv") {
                float r[4];
                for (int k = 0; k < 4; k++)
                    if (!real(&r[k]))
                        return false;
                const float corners[8] = {r[0], r[1], r[2], r[1], r[2], r[3], r[0], r[3]};
                std::memcpy(it->uv, corners, sizeof(corners));
                hasUv = true;
            } else if (kw == "uv4") {
                for (int k = 0; k < 8; k++)
                    if (!real(&it->uv[k]))
                        return false;
                hasUv = true;
            } else if (kw == "color") {
                uint8_t c[4];
                if (!color(c))
                    return false;
                for (int k = 0; k < 4; k++)
                    std::memcpy(it->colors + 4 * k, c, 4);
                hasColors = true;
            } else if (kw == "colors") {
                for (int k = 0; k < 4; k++)
                    if (!color(it->colors + 4 * k))
                        return false;
                hasColors = true;
            } else if (kw == "text") {
                if (!text(&it->text))
                    return false;
                hasText = true;
            } else if (kw == "cursor") {
                if (!named(kCursorNames, 3, &cursor, 15))
                    return false;
            } else if (kw == "blend") {
                if (!named(kBlendNames, 3, &blend, 15))
                    return false;
            } else if (kw == "frame") {
                if (!named(kFrameNames, 4, &frame, 15))
                    return false;
            } else if (kw == "gzip") {
                gzip = true;
            } else if (kw == "linear") {
                linear = true;
            } else if (kw == "fade_in") {
                if (!uint(&fadeIn, 15))
                    return false;
            } else if (kw == "fade_out") {
                if (!uint(&fadeOut, 15))
                    return false;
            } else if (kw == "flags") {
                if (!uint(&extra, 0xFFFFFFFFu))
                    return false;
                if (extra & F_KNOWN)
                    return fail("an item's flags are its bits from 0x10000000 (the statements say the others)");
            } else if (kw == "active") {
                if (it->active)
                    return fail("an item has one active item");
                it->active.reset(new Item);
                if (!item(it->active.get(), depth + 1))
                    return false;
            } else {
                tok.line = kwLine;
                return fail("'" + kw + "' is not an item's statement");
            }
        }
        if (!hasRect)
            return fail("the item " + std::string(it->name) + " has no rect");
        if (hasUv && !hasImage)
            return fail("uv without an image");
        it->flags = (hasImage ? F_TEXTURE : 0) | (hasColors ? F_COLORS : 0) | (hasText ? F_TEXT : 0) |
                    (it->active ? F_ACTIVE : 0) | (blend << 4) | (frame << 8) | (cursor << 12) | (kind << 16) |
                    (gzip ? F_GZIP : 0) | (linear ? F_LINEAR : 0) | (fadeIn << 20) | (fadeOut << 24) | extra;
        return expect(Tok::close, "'}' (the item's end)");
    }

    bool layout(Layout *l)
    {
        if (!next())
            return false;
        while (tok.kind == Tok::word) {
            const std::string kw = tok.text;
            next();
            if (kw == "keep_order") {
                l->keepOrder = true;
            } else if (kw == "format") {
                uint32_t v;
                if (!uint(&v, 1000))
                    return false;
                if (v != 1)
                    return fail("format " + std::to_string(v) + ": this game knows format 1");
            } else if (kw == "encoding") {
                std::string name;
                if (!word(&name))
                    return false;
                if (name == "gt3")
                    enc = Encoding::gt3;
                else if (name == "euc-jp")
                    enc = Encoding::euc_jp;
                else
                    return fail("encoding " + name + ": gt3 or euc-jp");
            } else if (kw == "page") {
                Page pg;
                if (!str(&pg.name, kNameSize - 1, "the page's name") || !expect(Tok::open, "'{'"))
                    return false;
                while (tok.kind == Tok::word) {
                    const std::string k2 = tok.text;
                    next();
                    if (k2 == "keep_order") {
                        pg.keepOrder = true;
                    } else if (k2 == "item") {
                        pg.items.emplace_back();
                        if (!item(&pg.items.back(), 0))
                            return false;
                    } else if (k2 == "layer") {
                        Layer ly;
                        if (!str(&ly.name, kNameSize - 1, "the layer's name") || !expect(Tok::open, "'{'"))
                            return false;
                        while (tok.kind == Tok::word) {
                            const std::string k3 = tok.text;
                            next();
                            if (k3 == "keep_order") {
                                ly.keepOrder = true;
                                continue;
                            }
                            if (k3 != "item" && k3 != "object")
                                return fail("'" + k3 + "': a layer has items and objects");
                            std::vector<Item> &list = k3 == "item" ? ly.items : ly.objects;
                            list.emplace_back();
                            if (!item(&list.back(), 0))
                                return false;
                        }
                        if (!expect(Tok::close, "'}' (the layer's end)"))
                            return false;
                        pg.layers.push_back(std::move(ly));
                    } else {
                        return fail("'" + k2 + "': a page has items and layers");
                    }
                }
                if (!expect(Tok::close, "'}' (the page's end)"))
                    return false;
                l->pages.push_back(std::move(pg));
            } else {
                return fail("'" + kw + "': page, format or encoding expected");
            }
            if (!err.empty())
                return false;
        }
        if (tok.kind != Tok::end)
            return fail("page expected");
        return err.empty();
    }
};

/* ---- the texture file ---- */

bool tex_header(const uint8_t *tex, size_t size, uint32_t *count, uint32_t *align, std::string *why)
{
    if (tex == nullptr || size < kTexHeader) {
        *why = "the .tex file is missing or too short";
        return false;
    }
    uint32_t magic;
    std::memcpy(&magic, tex, 4);
    std::memcpy(count, tex + 4, 4);
    std::memcpy(align, tex + 8, 4);
    if (magic != kTexMagic || *align >= 16 || *count > kMaxCount || (size_t)*count * 8 > size - kTexHeader) {
        *why = "the .tex file is not a layout's texture file";
        return false;
    }
    return true;
}

/* every list in the game's order (it finds pages, layers and items by a
 * binary search of their names): a layout edited out of order still works */
void sort_layout(Layout *l)
{
    if (!l->keepOrder)
        sort_by_name(l->pages);
    for (Page &pg : l->pages) {
        if (!pg.keepOrder) {
            sort_by_name(pg.items);
            sort_by_name(pg.layers);
        }
        for (Layer &ly : pg.layers)
            if (!ly.keepOrder) {
                sort_by_name(ly.items);
                sort_by_name(ly.objects);
            }
    }
}

} // namespace

const char *encoding_name(Encoding e)
{
    return e == Encoding::euc_jp ? "euc-jp" : "gt3";
}

bool compile(const char *source, size_t sourceSize, const uint8_t *tex, size_t texSize, std::vector<uint8_t> *pmb,
             std::string *error)
{
    uint32_t count = 0, align = 0;
    if (!tex_header(tex, texSize, &count, &align, error))
        return false;
    Parser ps;
    ps.p = source;
    ps.e = source + sourceSize;
    /* (a UTF-8 byte order mark) */
    if (sourceSize >= 3 && std::memcmp(source, "\xEF\xBB\xBF", 3) == 0)
        ps.p += 3;
    ps.textures = count;
    Layout l;
    if (!ps.layout(&l)) {
        *error = ps.err.empty() ? "line " + std::to_string(ps.line) + ": not a layout" : ps.err;
        return false;
    }
    /* (each texture in the file: the game reads its first bytes, a "Tex1"
     * image's or a gzip stream's, where the table says) */
    for (uint32_t i = 0; i < count; i++) {
        uint32_t off;
        std::memcpy(&off, tex + kTexHeader + 8 * (size_t)i + 4, 4);
        if ((size_t)off < (size_t)count * 8 || (size_t)off + 4 > texSize - kTexHeader) {
            *error = "the .tex file's texture " + std::to_string(i) + " is out of the file";
            return false;
        }
    }
    sort_layout(&l);
    Writer w;
    w.layout(l);
    /* the textures: the table where the original had it modulo 16 (the
     * images' alignment), its offsets from the table */
    while (w.b.size() % 16 != align)
        w.b.push_back(0);
    const size_t table = w.b.size();
    w.w32(8, count);
    w.w32(12, (uint32_t)table);
    w.b.insert(w.b.end(), tex + kTexHeader, tex + texSize);
    for (uint32_t i = 0; i < count; i++) {
        const size_t e = table + 8 * (size_t)i + 4;
        uint32_t off;
        std::memcpy(&off, &w.b[e], 4);
        w.w32(e, off + (uint32_t)table);
    }
    pmb->swap(w.b);
    return true;
}

bool decompile(const uint8_t *pmb, size_t size, Encoding encoding, const std::string &name, std::string *source,
               std::vector<uint8_t> *tex, std::string *why)
{
    Reader r{pmb, size, std::string()};
    Layout l;
    uint32_t texCount = 0, texTable = 0;
    if (pmb == nullptr || size == 0) {
        *why = "an empty file";
        return false;
    }
    if (!r.layout(&l, &texCount, &texTable)) {
        *why = "not a layout of the game's format: " + r.err;
        return false;
    }
    if (texTable > size || texCount > kMaxCount || (size_t)texCount * 8 > size - texTable) {
        *why = "its texture table is out of the file";
        return false;
    }
    /* the texture file: the table and everything after it, the offsets from
     * the table */
    tex->assign(kTexHeader, 0);
    const uint32_t magic = kTexMagic, align = texTable % 16;
    std::memcpy(&(*tex)[0], &magic, 4);
    std::memcpy(&(*tex)[4], &texCount, 4);
    std::memcpy(&(*tex)[8], &align, 4);
    tex->insert(tex->end(), pmb + texTable, pmb + size);
    for (uint32_t i = 0; i < texCount; i++) {
        uint32_t off;
        std::memcpy(&off, &(*tex)[kTexHeader + 8 * (size_t)i + 4], 4);
        if (off < texTable) {
            *why = "a texture before its table";
            return false;
        }
        off -= texTable;
        std::memcpy(&(*tex)[kTexHeader + 8 * (size_t)i + 4], &off, 4);
    }
    Emitter em;
    em.enc = encoding;
    em.layout(l, name, texCount);
    source->swap(em.out);
    /* checked: it compiles back to the file */
    std::vector<uint8_t> back;
    std::string err;
    if (!compile(source->data(), source->size(), tex->data(), tex->size(), &back, &err)) {
        *why = "its source does not compile: " + err;
        return false;
    }
    if (back.size() != size || std::memcmp(back.data(), pmb, size) != 0) {
        size_t at = 0;
        while (at < back.size() && at < size && back[at] == pmb[at])
            at++;
        char buf[96];
        std::snprintf(buf, sizeof(buf), "it does not come back the same (first difference at 0x%zX of 0x%zX)", at,
                      size);
        *why = buf;
        return false;
    }
    return true;
}

} // namespace menu_layout
