/* ui_text.cpp - GT3 PC's own interface in other languages (ui_text.h).
 * PC (2026-11-16). */
#include "platform/ui_text.h"
#include "platform/log.h"
#include "platform/ui_text_check.h"           /* the rule a translation keeps (tools/lang_sync's too) */

#include <windows.h>

#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ui_text {
namespace {

struct Lang {
    const char *value, *code, *native, *upper;
    const wchar_t *nativeW;
    WORD primary;                               /* Windows' primary language id */
};
const Lang kLangs[kCount] = {
    {"english", "en", "English", "ENGLISH", L"English", LANG_ENGLISH},
    {"french", "fr", "Français", "FRANÇAIS", L"Français", LANG_FRENCH},
    {"german", "de", "Deutsch", "DEUTSCH", L"Deutsch", LANG_GERMAN},
    {"italian", "it", "Italiano", "ITALIANO", L"Italiano", LANG_ITALIAN},
    {"spanish", "es", "Español", "ESPAÑOL", L"Español", LANG_SPANISH},
    {"japanese", "ja", "日本語", "日本語", L"日本語", LANG_JAPANESE},
    {"russian", "ru", "Русский", "РУССКИЙ", L"Русский", LANG_RUSSIAN},
    {"portuguese_br", "pt_BR", "Português (Brasil)", "PORTUGUÊS (BRASIL)", L"Português (Brasil)", LANG_PORTUGUESE},
    {"polish", "pl", "Polski", "POLSKI", L"Polski", LANG_POLISH},
};

/* A language's translations, read once and never freed (a string tr gave
 * out stays valid while another language is chosen). */
struct Catalogue {
    std::deque<std::string> text;               /* stable storage of the keys and texts */
    std::deque<std::wstring> wide;
    std::unordered_map<std::string_view, const char *> map;
    std::unordered_map<std::wstring_view, const wchar_t *> wmap;
    int count = 0;
    bool present = false;
};
Catalogue *g_cats[kCount];
std::mutex g_load;
std::atomic<const Catalogue *> g_cur{nullptr};
std::atomic<int> g_lang{kEnglish};

std::wstring widen(const std::string &s)
{
    if (s.empty())
        return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

/* A quoted .po string's text (C escapes) appended to out; false: no string. */
bool unquote(const char *p, const char *end, std::string &out)
{
    while (p < end && (*p == ' ' || *p == '\t'))
        p++;
    if (p >= end || *p != '"')
        return false;
    for (p++; p < end && *p != '"'; p++) {
        if (*p != '\\' || p + 1 >= end) {
            out += *p;
            continue;
        }
        switch (*++p) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case '0': out += '\0'; break;
        default: out += *p; break;          /* \" \\ */
        }
    }
    return true;
}

void add(Catalogue &c, const std::string &id, const std::string &str, const char *code)
{
    if (!ui_text_check::conversions_match(id, str)) {
        pclog::write("ui_text: %s: a translation with other printf conversions than its English is not taken: "
                     "\"%.60s\"", code, id.c_str());
        return;
    }
    c.text.push_back(id);
    const std::string &k = c.text.back();
    c.text.push_back(str);
    if (!c.map.emplace(std::string_view(k), c.text.back().c_str()).second)
        return;
    c.wide.push_back(widen(id));
    const std::wstring &wk = c.wide.back();
    c.wide.push_back(widen(str));
    c.wmap.emplace(std::wstring_view(wk), c.wide.back().c_str());
    c.count++;
}

/* gettext's .po: msgid / msgstr (each one quoted string or more, joined),
 * "#, fuzzy" flags; comments, obsolete (#~) entries and msgctxt ones skipped. */
void parse(Catalogue &c, const char *data, size_t size, const char *code)
{
    std::string id, str;
    int field = 0;                              /* 1 msgid, 2 msgstr, 3 other (msgctxt, msgid_plural) */
    bool fuzzy = false, ctxt = false;
    auto finish = [&]() {
        if (!id.empty() && !str.empty() && !fuzzy && !ctxt)
            add(c, id, str, code);
        id.clear();
        str.clear();
        field = 0;
        fuzzy = ctxt = false;
    };
    const char *p = data, *end = data + size;
    if (size >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
        p += 3;
    while (p < end) {
        const char *eol = (const char *)memchr(p, '\n', (size_t)(end - p));
        if (eol == nullptr)
            eol = end;
        const char *e = eol;
        if (e > p && e[-1] == '\r')
            e--;
        const char *q = p;
        while (q < e && (*q == ' ' || *q == '\t'))
            q++;
        if (q == e) {
            /* a blank line ends an entry */
        } else if (*q == '#') {
            if (field == 2)
                finish();
            if (e - q >= 2 && q[1] == ',' && std::string_view(q, (size_t)(e - q)).find("fuzzy") != std::string_view::npos)
                fuzzy = true;
        } else if (strncmp(q, "msgctxt", 7) == 0) {
            if (field == 2)
                finish();
            ctxt = true;
            field = 3;
        } else if (strncmp(q, "msgid_plural", 12) == 0) {
            field = 3;
        } else if (strncmp(q, "msgid", 5) == 0) {
            if (field == 2)
                finish();
            field = 1;
            unquote(q + 5, e, id);
        } else if (strncmp(q, "msgstr", 6) == 0) {
            field = 2;
            const char *s = q + 6;
            if (s < e && *s == '[')         /* msgstr[0] of a plural: not used */
                field = 3;
            else
                unquote(s, e, str);
        } else if (*q == '"') {
            if (field == 1)
                unquote(q, e, id);
            else if (field == 2)
                unquote(q, e, str);
        }
        p = eol + 1;
    }
    if (field == 2)
        finish();
}

const Catalogue *load(int lang)
{
    std::lock_guard<std::mutex> l(g_load);
    if (g_cats[lang] != nullptr)
        return g_cats[lang];
    Catalogue *c = new Catalogue;
    const char *code = kLangs[lang].code;
    char dir[MAX_PATH];
    const DWORD n = GetEnvironmentVariableA("GT3PC_LANG_DIR", dir, sizeof(dir));
    if (n > 0 && n < sizeof(dir)) {
        /* a translator's aid: the folder's file instead of the built-in one */
        std::string path = std::string(dir) + "\\" + code + ".po";
        HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            const DWORD size = GetFileSize(f, nullptr);
            std::string buf(size, '\0');
            DWORD got = 0;
            if (size != INVALID_FILE_SIZE && ReadFile(f, &buf[0], size, &got, nullptr) && got == size) {
                parse(*c, buf.data(), buf.size(), code);
                c->present = true;
            }
            CloseHandle(f);
        }
        pclog::write("ui_text: %s: %s (GT3PC_LANG_DIR): %d translations", code, path.c_str(), c->count);
    } else if (lang != kEnglish) {
        char name[16];
        size_t i = 0;
        for (; code[i] != '\0' && i + 1 < sizeof(name); i++)
            name[i] = (char)toupper((unsigned char)code[i]);
        name[i] = '\0';
        HMODULE m = GetModuleHandleA(nullptr);
        HRSRC r = FindResourceA(m, name, "GT3LANG");
        HGLOBAL h = r != nullptr ? LoadResource(m, r) : nullptr;
        const char *data = h != nullptr ? (const char *)LockResource(h) : nullptr;
        if (data != nullptr) {
            parse(*c, data, SizeofResource(m, r), code);
            c->present = true;
        }
        pclog::write("ui_text: %s: %d translations%s", code, c->count, c->present ? "" : " (no catalogue built in)");
    }
    g_cats[lang] = c;
    return c;
}

int from_value_a(const char *v)
{
    for (int i = 0; i < kCount; i++)
        if (_stricmp(v, kLangs[i].value) == 0)
            return i;
    return kAuto;
}

} // namespace

