/* mp4_movie.cpp - H.264 / AAC movies through Windows Media Foundation
 * (mp4_movie.h). */
#include "engine/media/native/mp4_movie.h"

#include "platform/log.h"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

namespace gt3media {
namespace {

std::mutex g_mfLock;
bool g_mfStarted;

template <class T> void release(T *&p)
{
    if (p != nullptr) {
        p->Release();
        p = nullptr;
    }
}

std::wstring widen(const char *s)
{
    const int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? (size_t)n : 1, L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n);
    w.resize(wcslen(w.c_str()));
    return w;
}

/* A path for the log (the ANSI code page's, its nearest characters). */
std::string shown(const wchar_t *path)
{
    const int n = WideCharToMultiByte(CP_ACP, 0, path, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? (size_t)n : 1, '\0');
    if (n > 0)
        WideCharToMultiByte(CP_ACP, 0, path, -1, &s[0], n, nullptr, nullptr);
    s.resize(strlen(s.c_str()));
    return s;
}

/* A source reader of path with only the first stream of `stream' selected
 * (MF_SOURCE_READER_FIRST_VIDEO_STREAM / _AUDIO_STREAM). */
IMFSourceReader *open_reader(const wchar_t *path, DWORD stream, bool videoProcessing)
{
    IMFAttributes *attr = nullptr;
    IMFSourceReader *reader = nullptr;
    if (FAILED(MFCreateAttributes(&attr, 1)))
        return nullptr;
    if (videoProcessing)
        attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    const HRESULT hr = MFCreateSourceReaderFromURL(path, attr, &reader);
    release(attr);
    if (FAILED(hr)) {
        pclog::write("movie: %s: Media Foundation cannot open it (0x%08lX)", shown(path).c_str(), (unsigned long)hr);
        return nullptr;
    }
    reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (FAILED(reader->SetStreamSelection(stream, TRUE))) {
        release(reader);
        return nullptr;
    }
    return reader;
}

/* Big-endian reads of the file (mp4_duration). */
bool read_at(HANDLE h, uint64_t off, void *dst, DWORD n)
{
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)off;
    DWORD got = 0;
    return SetFilePointerEx(h, li, nullptr, FILE_BEGIN) && ReadFile(h, dst, n, &got, nullptr) && got == n;
}

uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

uint64_t be64(const uint8_t *p)
{
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

/* The box of type `type' among those from `off' to `end': its payload's
 * offset and end. */
bool find_box(HANDLE h, uint64_t off, uint64_t end, const char *type, uint64_t *payload, uint64_t *boxEnd)
{
    while (off + 8 <= end) {
        uint8_t hdr[16];
        if (!read_at(h, off, hdr, 8))
            return false;
        uint64_t size = be32(hdr);
        uint64_t head = 8;
        if (size == 1) {
            if (!read_at(h, off + 8, hdr + 8, 8))
                return false;
            size = be64(hdr + 8);
            head = 16;
        } else if (size == 0) {
            size = end - off;
        }
        if (size < head || off + size > end)
            return false;
        if (std::memcmp(hdr + 4, type, 4) == 0) {
            *payload = off + head;
            *boxEnd = off + size;
            return true;
        }
        off += size;
    }
    return false;
}

} // namespace

/* ---- MfScope ---- */

MfScope::MfScope()
{
    /* (S_FALSE: already on this thread, balanced as well; RPC_E_CHANGED_MODE:
     * an apartment thread, which Media Foundation takes too) */
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    com_ = SUCCEEDED(hr);
    std::lock_guard<std::mutex> l(g_mfLock);
    if (!g_mfStarted) {
        const HRESULT s = MFStartup(MF_VERSION, MFSTARTUP_FULL);
        if (FAILED(s)) {
            pclog::write("movie: Media Foundation does not start (0x%08lX): no MP4 movies", (unsigned long)s);
            return;
        }
        g_mfStarted = true;                 /* (started once for the process: kept) */
    }
    mf_ = true;
}

MfScope::~MfScope()
{
    if (com_)
        CoUninitialize();
}

/* ---- the duration ---- */

int64_t mp4_duration(const char *path)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    int64_t result = -1;
    LARGE_INTEGER size;
    uint64_t moov, moovEnd, mvhd, mvhdEnd;
    if (GetFileSizeEx(h, &size) && find_box(h, 0, (uint64_t)size.QuadPart, "moov", &moov, &moovEnd) &&
        find_box(h, moov, moovEnd, "mvhd", &mvhd, &mvhdEnd)) {
        uint8_t b[32];
        if (read_at(h, mvhd, b, 1)) {
            /* version 0: 32-bit times and duration; 1: 64-bit */
            const bool v1 = b[0] == 1;
            if (read_at(h, mvhd + 4, b, v1 ? 28 : 16)) {
                const uint32_t scale = be32(b + (v1 ? 16 : 8));
                const uint64_t dur = v1 ? be64(b + 20) : be32(b + 12);
                if (scale > 0 && dur != (v1 ? ~0ull : 0xFFFFFFFFull))
                    result = (int64_t)(dur * 10000000ull / scale);
            }
        }
    }
    CloseHandle(h);
    return result;
}

