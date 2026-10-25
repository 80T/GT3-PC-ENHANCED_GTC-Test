/* audio_ks.cpp - low-latency sound output through WDM kernel streaming
 * (audio_ks.h).
 *
 * The parts, in order: finding the KS filter and sink pin behind the
 * default render endpoint (the Core Audio device topology, walked upstream
 * from the endpoint's jack to the first software-IO connector); describing
 * a pin (KSPROPERTY_PIN_*); creating it in the first format it takes;
 * setting up WaveRT (a looped buffer with notifications, or polled) or
 * standard streaming (packets); the render thread for each of the three
 * modes; start / stop.
 *
 * Every pin and filter handle is asynchronous (KS opens them so), so every
 * DeviceIoControl goes through ks_ioctl with an OVERLAPPED and an event,
 * and waits at most 1 s. */
#include "platform/audio_ks.h"

/* The order matters, and must hold with WIN32_LEAN_AND_MEAN (which leaves
 * winioctl.h, mmsystem.h and the COM headers out of windows.h): cguid.h
 * (through objbase.h) before ks.h, whose GUID_NULL macro would break it;
 * mmreg.h before ksmedia.h; devicetopology.h includes ks.h / ksmedia.h
 * itself, after defining _IKsControl_ so that ks.h leaves IKsControl to it. */
#include <windows.h>
#include <winioctl.h>
#include <objbase.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <devicetopology.h>
#include <ks.h>
#include <ksmedia.h>
#include <setupapi.h>
#include <avrt.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include <atomic>
#include <string>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "ksuser.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "advapi32.lib")

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace audio_ks {
namespace {

constexpr DWORD kIoTimeoutMs = 1000;      /* the longest one driver call may take */
constexpr int kPackets = 4;               /* standard streaming: packets in flight */
constexpr int kStormCount = 32;           /* underruns within one second that end the stream */
constexpr DWORD kNoProgressMs = 1000;     /* no notification / no position change: failed */

/* Test hook (tools/kstest, which includes this file): WaveRT without
 * notifications even when the driver has them, to exercise the polled
 * mode.  Never set by the game. */
bool g_testNoNotify = false;

/* ---- small things ----------------------------------------------------- */

void (*g_log)(const char *line) = nullptr;

void logf(const char *fmt, ...)
{
    if (!g_log)
        return;
    char line[640];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    g_log(line);
}

/* "error 170 (The requested resource is in use.)" */
const char *errstr(DWORD err, char *buf, size_t size)
{
    char msg[256];
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err, 0, msg,
                             sizeof(msg), nullptr);
    while (n > 0 && (msg[n - 1] == '\n' || msg[n - 1] == '\r' || msg[n - 1] == ' ' || msg[n - 1] == '.'))
        msg[--n] = 0;
    if (n > 0)
        snprintf(buf, size, "error %lu (%s)", (unsigned long)err, msg);
    else
        snprintf(buf, size, "error %lu", (unsigned long)err);
    return buf;
}

std::string utf8(const wchar_t *w)
{
    if (!w || !*w)
        return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1)
        return std::string();
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

double g_qpcToSeconds = 0.0;

double qpc_now()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * g_qpcToSeconds;
}

bool is_null_guid(const GUID &g)
{
    static const GUID zero = {};
    return memcmp(&g, &zero, sizeof(GUID)) == 0;
}

template <class T> struct Com {
    T *p = nullptr;
    Com() = default;
    Com(const Com &) = delete;
    Com &operator=(const Com &) = delete;
    ~Com() { reset(); }
    void reset()
    {
        if (p) {
            p->Release();
            p = nullptr;
        }
    }
    T **out()
    {
        reset();
        return &p;
    }
    T *operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

struct CoStr {
    LPWSTR s = nullptr;
    ~CoStr() { CoTaskMemFree(s); }
    LPWSTR *out()
    {
        CoTaskMemFree(s);
        s = nullptr;
        return &s;
    }
};

/* ---- KS I/O ------------------------------------------------------------- */

/* One DeviceIoControl on a KS handle; waits at most kIoTimeoutMs.  Returns
 * a Win32 error (0: done); *got is the byte count (the size needed with
 * ERROR_MORE_DATA). */
DWORD ks_ioctl(HANDLE h, HANDLE ev, DWORD code, void *in, DWORD inSize, void *out, DWORD outSize, DWORD *got)
{
    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = ev;
    DWORD n = 0;
    if (got)
        *got = 0;
    BOOL ok = DeviceIoControl(h, code, in, inSize, out, outSize, &n, &ov);
    DWORD err = ok ? ERROR_SUCCESS : GetLastError();
    if (err == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ev, kIoTimeoutMs) != WAIT_OBJECT_0) {
            CancelIoEx(h, &ov);
            GetOverlappedResult(h, &ov, &n, TRUE); /* ov lives on this stack */
            return ERROR_TIMEOUT;
        }
        err = GetOverlappedResult(h, &ov, &n, FALSE) ? ERROR_SUCCESS : GetLastError();
    } else if (err == ERROR_SUCCESS || err == ERROR_MORE_DATA) {
        n = (DWORD)ov.InternalHigh;
    }
    if (got)
        *got = n;
    return err;
}

KSPROPERTY make_prop(const GUID &set, ULONG id, ULONG flags)
{
    KSPROPERTY p;
    memset(&p, 0, sizeof(p));
    p.Set = set;
    p.Id = id;
    p.Flags = flags;
    return p;
}

DWORD ks_get(HANDLE h, HANDLE ev, const GUID &set, ULONG id, void *out, DWORD size, DWORD *got = nullptr)
{
    KSPROPERTY p = make_prop(set, id, KSPROPERTY_TYPE_GET);
    return ks_ioctl(h, ev, IOCTL_KS_PROPERTY, &p, sizeof(p), out, size, got);
}

DWORD ks_set(HANDLE h, HANDLE ev, const GUID &set, ULONG id, void *value, DWORD size)
{
    KSPROPERTY p = make_prop(set, id, KSPROPERTY_TYPE_SET);
    return ks_ioctl(h, ev, IOCTL_KS_PROPERTY, &p, sizeof(p), value, size, nullptr);
}

DWORD pin_get(HANDLE filter, HANDLE ev, ULONG pin, ULONG id, void *out, DWORD size, DWORD *got = nullptr)
{
    KSP_PIN p;
    memset(&p, 0, sizeof(p));
    p.Property = make_prop(KSPROPSETID_Pin, id, KSPROPERTY_TYPE_GET);
    p.PinId = pin;
    return ks_ioctl(filter, ev, IOCTL_KS_PROPERTY, &p, sizeof(p), out, size, got);
}

/* A pin property of variable size (KSMULTIPLE_ITEM lists, the name). */
DWORD pin_get_var(HANDLE filter, HANDLE ev, ULONG pin, ULONG id, std::vector<uint8_t> &out)
{
    DWORD size = 4096;
    for (int tries = 0; tries < 5; tries++) {
        out.assign(size, 0);
        DWORD got = 0;
        DWORD err = pin_get(filter, ev, pin, id, out.data(), size, &got);
        if (err == ERROR_SUCCESS) {
            out.resize(got <= size ? got : size);
            return 0;
        }
        if (err != ERROR_MORE_DATA && err != ERROR_INSUFFICIENT_BUFFER) {
            out.clear();
            return err;
        }
        size = got > size ? got : size * 4;
    }
    out.clear();
    return ERROR_MORE_DATA;
}

HANDLE open_filter(const std::wstring &path, DWORD *err)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (err)
            *err = GetLastError();
        return nullptr;
    }
    if (err)
        *err = 0;
    return h;
}

/* ---- describing a pin --------------------------------------------------- */

/* An audio data range of a pin (KSDATARANGE_AUDIO). */
struct Range {
    bool pcm = false, flt = false; /* subformats it admits */
    ULONG maxChannels = 0, minBits = 0, maxBits = 0, minRate = 0, maxRate = 0;
};

struct PinInfo {
    ULONG id = 0;
    bool ok = false; /* dataflow and communication were read */
    KSPIN_DATAFLOW flow = KSPIN_DATAFLOW_IN;
    KSPIN_COMMUNICATION comm = KSPIN_COMMUNICATION_NONE;
    /* KSPROPERTY_PIN_CINSTANCES counts the instances of this filter handle
     * only; KSPROPERTY_PIN_GLOBALCINSTANCES counts them for every program
     * (the Windows audio engine included): busy is decided on the global
     * count when the driver gives it */
    bool haveInstances = false;
    KSPIN_CINSTANCES instances = {};
    bool haveGlobal = false;
    KSPIN_CINSTANCES global = {};
    bool looped = false, standard = false; /* KSINTERFACE_STANDARD_(LOOPED_)STREAMING */
    std::vector<Range> ranges;
    std::wstring name;

    /* a pin a program can create to send audio into the filter */
    bool sink() const
    {
        return ok && flow == KSPIN_DATAFLOW_IN &&
               (comm == KSPIN_COMMUNICATION_SINK || comm == KSPIN_COMMUNICATION_BOTH);
    }
    bool busy() const
    {
        if (haveGlobal && global.PossibleCount > 0)
            return global.CurrentCount >= global.PossibleCount;
        return haveInstances && instances.CurrentCount >= instances.PossibleCount;
    }
};

size_t align8(size_t n)
{
    return (n + 7) & ~(size_t)7;
}

/* The audio ranges of a KSPROPERTY_PIN_DATARANGES reply.  Ranges are 8-byte
 * aligned; a range with KSDATARANGE_ATTRIBUTES is followed by its attribute
 * list (a KSMULTIPLE_ITEM), skipped here.  The walk goes by the bytes, not
 * by Count, so it does not matter whether a driver counts the lists. */
