/* install.cpp - installs the game files from the user's disc image (install.h).
 *
 * The unpack reads the volume's files in disc order (one sequential pass over
 * the image, the cache manager reading ahead) and writes each to its loose
 * file; gzip-packed files are inflated in memory and checked against their
 * CRC-32 and size before they are written. */
#include "install.h"
#include "inflate.h"
#include "util.h"
#include "volume.h"

#include <windows.h>

#include <string.h>

#include <algorithm>
#include <vector>

namespace install {
namespace {

constexpr const wchar_t *kPartSuffix = L".part";     /* data.part while unpacking */
constexpr const wchar_t *kOldSuffix = L".old";       /* the replaced data folder, until deleted */

/* The Gran Turismo 3 engine releases known by name, by their boot file;
 * any other disc with CORE.GT3 is installed too (release_name). */
struct Release {
    const char *boot;
    const wchar_t *name;
};
constexpr Release kReleases[] = {
    {"SCUS_971.02", L"Gran Turismo 3 A-Spec (USA, SCUS-97102)"},
    {"PBPX_955.03", L"Gran Turismo 3 A-Spec (USA, PBPX-95503)"},
    {"SCES_502.94", L"Gran Turismo 3 A-Spec (Europe, SCES-50294)"},
    {"SCPS_150.09", L"Gran Turismo 3 A-Spec (Japan, SCPS-15009)"},
    {"SCPS_150.10", L"Gran Turismo Concept 2001 Tokyo (Japan, SCPS-15010)"},
    {"SCES_508.58", L"Gran Turismo Concept 2002 Tokyo-Geneva (Europe, SCES-50858)"},
    {"SCPS_560.05", L"Gran Turismo Concept 2002 Tokyo-Seoul (Korea, SCPS-56005)"},
};
/* What an earlier launcher installed (the only disc it took). */
constexpr const wchar_t *kEarlierRelease = L"Gran Turismo 3 A-Spec (USA, SCUS-97102)";

bool same_text(const std::string &a, const char *b)
{
    return _stricmp(a.c_str(), b) == 0;
}

/* The playable version of a boot file (the bundle has the US disc's files),
 * else data only. */
Version version_of(const std::string &boot)
{
    if (same_text(boot, "SCUS_971.02") || same_text(boot, "PBPX_955.03"))
        return Version::us;
    if (same_text(boot, "SCES_502.94"))
        return Version::eu;
    if (same_text(boot, "SCPS_150.09"))
        return Version::jp;
    return Version::data_only;
}

/* A disc's data folder: its version's, else data_<boot file> (a demo
 * disc's game, or a disc without SYSTEM.CNF: data_<its folder or volume
 * name>), with the characters a folder name cannot have replaced. */
std::wstring data_folder_of(Version v, const std::string &boot, const std::string &folder, const std::string &volume_id)
{
    if (v != Version::data_only)
        return version_folder(v);
    std::string what = !folder.empty() ? folder : !boot.empty() ? boot : volume_id.empty() ? "disc" : volume_id;
    for (char &c : what)
        if (c < 0x20 || strchr("<>:\"/\\|?* ", c) != nullptr)
            c = '_';
    return std::wstring(kDataFolder) + L"_" + util::widen(what);
}

/* The courses' region folder of a language (the game's course_set_region:
 * Japanese JP, English (US) US, English (UK) UK, the others EU). */
std::string course_region(const std::string &language)
{
    if (same_text(language, "JP"))
        return "JP";
    if (same_text(language, "US"))
        return "US";
    if (same_text(language, "BR"))
        return "UK";
    return "EU";
}

/* The unified structure (install.h): a volume path's place in the data
 * folder.  `flatCourses`: the volume has its courses in courses/ itself (the
 * Japanese disc), which go into courses/<region>/ (courses/.list stays). */
std::string unified_path(const std::string &path, bool flatCourses, const std::string &region)
{
    if (flatCourses && path.size() > 8 && _strnicmp(path.c_str(), "courses/", 8) == 0 &&
        path.find('/', 8) == std::string::npos && _stricmp(path.c_str() + 8, ".list") != 0)
        return path.substr(0, 8) + region + "/" + path.substr(8);
    return path;
}

/* What the game needs to know of a release, from its CORE.GT3: the car
 * database files its code reads, and its own language (install.h). */
struct CoreFacts {
    std::string database, strings, unistrings;  /* "database/paramdb_eu.db", ... ("" unknown) */
    std::string language;                       /* "US", "BR", "JP" */
};

/* "SCES_502.94" -> "SCES-50294". */
std::string serial(const std::string &boot)
{
    std::string s;
    for (char c : boot)
        if (c == '_')
            s += '-';
        else if (c != '.')
            s += c;
    return s;
}

/* The release's name: a known boot file's, else what the disc says of it
 * (its volume name, the game's folder, the boot file). */
std::wstring release_name(const std::string &boot, const std::string &folder, const std::string &volume_id)
{
    if (folder.empty())
        for (const Release &r : kReleases)
            if (same_text(boot, r.boot))
                return r.name;
    std::string what = volume_id.empty() ? std::string("a disc") : volume_id;
    if (!folder.empty())
        what += "\\" + folder;
    else if (!boot.empty())
        what += ", " + serial(boot);
    return L"A Gran Turismo 3 engine game (" + util::widen(what) + L")";
}

/* The release's own language, by its car database (each release's code
 * reads its region's: the US build _us, the PAL build _eu, the Japanese one
 * none; GT Concept _bs (Japan), _gtc_eu (Europe), _kr (Korea: English
 * here)), else by its boot file's prefix. */
std::string language_of(const std::string &database, const std::string &boot)
{
    static const char kBase[] = "database/paramdb";
    const size_t nb = sizeof(kBase) - 1;
    if (database.size() >= nb + 3) {
        const std::string suffix = database.substr(nb, database.size() - nb - 3);
        auto ends = [&suffix](const char *e) {
            const size_t n = strlen(e);
            return suffix.size() >= n && _stricmp(suffix.c_str() + suffix.size() - n, e) == 0;
        };
        if (suffix.empty() || ends("_bs"))
            return "JP";
        if (ends("_us") || ends("_kr") || ends("_tw") || ends("_cn"))
            return "US";
        if (ends("_eu"))
            return "BR";
    }
    const std::string p = boot.substr(0, 4);
    if (same_text(p, "SCES") || same_text(p, "SLES") || same_text(p, "SCED"))
        return "BR";
    if (same_text(p, "SCPS") || same_text(p, "SLPS") || same_text(p, "SLPM") || same_text(p, "SCAJ") ||
        same_text(p, "PCPX") || same_text(p, "PAPX"))
        return "JP";
    return "US";
}

/* The NUL-terminated string of `body` at the first `prefix` that ends in
 * `suffix` ("database/paramdb" .. ".db"), "" if none. */
std::string find_string(const std::vector<uint8_t> &body, const char *prefix, const char *suffix)
{
    const size_t np = strlen(prefix), ns = strlen(suffix);
    for (size_t i = 0; i + np < body.size(); i++) {
        if (memcmp(&body[i], prefix, np) != 0)
            continue;
        size_t e = i + np;
        while (e < body.size() && e - i < 128 && body[e] >= 0x20 && body[e] < 0x7F)
            e++;
        if (e < body.size() && body[e] == 0 && e - i >= np + ns && memcmp(&body[e - ns], suffix, ns) == 0)
            return std::string((const char *)&body[i], e - i);
    }
    return std::string();
}

/* The facts of a CORE.GT3 (its bytes: two load-source bytes, the u32
 * unpacked size, raw deflate data; tools/gt3/gt3pack.py); the language by
 * the boot file alone when it cannot be unpacked. */
CoreFacts core_facts(const std::vector<uint8_t> &core, const std::string &boot)
{
    CoreFacts f;
    std::vector<uint8_t> body;
    std::wstring err;
    if (core.size() > 6) {
        uint32_t raw = 0;
        memcpy(&raw, core.data() + 2, 4);
        if (inflate::inflate_raw(core.data() + 6, core.size() - 6, raw, &body, &err)) {
            f.database = find_string(body, "database/paramdb", ".db");
            f.strings = find_string(body, "database/paramstr", ".db");
            f.unistrings = find_string(body, "database/paramunistr", ".db");
        }
    }
    f.language = language_of(f.database, boot);
    return f;
}

/* The marker's lines before the counts (install.h). */
std::string marker_head(const std::wstring &release, const std::string &boot, const CoreFacts &f)
{
    const std::string name = util::narrow(release);
    std::string m = name + ": the game files, unpacked from its volume\r\n";
    m += "release " + name + "\r\n";
    if (!boot.empty())
        m += "boot " + boot + "\r\n";
    m += "language " + f.language + "\r\n";
    if (!f.database.empty())
        m += "database " + f.database + "\r\n";
    if (!f.strings.empty())
        m += "strings " + f.strings + "\r\n";
    if (!f.unistrings.empty())
        m += "unistrings " + f.unistrings + "\r\n";
    return m;
}

std::string trim(const std::string &s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

/* The value of `key` in SYSTEM.CNF ("BOOT2 = cdrom0:\SCUS_971.02;1"). */
bool cnf_value(const std::string &cnf, const char *key, std::string *value)
{
    size_t pos = 0;
    while (pos < cnf.size()) {
        size_t end = cnf.find('\n', pos);
        if (end == std::string::npos)
            end = cnf.size();
        std::string line = cnf.substr(pos, end - pos);
        pos = end + 1;
        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        if (same_text(trim(line.substr(0, eq)), key)) {
            *value = trim(line.substr(eq + 1));
            return true;
        }
    }
    return false;
}

/* "cdrom0:\SCUS_971.02;1" -> "SCUS_971.02". */
std::string boot_file(const std::string &boot2)
{
    size_t start = boot2.find_last_of("\\/:");
    std::string name = start == std::string::npos ? boot2 : boot2.substr(start + 1);
    size_t semi = name.find(';');
    if (semi != std::string::npos)
        name.resize(semi);
    return trim(name);
}

/* The RoFS table of contents at the start of a volume (volume.h): magic
 * "RoFS", or its bitwise inverse when the TOC body is stored inverted; word 3
 * = header size, word 4 = body size. */
bool rofs_header_ok(const uint8_t *hdr, uint64_t file_size)
{
    static const uint8_t magic[4] = {'R', 'o', 'F', 'S'};
    static const uint8_t inverted[4] = {0xAD, 0x90, 0xB9, 0xAC};
    if (memcmp(hdr, magic, 4) != 0 && memcmp(hdr, inverted, 4) != 0)
        return false;
    uint32_t w3, w4;
    memcpy(&w3, hdr + 12, 4);
    memcpy(&w4, hdr + 16, 4);
    return w3 >= 0x20 && (uint64_t)w3 + w4 <= file_size;
}

bool is_folder(const std::wstring &p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool is_file(const std::wstring &p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* Deletes a folder and everything in it. */
void delete_tree(const std::wstring &folder)
{
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((util::with_slash(folder) + L"*").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
                continue;
            std::wstring p = util::with_slash(folder) + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                delete_tree(p);
            } else {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
                    SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(p.c_str());
            }
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    RemoveDirectoryW(folder.c_str());
}

bool write_file(const std::wstring &path, const uint8_t *p, size_t size, DWORD *err)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *err = GetLastError();
        return false;
    }
    while (size) {
        DWORD n = size > (64u << 20) ? (64u << 20) : (DWORD)size, put = 0;
        if (!WriteFile(h, p, n, &put, nullptr) || put == 0) {
            *err = GetLastError();
            CloseHandle(h);
            return false;
        }
        p += put;
        size -= put;
    }
    CloseHandle(h);
    return true;
}

std::wstring full_folder(const std::wstring &folder_in)
{
    wchar_t full[MAX_PATH * 4];
    DWORD fn = GetFullPathNameW(folder_in.c_str(), MAX_PATH * 4, full, nullptr);
    return util::with_slash(fn && fn < MAX_PATH * 4 ? std::wstring(full, fn) : folder_in);
}

/* How long a folder's rename waits for the other programs holding its files
 * open (move_folder). */
constexpr DWORD kMoveWaitMs = 120000;

/* Renames the folder `from` to `to`.  A folder with a file open in another
 * program cannot be renamed (access denied, or a sharing violation), and
 * right after the unpack an antivirus or the search indexer is often still
 * reading the new files (user report: the install failed at its end,
 * access denied, the files whole in data.part): tried again every 250 ms
 * for up to kMoveWaitMs, the phase saying so.  *err: the last error. */
bool move_folder(const std::wstring &from, const std::wstring &to, Listener &listener, DWORD *err)
{
    const DWORD start = GetTickCount();
    bool told = false;
    for (;;) {
        if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH))
            return true;
        *err = GetLastError();
        if ((*err != ERROR_ACCESS_DENIED && *err != ERROR_SHARING_VIOLATION) || GetTickCount() - start > kMoveWaitMs ||
            listener.cancel_requested())
            return false;
        if (!told) {
            listener.phase(L"Putting the game files in place: waiting for another program to let go of them "
                           L"(the game, an antivirus scan?)...");
            told = true;
        }
        Sleep(250);
    }
}

/* Unpacks every file of the volume `read` gives (`volume_size` bytes) into
 * folder\<dataName>.part, in the unified structure (`language`: the disc's,
 * for its courses' region folder), then puts it in place as
 * folder\<dataName>; the marker starts with `head` (marker_head). */
Result unpack(const volume::Reader &read, uint64_t volume_size, const std::wstring &folder_in,
              const std::wstring &dataName, const std::wstring &source, const std::string &head,
              const std::string &language, Listener &listener, std::wstring *message)
{
    std::wstring err;
    const std::wstring folder = full_folder(folder_in);
    if (!util::make_folders(folder)) {
        *message = L"The folder " + folder + L" could not be created: " + util::system_error_text(GetLastError()) + L".";
        return Result::failed;
    }
    const std::wstring data = folder + dataName, part = data + kPartSuffix, old = data + kOldSuffix;

    listener.phase(L"Reading the list of game files...");
    std::vector<volume::File> files;
    if (!volume::list_files(read, volume_size, &files, &err)) {
        *message = L"The game files could not be listed (" + err + L"). Make a new image of the disc.";
        return Result::failed;
    }
    /* the unified structure: courses in courses/ itself and none in a
     * folder of it (the Japanese disc) go into the region's folder */
    bool flatFiles = false, courseFolders = false;
    for (const volume::File &f : files)
        if (f.path.size() > 8 && _strnicmp(f.path.c_str(), "courses/", 8) == 0) {
            if (f.path.find('/', 8) != std::string::npos)
                courseFolders = true;
            else if (_stricmp(f.path.c_str() + 8, ".list") != 0)
                flatFiles = true;
        }
    const bool flatCourses = flatFiles && !courseFolders;
    const std::string region = course_region(language);
    /* in disc order: one pass over the image */
    std::sort(files.begin(), files.end(), [](const volume::File &a, const volume::File &b) { return a.lsn < b.lsn; });
    uint64_t total = 0;
    for (const volume::File &f : files)
        total += f.size;

    /* Room for the files (an installed data folder stays until the swap). */
    ULARGE_INTEGER avail = {};
    if (GetDiskFreeSpaceExW(folder.c_str(), &avail, nullptr, nullptr)) {
        uint64_t need = total + (64u << 20);
        if (avail.QuadPart < need) {
            *message = L"There is not enough free disk space in " + folder + L": the game files need " +
                       util::format_size(need) + L", and " + util::format_size(avail.QuadPart) + L" is free.";
            return Result::failed;
        }
    }

    /* Left over by an install that was killed. */
    if (is_folder(part))
        delete_tree(part);
    if (is_folder(old))
        delete_tree(old);
    if (!util::make_folders(part)) {
        *message = L"The folder " + part + L" could not be created: " + util::system_error_text(GetLastError()) + L".";
        return Result::failed;
    }

    listener.phase(L"Unpacking the game files...");
    std::vector<uint8_t> stored, unpacked;
    uint64_t done = 0;
    std::wstring lastFolder;
    listener.progress(0, total);
    for (const volume::File &f : files) {
        if (listener.cancel_requested()) {
            delete_tree(part);
            *message = L"The installation was cancelled.";
            return Result::cancelled;
        }
        std::wstring rel = util::widen(unified_path(f.path, flatCourses, region));
        std::replace(rel.begin(), rel.end(), L'/', L'\\');
        const std::wstring out = util::with_slash(part) + rel;
        const std::wstring outFolder = out.substr(0, out.find_last_of(L'\\'));
        if (outFolder != lastFolder) {
            if (!util::make_folders(outFolder)) {
                *message = L"The folder " + outFolder + L" could not be created: " +
                           util::system_error_text(GetLastError()) + L".";
                delete_tree(part);
                return Result::failed;
            }
            lastFolder = outFolder;
        }
        stored.resize(f.stored);
        if (f.stored && !read((uint64_t)f.lsn * volume::kSectorSize, stored.data(), f.stored)) {
            *message = L"The disc image could not be read (" + util::widen(f.path) + L").";
            delete_tree(part);
            return Result::failed;
        }
        const uint8_t *p = stored.data();
        size_t n = stored.size();
        if (f.packed) {
            if (!inflate::gunzip(stored.data(), stored.size(), &unpacked, &err) || unpacked.size() != f.size) {
                *message = L"The game file " + util::widen(f.path) + L" could not be unpacked (" +
                           (err.empty() ? std::wstring(L"wrong size") : err) + L"). Make a new image of the disc.";
                delete_tree(part);
                return Result::failed;
            }
            p = unpacked.data();
            n = unpacked.size();
        }
        DWORD werr = 0;
        if (!write_file(out, p, n, &werr)) {
            *message = L"Writing " + out + L" failed: " + util::system_error_text(werr) + L".";
            delete_tree(part);
            return Result::failed;
        }
        done += f.size;
        listener.progress(done, total);
    }

    /* The marker, last: a data folder without it is incomplete. */
    std::string marker = head;
    marker += "files " + std::to_string(files.size()) + "\r\nbytes " + std::to_string(total) + "\r\nsource " +
              util::narrow(source) + "\r\n";
    DWORD werr = 0;
    if (!write_file(util::with_slash(part) + kMarkerFile, (const uint8_t *)marker.data(), marker.size(), &werr)) {
        *message = L"Writing the install marker failed: " + util::system_error_text(werr) + L".";
        delete_tree(part);
        return Result::failed;
    }

    /* Into place. */
    listener.phase(L"Putting the game files in place...");
    DWORD e = 0;
    if (is_folder(data) && !move_folder(data, old, listener, &e)) {
        *message = L"The installed game files in " + data + L" could not be replaced: " + util::system_error_text(e) + L".";
        if (e == ERROR_SHARING_VIOLATION || e == ERROR_ACCESS_DENIED)
            *message += L"\n\nIf the game is running, close it and install again.";
        delete_tree(part);
        return Result::failed;
    }
    if (!move_folder(part, data, listener, &e)) {
        if (is_folder(old))
            MoveFileExW(old.c_str(), data.c_str(), MOVEFILE_WRITE_THROUGH);
        *message = L"The game files could not be put in place as " + data + L": " + util::system_error_text(e) + L".";
        if (e == ERROR_SHARING_VIOLATION || e == ERROR_ACCESS_DENIED)
            *message += L"\n\nAnother program kept a file of " + part + L" open (an antivirus scan of the new files, "
                        L"the search indexer, an Explorer window in it). Install again.";
        return Result::failed;
    }
    if (is_folder(old))
        delete_tree(old);
    if (install_state(folder, dataName) != InstallState::ok) {
        *message = L"The installed game files in " + data + L" could not be checked. Install again.";
        return Result::failed;
    }
    *message = L"Installed " + std::to_wstring(files.size()) + L" game files (" + util::format_size(total) + L") in " +
               data + L".";
    if (flatCourses)
        *message += L" Its courses are in courses\\" + util::widen(region) + L" (the unified structure).";
    return Result::ok;
}

} // namespace

