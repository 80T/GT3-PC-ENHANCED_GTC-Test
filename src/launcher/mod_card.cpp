/* mod_card.cpp - a folder's card (mod_card.h). */
#include "mod_card.h"

#include "resource.h"
#include "util.h"

#include <wincodec.h>

#include <algorithm>
#include <cstring>
#include <cwctype>

namespace mod_card {
namespace {

constexpr DWORD kMaxInfo = 64 * 1024;       /* (the most of a mod.txt read) */

std::wstring lower(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
    return s;
}

std::wstring trim(const std::wstring &s)
{
    const wchar_t *space = L" \t\r\n";
    const size_t a = s.find_first_not_of(space);
    return a == std::wstring::npos ? std::wstring() : s.substr(a, s.find_last_not_of(space) - a + 1);
}

/* A text file as text: by its byte order mark (UTF-16, UTF-8), else UTF-8, or
 * the ANSI code page's when it is not valid UTF-8.  "" when it cannot be read. */
std::wstring read_text(const std::wstring &path)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return std::wstring();
    std::string b(kMaxInfo, '\0');
    DWORD got = 0;
    if (!ReadFile(f, &b[0], kMaxInfo, &got, nullptr))
        got = 0;
    CloseHandle(f);
    b.resize(got);
    if (got >= 2 && ((b[0] == '\xFF' && b[1] == '\xFE') || (b[0] == '\xFE' && b[1] == '\xFF'))) {
        std::wstring w((got - 2) / 2, L'\0');
        memcpy(&w[0], b.data() + 2, w.size() * sizeof(wchar_t));
        if (b[0] == '\xFE')
            for (wchar_t &c : w)
                c = (wchar_t)_byteswap_ushort(c);
        return w;
    }
    if (b.compare(0, 3, "\xEF\xBB\xBF") == 0)
        b.erase(0, 3);
    if (b.empty())
        return std::wstring();
    UINT page = CP_UTF8;
    int n = MultiByteToWideChar(page, MB_ERR_INVALID_CHARS, b.data(), (int)b.size(), nullptr, 0);
    if (n <= 0) {
        page = CP_ACP;
        n = MultiByteToWideChar(page, 0, b.data(), (int)b.size(), nullptr, 0);
    }
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(page, 0, b.data(), (int)b.size(), &w[0], n);
    return w;
}

} // namespace

std::wstring full_path(const std::wstring &p)
{
    wchar_t b[MAX_PATH * 2];
    const DWORD n = GetFullPathNameW(p.c_str(), MAX_PATH * 2, b, nullptr);
    std::wstring f = n > 0 && n < MAX_PATH * 2 ? std::wstring(b) : p;
    while (f.size() > 3 && (f.back() == L'\\' || f.back() == L'/'))
        f.pop_back();
    return f;
}

Info read_info(const std::wstring &folder)
{
    Info in;
    const std::wstring t = read_text(util::with_slash(folder) + kInfoFile);
    for (size_t at = 0; at < t.size();) {
        const size_t end = std::min(t.find(L'\n', at), t.size());
        const std::wstring line = t.substr(at, end - at);
        at = end + 1;
        const size_t colon = line.find(L':');
        if (colon == std::wstring::npos)
            continue;
        const std::wstring key = lower(trim(line.substr(0, colon))), value = trim(line.substr(colon + 1));
        if (key == L"title") {
            in.title = value;
        } else if (key == L"author" || key == L"authors") {
            if (!in.authors.empty() && !value.empty())
                in.authors += L", ";
            in.authors += value;
        } else if (key == L"description") {
            const std::wstring text = trim(line.substr(colon + 1) + L"\n" + (at < t.size() ? t.substr(at) : std::wstring()));
            for (wchar_t c : text) {                /* (the edit control's line breaks) */
                if (c == L'\r')
                    continue;
                if (c == L'\n')
                    in.description += L'\r';
                in.description += c;
            }
            break;
        }
    }
    if (in.title.empty()) {
        const std::wstring f = full_path(folder);
        in.title = f.substr(f.find_last_of(L"\\/") + 1);
    }
    return in;
}