void parse_ranges(const std::vector<uint8_t> &buf, std::vector<Range> &out)
{
    if (buf.size() < sizeof(KSMULTIPLE_ITEM))
        return;
    const KSMULTIPLE_ITEM *mi = (const KSMULTIPLE_ITEM *)buf.data();
    size_t end = mi->Size < buf.size() ? mi->Size : buf.size();
    size_t off = sizeof(KSMULTIPLE_ITEM);
    bool attributesNext = false;
    for (int items = 0; items < 512 && off + sizeof(KSMULTIPLE_ITEM) <= end; items++) {
        if (attributesNext) {
            const KSMULTIPLE_ITEM *a = (const KSMULTIPLE_ITEM *)(buf.data() + off);
            if (a->Size < sizeof(KSMULTIPLE_ITEM))
                break;
            off += align8(a->Size);
            attributesNext = false;
            continue;
        }
        if (off + sizeof(KSDATARANGE) > end)
            break;
        const KSDATARANGE *r = (const KSDATARANGE *)(buf.data() + off);
        if (r->FormatSize < sizeof(KSDATARANGE) || off + r->FormatSize > end)
            break;
        bool major = r->MajorFormat == KSDATAFORMAT_TYPE_AUDIO || is_null_guid(r->MajorFormat);
        bool spec = r->Specifier == KSDATAFORMAT_SPECIFIER_WAVEFORMATEX || is_null_guid(r->Specifier) ||
                    r->Specifier == KSDATAFORMAT_SPECIFIER_DSOUND;
        if (major && spec) {
            Range g;
            bool anySub = is_null_guid(r->SubFormat);
            g.pcm = anySub || r->SubFormat == KSDATAFORMAT_SUBTYPE_PCM;
            g.flt = anySub || r->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
            if (r->FormatSize >= sizeof(KSDATARANGE_AUDIO)) {
                const KSDATARANGE_AUDIO *a = (const KSDATARANGE_AUDIO *)r;
                g.maxChannels = a->MaximumChannels;
                g.minBits = a->MinimumBitsPerSample;
                g.maxBits = a->MaximumBitsPerSample;
                g.minRate = a->MinimumSampleFrequency;
                g.maxRate = a->MaximumSampleFrequency;
            } else { /* a range without audio limits */
                g.maxChannels = 8;
                g.minBits = 8;
                g.maxBits = 32;
                g.minRate = 8000;
                g.maxRate = 192000;
            }
            if (g.pcm || g.flt)
                out.push_back(g);
        }
        attributesNext = (r->Flags & KSDATARANGE_ATTRIBUTES) != 0;
        off += align8(r->FormatSize);
    }
}

bool describe_pin(HANDLE filter, HANDLE ev, ULONG id, PinInfo &pi)
{
    pi = PinInfo();
    pi.id = id;
    KSPIN_DATAFLOW flow;
    KSPIN_COMMUNICATION comm;
    if (pin_get(filter, ev, id, KSPROPERTY_PIN_DATAFLOW, &flow, sizeof(flow)) != 0 ||
        pin_get(filter, ev, id, KSPROPERTY_PIN_COMMUNICATION, &comm, sizeof(comm)) != 0)
        return false;
    pi.flow = flow;
    pi.comm = comm;
    pi.ok = true;
    pi.haveInstances = pin_get(filter, ev, id, KSPROPERTY_PIN_CINSTANCES, &pi.instances, sizeof(pi.instances)) == 0;
    pi.haveGlobal = pin_get(filter, ev, id, KSPROPERTY_PIN_GLOBALCINSTANCES, &pi.global, sizeof(pi.global)) == 0;
    std::vector<uint8_t> b;
    if (pin_get_var(filter, ev, id, KSPROPERTY_PIN_INTERFACES, b) == 0 && b.size() >= sizeof(KSMULTIPLE_ITEM)) {
        const KSMULTIPLE_ITEM *mi = (const KSMULTIPLE_ITEM *)b.data();
        const KSIDENTIFIER *it = (const KSIDENTIFIER *)(mi + 1);
        size_t n = (b.size() - sizeof(*mi)) / sizeof(*it);
        if (mi->Count < n)
            n = mi->Count;
        for (size_t i = 0; i < n; i++) {
            if (it[i].Set != KSINTERFACESETID_Standard)
                continue;
            if (it[i].Id == KSINTERFACE_STANDARD_LOOPED_STREAMING)
                pi.looped = true;
            else if (it[i].Id == KSINTERFACE_STANDARD_STREAMING)
                pi.standard = true;
        }
    }
    if (pin_get_var(filter, ev, id, KSPROPERTY_PIN_DATARANGES, b) == 0)
        parse_ranges(b, pi.ranges);
    if (pin_get_var(filter, ev, id, KSPROPERTY_PIN_NAME, b) == 0 && b.size() >= sizeof(wchar_t))
        pi.name.assign((const wchar_t *)b.data(), wcsnlen((const wchar_t *)b.data(), b.size() / sizeof(wchar_t)));
    return true;
}

ULONG pin_count(HANDLE filter, HANDLE ev)
{
    ULONG n = 0;
    if (ks_get(filter, ev, KSPROPSETID_Pin, KSPROPERTY_PIN_CTYPES, &n, sizeof(n)) != 0)
        return 0;
    return n > 256 ? 256 : n;
}

/* ---- formats ------------------------------------------------------------ */

struct Fmt {
    int rate = 0;
    int channels = 0;
    int bits = 0;  /* container */
    int valid = 0; /* valid bits */
    bool isFloat = false;
    int block() const { return channels * bits / 8; }
};

bool admits(const Range &r, const Fmt &f)
{
    if (f.isFloat ? !r.flt : !r.pcm)
        return false;
    if (r.maxChannels != 0xFFFFFFFFu && (ULONG)f.channels > r.maxChannels)
        return false;
    /* drivers state either the container or the valid bits of 24-in-32 */
    bool bits = ((ULONG)f.bits >= r.minBits && (ULONG)f.bits <= r.maxBits) ||
                ((ULONG)f.valid >= r.minBits && (ULONG)f.valid <= r.maxBits);
    if (!bits)
        return false;
    return (ULONG)f.rate >= r.minRate && (ULONG)f.rate <= r.maxRate;
}

/* The formats to try on a pin, best first: 48 kHz before any other rate,
 * stereo before other channel counts, then 16-bit, 24-in-32, 32-bit,
 * float, 24-bit packed. */
std::vector<Fmt> candidate_formats(const PinInfo &pi)
{
    static const int rates[] = {48000, 44100, 96000, 88200, 192000, 176400, 32000};
    struct Sample {
        int bits, valid;
        bool isFloat;
    };
    static const Sample samples[] = {{16, 16, false}, {32, 24, false}, {32, 32, false}, {32, 32, true}, {24, 24, false}};
    ULONG maxCh = 0;
    for (const Range &r : pi.ranges)
        if (r.maxChannels > maxCh)
            maxCh = r.maxChannels;
    std::vector<int> chans = {2};
    if (maxCh >= 3 && maxCh <= 8)
        chans.push_back((int)maxCh);
    for (int c : {8, 6, 4, 1}) {
        bool have = false;
        for (int x : chans)
            have = have || x == c;
        if (!have)
            chans.push_back(c);
    }
    std::vector<Fmt> out;
    for (int rate : rates)
        for (int ch : chans)
            for (const Sample &s : samples) {
                Fmt f;
                f.rate = rate;
                f.channels = ch;
                f.bits = s.bits;
                f.valid = s.valid;
                f.isFloat = s.isFloat;
                for (const Range &r : pi.ranges)
                    if (admits(r, f)) {
                        out.push_back(f);
                        break;
                    }
            }
    return out;
}

DWORD channel_mask(int channels)
{
    switch (channels) {
    case 1: return KSAUDIO_SPEAKER_MONO;
    case 2: return KSAUDIO_SPEAKER_STEREO;
    case 4: return KSAUDIO_SPEAKER_QUAD;
    case 6: return KSAUDIO_SPEAKER_5POINT1;
    case 8: return KSAUDIO_SPEAKER_7POINT1_SURROUND;
    default: return 0;
    }
}

/* KsCreatePin with a KSPIN_CONNECT followed by the data format:
 * WAVEFORMATEXTENSIBLE, or the plain WAVEFORMATEX old drivers want. */
DWORD create_pin(HANDLE filter, ULONG pinId, const Fmt &f, bool looped, bool extensible, HANDLE *pin)
{
    alignas(8) uint8_t buf[sizeof(KSPIN_CONNECT) + sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE)];
    memset(buf, 0, sizeof(buf));
    KSPIN_CONNECT *c = (KSPIN_CONNECT *)buf;
    c->Interface.Set = KSINTERFACESETID_Standard;
    c->Interface.Id = looped ? KSINTERFACE_STANDARD_LOOPED_STREAMING : KSINTERFACE_STANDARD_STREAMING;
    c->Medium.Set = KSMEDIUMSETID_Standard;
    c->Medium.Id = KSMEDIUM_TYPE_ANYINSTANCE;
    c->PinId = pinId;
    c->PinToHandle = nullptr;
    c->Priority.PriorityClass = KSPRIORITY_NORMAL;
    c->Priority.PrioritySubClass = 1;
    KSDATAFORMAT *df = (KSDATAFORMAT *)(c + 1);
    const GUID &sub = f.isFloat ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
    df->MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    df->SubFormat = sub;
    df->Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
    df->SampleSize = (ULONG)f.block();
    WAVEFORMATEXTENSIBLE *w = (WAVEFORMATEXTENSIBLE *)(df + 1);
    w->Format.nChannels = (WORD)f.channels;
    w->Format.nSamplesPerSec = (DWORD)f.rate;
    w->Format.wBitsPerSample = (WORD)f.bits;
    w->Format.nBlockAlign = (WORD)f.block();
    w->Format.nAvgBytesPerSec = (DWORD)(f.rate * f.block());
    if (extensible) {
        df->FormatSize = sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEXTENSIBLE);
        w->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        w->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        w->Samples.wValidBitsPerSample = (WORD)f.valid;
        w->dwChannelMask = channel_mask(f.channels);
        w->SubFormat = sub;
    } else {
        df->FormatSize = sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEX);
        w->Format.wFormatTag = f.isFloat ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
        w->Format.cbSize = 0;
    }
    *pin = nullptr;
    DWORD err = KsCreatePin(filter, c, GENERIC_WRITE | GENERIC_READ, pin);
    if (err == ERROR_SUCCESS && (*pin == nullptr || *pin == INVALID_HANDLE_VALUE))
        err = ERROR_INVALID_HANDLE;
    if (err != ERROR_SUCCESS)
        *pin = nullptr;
    return err;
}