const wchar_t *version_name(Version v)
{
    switch (v) {
    case Version::us:
        return L"Gran Turismo 3 A-Spec (USA)";
    case Version::eu:
        return L"Gran Turismo 3 A-Spec (Europe)";
    case Version::jp:
        return L"Gran Turismo 3 A-Spec (Japan)";
    default:
        return L"data only";
    }
}

std::wstring version_folder(Version v)
{
    const char *c = version_code(v);
    return c[0] != 0 ? std::wstring(kDataFolder) + L"_" + util::widen(c) : std::wstring();
}

const char *version_code(Version v)
{
    switch (v) {
    case Version::us:
        return "US";
    case Version::eu:
        return "EU";
    case Version::jp:
        return "JP";
    default:
        return "";
    }
}

InstallState install_state(const std::wstring &folder, const std::wstring &data_name)
{
    const std::wstring data = util::with_slash(folder) + data_name;
    if (data_name.empty() || !is_folder(data))
        return InstallState::missing;
    return is_file(util::with_slash(data) + kMarkerFile) ? InstallState::ok : InstallState::damaged;
}

bool has_volume(const std::wstring &folder)
{
    HANDLE h = CreateFileW((util::with_slash(folder) + kVolumeFile).c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size = {};
    uint8_t hdr[0x40] = {};
    DWORD got = 0;
    bool ok = GetFileSizeEx(h, &size) && ReadFile(h, hdr, sizeof(hdr), &got, nullptr) && got == sizeof(hdr) &&
              rofs_header_ok(hdr, (uint64_t)size.QuadPart);
    CloseHandle(h);
    return ok;
}

namespace {

/* The marker's lines (folder\<data>\kMarkerFile), empty without one. */
std::vector<std::string> marker_lines(const std::wstring &folder, const std::wstring &data)
{
    std::vector<std::string> lines;
    HANDLE h = CreateFileW((util::with_slash(folder) + data + L"\\" + kMarkerFile).c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return lines;
    char buf[8192];
    DWORD got = 0;
    std::string text;
    if (ReadFile(h, buf, sizeof(buf), &got, nullptr))
        text.assign(buf, got);
    CloseHandle(h);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos)
            end = text.size();
        lines.push_back(trim(text.substr(pos, end - pos)));
        pos = end + 1;
    }
    return lines;
}

/* The value of the marker's line "<key> <value>", "" if none. */
std::string marker_value(const std::vector<std::string> &lines, const char *key)
{
    const size_t n = strlen(key);
    for (const std::string &l : lines)
        if (l.size() > n && l.compare(0, n, key) == 0 && l[n] == ' ')
            return trim(l.substr(n + 1));
    return std::string();
}

/* A RoFS volume, whole in the image. */
bool volume_ok(const iso9660::Image &image, const iso9660::Entry &v)
{
    uint8_t hdr[0x40];
    std::wstring err;
    return !v.is_directory && v.size >= sizeof(hdr) && image.read(v, 0, hdr, sizeof(hdr), &err) &&
           rofs_header_ok(hdr, v.size);
}

/* The file `path` into `out`; false if missing or larger than `max`. */
bool read_small_file(const std::wstring &path, size_t max, std::vector<uint8_t> *out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size = {};
    bool ok = GetFileSizeEx(h, &size) && (uint64_t)size.QuadPart <= max;
    if (ok) {
        out->resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = out->empty() || (ReadFile(h, out->data(), (DWORD)out->size(), &got, nullptr) && got == out->size());
    }
    CloseHandle(h);
    return ok;
}

} // namespace