bool default_image(const void **data, DWORD *size)
{
    HINSTANCE inst = GetModuleHandleW(nullptr);
    HRSRC r = FindResourceW(inst, MAKEINTRESOURCEW(IDR_MOD_IMAGE), RT_RCDATA);
    HGLOBAL res = r ? LoadResource(inst, r) : nullptr;
    *data = res ? LockResource(res) : nullptr;
    *size = r ? SizeofResource(inst, r) : 0;
    return *data != nullptr && *size != 0;
}

bool load_image(const std::wstring &path, int box, std::vector<uint32_t> *pixels, int *w, int *h)
{
    IWICImagingFactory *wic = nullptr;
    IWICStream *stream = nullptr;
    IWICBitmapDecoder *decoder = nullptr;
    IWICBitmapFrameDecode *frame = nullptr;
    IWICFormatConverter *conv = nullptr;
    IWICBitmapScaler *scaler = nullptr;
    const void *data = nullptr;
    DWORD size = 0;
    UINT iw = 0, ih = 0;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)));
    if (ok && path.empty())
        ok = default_image(&data, &size) && SUCCEEDED(wic->CreateStream(&stream)) &&
             SUCCEEDED(stream->InitializeFromMemory((BYTE *)data, size)) &&
             SUCCEEDED(wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder));
    else if (ok)
        ok = SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                      WICDecodeMetadataCacheOnDemand, &decoder));
    ok = ok && SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&iw, &ih)) && iw != 0 && ih != 0 &&
         SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
         SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                    WICBitmapPaletteTypeCustom));
    if (ok) {
        *w = iw >= ih ? box : std::max(1, MulDiv(box, (int)iw, (int)ih));
        *h = ih >= iw ? box : std::max(1, MulDiv(box, (int)ih, (int)iw));
        pixels->assign((size_t)*w * *h, 0);
        ok = SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
             SUCCEEDED(scaler->Initialize(conv, (UINT)*w, (UINT)*h, WICBitmapInterpolationModeFant)) &&
             SUCCEEDED(scaler->CopyPixels(nullptr, (UINT)*w * 4, (UINT)pixels->size() * 4, (BYTE *)pixels->data()));
    }
    for (IUnknown *u : {(IUnknown *)scaler, (IUnknown *)conv, (IUnknown *)frame, (IUnknown *)decoder,
                        (IUnknown *)stream, (IUnknown *)wic})
        if (u)
            u->Release();
    if (!ok) {
        pixels->clear();
        return false;
    }
    const uint32_t back = GetSysColor(COLOR_WINDOW);        /* (0x00BBGGRR) */
    const uint32_t backR = back & 0xFF, backG = (back >> 8) & 0xFF, backB = (back >> 16) & 0xFF;
    for (uint32_t &p : *pixels) {                           /* (premultiplied 0xAARRGGBB) */
        const uint32_t rest = 255 - (p >> 24);
        const uint32_t r = std::min(((p >> 16) & 0xFF) + backR * rest / 255, 255u),
                       gr = std::min(((p >> 8) & 0xFF) + backG * rest / 255, 255u),
                       b = std::min((p & 0xFF) + backB * rest / 255, 255u);
        p = (r << 16) | (gr << 8) | b;
    }
    return true;
}

void draw_image(const DRAWITEMSTRUCT *d, const std::vector<uint32_t> &pixels, int w, int h)
{
    RECT r = d->rcItem;
    FillRect(d->hDC, &r, GetSysColorBrush(COLOR_WINDOW));
    if (!pixels.empty()) {
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;         /* top-down */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        SetDIBitsToDevice(d->hDC, (r.right - w) / 2, (r.bottom - h) / 2, (DWORD)w, (DWORD)h, 0, 0, 0, (UINT)h,
                          pixels.data(), &bi, DIB_RGB_COLORS);
    }
    DrawEdge(d->hDC, &r, EDGE_SUNKEN, BF_RECT);
}

} // namespace mod_card