/* KsCreatePin errors that mean "someone else has the pin", not "wrong format". */
bool busy_error(DWORD err)
{
    return err == ERROR_BUSY || err == ERROR_SHARING_VIOLATION || err == ERROR_ACCESS_DENIED ||
           err == ERROR_DEVICE_IN_USE;
}

/* ---- finding the default endpoint's KS filter and pin ----------------- */

struct Target {
    std::wstring name;        /* the endpoint's friendly name */
    std::wstring path;        /* the KS filter that has the endpoint's sink pin */
    std::vector<ULONG> pins;  /* its candidate sink pins, best first */
    std::wstring jackPath;    /* the KS filter the endpoint's connector is on */
    ULONG jackPin = 0xFFFFFFFFu;
    bool approximate = false; /* found by device, not by the topology */
};

/* "{2}.\\?\hdaudio#...\elineoutwave" -> "\\?\hdaudio#...\elineoutwave" */
std::wstring filter_path_from_device_id(const wchar_t *id)
{
    if (!id)
        return std::wstring();
    const wchar_t *p = wcsstr(id, L"\\\\?\\");
    return std::wstring(p ? p : id);
}

/* The device part of a KS interface path ("\\?\hdaudio#...#4&...&0001"),
 * lower case: the same for every filter and category of one device. */
std::wstring device_part(const std::wstring &path)
{
    size_t e = path.find(L"#{");
    std::wstring s = path.substr(0, e);
    for (wchar_t &c : s)
        c = (wchar_t)towlower(c);
    return s;
}

/* The reference string ("elineoutwave"), lower case. */
std::wstring ref_part(const std::wstring &path)
{
    size_t e = path.find(L"}\\");
    std::wstring s = e == std::wstring::npos ? std::wstring() : path.substr(e + 2);
    for (wchar_t &c : s)
        c = (wchar_t)towlower(c);
    return s;
}

/* Two interface paths of one filter (they differ in the category GUID). */
bool same_filter(const std::wstring &a, const std::wstring &b)
{
    return device_part(a) == device_part(b) && ref_part(a) == ref_part(b);
}

const char *connector_type_name(ConnectorType t)
{
    switch (t) {
    case Physical_Internal: return "physical-internal";
    case Physical_External: return "physical-external";
    case Software_IO: return "software-IO";
    case Software_Fixed: return "software-fixed";
    case Network: return "network";
    default: return "unknown";
    }
}

std::wstring part_device_path(IPart *p)
{
    Com<IDeviceTopology> t;
    CoStr id;
    if (FAILED(p->GetTopologyObject(t.out())) || FAILED(t->GetDeviceId(id.out())))
        return std::wstring();
    return filter_path_from_device_id(id.s);
}

/* Walks the device topology upstream from the connector the endpoint is
 * connected to (the jack on the adapter's topology filter): through the
 * filter's nodes to its inputs, across their physical connections into the
 * wave filter, and through its nodes to the software-IO input connectors:
 * the pins a stream is created on.  A connector part's local ID carries the
 * KS pin id in its low 16 bits. */
void walk_to_sink_pins(IPart *jack, Target &t, bool verbose)
{
    std::vector<IPart *> todo;
    std::vector<std::wstring> seen;
    jack->AddRef();
    todo.push_back(jack);
    std::wstring lastPath;
    for (int steps = 0; !todo.empty() && steps < 1024; steps++) {
        IPart *p = todo.back();
        todo.pop_back();
        CoStr gid;
        if (FAILED(p->GetGlobalId(gid.out())) || !gid.s) {
            p->Release();
            continue;
        }
        std::wstring g(gid.s);
        bool dup = false;
        for (const std::wstring &s : seen)
            dup = dup || s == g;
        if (dup) {
            p->Release();
            continue;
        }
        seen.push_back(g);
        PartType pt = Subunit;
        UINT lid = 0;
        CoStr name;
        p->GetPartType(&pt);
        p->GetLocalId(&lid);
        p->GetName(name.out());
        bool walkInside = true;
        if (pt == Connector) {
            Com<IConnector> c;
            if (SUCCEEDED(p->QueryInterface(__uuidof(IConnector), (void **)c.out()))) {
                ConnectorType ct = Unknown_Connector;
                DataFlow df = In;
                BOOL connected = FALSE;
                c->GetType(&ct);
                c->GetDataFlow(&df);
                c->IsConnected(&connected);
                std::wstring path = part_device_path(p);
                if (verbose) {
                    if (path != lastPath)
                        logf("  filter %s", utf8(path.c_str()).c_str());
                    lastPath = path;
                    logf("    connector pin %u '%s': %s, %s%s", lid & 0xFFFF, utf8(name.s).c_str(),
                         df == In ? "in" : "out", connector_type_name(ct), connected ? ", connected" : "");
                }
                if (df == In) {
                    walkInside = false; /* an input: nothing upstream in this filter */
                    if (ct == Software_IO && !path.empty()) {
                        if (t.path.empty())
                            t.path = path;
                        if (same_filter(t.path, path)) {
                            bool have = false;
                            for (ULONG x : t.pins)
                                have = have || x == (lid & 0xFFFF);
                            if (!have)
                                t.pins.push_back(lid & 0xFFFF);
                        }
                    }
                    if (connected) {
                        Com<IConnector> other;
                        IPart *op = nullptr;
                        if (SUCCEEDED(c->GetConnectedTo(other.out())) &&
                            SUCCEEDED(other->QueryInterface(__uuidof(IPart), (void **)&op)))
                            todo.push_back(op);
                    }
                }
            }
        } else if (verbose) {
            logf("    node %u '%s'", lid & 0xFFFF, utf8(name.s).c_str());
        }
        if (walkInside) {
            Com<IPartsList> list;
            UINT n = 0;
            if (SUCCEEDED(p->EnumPartsIncoming(list.out())) && SUCCEEDED(list->GetCount(&n))) {
                for (UINT i = 0; i < n; i++) {
                    IPart *q = nullptr;
                    if (SUCCEEDED(list->GetPart(i, &q)) && q)
                        todo.push_back(q);
                }
            }
        }
        p->Release();
    }
    for (IPart *p : todo)
        p->Release();
}

/* The KS render filters (KSCATEGORY_RENDER that are also KSCATEGORY_AUDIO):
 * their KSCATEGORY_AUDIO interface paths and friendly names. */
struct FilterEntry {
    std::wstring path;
    std::wstring name;
};

std::vector<FilterEntry> enum_render_filters()
{
    std::vector<FilterEntry> out;
    HDEVINFO set = SetupDiGetClassDevsW(&KSCATEGORY_RENDER, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE)
        return out;
    for (DWORD i = 0; i < 256; i++) {
        SP_DEVICE_INTERFACE_DATA ifd;
        memset(&ifd, 0, sizeof(ifd));
        ifd.cbSize = sizeof(ifd);
        if (!SetupDiEnumDeviceInterfaces(set, nullptr, &KSCATEGORY_RENDER, i, &ifd))
            break;
        SP_DEVICE_INTERFACE_DATA alias;
        memset(&alias, 0, sizeof(alias));
        alias.cbSize = sizeof(alias);
        if (!SetupDiGetDeviceInterfaceAlias(set, &ifd, &KSCATEGORY_AUDIO, &alias))
            continue;
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &alias, nullptr, 0, &need, nullptr);
        if (need < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W))
            continue;
        std::vector<uint8_t> b(need);
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *d = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)b.data();
        d->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &alias, d, need, nullptr, nullptr))
            continue;
        FilterEntry e;
        e.path = d->DevicePath;
        HKEY key = SetupDiOpenDeviceInterfaceRegKey(set, &alias, 0, KEY_READ);
        if (key != INVALID_HANDLE_VALUE) {
            wchar_t name[256];
            DWORD size = sizeof(name) - sizeof(wchar_t), type = 0;
            if (RegQueryValueExW(key, L"FriendlyName", nullptr, &type, (BYTE *)name, &size) == ERROR_SUCCESS &&
                type == REG_SZ) {
                name[size / sizeof(wchar_t)] = 0;
                e.name = name;
            }
            RegCloseKey(key);
        }
        out.push_back(e);
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

/* When the topology walk finds no software-IO connector: the first render
 * filter of the jack's device that has a sink pin. */
void find_by_device(Target &t, HANDLE ev)
{
    std::wstring dev = device_part(t.jackPath);
    for (const FilterEntry &e : enum_render_filters()) {
        if (device_part(e.path) != dev)
            continue;
        HANDLE f = open_filter(e.path, nullptr);
        if (!f)
            continue;
        ULONG n = pin_count(f, ev);
        for (ULONG i = 0; i < n; i++) {
            PinInfo pi;
            if (describe_pin(f, ev, i, pi) && pi.sink() && !pi.ranges.empty())
                t.pins.push_back(i);
        }
        CloseHandle(f);
        if (!t.pins.empty()) {
            t.path = e.path;
            t.approximate = true;
            return;
        }
    }
}