std::wstring installed_release(const std::wstring &folder, const std::wstring &data)
{
    if (install_state(folder, data) == InstallState::missing)
        return std::wstring();
    const std::vector<std::string> lines = marker_lines(folder, data);
    std::string r = marker_value(lines, "release");
    if (r.empty() && !lines.empty()) {
        /* an earlier launcher's: "<release>: the game files, ..." */
        const size_t colon = lines[0].find(": the game files");
        r = colon != std::string::npos ? lines[0].substr(0, colon) : util::narrow(kEarlierRelease);
    }
    return util::widen(r);
}

std::string installed_language(const std::wstring &folder, const std::wstring &data)
{
    const std::string l = marker_value(marker_lines(folder, data), "language");
    return l.empty() ? std::string("US") : l;
}

bool language_installed(const std::wstring &folder, const std::wstring &data, const char *code)
{
    return is_file(util::with_slash(folder) + data + L"\\menu\\" + util::widen(code) + L"\\gt_mode301.pmb");
}

std::vector<Extraction> extractions(const std::wstring &folder)
{
    std::vector<Extraction> out;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((util::with_slash(folder) + kDataFolder + L"_*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE)
        return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        const std::wstring name = fd.cFileName;
        bool version = false;
        for (int v = 0; v < kVersions; v++)
            version = version || _wcsicmp(name.c_str(), version_folder((Version)v).c_str()) == 0;
        if (version || install_state(folder, name) != InstallState::ok)
            continue;                       /* (the versions, a .part / .old, an unfinished one) */
        out.push_back({name, installed_release(folder, name)});
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    std::sort(out.begin(), out.end(), [](const Extraction &a, const Extraction &b) { return a.data < b.data; });
    return out;
}

std::wstring migrate_earlier_install(const std::wstring &folder)
{
    if (install_state(folder, kDataFolder) != InstallState::ok)
        return std::wstring();
    const std::string boot = marker_value(marker_lines(folder, kDataFolder), "boot");
    /* an earlier launcher's marker without a boot file: the US disc */
    const Version v = boot.empty() ? Version::us : version_of(boot);
    const std::wstring target = data_folder_of(v, boot, std::string(), std::string());
    const std::wstring from = util::with_slash(folder) + kDataFolder, to = util::with_slash(folder) + target;
    if (is_folder(to) || !MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH))
        return std::wstring();
    return L"The game files in data were moved to " + target + L" (a data folder per version).";
}

bool check_disc(const iso9660::Image &image, DiscInfo *info, std::wstring *message, bool *wrong_disc)
{
    *wrong_disc = false;
    std::wstring err;

    /* CORE.GT3, the engine's main program: at the root, else in a folder of
     * the root (a demo disc's game) */
    info->folder.clear();
    bool found = image.find("CORE.GT3", &info->core, &err) && !info->core.is_directory;
    if (!found) {
        std::vector<iso9660::Entry> root;
        if (!image.list(image.root(), &root, &err)) {
            *message = L"The disc image could not be read: " + err + L".";
            return false;
        }
        for (const iso9660::Entry &d : root)
            if (d.is_directory && image.find(d.name + "/CORE.GT3", &info->core, &err) && !info->core.is_directory) {
                info->folder = d.name;
                found = true;
                break;
            }
    }
    if (!found) {
        *wrong_disc = true;
        *message = L"This disc image is not a Gran Turismo 3 engine disc: it has no CORE.GT3 (the engine's main "
                   L"program).";
        return false;
    }

    /* SYSTEM.CNF's boot file names a known release (on a demo disc, the
     * disc's menu: the game is named by its folder) */
    info->boot.clear();
    iso9660::Entry cnf_entry;
    std::string cnf, boot2;
    if (image.find("SYSTEM.CNF", &cnf_entry, &err) && image.read_all(cnf_entry, &cnf, &err) &&
        cnf_value(cnf, "BOOT2", &boot2))
        info->boot = boot_file(boot2);
    info->release = release_name(info->boot, info->folder, image.volume_id());
    /* the main versions are played, the rest extracted as data only */
    info->version = info->folder.empty() ? version_of(info->boot) : Version::data_only;
    info->data = data_folder_of(info->version, info->folder.empty() ? info->boot : std::string(), info->folder,
                                image.volume_id());

    /* the volume beside it: GT3.VOL, else another RoFS volume (*.VOL); whole
     * in the image, and starting with its table of contents */
    const std::string dir = info->folder.empty() ? std::string() : info->folder + "/";
    bool vol = image.find(dir + "GT3.VOL", &info->volume, &err) && !info->volume.is_directory;
    if (!vol) {
        iso9660::Entry d;
        std::vector<iso9660::Entry> entries;
        if ((info->folder.empty() ? (d = image.root(), true) : image.find(info->folder, &d, &err)) &&
            image.list(d, &entries, &err))
            for (const iso9660::Entry &e : entries)
                if (e.name.size() > 4 && same_text(e.name.substr(e.name.size() - 4), ".VOL") && volume_ok(image, e)) {
                    info->volume = e;
                    vol = true;
                    break;
                }
    }
    if (!vol) {
        *message = L"The disc image has CORE.GT3 but no game volume (GT3.VOL) beside it: the image is damaged. "
                   L"Make a new image of the disc.";
        return false;
    }
    const std::wstring name = util::widen(info->volume.name);
    if (!image.inside_image(info->volume)) {
        *message = L"The disc image is incomplete: it ends before " + name + L" does. Make a new image of the disc.";
        return false;
    }
    if (!volume_ok(image, info->volume)) {
        *message = name + L" on this disc image is damaged (it has no RoFS table of contents). "
                          L"Make a new image of the disc.";
        return false;
    }
    return true;
}

Result install_from_iso(const std::wstring &iso_path, const std::wstring &folder, Listener &listener,
                        std::wstring *message)
{
    listener.phase(L"Checking the disc image...");
    iso9660::Image image;
    std::wstring err;
    if (!image.open(iso_path, &err)) {
        *message = L"The disc image could not be used: " + err + L".";
        return Result::failed;
    }
    DiscInfo disc;
    bool wrong_disc = false;
    if (!check_disc(image, &disc, message, &wrong_disc))
        return wrong_disc ? Result::wrong_disc : Result::failed;
    /* what its code reads (the databases) and its language */
    std::string core;
    std::vector<uint8_t> coreBytes;
    if (image.read_all(disc.core, &core, &err))
        coreBytes.assign(core.begin(), core.end());
    const CoreFacts facts = core_facts(coreBytes, disc.folder.empty() ? disc.boot : std::string());
    listener.phase((disc.version == Version::data_only ? L"Extracting " : L"Installing ") + disc.release + L"...");
    const iso9660::Entry &vol = disc.volume;
    volume::Reader read = [&image, &vol](uint64_t offset, void *buf, uint32_t size) {
        std::wstring e;
        return image.read(vol, offset, buf, size, &e);
    };
    return unpack(read, vol.size, folder, disc.data, iso_path, marker_head(disc.release, disc.boot, facts),
                  facts.language, listener, message);
}

Result install_from_volume(const std::wstring &volume_path, const std::wstring &folder, Listener &listener,
                           std::wstring *message)
{
    listener.phase(L"Checking gt3.vol...");
    HANDLE h = CreateFileW(volume_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *message = L"The file " + volume_path + L" could not be opened: " + util::system_error_text(GetLastError()) + L".";
        return Result::failed;
    }
    LARGE_INTEGER size = {};
    GetFileSizeEx(h, &size);
    /* the release: a CORE.GT3 and SYSTEM.CNF beside the volume (the
     * development tree's extracted disc), else what an earlier launcher
     * installed (the US disc) */
    const std::wstring dir = volume_path.substr(0, volume_path.find_last_of(L"\\/") + 1);
    std::vector<uint8_t> core, cnf;
    std::string boot, boot2;
    if (read_small_file(dir + L"SYSTEM.CNF", 4096, &cnf) &&
        cnf_value(std::string(cnf.begin(), cnf.end()), "BOOT2", &boot2))
        boot = boot_file(boot2);
    std::wstring release = kEarlierRelease;
    CoreFacts facts;
    facts.language = "US";
    Version version = Version::us;
    if (read_small_file(dir + L"CORE.GT3", 64u << 20, &core)) {
        facts = core_facts(core, boot);
        release = release_name(boot, std::string(), std::string());
        version = version_of(boot);
    }
    const std::wstring data = data_folder_of(version, boot, std::string(), std::string());
    volume::Reader read = [h](uint64_t offset, void *buf, uint32_t n) {
        uint8_t *p = static_cast<uint8_t *>(buf);
        while (n) {
            OVERLAPPED ov = {};
            ov.Offset = (DWORD)offset;
            ov.OffsetHigh = (DWORD)(offset >> 32);
            DWORD got = 0;
            if (!ReadFile(h, p, n, &got, &ov) || got == 0)
                return false;
            p += got;
            offset += got;
            n -= got;
        }
        return true;
    };
    Result r = unpack(read, (uint64_t)size.QuadPart, folder, data, volume_path, marker_head(release, boot, facts),
                      facts.language, listener, message);
    CloseHandle(h);
    return r;
}

} // namespace install