/* ---- video ---- */

Mp4VideoReader::~Mp4VideoReader()
{
    close();
}

void Mp4VideoReader::close()
{
    release(reader_);
}

bool Mp4VideoReader::open(const char *path)
{
    close();
    reader_ = open_reader(widen(path).c_str(), (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, true);
    if (reader_ == nullptr)
        return false;
    IMFMediaType *t = nullptr;
    HRESULT hr = MFCreateMediaType(&t);
    if (SUCCEEDED(hr)) {
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        hr = reader_->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, t);
        release(t);
    }
    if (FAILED(hr) || !read_type()) {
        pclog::write("movie: %s: its video does not decode (0x%08lX; H.264 in an .mp4 is the format)", path,
                     (unsigned long)hr);
        close();
        return false;
    }
    return true;
}

bool Mp4VideoReader::read_type()
{
    IMFMediaType *cur = nullptr;
    if (FAILED(reader_->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur)))
        return false;
    UINT32 w = 0, h = 0;
    MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &w, &h);
    frameW_ = (int)w;
    frameH_ = (int)h;
    UINT32 n = 1, d = 1;
    par_ = SUCCEEDED(MFGetAttributeRatio(cur, MF_MT_PIXEL_ASPECT_RATIO, &n, &d)) && n > 0 && d > 0 ? (double)n / d : 1.0;
    /* the display area (an H.264 frame is coded in 16-line rows: 1088 for
     * 1080) */
    MFVideoArea area;
    UINT32 got = 0;
    cropX_ = cropY_ = 0;
    w_ = frameW_;
    h_ = frameH_;
    if (SUCCEEDED(cur->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8 *)&area, sizeof(area), &got)) &&
        got == sizeof(area) && area.Area.cx > 0 && area.Area.cy > 0) {
        cropX_ = area.OffsetX.value > 0 ? area.OffsetX.value : 0;
        cropY_ = area.OffsetY.value > 0 ? area.OffsetY.value : 0;
        w_ = (int)area.Area.cx;
        h_ = (int)area.Area.cy;
        if (cropX_ + w_ > frameW_)
            w_ = frameW_ - cropX_;
        if (cropY_ + h_ > frameH_)
            h_ = frameH_ - cropY_;
    }
    UINT32 stride = 0;
    if (SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride))) {
        stride_ = (int)(INT32)stride;
    } else {
        LONG s = 0;
        stride_ = SUCCEEDED(MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, (DWORD)frameW_, &s)) ? (int)s
                                                                                                             : frameW_ * 4;
    }
    release(cur);
    if (w_ <= 0 || h_ <= 0)
        return false;
    pixels_.assign((size_t)w_ * h_ * 4, 0);
    return true;
}

bool Mp4VideoReader::next(int64_t *time, const uint8_t **pixels)
{
    if (reader_ == nullptr)
        return false;
    for (;;) {
        DWORD index = 0, flags = 0;
        LONGLONG ts = 0;
        IMFSample *sample = nullptr;
        const HRESULT hr = reader_->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &index, &flags, &ts, &sample);
        if (FAILED(hr)) {
            pclog::write("movie: a video frame does not decode (0x%08lX)", (unsigned long)hr);
            return false;
        }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
            read_type();
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            release(sample);
            return false;
        }
        if (sample == nullptr)
            continue;                       /* (a gap) */
        IMFMediaBuffer *buf = nullptr;
        if (FAILED(sample->GetBufferByIndex(0, &buf))) {
            release(sample);
            continue;
        }
        /* rows top-down: a 2D buffer's pitch, or the default stride of a
         * contiguous one (< 0: bottom-up) */
        IMF2DBuffer *b2 = nullptr;
        BYTE *scan0 = nullptr, *data = nullptr;
        LONG pitch = 0;
        DWORD len = 0;
        bool locked2d = false, locked = false;
        if (SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(&b2))) && SUCCEEDED(b2->Lock2D(&scan0, &pitch))) {
            locked2d = true;
        } else if (SUCCEEDED(buf->Lock(&data, nullptr, &len))) {
            locked = true;
            pitch = stride_;
            scan0 = pitch < 0 ? data + (size_t)(frameH_ - 1) * (size_t)(-pitch) : data;
            if ((size_t)len < (size_t)frameH_ * (size_t)(pitch < 0 ? -pitch : pitch))
                scan0 = nullptr;            /* (too small for the frame) */
        }
        if (scan0 != nullptr)
            for (int y = 0; y < h_; y++)
                std::memcpy(&pixels_[(size_t)y * w_ * 4], scan0 + (ptrdiff_t)(cropY_ + y) * pitch + cropX_ * 4,
                            (size_t)w_ * 4);
        if (locked2d)
            b2->Unlock2D();
        if (locked)
            buf->Unlock();
        release(b2);
        release(buf);
        release(sample);
        if (scan0 == nullptr)
            continue;
        *time = ts;
        *pixels = pixels_.data();
        return true;
    }
}

