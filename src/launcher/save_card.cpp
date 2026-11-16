/* save_card.cpp - what a save folder holds (save_card.h).  PC (2026-11-15). */
#include "save_card.h"

#include "util.h"

#include <algorithm>
#include <cstring>

namespace save_card {
namespace {

constexpr size_t kIconSysSize = 0x3C4;
constexpr size_t kThumbSize = 0x60;
constexpr size_t kTitleAt = 0xC0, kTitleLen = 68, kIconNameAt = 0x104, kIconNameLen = 64;

std::vector<uint8_t> read_file(const std::wstring &p, size_t max)
{
    std::vector<uint8_t> d;
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return d;
    LARGE_INTEGER size = {};
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0) {
        d.resize((size_t)std::min<long long>(size.QuadPart, (long long)max));
        DWORD got = 0;
        if (!ReadFile(h, d.data(), (DWORD)d.size(), &got, nullptr))
            got = 0;
        d.resize(got);
    }
    CloseHandle(h);
    return d;
}

/* Shift-JIS text, the full-width letters, digits and signs made plain
 * (U+FF01..FF5E, the ideographic space, the minus sign). */
std::wstring plain_sjis(const uint8_t *p, size_t n)
{
    size_t len = 0;
    while (len < n && p[len] != 0)
        len++;
    if (len == 0)
        return std::wstring();
    const int w = MultiByteToWideChar(932, 0, (const char *)p, (int)len, nullptr, 0);
    std::wstring s((size_t)std::max(w, 0), L'\0');
    if (w > 0)
        MultiByteToWideChar(932, 0, (const char *)p, (int)len, &s[0], w);
    for (wchar_t &c : s) {
        if (c >= 0xFF01 && c <= 0xFF5E)
            c = (wchar_t)(c - 0xFEE0);
        else if (c == 0x3000)
            c = L' ';
        else if (c == 0x2212)
            c = L'-';
    }
    while (!s.empty() && s.back() == L' ')
        s.pop_back();
    return s;
}

/* An ASCII field of the thumbnail (the comment, the mode): printable
 * characters up to its end or a 0. */
std::wstring ascii_field(const std::vector<uint8_t> &t, size_t at, size_t n)
{
    std::wstring s;
    for (size_t i = at; i < at + n && i < t.size() && t[i] != 0; i++)
        if (t[i] >= 0x20 && t[i] < 0x7F)
            s += (wchar_t)t[i];
    while (!s.empty() && s.back() == L' ')
        s.pop_back();
    return s;
}

template <typename T> T field(const std::vector<uint8_t> &t, size_t at)
{
    T v{};
    if (at + sizeof(T) <= t.size())
        memcpy(&v, t.data() + at, sizeof(T));
    return v;
}

int count_entries(const std::wstring &dir)
{
    int n = 2;                                  /* "." and ".." */
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE)
        return n;
    do {
        if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L".."))
            n++;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return n;
}

/* memcard.cpp used_clusters; *latest: the latest write found */
int used_clusters(const std::wstring &dir, FILETIME *latest)
{
    int used = (count_entries(dir) + 1) / 2;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE)
        return used;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        if (latest != nullptr && CompareFileTime(&fd.ftLastWriteTime, latest) > 0)
            *latest = fd.ftLastWriteTime;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            used += used_clusters(dir + L"\\" + fd.cFileName, latest);
        else
            used += (int)((((ULONGLONG)fd.nFileSizeHigh << 32 | fd.nFileSizeLow) + 1023) / 1024);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return used;
}

/* format_time (T494): "1'23.456", "1:02'03.456" */
std::wstring race_time(uint32_t ms)
{
    wchar_t b[64];
    const uint32_t msec = ms % 1000, rest = ms / 1000, sec = rest % 60, min = (rest / 60) % 60, hours = rest / 3600;
    if (hours != 0)
        swprintf(b, 64, L"%u:%02u'%02u.%03u", hours, min, sec, msec);
    else
        swprintf(b, 64, L"%u'%02u.%03u", min, sec, msec);
    return b;
}

std::wstring fixed1(double v)
{
    wchar_t b[32];
    swprintf(b, 32, L"%.1f", v);
    return b;
}

void set_error(std::wstring *error, const std::wstring &what, DWORD code)
{
    if (error != nullptr)
        *error = what + L": " + util::system_error_text(code);
}

int kind_order(Kind k)
{
    switch (k) {
    case Kind::game: return 0;
    case Kind::replay: return 1;
    case Kind::bestLap: return 2;
    case Kind::licence: return 3;
    case Kind::setting: return 4;
    default: return 5;
    }
}

} // namespace

std::wstring number(long long v)
{
    const bool neg = v < 0;
    unsigned long long u = neg ? (unsigned long long)(-(v + 1)) + 1 : (unsigned long long)v;
    std::wstring s;
    int digits = 0;
    do {
        if (digits > 0 && digits % 3 == 0)
            s += L',';
        s += (wchar_t)(L'0' + u % 10);
        u /= 10;
        digits++;
    } while (u != 0);
    if (neg)
        s += L'-';
    std::reverse(s.begin(), s.end());
    return s;
}