/* The KS filter and sink pin behind the default render endpoint (eConsole).
 * COM must be initialized on the calling thread. */
bool find_default_target(Target &t, std::string &why, HANDLE ev, bool verbose)
{
    t = Target();
    Com<IMMDeviceEnumerator> en;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  (void **)en.out());
    char tmp[64];
    if (FAILED(hr)) {
        snprintf(tmp, sizeof(tmp), "no MMDevice enumerator (0x%08lX)", (unsigned long)hr);
        why = tmp;
        return false;
    }
    Com<IMMDevice> dev;
    hr = en->GetDefaultAudioEndpoint(eRender, eConsole, dev.out());
    if (FAILED(hr)) {
        snprintf(tmp, sizeof(tmp), "no default render device (0x%08lX)", (unsigned long)hr);
        why = tmp;
        return false;
    }
    Com<IPropertyStore> props;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, props.out()))) {
        /* PKEY_Device_FriendlyName (functiondiscoverykeys_devpkey.h) */
        static const PROPERTYKEY kFriendlyName = {
            {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
            t.name = v.pwszVal;
        PropVariantClear(&v);
    }
    Com<IDeviceTopology> topo;
    Com<IConnector> epConn, jack;
    hr = dev->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, nullptr, (void **)topo.out());
    if (SUCCEEDED(hr))
        hr = topo->GetConnector(0, epConn.out());
    if (SUCCEEDED(hr))
        hr = epConn->GetConnectedTo(jack.out());
    if (FAILED(hr)) {
        snprintf(tmp, sizeof(tmp), "no device topology for the endpoint (0x%08lX)", (unsigned long)hr);
        why = tmp;
        return false;
    }
    Com<IPart> jackPart;
    if (FAILED(jack->QueryInterface(__uuidof(IPart), (void **)jackPart.out()))) {
        why = "no device topology for the endpoint (its connector is not a part)";
        return false;
    }
    UINT lid = 0;
    jackPart->GetLocalId(&lid);
    t.jackPath = part_device_path(jackPart.p);
    t.jackPin = lid & 0xFFFF;
    if (verbose)
        logf("topology walk from the endpoint '%s':", utf8(t.name.c_str()).c_str());
    walk_to_sink_pins(jackPart.p, t, verbose);
    if (t.pins.empty() && !t.jackPath.empty())
        find_by_device(t, ev);
    if (t.pins.empty()) {
        why = "no KS filter with a sink pin behind the default device (not a kernel streaming device?)";
        return false;
    }
    return true;
}

/* ---- the stream ---------------------------------------------------------- */

enum Mode { kNone, kRtEvent, kRtPolled, kStandard };
enum Fail { kFailNone, kFailIo, kFailNoProgress, kFailStorm, kFailWait };

struct Packet {
    KSSTREAM_HEADER hdr;
    OVERLAPPED ov;
    HANDLE ev;
    uint8_t *data;
    bool submitted;
};

struct Stream {
    RenderFn fn = nullptr;
    void *user = nullptr;
    HANDLE filter = nullptr, pin = nullptr;
    HANDLE ioEvent = nullptr;   /* driver calls from start() / stop() */
    HANDLE rtIoEvent = nullptr; /* driver calls from the render thread */
    HANDLE notify = nullptr;    /* WaveRT notifications */
    HANDLE quit = nullptr, ready = nullptr, timer = nullptr, thread = nullptr;
    Mode mode = kNone;
    Fmt fmt;
    KSSTATE state = KSSTATE_STOP;
    bool notifyRegistered = false;
    bool mmcss = false;
    /* WaveRT */
    uint8_t *buf = nullptr;
    uint32_t bufBytes = 0;
    bool barrier = false;
    volatile ULONG *posReg = nullptr;
    uint32_t targetBytes = 0; /* polled: how far ahead of the play position to write */
    /* standard streaming */
    Packet pk[kPackets];
    uint8_t *pkMem = nullptr;
    /* both */
    int periodFrames = 0, bufferFrames = 0;
    std::vector<float> mix;
    std::string device;
    uint32_t possible = 0;      /* the pin's possible instances (Info::instances) */
    /* counters (the render thread writes, stop() reads after the join;
     * underruns() reads the count while it runs) */
    uint32_t periods = 0, underruns = 0;
    double stormStart = 0.0;
    int stormCount = 0;
    double startTime = 0.0;
    double firstUnderrun = 0.0; /* seconds after RUN */
    Fail failReason = kFailNone;
    DWORD failErr = 0;
    std::atomic<bool> failed{false};
    /* PC: the refills' timing (WaveRT with notifications; timing_line):
     * the render thread's window, and the last one it published (a
     * sequence lock: odd while written) */
    struct Timing {
        uint32_t periods = 0, late = 0, tight = 0;
        double wakeMax = 0.0, wakeSum = 0.0;    /* frames into the playing half at the notification */
        double marginMin = 1e9, marginSum = 0.0; /* frames of it left once the other half is written */
        double gapMax = 0.0;                    /* ms between notifications */
        double renderMax = 0.0;                 /* ms from the notification to the refill done */
        double from = 0.0;                      /* the window's start (s) */
    } timing, timingPub;
    std::atomic<uint32_t> timingSeq{0};
    uint32_t fifoBytes = 0;     /* KSRTAUDIO_HWLATENCY.FifoSize */
};

constexpr double kTimingWindow = 10.0;     /* s */
constexpr double kTimingTight = 16.0;      /* frames: a refill with less left than this is tight */

Stream g;
bool g_startBusy;                           /* the last start() found the device busy (start_busy) */

void fail(Stream &s, Fail why, DWORD err)
{
    if (s.failReason == kFailNone) {
        s.failReason = why;
        s.failErr = err;
    }
    s.failed.store(true);
}

void underrun(Stream &s, double now)
{
    if (s.underruns++ == 0)
        s.firstUnderrun = now - s.startTime;
    if (now - s.stormStart > 1.0) {
        s.stormStart = now;
        s.stormCount = 0;
    }
    if (++s.stormCount >= kStormCount)
        fail(s, kFailStorm, 0);
}

float clampf(float v)
{
    if (v > 1.0f)
        return 1.0f;
    if (v < -1.0f)
        return -1.0f;
    if (v != v)
        return 0.0f;
    return v;
}

/* Interleaved stereo float -> the device's format: L / R into the first
 * two channels, the others silent (mono: their mean); clipped. */
void convert(const Fmt &f, const float *src, int frames, uint8_t *dst)
{
    const int ch = f.channels;
    for (int i = 0; i < frames; i++) {
        float l = clampf(src[2 * i]), r = clampf(src[2 * i + 1]);
        for (int c = 0; c < ch; c++) {
            float v = ch == 1 ? (l + r) * 0.5f : (c == 0 ? l : (c == 1 ? r : 0.0f));
            if (f.isFloat) {
                memcpy(dst, &v, 4);
                dst += 4;
            } else if (f.bits == 16) {
                int16_t s = (int16_t)(v * 32767.0f + (v >= 0.0f ? 0.5f : -0.5f));
                memcpy(dst, &s, 2);
                dst += 2;
            } else if (f.bits == 24) {
                int32_t s = (int32_t)(v * 8388607.0f + (v >= 0.0f ? 0.5f : -0.5f));
                dst[0] = (uint8_t)s;
                dst[1] = (uint8_t)(s >> 8);
                dst[2] = (uint8_t)(s >> 16);
                dst += 3;
            } else {
                int32_t s = (int32_t)(v * 2147483647.0 + (v >= 0.0f ? 0.5 : -0.5));
                memcpy(dst, &s, 4);
                dst += 4;
            }
        }
    }
}

/* One period from the caller into one or two runs of the device buffer
 * (two when it wraps). */
void render_period(Stream &s, uint8_t *dst0, int frames0, uint8_t *dst1, int frames1, double now)
{
    int frames = frames0 + frames1;
    memset(s.mix.data(), 0, sizeof(float) * 2 * (size_t)frames);
    s.fn(s.user, s.mix.data(), frames, now);
    convert(s.fmt, s.mix.data(), frames0, dst0);
    if (frames1 > 0)
        convert(s.fmt, s.mix.data() + 2 * frames0, frames1, dst1);
    if (s.barrier)
        MemoryBarrier();
    s.periods++;
}

/* The play position in the WaveRT buffer (bytes, 0..bufBytes-1). */
bool read_position(Stream &s, uint32_t *pos)
{
    if (s.posReg) {
        *pos = (uint32_t)(*s.posReg % s.bufBytes);
        return true;
    }
    KSAUDIO_POSITION p;
    memset(&p, 0, sizeof(p));
    DWORD err = ks_get(s.pin, s.rtIoEvent, KSPROPSETID_Audio, KSPROPERTY_AUDIO_POSITION, &p, sizeof(p));
    if (err != 0) {
        fail(s, kFailIo, err);
        return false;
    }
    *pos = (uint32_t)(p.PlayOffset % s.bufBytes);
    return true;
}

/* WaveRT with notifications: the driver signals when the play position
 * crosses the middle and the end of the buffer; each time the half it just
 * left is refilled.  The halves alternate; the position is read to see a
 * missed notification (the hardware is already back in the half about to
 * be written: a whole period was late) and a late write.  The last quarter
 * of a half counts as "not yet in it": some position registers lag the
 * notification a little.  A stall of a whole buffer or more brings the
 * position round to where it is expected (the halves cannot tell): the time
 * since the last look against how far the position moved does - more than
 * three quarters of a buffer unaccounted for is an underrun (user report:
 * crackles heard, none counted, on onboard audio at a 2.7 ms buffer). */
