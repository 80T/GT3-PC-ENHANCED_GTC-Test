/* host_file.cpp - "host:" files of GT3 PC (host_file.h). */
#include "platform/host_file.h"
#include "platform/log.h"
#include "platform/paths.h"
#include "platform/settings.h"

#include <windows.h>

#include <string.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

namespace {

const int kFiles = 64;
HANDLE g_files[kFiles];
bool g_used[kFiles];
std::mutex g_mutex;
thread_local bool t_quiet;                  /* host_file_quiet */

/* The path in the data folder (its own separators) */
std::string relative_path(const char *path)
{
    std::string p = path;
    if (p.compare(0, 5, "host:") == 0)
        p = p.substr(5);
    else if (p.compare(0, 6, "host0:") == 0)
        p = p.substr(6);
    while (!p.empty() && (p[0] == '/' || p[0] == '\\'))
        p.erase(0, 1);
    for (char &c : p)
        if (c == '/')
            c = '\\';
    return p;
}

/* "data\..." names the data folder (data_US\ ..., platform/paths.h), any
 * other host path the folder holding it. */
std::string host_path(const char *path)
{
    const std::string rel = relative_path(path);
    if (_strnicmp(rel.c_str(), "data", 4) == 0 && (rel.size() == 4 || rel[4] == '\\'))
        return paths::data_folder() + (rel.size() > 5 ? rel.substr(5) : std::string());
    return paths::data_dir() + rel;
}

bool file_log()
{
    return !t_quiet && settings::debug_file_log();
}

int add(HANDLE h)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (int i = 0; i < kFiles; i++)
        if (!g_used[i]) {
            g_used[i] = true;
            g_files[i] = h;
            return i;
        }
    CloseHandle(h);
    return -1;
}

HANDLE get(int fd)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return fd >= 0 && fd < kFiles && g_used[fd] ? g_files[fd] : INVALID_HANDLE_VALUE;
}

/* ---- host_file_toc --------------------------------------------------------- */

/* A file or folder of the listed tree; off / nameOff: its record and name in
 * the TOC image. */
struct TocNode {
    std::string name;
    bool dir = false;
    uint32_t size = 0;
    std::vector<TocNode> children;
    uint32_t off = 0;
};

constexpr uint32_t kTocRoot = 0x14;         /* the root record (VOL_ROOT_DIR) */
/* Folders deeper than this are listed empty (logged), not followed: a
 * guard against a link loop, far past any path the file API can open. */
constexpr int kTocMaxDepth = 256;

bool ends_with_gz(const std::string &s)
{
    return s.size() > 3 && _stricmp(s.c_str() + s.size() - 3, ".gz") == 0;
}

/* Lists `folder' into node.  False only when the data folder itself (depth
 * 0) cannot be listed; a sub-folder that cannot be (too deep, a path past
 * what the file API takes, no access) is kept empty and logged. */
bool toc_list(const std::string &folder, TocNode *node, int depth, uint32_t *files, uint32_t *folders)
{
    if (depth > kTocMaxDepth) {
        pclog::write("data: %s is more than %d folders deep; its files are not listed", folder.c_str(),
                     kTocMaxDepth);
        return true;
    }
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileExA((folder + "\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (f == INVALID_HANDLE_VALUE) {
        if (depth == 0)
            return false;
        pclog::write("data: %s could not be listed (error %lu); its files are left out", folder.c_str(),
                     (unsigned long)GetLastError());
        return true;
    }
    bool ok = true;
    do {
        std::string name = fd.cFileName;
        if (name == "." || name == ".." || (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) ||
            ends_with_gz(name) || (depth == 0 && _stricmp(name.c_str(), paths::kInstallMarker) == 0))
            continue;
        TocNode child;
        child.name = name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            child.dir = true;
            (*folders)++;
            if (!toc_list(folder + "\\" + name, &child, depth + 1, files, folders)) {
                ok = false;
                break;
            }
        } else {
            child.size = fd.nFileSizeHigh ? 0xFFFFFFFFu : fd.nFileSizeLow;
            (*files)++;
        }
        node->children.push_back(std::move(child));
    } while (FindNextFileA(f, &fd));
    FindClose(f);
    /* the volume's order: vol_dir_find's binary search compares bytes */
    std::sort(node->children.begin(), node->children.end(),
              [](const TocNode &a, const TocNode &b) { return a.name < b.name; });
    return ok;
}

/* Records in pre-order from *next: a folder's 0xC bytes + 4 per entry (at
 * least 0x10), a file's 0x10. */
void toc_place(TocNode *n, uint32_t *next)
{
    n->off = *next;
    uint32_t bytes = n->dir ? 0xC + 4 * (uint32_t)n->children.size() : 0x10;
    *next += bytes < 0x10 ? 0x10 : bytes;
    for (TocNode &c : n->children)
        toc_place(&c, next);
}

void toc_put(std::vector<uint8_t> &img, uint32_t off, uint32_t v)
{
    memcpy(&img[off], &v, 4);
}

/* Writes n's record (type 1 for a folder, else 0) and its name into `names`,
 * which follows the records at `namesAt`. */