bool Mp4VideoReader::seek(int64_t t)
{
    if (reader_ == nullptr)
        return false;
    PROPVARIANT v;
    PropVariantInit(&v);
    v.vt = VT_I8;
    v.hVal.QuadPart = t;
    const HRESULT hr = reader_->SetCurrentPosition(GUID_NULL, v);
    PropVariantClear(&v);
    return SUCCEEDED(hr);
}

/* ---- audio ---- */

Mp4AudioReader::~Mp4AudioReader()
{
    close();
}

void Mp4AudioReader::close()
{
    release(reader_);
}

bool Mp4AudioReader::open(const char *path)
{
    return open(widen(path).c_str());
}

bool Mp4AudioReader::open(const wchar_t *path)
{
    close();
    skipTo_ = -1;
    reader_ = open_reader(path, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, false);
    if (reader_ == nullptr) {
        pclog::write("movie: %s: no audio stream", shown(path).c_str());
        return false;
    }
    IMFMediaType *t = nullptr;
    HRESULT hr = MFCreateMediaType(&t);
    if (SUCCEEDED(hr)) {
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        hr = reader_->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, t);
        release(t);
    }
    IMFMediaType *cur = nullptr;
    if (SUCCEEDED(hr))
        hr = reader_->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &cur);
    if (SUCCEEDED(hr)) {
        rate_ = (int)MFGetAttributeUINT32(cur, MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
        channels_ = (int)MFGetAttributeUINT32(cur, MF_MT_AUDIO_NUM_CHANNELS, 0);
        release(cur);
    }
    if (FAILED(hr) || rate_ <= 0 || channels_ <= 0) {
        pclog::write("movie: %s: its audio does not decode (0x%08lX; AAC / MP3 are the formats)", shown(path).c_str(),
                     (unsigned long)hr);
        close();
        return false;
    }
    return true;
}

bool Mp4AudioReader::rewind()
{
    return seek(0);
}

bool Mp4AudioReader::seek(int64_t t)
{
    if (reader_ == nullptr)
        return false;
    PROPVARIANT v;
    PropVariantInit(&v);
    v.vt = VT_I8;
    v.hVal.QuadPart = t;
    const HRESULT hr = reader_->SetCurrentPosition(GUID_NULL, v);
    PropVariantClear(&v);
    skipTo_ = SUCCEEDED(hr) && t > 0 ? t : -1;
    return SUCCEEDED(hr);
}

bool Mp4AudioReader::next(std::vector<int16_t> &out)
{
    if (reader_ == nullptr)
        return false;
    for (;;) {
        DWORD index = 0, flags = 0;
        LONGLONG ts = 0;
        IMFSample *sample = nullptr;
        const HRESULT hr = reader_->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &index, &flags, &ts, &sample);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
            release(sample);
            return false;
        }
        if (sample == nullptr)
            continue;
        IMFMediaBuffer *buf = nullptr;
        BYTE *data = nullptr;
        DWORD len = 0;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf)) && SUCCEEDED(buf->Lock(&data, nullptr, &len))) {
            const size_t frames = len / (2u * (unsigned)channels_);
            /* PC: after a seek, the frames before its time (the source lands
             * on a frame of its own at or before it) */
            size_t first = 0;
            if (skipTo_ >= 0) {
                const int64_t ahead = skipTo_ - ts;
                first = ahead > 0 ? (size_t)std::min<int64_t>(ahead * rate_ / 10000000, (int64_t)frames) : 0;
                if (first < frames)
                    skipTo_ = -1;
            }
            const int16_t *s = (const int16_t *)data + first * (size_t)channels_;
            if (channels_ <= 2) {
                out.insert(out.end(), s, s + (frames - first) * (size_t)channels_);
            } else {
                /* more than two: the front pair with the centre (L R C ...) */
                for (size_t i = 0; i < frames - first; i++) {
                    const int16_t *f = s + i * (size_t)channels_;
                    const float c = 0.7071f * f[2];
                    const float l = (f[0] + c) * 0.6f, r = (f[1] + c) * 0.6f;
                    out.push_back((int16_t)(l < -32768.0f ? -32768 : l > 32767.0f ? 32767 : (int)l));
                    out.push_back((int16_t)(r < -32768.0f ? -32768 : r > 32767.0f ? 32767 : (int)r));
                }
            }
            buf->Unlock();
        }
        release(buf);
        release(sample);
        return true;
    }
}

} // namespace gt3media