std::wstring date_text(const FILETIME &ft)
{
    if (ft.dwLowDateTime == 0 && ft.dwHighDateTime == 0)
        return std::wstring();
    FILETIME local;
    SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&ft, &local) || !FileTimeToSystemTime(&local, &st))
        return std::wstring();
    wchar_t d[64] = L"", t[64] = L"";
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &st, nullptr, d, 64, nullptr);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, t, 64);
    return std::wstring(d) + L" " + t;
}

int used_kb(const std::wstring &folder)
{
    std::wstring f = folder;
    while (!f.empty() && (f.back() == L'\\' || f.back() == L'/'))
        f.pop_back();
    return used_clusters(f, nullptr);
}

Card read(const std::wstring &folderIn)
{
    Card card;
    std::wstring folder = folderIn;
    while (!folder.empty() && (folder.back() == L'\\' || folder.back() == L'/'))
        folder.pop_back();
    const DWORD a = GetFileAttributesW(folder.c_str());
    card.there = a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
    if (!card.there)
        return card;
    card.usedKB = used_clusters(folder, nullptr);
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((folder + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE)
        return card;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !wcscmp(fd.cFileName, L".") ||
            !wcscmp(fd.cFileName, L".."))
            continue;
        Save s;
        s.dir = fd.cFileName;
        const std::wstring path = folder + L"\\" + s.dir;
        s.saved = fd.ftLastWriteTime;
        s.kb = used_clusters(path, &s.saved);
        const std::vector<uint8_t> sys = read_file(path + L"\\icon.sys", kIconSysSize);
        if (sys.size() >= kTitleAt + kTitleLen && memcmp(sys.data(), "PS2D", 4) == 0) {
            const size_t split = std::min<size_t>(field<uint16_t>(sys, 6), kTitleLen);
            s.title1 = plain_sjis(sys.data() + kTitleAt, split);
            s.title2 = plain_sjis(sys.data() + kTitleAt + split, kTitleLen - split);
            if (sys.size() >= kIconNameAt + kIconNameLen) {
                std::string name((const char *)sys.data() + kIconNameAt, kIconNameLen);
                name = name.substr(0, name.find('\0'));
                if (!name.empty() && name.find_first_of("\\/:") == std::string::npos)
                    s.icon = path + L"\\" + util::widen(name);
            }
        }
        if (s.title1.empty() && s.title2.empty())
            s.title1 = s.dir;
        s.thumb = read_file(path + L"\\thumbnail", kThumbSize + 1);
        if (s.thumb.size() != kThumbSize)
            s.thumb.clear();
        if (!s.thumb.empty()) {
            switch (field<int32_t>(s.thumb, 0)) {
            case 0: s.kind = Kind::game; break;
            case 1: s.kind = Kind::setting; break;
            case 2: s.kind = Kind::replay; break;
            case 3: {
                const int32_t category = field<int32_t>(s.thumb, 0x54);
                s.kind = category >= 9 && category < 15 ? Kind::licence : Kind::bestLap;
                break;
            }
            default: s.thumb.clear(); break;
            }
        }
        if (s.dir.size() >= 2 && s.dir[0] == L'B')
            s.region = s.dir[1] == L'A' ? L"USA" : s.dir[1] == L'E' ? L"Europe" : s.dir[1] == L'I' ? L"Japan" : L"";
        card.saves.push_back(s);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    std::sort(card.saves.begin(), card.saves.end(), [](const Save &x, const Save &y) {
        if (kind_order(x.kind) != kind_order(y.kind))
            return kind_order(x.kind) < kind_order(y.kind);
        return CompareFileTime(&x.saved, &y.saved) > 0;
    });
    return card;
}

std::wstring title(const Save &s)
{
    if (s.title1.empty())
        return s.title2;
    return s.title2.empty() ? s.title1 : s.title1 + L" " + s.title2;
}

std::wstring kind_name(Kind k, bool plural)
{
    switch (k) {
    case Kind::game: return L"Game data";
    case Kind::setting: return plural ? L"Car settings" : L"Car setting";
    case Kind::replay: return plural ? L"Replays" : L"Replay";
    case Kind::bestLap: return plural ? L"Best lap replays" : L"Best lap replay";
    case Kind::licence: return plural ? L"License replays" : L"License replay";
    default: return plural ? L"Other saves" : L"Other save";
    }
}

std::wstring game_summary(const Save &s)
{
    if (s.kind != Kind::game || s.thumb.empty())
        return std::wstring();
    static const wchar_t *const licences[] = {L"B", L"A", L"IB", L"IA", L"S", L"D"};   /* T069 g_002A4E08 */
    const uint32_t licence = field<uint32_t>(s.thumb, 0x14);
    std::wstring t = L"Day " + number(field<int32_t>(s.thumb, 0x18)) + L", Cr. " +
                     number(field<int64_t>(s.thumb, 0x08)) + L", " + number(field<int32_t>(s.thumb, 0x10)) +
                     L" cars";
    if (licence < 6)
        t += std::wstring(L", license ") + licences[licence];
    const int tenths = (int)(field<float>(s.thumb, 0x1C) * 10.0f);
    t += L", " + std::to_wstring(tenths / 10) + L"." + std::to_wstring(tenths % 10) + L" %";
    return t;
}