void run_rt_event(Stream &s)
{
    const int block = s.fmt.block();
    const int f0 = s.bufferFrames / 2, f1 = s.bufferFrames - f0;
    const uint32_t start[2] = {0, (uint32_t)(f0 * block)};
    const uint32_t len[2] = {(uint32_t)(f0 * block), (uint32_t)(f1 * block)};
    const int frames[2] = {f0, f1};
    HANDLE objs[2] = {s.quit, s.notify};
    int last = 1; /* the first notification (into half 1) frees half 0 */
    DWORD silentMs = 0;
    uint32_t lastPos = 0;   /* the position at the last look, and when */
    double lastAt = 0.0;
    bool looked = false;
    double lastWake = 0.0;  /* (the timing) */
    s.timing = Stream::Timing();
    s.timing.from = qpc_now();
    while (!s.failed.load(std::memory_order_relaxed)) {
        DWORD w = WaitForMultipleObjects(2, objs, FALSE, 250);
        if (w == WAIT_OBJECT_0)
            break;
        if (w == WAIT_TIMEOUT) {
            silentMs += 250;
            if (silentMs >= kNoProgressMs)
                fail(s, kFailNoProgress, 0);
            continue;
        }
        if (w != WAIT_OBJECT_0 + 1) {
            fail(s, kFailWait, GetLastError());
            break;
        }
        silentMs = 0;
        double now = qpc_now();
        uint32_t pos;
        if (!read_position(s, &pos))
            break;
        /* before the first period nothing was queued to miss: a
         * notification left over from the start only resynchronizes */
        bool counting = s.periods > 0;
        /* a stall of a buffer or more: the position went round unseen */
        if (counting && looked) {
            const double moved = (double)((pos + s.bufBytes - lastPos) % s.bufBytes) / block;
            const double elapsed = (now - lastAt) * s.fmt.rate;
            if (elapsed - moved > s.bufferFrames * 0.75)
                underrun(s, now);
        }
        int h = 1 - last;
        uint32_t into = (pos + s.bufBytes - start[h]) % s.bufBytes;
        if (into < len[h] - len[h] / 4) {
            if (counting)
                underrun(s, now); /* missed a notification: write the half played next */
            h = 1 - h;
        }
        const double wake = (double)((pos + s.bufBytes - start[1 - h]) % s.bufBytes) / block;
        render_period(s, s.buf + start[h], frames[h], nullptr, 0, now);
        last = h;
        if (!read_position(s, &pos))
            break;
        lastPos = pos;
        lastAt = qpc_now();
        looked = true;
        into = (pos + s.bufBytes - start[h]) % s.bufBytes;
        if (counting && into < len[h] - len[h] / 4)
            underrun(s, now); /* the hardware reached the half while it was being written */
        /* by the clock: the refill took longer than what was left of the
         * half playing at the notification (a register a little behind the
         * notification: all of it) - a stall of 1.6 .. 2.7 ms at a 2.7 ms
         * buffer left the position past the tolerance above, not yet round */
        else if (counting) {
            const double left = wake < frames[1 - h] ? frames[1 - h] - wake : frames[1 - h];
            if ((lastAt - now) * s.fmt.rate > left + 1.0)
                underrun(s, now);
            /* the hardware's FIFO: its DMA fetches ahead of the position the
             * register gives, so a refill done with less than half the FIFO
             * left of the playing half was read too late (the register not
             * behind the notification only).  User report: crackles at the
             * movie -> menu change, no refill late by the register, three
             * with 13..15 frames left of a 32-frame FIFO (17 and more where
             * nothing was heard) */
            else if (wake < frames[1 - h] && s.fifoBytes > 0) {
                const double margin =
                    frames[1 - h] - (double)((pos + s.bufBytes - start[1 - h]) % s.bufBytes) / block;
                if (margin < (double)s.fifoBytes / block / 2.0)
                    underrun(s, now);
            }
        }
        /* the timing: how far into the half it plays the hardware was at
         * the notification, how much of that half was left once this one was
         * written (negative: it was in this one already) */
        if (counting) {
            Stream::Timing &t = s.timing;
            const double margin = frames[1 - h] - (double)((pos + s.bufBytes - start[1 - h]) % s.bufBytes) / block;
            t.periods++;
            t.wakeSum += wake;
            if (wake > t.wakeMax)
                t.wakeMax = wake;
            t.marginSum += margin;
            if (margin < t.marginMin)
                t.marginMin = margin;
            if (margin < 0.0)
                t.late++;
            else if (margin < kTimingTight)
                t.tight++;
            if (lastWake > 0.0 && (now - lastWake) * 1000.0 > t.gapMax)
                t.gapMax = (now - lastWake) * 1000.0;
            if ((lastAt - now) * 1000.0 > t.renderMax)
                t.renderMax = (lastAt - now) * 1000.0;
            if (lastAt - t.from >= kTimingWindow) {
                s.timingSeq.fetch_add(1, std::memory_order_acq_rel);
                s.timingPub = t;
                s.timingSeq.fetch_add(1, std::memory_order_acq_rel);
                t = Stream::Timing();
                t.from = lastAt;
            }
        }
        lastWake = now;
    }
}

/* WaveRT without notifications: a high-resolution timer every half period;
 * the play position is unwrapped into a running byte count and the buffer
 * is kept filled targetBytes ahead of it, one period at a time. */
void run_rt_polled(Stream &s)
{
    const int block = s.fmt.block();
    const uint32_t periodBytes = (uint32_t)(s.periodFrames * block);
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)(s.periodFrames * 5000000.0 / s.fmt.rate); /* half a period, 100 ns units */
    if (due.QuadPart > -2000)
        due.QuadPart = -2000;
    HANDLE objs[2] = {s.quit, s.timer};
    uint64_t playAbs = 0, writeAbs = 0;
    uint32_t lastPos = 0;
    bool primed = false;
    double lastMove = qpc_now(), lastTick = lastMove;
    const double bufSeconds = (double)s.bufferFrames / s.fmt.rate;
    while (!s.failed.load(std::memory_order_relaxed)) {
        if (!SetWaitableTimer(s.timer, &due, 0, nullptr, nullptr, FALSE)) {
            fail(s, kFailWait, GetLastError());
            break;
        }
        DWORD w = WaitForMultipleObjects(2, objs, FALSE, 1000);
        if (w == WAIT_OBJECT_0)
            break;
        if (w != WAIT_OBJECT_0 + 1) {
            fail(s, kFailWait, w == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());
            break;
        }
        double now = qpc_now();
        uint32_t pos;
        if (!read_position(s, &pos))
            break;
        uint32_t delta = (pos + s.bufBytes - lastPos) % s.bufBytes;
        lastPos = pos;
        playAbs += delta;
        if (delta != 0) {
            lastMove = now;
        } else if (now - lastMove > kNoProgressMs / 1000.0) {
            fail(s, kFailNoProgress, 0);
            break;
        }
        /* a stall longer than the buffer hides whole wraps of the position:
         * everything queued was played (and old data again): start over */
        if (primed && now - lastTick > bufSeconds * 0.9) {
            underrun(s, now);
            writeAbs = (playAbs + block - 1) / block * block; /* playAbs stays = pos modulo the buffer */
        }
        lastTick = now;
        if (writeAbs < playAbs) {
            if (primed)
                underrun(s, now);
            writeAbs = (playAbs + block - 1) / block * block;
        }
        while (writeAbs - playAbs < s.targetBytes) {
            uint32_t at = (uint32_t)(writeAbs % s.bufBytes);
            uint32_t first = s.bufBytes - at < periodBytes ? s.bufBytes - at : periodBytes;
            render_period(s, s.buf + at, (int)(first / block), s.buf, (int)((periodBytes - first) / block), now);
            writeAbs += periodBytes;
            primed = true;
            now = qpc_now(); /* a second period in the same tick begins later */
        }
    }
}

DWORD submit_packet(Stream &s, Packet &p)
{
    uint32_t bytes = (uint32_t)(s.periodFrames * s.fmt.block());
    memset(&p.hdr, 0, sizeof(p.hdr));
    p.hdr.Size = sizeof(KSSTREAM_HEADER);
    p.hdr.PresentationTime.Numerator = 1;
    p.hdr.PresentationTime.Denominator = 1;
    p.hdr.Data = p.data;
    p.hdr.FrameExtent = bytes;
    p.hdr.DataUsed = bytes;
    memset(&p.ov, 0, sizeof(p.ov));
    p.ov.hEvent = p.ev;
    p.submitted = true;
    if (!DeviceIoControl(s.pin, IOCTL_KS_WRITE_STREAM, nullptr, 0, &p.hdr, sizeof(p.hdr), nullptr, &p.ov)) {
        DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING) {
            p.submitted = false;
            return err;
        }
    }
    return 0;
}

/* Standard streaming: the packets complete in order; each one is refilled
 * and queued again.  When every packet has completed at the moment the
 * oldest is handled, the driver ran dry: an underrun. */