const char *value(int lang)
{
    return lang >= 0 && lang < kCount ? kLangs[lang].value : "auto";
}

int from_value(const char *v)
{
    return v != nullptr ? from_value_a(v) : kAuto;
}

int from_value(const wchar_t *v)
{
    char a[32];
    size_t i = 0;
    for (; v != nullptr && v[i] != L'\0' && i + 1 < sizeof(a); i++)
        a[i] = v[i] < 0x80 ? (char)v[i] : '?';
    a[i] = '\0';
    return from_value_a(a);
}

const char *code(int lang)
{
    return lang >= 0 && lang < kCount ? kLangs[lang].code : "en";
}

const char *native_name(int lang)
{
    return lang >= 0 && lang < kCount ? kLangs[lang].native : "";
}

const char *native_upper(int lang)
{
    return lang >= 0 && lang < kCount ? kLangs[lang].upper : "";
}

const wchar_t *native_name_w(int lang)
{
    return lang >= 0 && lang < kCount ? kLangs[lang].nativeW : L"";
}

int windows_language()
{
    const WORD primary = PRIMARYLANGID(GetUserDefaultUILanguage());
    for (int i = 0; i < kCount; i++)
        if (kLangs[i].primary == primary)
            return i;
    return kEnglish;
}

int resolve(int setting)
{
    return setting >= 0 && setting < kCount ? setting : windows_language();
}

void set_language(int lang)
{
    if (lang < 0 || lang >= kCount)
        lang = kEnglish;
    const Catalogue *c = lang == kEnglish ? nullptr : load(lang);
    g_lang = lang;
    g_cur.store(c != nullptr && c->count > 0 ? c : nullptr, std::memory_order_release);
}

int language()
{
    return g_lang.load(std::memory_order_relaxed);
}

const char *tr(const char *english)
{
    const Catalogue *c = g_cur.load(std::memory_order_acquire);
    if (c == nullptr || english == nullptr)
        return english;
    const auto it = c->map.find(std::string_view(english));
    return it != c->map.end() ? it->second : english;
}

const wchar_t *tr(const wchar_t *english)
{
    const Catalogue *c = g_cur.load(std::memory_order_acquire);
    if (c == nullptr || english == nullptr)
        return english;
    const auto it = c->wmap.find(std::wstring_view(english));
    return it != c->wmap.end() ? it->second : english;
}

int translated(int lang)
{
    if (lang <= kEnglish || lang >= kCount)
        return lang == kEnglish ? 0 : -1;
    const Catalogue *c = load(lang);
    return c->present ? c->count : -1;
}

} // namespace ui_text

std::string trf(const char *english, ...)
{
    const char *f = ui_text::tr(english);
    va_list ap;
    va_start(ap, english);
    const int n = _vscprintf(f, ap);
    va_end(ap);
    if (n <= 0)
        return std::string();
    std::string out((size_t)n, '\0');
    va_start(ap, english);
    vsnprintf(&out[0], (size_t)n + 1, f, ap);
    va_end(ap);
    return out;
}

std::wstring trf(const wchar_t *english, ...)
{
    const wchar_t *f = ui_text::tr(english);
    va_list ap;
    va_start(ap, english);
    const int n = _vscwprintf(f, ap);
    va_end(ap);
    if (n <= 0)
        return std::wstring();
    std::wstring out((size_t)n, L'\0');
    va_start(ap, english);
    vswprintf(&out[0], (size_t)n + 1, f, ap);
    va_end(ap);
    return out;
}