std::wstring details(const Save &s)
{
    std::wstring t = title(s);
    t += L"\r\n" + kind_name(s.kind) + (s.region.empty() ? L"" : L" (" + s.region + L")") + L", " + number(s.kb) +
         L" KB, saved " + date_text(s.saved);
    const std::vector<uint8_t> &th = s.thumb;
    if (!th.empty()) {
        const std::wstring comment = ascii_field(th, 0x28, 0x10), mode = ascii_field(th, 0x39, 0x18);
        switch (s.kind) {
        case Kind::game:
            t += L"\r\n" + game_summary(s);
            break;
        case Kind::setting: {
            const float power = field<float>(th, 0x18);
            if (power > 0.0f)
                t += L"\r\nPower " + number((long long)power) + L" PS";
            break;
        }
        case Kind::replay: {
            std::wstring line;
            const int32_t time = field<int32_t>(th, 0x08), laps = field<int32_t>(th, 0x20);
            const float power = field<float>(th, 0x24);
            if (time > 0)
                line += L"Time " + race_time((uint32_t)time);
            if (laps > 0)
                line += (line.empty() ? L"" : L", ") + number(laps) + (laps < 2 ? L" lap" : L" laps");
            if (power > 0.0f)
                line += (line.empty() ? L"" : L", ") + number((long long)power) + L" PS";
            if (!line.empty())
                t += L"\r\n" + line;
            break;
        }
        case Kind::bestLap:
        case Kind::licence: {
            std::wstring line;
            if (field<int32_t>(th, 0x54) == 0x11) {     /* MC_CATEGORY_MAX_SPEED: a top speed (m/s) */
                const float mps = field<float>(th, 0x08);
                if (mps > 0.0f)
                    line += L"Top speed " + fixed1(mps * 3.6) + L" km/h";
            } else if (field<int32_t>(th, 0x08) > 0) {
                line += L"Time " + race_time((uint32_t)field<int32_t>(th, 0x08));
            }
            const float power = field<float>(th, 0x20), mass = field<float>(th, 0x24);
            if (power > 0.0f)
                line += (line.empty() ? L"" : L", ") + number((long long)power) + L" PS";
            if (mass > 0.0f)
                line += (line.empty() ? L"" : L", ") + number((long long)mass) + L" kg";
            if (!line.empty())
                t += L"\r\n" + line;
            break;
        }
        default:
            break;
        }
        if (!mode.empty())
            t += L"\r\nMode: " + mode;
        if (!comment.empty())
            t += L"\r\nComment: " + comment;
    }
    t += L"\r\nFolder: " + s.dir;
    return t;
}

bool remove_tree(const std::wstring &dir, bool self, std::wstring *error)
{
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    bool ok = true;
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
                continue;
            const std::wstring p = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
                SetFileAttributesW(p.c_str(), fd.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                if (!remove_tree(p, true, error))
                    ok = false;
            } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!RemoveDirectoryW(p.c_str())) {     /* a link: the link only */
                    set_error(error, p, GetLastError());
                    ok = false;
                }
            } else if (!DeleteFileW(p.c_str())) {
                set_error(error, p, GetLastError());
                ok = false;
            }
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    if (ok && self && !RemoveDirectoryW(dir.c_str())) {
        set_error(error, dir, GetLastError());
        ok = false;
    }
    return ok;
}

bool copy_tree(const std::wstring &from, const std::wstring &to, std::wstring *error)
{
    if (!CreateDirectoryW(to.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        set_error(error, to, GetLastError());
        return false;
    }
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((from + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) {
        set_error(error, from, GetLastError());
        return false;
    }
    bool ok = true;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        const std::wstring s = from + L"\\" + fd.cFileName, d = to + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!copy_tree(s, d, error))
                ok = false;
        } else if (!CopyFileW(s.c_str(), d.c_str(), FALSE)) {
            set_error(error, s, GetLastError());
            ok = false;
        }
    } while (ok && FindNextFileW(f, &fd));
    FindClose(f);
    /* the folder's times as the original's (the card's dates) */
    HANDLE src = CreateFileW(from.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    HANDLE dst = CreateFileW(to.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (src != INVALID_HANDLE_VALUE && dst != INVALID_HANDLE_VALUE) {
        FILETIME c, a, w;
        if (GetFileTime(src, &c, &a, &w))
            SetFileTime(dst, &c, &a, &w);
    }
    if (src != INVALID_HANDLE_VALUE)
        CloseHandle(src);
    if (dst != INVALID_HANDLE_VALUE)
        CloseHandle(dst);
    return ok;
}

} // namespace save_card
