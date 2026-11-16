/* png_image.cpp - PNG files through the Windows Imaging Component (png_image.h). */
#include "platform/png_image.h"

#include <windows.h>
#include <wincodec.h>

#include <cmath>
#include <cstring>

namespace png_image {
namespace {

/* (the dots per inch of a square pixel: only their ratio means anything) */
constexpr double kDpi = 96.0;

template <class T> void release(T *&p)
{
    if (p != nullptr) {
        p->Release();
        p = nullptr;
    }
}

/* COM on this thread, as MfScope: S_FALSE (already on it) is balanced as well,
 * RPC_E_CHANGED_MODE (an apartment thread, the launcher's window) takes WIC
 * as it is. */
class ComScope {
public:
    ComScope() : on_(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
    ~ComScope()
    {
        if (on_)
            CoUninitialize();
    }
    ComScope(const ComScope &) = delete;
    ComScope &operator=(const ComScope &) = delete;

private:
    bool on_;
};

IWICImagingFactory *new_factory()
{
    IWICImagingFactory *wic = nullptr;
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ? wic
                                                                                                                  : nullptr;
}

/* bgr: rows of B, G, R (and A when alpha: PC 2026-11-08, the image packs'
 * pictures) bytes */
bool write_png(IWICImagingFactory *wic, const wchar_t *path, const std::vector<uint8_t> &bgr, int w, int h, double aspect,
               bool alpha)
{
    IWICStream *stream = nullptr;
    IWICBitmapEncoder *encoder = nullptr;
    IWICBitmapFrameEncode *frame = nullptr;
    const WICPixelFormatGUID want = alpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR;
    WICPixelFormatGUID format = want;
    const UINT stride = (UINT)w * (alpha ? 4 : 3);
    bool ok = SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path, GENERIC_WRITE)) &&
              SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
              SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
              SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
              SUCCEEDED(frame->SetSize((UINT)w, (UINT)h)) && SUCCEEDED(frame->SetResolution(kDpi / aspect, kDpi)) &&
              SUCCEEDED(frame->SetPixelFormat(&format)) && IsEqualGUID(format, want) &&
              SUCCEEDED(frame->WritePixels((UINT)h, stride, stride * (UINT)h, const_cast<BYTE *>(bgr.data()))) &&
              SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    release(frame);
    release(encoder);
    release(stream);
    return ok;
}

bool decode_with(IWICImagingFactory *wic, const void *data, size_t size, std::vector<uint8_t> *rgba, int *w, int *h,
                 double *aspect)
{
    if (wic == nullptr || data == nullptr || size == 0 || size > 0xFFFFFFFFu)
        return false;
    IWICStream *stream = nullptr;
    IWICBitmapDecoder *decoder = nullptr;
    IWICBitmapFrameDecode *frame = nullptr;
    IWICFormatConverter *conv = nullptr;
    UINT fw = 0, fh = 0;
    bool ok = SUCCEEDED(wic->CreateStream(&stream)) &&
              SUCCEEDED(stream->InitializeFromMemory(static_cast<BYTE *>(const_cast<void *>(data)), (DWORD)size)) &&
              SUCCEEDED(wic->CreateDecoderFromStream(stream, &GUID_ContainerFormatPng, WICDecodeMetadataCacheOnDemand,
                                                     &decoder)) &&
              SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&fw, &fh)) &&
              fw > 0 && fh > 0 && fw <= (UINT)kMaxSide && fh <= (UINT)kMaxSide &&
              (uint64_t)fw * fh * 4 <= kMaxBytes && SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
              SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeCustom));
    if (ok) {
        rgba->resize((size_t)fw * fh * 4);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, fw * 4, (UINT)rgba->size(), rgba->data()));
    }
    if (ok) {
        *w = (int)fw;
        *h = (int)fh;
        if (aspect != nullptr) {
            double dx = 0.0, dy = 0.0;
            *aspect = SUCCEEDED(frame->GetResolution(&dx, &dy)) && dx > 0.0 && dy > 0.0 ? dy / dx : 1.0;
        }
    }
    release(conv);
    release(frame);
    release(decoder);
    release(stream);
    return ok;
}

bool encode_with(IWICImagingFactory *wic, const std::wstring &path, const uint8_t *rgba, int w, int h, double aspect,
                 bool alpha)
{
    if (wic == nullptr || rgba == nullptr || w <= 0 || h <= 0 || !(aspect > 0.0))
        return false;
    const size_t n = alpha ? 4 : 3;
    std::vector<uint8_t> bgr((size_t)w * h * n);
    const size_t pixels = (size_t)w * h;
    for (size_t i = 0; i < pixels; i++) {
        bgr[i * n] = rgba[i * 4 + 2];
        bgr[i * n + 1] = rgba[i * 4 + 1];
        bgr[i * n + 2] = rgba[i * 4];
        if (alpha)
            bgr[i * n + 3] = rgba[i * 4 + 3];
    }
    const std::wstring part = path + L".part";
    bool ok = write_png(wic, part.c_str(), bgr, w, h, aspect, alpha);
    if (ok)
        ok = MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok)
        DeleteFileW(part.c_str());
    return ok;
}

} // namespace

bool decode(const void *data, size_t size, std::vector<uint8_t> *rgba, int *w, int *h, double *aspect)
{
    ComScope com;
    IWICImagingFactory *wic = new_factory();
    const bool ok = decode_with(wic, data, size, rgba, w, h, aspect);
    release(wic);
    return ok;
}

bool encode_file(const std::wstring &path, const uint8_t *rgba, int w, int h, double aspect, bool alpha)
{
    ComScope com;
    IWICImagingFactory *wic = new_factory();
    const bool ok = encode_with(wic, path, rgba, w, h, aspect, alpha);
    release(wic);
    return ok;
}

/* PC (2026-11-08): COM and the factory once for many pictures (creating the
 * factory costs some 4 ms a call, the image packs' thousands of small
 * pictures' time). */
Session::Session() : com_(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
{
    wic_ = new_factory();
}

Session::~Session()
{
    IWICImagingFactory *wic = static_cast<IWICImagingFactory *>(wic_);
    release(wic);
    if (com_)
        CoUninitialize();
}

bool Session::decode(const void *data, size_t size, std::vector<uint8_t> *rgba, int *w, int *h, double *aspect)
{
    return decode_with(static_cast<IWICImagingFactory *>(wic_), data, size, rgba, w, h, aspect);
}

bool Session::encode_file(const std::wstring &path, const uint8_t *rgba, int w, int h, double aspect, bool alpha)
{
    return encode_with(static_cast<IWICImagingFactory *>(wic_), path, rgba, w, h, aspect, alpha);
}

} // namespace png_image