void run_standard(Stream &s)
{
    int next = 0;
    while (!s.failed.load(std::memory_order_relaxed)) {
        Packet &p = s.pk[next];
        HANDLE objs[2] = {s.quit, p.ev};
        DWORD w = WaitForMultipleObjects(2, objs, FALSE, kNoProgressMs);
        if (w == WAIT_OBJECT_0)
            break;
        if (w == WAIT_TIMEOUT) {
            fail(s, kFailNoProgress, 0);
            break;
        }
        if (w != WAIT_OBJECT_0 + 1) {
            fail(s, kFailWait, GetLastError());
            break;
        }
        double now = qpc_now();
        DWORD n = 0;
        p.submitted = false;
        if (!GetOverlappedResult(s.pin, &p.ov, &n, FALSE)) {
            fail(s, kFailIo, GetLastError());
            break;
        }
        bool allDone = true;
        for (int i = 0; i < kPackets; i++)
            if (i != next && WaitForSingleObject(s.pk[i].ev, 0) != WAIT_OBJECT_0)
                allDone = false;
        if (allDone)
            underrun(s, now);
        render_period(s, p.data, s.periodFrames, nullptr, 0, now);
        DWORD err = submit_packet(s, p);
        if (err != 0) {
            fail(s, kFailIo, err);
            break;
        }
        next = (next + 1) % kPackets;
    }
}

DWORD WINAPI thread_main(void *)
{
    Stream &s = g;
    DWORD task = 0;
    HANDLE av = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    if (av)
        AvSetMmThreadPriority(av, AVRT_PRIORITY_CRITICAL);
    else
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    s.mmcss = av != nullptr;
    s.stormStart = qpc_now();
    SetEvent(s.ready);
    switch (s.mode) {
    case kRtEvent: run_rt_event(s); break;
    case kRtPolled: run_rt_polled(s); break;
    case kStandard: run_standard(s); break;
    default: break;
    }
    /* the looped buffer would repeat its last periods while the pin goes
     * down (PC: on every stop, not only a failure: a restart, the game's
     * close - user report: crackles as the session ended) */
    if (s.buf) {
        memset(s.buf, 0, s.bufBytes);
        MemoryBarrier();
    }
    if (av)
        AvRevertMmThreadCharacteristics(av);
    return 0;
}

/* ---- pin setup and teardown --------------------------------------------- */

DWORD set_state(Stream &s, KSSTATE st)
{
    DWORD err = ks_set(s.pin, s.ioEvent, KSPROPSETID_Connection, KSPROPERTY_CONNECTION_STATE, &st, sizeof(st));
    if (err == 0)
        s.state = st;
    return err;
}

/* Up one state at a time (STOP -> ACQUIRE -> PAUSE -> RUN). */
DWORD state_up(Stream &s, KSSTATE to)
{
    while (s.state < to) {
        DWORD err = set_state(s, (KSSTATE)(s.state + 1));
        if (err != 0)
            return err;
    }
    return 0;
}

void state_down(Stream &s)
{
    while (s.state > KSSTATE_STOP) {
        if (set_state(s, (KSSTATE)(s.state - 1)) != 0) {
            set_state(s, KSSTATE_STOP);
            s.state = KSSTATE_STOP;
        }
    }
}

uint32_t gcd32(uint32_t a, uint32_t b)
{
    while (b) {
        uint32_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

/* A WaveRT buffer of about `bytes' (rounded up to 128 bytes, the HD Audio
 * DMA granularity, and to whole frames in each half); with notifications
 * (two per buffer) when withNotify. */
DWORD rt_buffer(Stream &s, uint32_t bytes, bool withNotify, KSRTAUDIO_BUFFER *out)
{
    uint32_t unit = (uint32_t)(2 * s.fmt.block());
    uint32_t step = unit / gcd32(unit, 128) * 128;
    bytes = (bytes + step - 1) / step * step;
    memset(out, 0, sizeof(*out));
    if (withNotify) {
        KSRTAUDIO_BUFFER_PROPERTY_WITH_NOTIFICATION p;
        memset(&p, 0, sizeof(p));
        p.Property = make_prop(KSPROPSETID_RtAudio, KSPROPERTY_RTAUDIO_BUFFER_WITH_NOTIFICATION, KSPROPERTY_TYPE_GET);
        p.BaseAddress = nullptr;
        p.RequestedBufferSize = bytes;
        p.NotificationCount = 2;
        return ks_ioctl(s.pin, s.ioEvent, IOCTL_KS_PROPERTY, &p, sizeof(p), out, sizeof(*out), nullptr);
    }
    KSRTAUDIO_BUFFER_PROPERTY p;
    memset(&p, 0, sizeof(p));
    p.Property = make_prop(KSPROPSETID_RtAudio, KSPROPERTY_RTAUDIO_BUFFER, KSPROPERTY_TYPE_GET);
    p.BaseAddress = nullptr;
    p.RequestedBufferSize = bytes;
    return ks_ioctl(s.pin, s.ioEvent, IOCTL_KS_PROPERTY, &p, sizeof(p), out, sizeof(*out), nullptr);
}

DWORD rt_notification_event(Stream &s, bool reg)
{
    KSRTAUDIO_NOTIFICATION_EVENT_PROPERTY p;
    memset(&p, 0, sizeof(p));
    p.Property = make_prop(KSPROPSETID_RtAudio,
                           reg ? KSPROPERTY_RTAUDIO_REGISTER_NOTIFICATION_EVENT
                               : KSPROPERTY_RTAUDIO_UNREGISTER_NOTIFICATION_EVENT,
                           KSPROPERTY_TYPE_GET);
    p.NotificationEvent = s.notify;
    return ks_ioctl(s.pin, s.ioEvent, IOCTL_KS_PROPERTY, &p, sizeof(p), &p, sizeof(p), nullptr);
}

/* WaveRT setup on a created pin; sets s.mode, the buffer and the period.
 * Notifications first (event driven), then a polled buffer; each with the
 * size asked for and then larger, in case the driver has a minimum. */
bool setup_rt(Stream &s, int periodFrames, double *hwMs, std::string &why)
{
    const int block = s.fmt.block();
    KSRTAUDIO_BUFFER b;
    memset(&b, 0, sizeof(b));
    DWORD err = 0;
    bool got = false, withNotify = false;
    for (int mul = 1; mul <= 8 && !got && !g_testNoNotify; mul *= 2) {
        err = rt_buffer(s, (uint32_t)(2 * periodFrames * block * mul), true, &b);
        got = err == 0;
        withNotify = got;
    }
    for (int mul = 1; mul <= 8 && !got; mul *= 2) {
        err = rt_buffer(s, (uint32_t)(4 * periodFrames * block * mul), false, &b);
        got = err == 0;
    }
    char e[160];
    if (!got) {
        why = std::string("the WaveRT pin gave no buffer: ") + errstr(err, e, sizeof(e));
        return false;
    }
    s.buf = (uint8_t *)b.BufferAddress;
    s.bufBytes = b.ActualBufferSize - b.ActualBufferSize % (ULONG)block;
    s.barrier = b.CallMemoryBarrier != FALSE;
    if (!s.buf || s.bufBytes < (uint32_t)(4 * block)) {
        why = "the WaveRT pin gave an unusable buffer";
        s.buf = nullptr;
        return false;
    }
    memset(s.buf, 0, b.ActualBufferSize);
    s.bufferFrames = (int)(s.bufBytes / block);

    if (withNotify) {
        s.notify = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (s.notify && rt_notification_event(s, true) == 0)
            s.notifyRegistered = true;
    }

    /* the position: a register mapped into the process, else a property */
    KSRTAUDIO_HWREGISTER_PROPERTY rp;
    KSRTAUDIO_HWREGISTER reg;
    memset(&rp, 0, sizeof(rp));
    memset(&reg, 0, sizeof(reg));
    rp.Property = make_prop(KSPROPSETID_RtAudio, KSPROPERTY_RTAUDIO_POSITIONREGISTER, KSPROPERTY_TYPE_GET);
    err = ks_ioctl(s.pin, s.ioEvent, IOCTL_KS_PROPERTY, &rp, sizeof(rp), &reg, sizeof(reg), nullptr);
    if (err != 0) {
        rp.Property.Flags = KSPROPERTY_TYPE_SET;
        err = ks_ioctl(s.pin, s.ioEvent, IOCTL_KS_PROPERTY, &rp, sizeof(rp), &reg, sizeof(reg), nullptr);
    }
    if (err == 0 && reg.Register && reg.Width >= 32)
        s.posReg = (volatile ULONG *)reg.Register;
    if (!s.posReg) {
        KSAUDIO_POSITION p;
        err = ks_get(s.pin, s.ioEvent, KSPROPSETID_Audio, KSPROPERTY_AUDIO_POSITION, &p, sizeof(p));
        if (err != 0) {
            why = std::string("the WaveRT pin reports no play position: ") + errstr(err, e, sizeof(e));
            return false;
        }
    }

    KSRTAUDIO_HWLATENCY lat;
    memset(&lat, 0, sizeof(lat));
    if (ks_get(s.pin, s.ioEvent, KSPROPSETID_RtAudio, KSPROPERTY_RTAUDIO_HWLATENCY, &lat, sizeof(lat)) == 0) {
        *hwMs = lat.FifoSize * 1000.0 / ((double)s.fmt.rate * block) + (lat.ChipsetDelay + lat.CodecDelay) / 10000.0;
        s.fifoBytes = lat.FifoSize;
        logf("audio_ks: hardware latency: FIFO %lu bytes (%.2f ms), chipset %.2f ms, codec %.2f ms",
             (unsigned long)lat.FifoSize, lat.FifoSize * 1000.0 / ((double)s.fmt.rate * block),
             lat.ChipsetDelay / 10000.0, lat.CodecDelay / 10000.0);
    }

    if (s.notifyRegistered) {
        s.mode = kRtEvent;
        s.periodFrames = s.bufferFrames / 2;
    } else {
        s.mode = kRtPolled;
        s.periodFrames = periodFrames;
        if (3 * s.periodFrames > s.bufferFrames)
            s.periodFrames = s.bufferFrames / 3;
        s.targetBytes = (uint32_t)(2 * s.periodFrames * block);
        s.timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!s.timer)
            s.timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        if (!s.timer) {
            why = "no waitable timer for the polled WaveRT stream";
            return false;
        }
    }
    return true;
}

/* Standard streaming setup: kPackets packets of one period, page-aligned. */
bool setup_standard(Stream &s, int periodFrames, double *hwMs, std::string &why)
{
    s.mode = kStandard;
    s.periodFrames = periodFrames;
    s.bufferFrames = periodFrames * kPackets;
    size_t bytes = (size_t)periodFrames * s.fmt.block();
    size_t stride = (bytes + 4095) & ~(size_t)4095;
    s.pkMem = (uint8_t *)VirtualAlloc(nullptr, stride * kPackets, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!s.pkMem) {
        why = "out of memory for the packets";
        return false;
    }
    for (int i = 0; i < kPackets; i++) {
        Packet &p = s.pk[i];
        memset(&p, 0, sizeof(p));
        p.data = s.pkMem + stride * i;
        p.ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!p.ev) {
            why = "no event for a packet";
            return false;
        }
    }
    KSTIME lat;
    memset(&lat, 0, sizeof(lat));
    if (ks_get(s.pin, s.ioEvent, KSPROPSETID_Audio, KSPROPERTY_AUDIO_LATENCY, &lat, sizeof(lat)) == 0 &&
        lat.Denominator != 0)
        *hwMs = (double)lat.Time * lat.Numerator / lat.Denominator / 10000.0;
    return true;
}