void toc_write(const TocNode &n, uint32_t parent, std::vector<uint8_t> &img, std::string &names, uint32_t namesAt)
{
    uint32_t nameOff = namesAt + (uint32_t)names.size();
    names += n.name;
    names += '\0';
    toc_put(img, n.off, nameOff | ((n.dir ? 1u : 0u) << 24));
    if (!n.dir) {
        toc_put(img, n.off + 4, 0);         /* no sector: file_resolve asks host: */
        toc_put(img, n.off + 8, n.size);
        toc_put(img, n.off + 0xC, 0);
        return;
    }
    toc_put(img, n.off + 4, (uint32_t)n.children.size() + 1);
    toc_put(img, n.off + 8, parent);
    for (size_t i = 0; i < n.children.size(); i++)
        toc_put(img, n.off + 0xC + 4 * (uint32_t)i, n.children[i].off);
    for (const TocNode &c : n.children)
        toc_write(c, n.off, img, names, namesAt);
}

} // namespace

extern "C" {

int host_file_full_path(const char *path, char *out, uint32_t size)
{
    const std::string p = host_path(path);
    if (p.size() + 1 > size)
        return -1;
    memcpy(out, p.c_str(), p.size() + 1);
    return 0;
}

int host_file_present(const char *path)
{
    const DWORD a = GetFileAttributesA(host_path(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
}

int host_file_open(const char *path)
{
    std::string p = host_path(path);
    HANDLE h = CreateFileA(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        /* (probes are normal: file_resolve tries host: first; quiet) */
        if (file_log())
            pclog::write("file: %s missing", relative_path(path).c_str());
        return -1;
    }
    if (file_log()) {
        LARGE_INTEGER sz;
        if (GetFileSizeEx(h, &sz))
            pclog::write("file: %s (%lld bytes)", relative_path(path).c_str(), (long long)sz.QuadPart);
        else
            pclog::write("file: %s", relative_path(path).c_str());
    }
    return add(h);
}

int host_file_create(const char *path)
{
    std::string p = host_path(path);
    HANDLE h = CreateFileA(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_log())
        pclog::write(h == INVALID_HANDLE_VALUE ? "file: %s could not be written" : "file: %s written",
                     relative_path(path).c_str());
    return h == INVALID_HANDLE_VALUE ? -1 : add(h);
}

void host_file_quiet(int on)
{
    t_quiet = on != 0;
}

void host_file_close(int fd)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (fd >= 0 && fd < kFiles && g_used[fd]) {
        CloseHandle(g_files[fd]);
        g_used[fd] = false;
    }
}

int64_t host_file_size(int fd)
{
    HANDLE h = get(fd);
    LARGE_INTEGER sz;
    if (h == INVALID_HANDLE_VALUE || !GetFileSizeEx(h, &sz))
        return -1;
    return sz.QuadPart;
}

int host_file_read_at(int fd, uint64_t offset, void *buf, uint32_t bytes)
{
    HANDLE h = get(fd);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    OVERLAPPED ov = {};
    ov.Offset = (DWORD)offset;
    ov.OffsetHigh = (DWORD)(offset >> 32);
    DWORD got = 0;
    if (!ReadFile(h, buf, bytes, &got, &ov))
        return GetLastError() == ERROR_HANDLE_EOF ? 0 : -1;
    return (int)got;
}

int host_file_write_at(int fd, uint64_t offset, const void *buf, uint32_t bytes)
{
    HANDLE h = get(fd);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    OVERLAPPED ov = {};
    ov.Offset = (DWORD)offset;
    ov.OffsetHigh = (DWORD)(offset >> 32);
    DWORD put = 0;
    if (!WriteFile(h, buf, bytes, &put, &ov))
        return -1;
    return (int)put;
}

uint8_t *host_file_toc(const char *path, void *(*alloc)(uint32_t size), uint32_t *size)
{
    *size = 0;
    std::string folder = host_path(path);
    while (!folder.empty() && folder.back() == '\\')
        folder.pop_back();
    TocNode root;
    root.dir = true;
    uint32_t files = 0, folders = 0;
    if (!toc_list(folder, &root, 0, &files, &folders)) {
        pclog::write("data: %s could not be listed", folder.c_str());
        return nullptr;
    }

    uint32_t namesAt = kTocRoot;
    toc_place(&root, &namesAt);
    std::vector<uint8_t> img(namesAt, 0);
    std::string names;
    toc_write(root, 0, img, names, namesAt);
    if (namesAt + names.size() > 0xFFFFFF) {         /* names are 24-bit offsets */
        pclog::write("data: %s holds too many files to list", folder.c_str());
        return nullptr;
    }
    img.insert(img.end(), names.begin(), names.end());
    /* The header: magic, GT3.VOL's word 1, the TOC size, the records' size
     * (the "header") and the names' (the "body"). */
    memcpy(&img[0], "RoFS", 4);
    toc_put(img, 4, 0x20002);
    toc_put(img, 8, (uint32_t)img.size());
    toc_put(img, 12, namesAt);
    toc_put(img, 16, (uint32_t)names.size());

    uint8_t *out = (uint8_t *)alloc((uint32_t)img.size());
    if (out == nullptr)
        return nullptr;
    memcpy(out, img.data(), img.size());
    *size = (uint32_t)img.size();
    pclog::write("data: %u files in %u folders listed (a %u-byte table)", files, folders, *size);
    return out;
}

} // extern "C"