/* Closes the pin and frees everything; no log.  Works from any state. */
void teardown(Stream &s)
{
    if (s.thread) {
        SetEvent(s.quit);
        WaitForSingleObject(s.thread, INFINITE);
        CloseHandle(s.thread);
        s.thread = nullptr;
    }
    bool leakPackets = false;
    if (s.pin) {
        state_down(s);
        if (s.notifyRegistered)
            rt_notification_event(s, false);
        if (s.mode == kStandard) {
            CancelIoEx(s.pin, nullptr);
            for (int i = 0; i < kPackets; i++) {
                Packet &p = s.pk[i];
                if (!p.submitted)
                    continue;
                DWORD n;
                if (WaitForSingleObject(p.ev, kIoTimeoutMs) == WAIT_OBJECT_0)
                    GetOverlappedResult(s.pin, &p.ov, &n, FALSE);
                else
                    leakPackets = true; /* the driver still owns it: keep its memory */
            }
        }
        CloseHandle(s.pin);
        s.pin = nullptr;
    }
    for (int i = 0; i < kPackets; i++) {
        if (s.pk[i].ev && !leakPackets)
            CloseHandle(s.pk[i].ev);
        s.pk[i].ev = nullptr;
        s.pk[i].submitted = false;
    }
    if (s.pkMem && !leakPackets)
        VirtualFree(s.pkMem, 0, MEM_RELEASE);
    s.pkMem = nullptr;
    if (s.filter)
        CloseHandle(s.filter);
    s.filter = nullptr;
    HANDLE *handles[] = {&s.ioEvent, &s.rtIoEvent, &s.notify, &s.quit, &s.ready, &s.timer};
    for (HANDLE *h : handles) {
        if (*h)
            CloseHandle(*h);
        *h = nullptr;
    }
    s.mode = kNone;
    s.state = KSSTATE_STOP;
    s.notifyRegistered = false;
    s.buf = nullptr;
    s.bufBytes = 0;
    s.posReg = nullptr;
    s.barrier = false;
    s.mix.clear();
    s.fn = nullptr;
    s.user = nullptr;
    s.periods = s.underruns = 0;
    s.firstUnderrun = 0.0;
    s.stormCount = 0;
    s.failReason = kFailNone;
    s.failErr = 0;
    s.failed.store(false);
}

/* Undoes a pin whose streaming setup failed (before any state change). */
void drop_pin(Stream &s)
{
    if (s.notifyRegistered)
        rt_notification_event(s, false);
    s.notifyRegistered = false;
    if (s.pin)
        CloseHandle(s.pin);
    s.pin = nullptr;
    s.buf = nullptr;
    s.bufBytes = 0;
    s.posReg = nullptr;
    s.mode = kNone;
    HANDLE *handles[] = {&s.notify, &s.timer};
    for (HANDLE *h : handles) {
        if (*h)
            CloseHandle(*h);
        *h = nullptr;
    }
    for (int i = 0; i < kPackets; i++) {
        if (s.pk[i].ev)
            CloseHandle(s.pk[i].ev);
        s.pk[i].ev = nullptr;
    }
    if (s.pkMem)
        VirtualFree(s.pkMem, 0, MEM_RELEASE);
    s.pkMem = nullptr;
}

/* Creates the pin in the first format it takes and sets up its streaming:
 * WaveRT when the pin offers looped streaming, else (or when the WaveRT
 * setup fails: some drivers offer looped streaming without the RtAudio
 * properties) standard streaming.  A streaming setup that fails once is not
 * tried again with other formats: it does not depend on the format.  Fills
 * s (pin, fmt, mode, buffer); false with the reason in why. */
bool open_pin(Stream &s, const PinInfo &pi, int *periodFrames, double periodMs, double *hwMs, std::string &why)
{
    char e[160];
    std::vector<Fmt> fmts = candidate_formats(pi);
    if (fmts.empty()) {
        why = "the pin has no PCM / float format in common (no 16-32 bit, 32-192 kHz range)";
        return false;
    }
    /* [0]: WaveRT (looped), [1]: standard; a pin that names neither is
     * tried with standard streaming */
    bool usable[2] = {pi.looped, pi.standard || !pi.looped};
    std::string setupWhy[2];
    DWORD lastErr = 0;
    bool tried = false;
    for (const Fmt &f : fmts) {
        for (int ext = 1; ext >= 0; ext--) {
            if (!ext && !(f.channels <= 2 && (f.bits == 16 || f.isFloat)))
                continue;
            for (int m = 0; m < 2; m++) {
                if (!usable[m])
                    continue;
                HANDLE pin = nullptr;
                tried = true;
                DWORD err = create_pin(s.filter, pi.id, f, m == 0, ext != 0, &pin);
                if (err != 0) {
                    lastErr = err;
                    if (busy_error(err)) {
                        why = std::string("busy: the pin is in use (the Windows audio engine or another program "
                                          "holds the device): ") +
                              errstr(err, e, sizeof(e));
                        return false;
                    }
                    continue;
                }
                s.pin = pin;
                s.fmt = f;
                s.state = KSSTATE_STOP;
                int pf = (int)(periodMs * f.rate / 1000.0 + 0.5);
                if (pf < 16)
                    pf = 16;
                *periodFrames = pf;
                bool ok = m == 0 ? setup_rt(s, pf, hwMs, setupWhy[m]) : setup_standard(s, pf, hwMs, setupWhy[m]);
                if (ok) {
                    if (m == 1 && !setupWhy[0].empty())
                        logf("audio_ks: pin %lu offers looped streaming but %s: using standard streaming",
                             (unsigned long)pi.id, setupWhy[0].c_str());
                    return true;
                }
                drop_pin(s);
                usable[m] = false;
            }
            if (!usable[0] && !usable[1]) {
                why = setupWhy[0];
                if (!setupWhy[1].empty())
                    why += (why.empty() ? "" : "; ") + setupWhy[1];
                return false;
            }
        }
    }
    /* a pin taken by another program between the description and here, or
     * whose driver does not report the global count: it refuses every
     * format (HD Audio: error 22, STATUS_INVALID_DEVICE_REQUEST) */
    KSPIN_CINSTANCES gl = {};
    if (pin_get(s.filter, s.ioEvent, pi.id, KSPROPERTY_PIN_GLOBALCINSTANCES, &gl, sizeof(gl)) == 0 &&
        gl.PossibleCount > 0 && gl.CurrentCount >= gl.PossibleCount) {
        why = "busy: " + std::to_string(gl.CurrentCount) + " of " + std::to_string(gl.PossibleCount) +
              " instances in use (the Windows audio engine or another program holds the device)";
        return false;
    }
    why = std::string("the pin took none of its formats (") + std::to_string(fmts.size()) + " tried, last " +
          (tried ? errstr(lastErr, e, sizeof(e)) : "none") + ")";
    if (lastErr == ERROR_BAD_COMMAND || lastErr == ERROR_NO_SYSTEM_RESOURCES)
        why += " - probably busy (another program or the Windows audio engine holds the device)";
    if (!setupWhy[0].empty())
        why += "; " + setupWhy[0];
    return false;
}

const char *mode_name(Mode m)
{
    switch (m) {
    case kRtEvent: return "WaveRT, event driven";
    case kRtPolled: return "WaveRT, polled";
    case kStandard: return "standard streaming";
    default: return "none";
    }
}

} // namespace

void set_log(void (*log)(const char *line))
{
    g_log = log;
}

bool failed()
{
    return g.failed.load();
}

void stop()
{
    Stream &s = g;
    bool running = s.thread != nullptr;
    double secs = running ? qpc_now() - s.startTime : 0.0;
    if (s.thread) {
        SetEvent(s.quit);
        WaitForSingleObject(s.thread, INFINITE);
        CloseHandle(s.thread);
        s.thread = nullptr;
        /* PC: the timing's last window, unfinished (timing_line) */
        if (s.timing.periods > 0) {
            s.timingSeq.fetch_add(1, std::memory_order_acq_rel);
            s.timingPub = s.timing;
            s.timingSeq.fetch_add(1, std::memory_order_acq_rel);
        }
    }
    if (running) {
        char e[160];
        const char *reason = "";
        switch (s.failReason) {
        case kFailIo: reason = "an I/O error"; break;
        case kFailNoProgress: reason = "the device stopped (no notification / position change for 1 s)"; break;
        case kFailStorm: reason = "an underrun storm"; break;
        case kFailWait: reason = "a wait error"; break;
        default: break;
        }
        char first[48] = "";
        if (s.underruns > 0)
            snprintf(first, sizeof(first), " (the first %.3f s after the start)", s.firstUnderrun);
        logf("audio_ks: stopped after %.2f s: %u periods, %u underruns%s%s%s%s", secs, s.periods, s.underruns, first,
             s.failReason != kFailNone ? "; the stream had failed: " : "", reason,
             s.failErr ? (std::string(", ") + errstr(s.failErr, e, sizeof(e))).c_str() : "");
    }
    teardown(s);
}

namespace {

/* start() on the default endpoint's pin, or on `forced' (a filter and its
 * pins: the test tool's way to reach devices that are not the default). */
bool start_impl(const Target *forced, RenderFn fn, void *user, double periodMs, Info *info)
{
    stop();
    Stream &s = g;
    g_startBusy = false;
    if (!fn) {
        logf("audio_ks: no render function");
        return false;
    }
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    g_qpcToSeconds = 1.0 / (double)freq.QuadPart;
    if (!(periodMs >= 0.5))
        periodMs = 0.5;
    if (periodMs > 100.0)
        periodMs = 100.0;

    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool uninit = SUCCEEDED(hrCom); /* RPC_E_CHANGED_MODE: an STA thread, which is fine */
    s.ioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    s.rtIoEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    s.quit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    s.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::string why;
    Target t;
    bool ok = s.ioEvent && s.rtIoEvent && s.quit && s.ready;
    if (!ok)
        why = "no events";
    if (ok && forced)
        t = *forced;
    else if (ok)
        ok = find_default_target(t, why, s.ioEvent, false);
    if (uninit)
        CoUninitialize();
    if (!ok) {
        logf("audio_ks: %s", why.c_str());
        teardown(s);
        return false;
    }
    s.device = utf8(t.name.c_str());
    std::string path = utf8(t.path.c_str());

    char e[160];
    DWORD err = 0;
    s.filter = open_filter(t.path, &err);
    if (!s.filter) {
        logf("audio_ks: '%s': cannot open the KS filter %s: %s", s.device.c_str(), path.c_str(),
             errstr(err, e, sizeof(e)));
        teardown(s);
        return false;
    }

    /* the candidate pins, as found; then any other sink pin of the filter
     * (in case a driver numbers its topology parts differently) */
    std::vector<ULONG> pins = t.pins;
    ULONG n = pin_count(s.filter, s.ioEvent);
    for (ULONG i = 0; i < n; i++) {
        bool have = false;
        for (ULONG x : pins)
            have = have || x == i;
        if (!have)
            pins.push_back(i);
    }
    int periodFrames = 0;
    double hwMs = 0.0;
    bool opened = false, busy = false;
    std::string reasons;
    for (ULONG id : pins) {
        PinInfo pi;
        if (!describe_pin(s.filter, s.ioEvent, id, pi) || !pi.sink() || pi.ranges.empty())
            continue;
        if (pi.busy()) {
            const KSPIN_CINSTANCES &c = pi.haveGlobal && pi.global.PossibleCount > 0 ? pi.global : pi.instances;
            why = "busy: " + std::to_string(c.CurrentCount) + " of " + std::to_string(c.PossibleCount) +
                  " instances in use (the Windows audio engine or another program holds the device)";
            busy = true;
        } else if (open_pin(s, pi, &periodFrames, periodMs, &hwMs, why)) {
            const KSPIN_CINSTANCES &c = pi.haveGlobal && pi.global.PossibleCount > 0 ? pi.global : pi.instances;
            s.possible = pi.haveGlobal || pi.haveInstances ? c.PossibleCount : 0;
            logf("audio_ks: '%s': %s pin %lu ('%s')%s, %s", s.device.c_str(), path.c_str(), (unsigned long)id,
                 utf8(pi.name.c_str()).c_str(), t.approximate ? " (found by device, not by topology)" : "",
                 s.possible == 0 ? "instances not reported"
                 : s.possible == 1 ? "1 instance: the device is this program's alone while it plays"
                                   : "several instances: shared with the Windows audio engine");
            opened = true;
            break;
        }
        /* (open_pin's busy reasons: "busy: ...", "... - probably busy (...)") */
        busy = busy || why.find("busy") != std::string::npos;
        reasons += (reasons.empty() ? "" : "; ") + std::string("pin ") + std::to_string(id) + ": " + why;
    }
    if (!opened) {
        logf("audio_ks: '%s': no kernel streaming: %s", s.device.c_str(),
             reasons.empty() ? "the filter has no audio sink pin" : reasons.c_str());
        g_startBusy = busy;
        teardown(s);
        return false;
    }

    s.fn = fn;
    s.user = user;
    s.mix.assign((size_t)(s.periodFrames + 2) * 2, 0.0f);

    /* Info is complete before the render thread exists, so the first
     * RenderFn call can read it (the caller sets up its rate converter from
     * Info::rate).  No RenderFn call is made before the thread: the buffer
     * and the first packets start silent. */
    int block = s.fmt.block();
    double queued = s.mode == kRtPolled ? (double)s.targetBytes / block : (double)s.bufferFrames;
    double latencyMs = queued * 1000.0 / s.fmt.rate + hwMs;
    if (info) {
        memset(info, 0, sizeof(*info));
        snprintf(info->device, sizeof(info->device), "%s", s.device.c_str());
        info->rate = s.fmt.rate;
        info->channels = s.fmt.channels;
        info->bits = s.fmt.bits;
        info->isFloat = s.fmt.isFloat;
        info->waveRt = s.mode != kStandard;
        info->periodFrames = s.periodFrames;
        info->bufferFrames = s.bufferFrames;
        info->latencyMs = latencyMs;
        info->instances = s.possible > (uint32_t)INT_MAX ? INT_MAX : (int)s.possible;
    }

    /* standard streaming: the packets go in (silent) before RUN */
    err = state_up(s, KSSTATE_PAUSE);
    if (err == 0 && s.mode == kStandard)
        for (int i = 0; i < kPackets && err == 0; i++) {
            memset(s.pk[i].data, 0, (size_t)s.periodFrames * s.fmt.block());
            err = submit_packet(s, s.pk[i]);
        }
    if (err != 0) {
        logf("audio_ks: '%s': the pin would not get ready: %s", s.device.c_str(), errstr(err, e, sizeof(e)));
        teardown(s);
        return false;
    }
    s.startTime = qpc_now(); /* before the thread: it reads it */
    s.thread = CreateThread(nullptr, 0, thread_main, nullptr, 0, nullptr);
    if (!s.thread) {
        logf("audio_ks: no render thread: %s", errstr(GetLastError(), e, sizeof(e)));
        teardown(s);
        return false;
    }
    WaitForSingleObject(s.ready, kIoTimeoutMs);
    err = state_up(s, KSSTATE_RUN);
    if (err != 0) {
        logf("audio_ks: '%s': the pin would not run: %s", s.device.c_str(), errstr(err, e, sizeof(e)));
        teardown(s);
        return false;
    }

    logf("audio_ks: %s, %d Hz, %d channels, %d-bit%s%s; period %d frames (%.2f ms), buffer %d frames (%.2f ms)%s, "
         "device latency %.2f ms, latency %.2f ms, %s",
         mode_name(s.mode), s.fmt.rate, s.fmt.channels, s.fmt.isFloat ? 32 : s.fmt.valid,
         s.fmt.isFloat ? " float" : "", (!s.fmt.isFloat && s.fmt.valid != s.fmt.bits) ? " in 32" : "",
         s.periodFrames, s.periodFrames * 1000.0 / s.fmt.rate, s.bufferFrames, s.bufferFrames * 1000.0 / s.fmt.rate,
         s.mode == kRtPolled || s.mode == kRtEvent ? (s.posReg ? ", position register" : ", position property") : "",
         hwMs, latencyMs, s.mmcss ? "MMCSS Pro Audio" : "no MMCSS (time-critical priority)");
    return true;
}

} // namespace

bool start(RenderFn fn, void *user, double periodMs, Info *info)
{
    return start_impl(nullptr, fn, user, periodMs, info);
}

bool start_busy()
{
    return g_startBusy;
}

uint32_t underruns()
{
    /* (one aligned word the render thread increments: MSVC's volatile read) */
    return *(volatile uint32_t *)&g.underruns;
}

bool timing_line(char *out, size_t size)
{
    static uint32_t seen = 0;
    const uint32_t a = g.timingSeq.load(std::memory_order_acquire);
    if ((a & 1) != 0 || a == seen)
        return false;
    const Stream::Timing t = g.timingPub;
    if (g.timingSeq.load(std::memory_order_acquire) != a || t.periods == 0)
        return false;
    seen = a;
    const double fpm = g.fmt.rate / 1000.0;    /* frames per ms */
    snprintf(out, size,
             "timing of %u refills (period %d frames, FIFO %u bytes): the notification %.1f frames into the half "
             "on average (at most %.1f, %.2f ms); left of it once the other was written %.1f on average, at least "
             "%.1f; %u tight (under %.0f), %u late; the longest gap between notifications %.2f ms, the longest "
             "refill %.2f ms",
             t.periods, g.periodFrames, g.fifoBytes, t.wakeSum / t.periods, t.wakeMax, t.wakeMax / fpm,
             t.marginSum / t.periods, t.marginMin, t.tight, kTimingTight, t.late, t.gapMax, t.renderMax);
    return true;
}

} // namespace audio_ks
