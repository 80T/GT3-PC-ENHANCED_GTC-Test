/* memcard.cpp - the PS2 memory cards of GT3 PC (memcard.h). */
#include "platform/memcard.h"
#include "platform/log.h"
#include "platform/paths.h"
#include "sifdev.h"

#include <windows.h>

#include <mutex>
#include <string>
#include <vector>

namespace memcard {
namespace {

constexpr int kTotalClusters = 8000;        /* an 8 MB card as libmc reports it, 1 KB clusters */
constexpr int kMaxFds = 16;
constexpr unsigned short kAttrDir = 0x8427;  /* exists | subdir | rwx (as cards show them) */
constexpr unsigned short kAttrFile = 0x8497; /* exists | closed | file | rwx */
constexpr int64_t kJstOffset = 9 * 3600;

std::mutex g_mutex;

struct Fd {
    bool   used;
    bool   writer;                  /* opened for writing (busy) */
    HANDLE h;
    int    port;
};
Fd g_fds[kMaxFds];
/* GetTickCount64 of the last change to a card (busy); 0 none yet */
ULONGLONG g_lastChange;
constexpr ULONGLONG kBusyMs = 2000;

void changed()
{
    g_lastChange = GetTickCount64();
}

struct Listing {
    std::vector<sceMcTblGetDir> entries;
    size_t next = 0;
};
Listing g_listing[2];
/* The card has been reported since start-up (libmc answers sceMcGetInfo
 * with -1, "a new formatted card", once per card it finds; then 0, "the
 * same card"). */
bool g_reported[2];

std::string card_root(int port)
{
    return paths::game_dir() + (port == 0 ? "memcard1\\" : "memcard2\\");
}

bool valid_card(int port, int slot)
{
    return (port == 0 || port == 1) && slot == 0;
}

std::string card_path(int port, const char *name)
{
    std::string p = name ? name : "";
    while (!p.empty() && (p[0] == '/' || p[0] == '\\'))
        p.erase(0, 1);
    for (char &c : p)
        if (c == '/')
            c = '\\';
    return card_root(port) + p;
}

bool is_dir(const std::string &p)
{
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool exists(const std::string &p)
{
    return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::string parent_of(const std::string &p)
{
    size_t s = p.find_last_of('\\');
    return s == std::string::npos ? std::string() : p.substr(0, s);
}

/* A FILETIME as the card's date (Japan time, as the PS2's clock). */
void card_date(const FILETIME &ft, sceMcStDateTime *d)
{
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    u.QuadPart += (ULONGLONG)kJstOffset * 10000000ULL;
    FILETIME j;
    j.dwLowDateTime = u.LowPart;
    j.dwHighDateTime = u.HighPart;
    SYSTEMTIME st;
    FileTimeToSystemTime(&j, &st);
    d->Resv2 = 0;
    d->Sec = (unsigned char)st.wSecond;
    d->Min = (unsigned char)st.wMinute;
    d->Hour = (unsigned char)st.wHour;
    d->Day = (unsigned char)st.wDay;
    d->Month = (unsigned char)st.wMonth;
    d->Year = st.wYear;
}

bool card_filetime(const sceMcStDateTime *d, FILETIME *out)
{
    SYSTEMTIME st = {};
    st.wYear = d->Year;
    st.wMonth = d->Month;
    st.wDay = d->Day;
    st.wHour = d->Hour;
    st.wMinute = d->Min;
    st.wSecond = d->Sec;
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft))
        return false;
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    u.QuadPart -= (ULONGLONG)kJstOffset * 10000000ULL;
    out->dwLowDateTime = u.LowPart;
    out->dwHighDateTime = u.HighPart;
    return true;
}

bool match(const char *pat, const char *s)
{
    for (; *pat; pat++, s++) {
        if (*pat == '*') {
            for (;; s++) {
                if (match(pat + 1, s))
                    return true;
                if (!*s)
                    return false;
            }
        }
        if (!*s)
            return false;
        if (*pat != '?' && toupper((unsigned char)*pat) != toupper((unsigned char)*s))
            return false;
    }
    return *s == 0;
}

int count_entries(const std::string &dir)
{
    int n = 2;                                  /* "." and ".." */
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE)
        return n;
    do {
        if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, ".."))
            n++;
    } while (FindNextFileA(f, &fd));
    FindClose(f);
    return n;
}

/* Clusters used below dir (files in 1 KB clusters, directories two entries
 * per cluster). */
int used_clusters(const std::string &dir)
{
    int used = (count_entries(dir) + 1) / 2;
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE)
        return used;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            used += used_clusters(dir + "\\" + fd.cFileName);
        else
            used += (int)((((ULONGLONG)fd.nFileSizeHigh << 32 | fd.nFileSizeLow) + 1023) / 1024);
    } while (FindNextFileA(f, &fd));
    FindClose(f);
    return used;
}

int free_clusters(int port)
{
    std::string root = card_root(port);
    root.pop_back();
    int f = kTotalClusters - used_clusters(root);
    return f < 0 ? 0 : f;
}

void fill_entry(sceMcTblGetDir *e, const char *name, const WIN32_FIND_DATAA *fd, int size, bool dir)
{
    memset(e, 0, sizeof(*e));
    if (fd) {
        card_date(fd->ftCreationTime, &e->_Create);
        card_date(fd->ftLastWriteTime, &e->_Modify);
    } else {
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        card_date(now, &e->_Create);
        e->_Modify = e->_Create;
    }
    e->FileSizeByte = (unsigned int)size;
    e->AttrFile = dir ? kAttrDir : kAttrFile;
    strncpy((char *)e->EntryName, name, sizeof(e->EntryName) - 1);
}

void delete_tree(const std::string &dir, bool removeSelf)
{
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
                continue;
            std::string p = dir + "\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                delete_tree(p, true);
            else
                DeleteFileA(p.c_str());
        } while (FindNextFileA(f, &fd));
        FindClose(f);
    }
    if (removeSelf)
        RemoveDirectoryA(dir.c_str());
}

} // namespace

void init()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (int port = 0; port < 2; port++) {
        std::string root = card_root(port);
        CreateDirectoryA(root.c_str(), nullptr);
    }
    pclog::write("memcard: %smemcard1, %smemcard2", paths::game_dir().c_str(), paths::game_dir().c_str());
}

int get_slot_max(int port)
{
    return (port == 0 || port == 1) ? 1 : 0;
}

int get_info(int port, int slot, int *type, int *freeClusters, int *format)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!valid_card(port, slot)) {
        if (type) *type = sceMcTypeNoCard;
        return -10;
    }
    std::string root = card_root(port);
    bool formatted = is_dir(root.substr(0, root.size() - 1));
    if (type) *type = sceMcTypePS2;
    if (freeClusters) *freeClusters = formatted ? free_clusters(port) : 0;
    if (format) *format = formatted ? 1 : 0;
    if (!formatted)
        return sceMcResNoFormat;
    /* The first answer is "a new card": the game's card manager (T501
     * mc_slot_poll_info) reads the free space and the format only then; a
     * plain 0 at start left its slot unformatted until it formatted the
     * card itself (which empties the folder). */
    if (!g_reported[port]) {
        g_reported[port] = true;
        return sceMcResChangedCard;
    }
    return sceMcResSucceed;
}

int open(int port, int slot, const char *name, int mode)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!valid_card(port, slot))
        return -10;
    std::string p = card_path(port, name);
    if (mode & sceMcFileCreateDir) {
        if (exists(p))
            return sceMcResNoEntry;
        changed();
        return CreateDirectoryA(p.c_str(), nullptr) ? sceMcResSucceed : sceMcResNoEntry;
    }
    int fd = -1;
    for (int i = 0; i < kMaxFds; i++)
        if (!g_fds[i].used) {
            fd = i;
            break;
        }
    if (fd < 0)
        return sceMcResUpLimitHandle;
    if (is_dir(p))
        return sceMcResNoEntry;
    DWORD access = 0;
    int rw = mode & 3;
    if (rw == SCE_RDONLY || rw == SCE_RDWR) access |= GENERIC_READ;
    if (rw == SCE_WRONLY || rw == SCE_RDWR) access |= GENERIC_WRITE;
    DWORD disp = OPEN_EXISTING;
    if (mode & SCE_CREAT)
        disp = (mode & SCE_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS;
    else if (mode & SCE_TRUNC)
        disp = TRUNCATE_EXISTING;
    if ((mode & SCE_CREAT) && !is_dir(parent_of(p)))
        return sceMcResNoEntry;
    HANDLE h = CreateFileA(p.c_str(), access, FILE_SHARE_READ, nullptr, disp, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return sceMcResNoEntry;
    g_fds[fd].used = true;
    g_fds[fd].writer = (access & GENERIC_WRITE) != 0;
    if (g_fds[fd].writer)
        changed();
    g_fds[fd].h = h;
    g_fds[fd].port = port;
    return fd;
}

int close(int fd)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (fd < 0 || fd >= kMaxFds || !g_fds[fd].used)
        return sceMcResDeniedPermit;
    CloseHandle(g_fds[fd].h);
    if (g_fds[fd].writer)
        changed();
    g_fds[fd].used = false;
    return sceMcResSucceed;
}

int seek(int fd, int offset, int origin)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (fd < 0 || fd >= kMaxFds || !g_fds[fd].used)
        return sceMcResDeniedPermit;
    DWORD how = origin == SCE_SEEK_END ? FILE_END : origin == SCE_SEEK_CUR ? FILE_CURRENT : FILE_BEGIN;
    DWORD pos = SetFilePointer(g_fds[fd].h, offset, nullptr, how);
    return pos == INVALID_SET_FILE_POINTER ? sceMcResDeniedPermit : (int)pos;
}

int read(int fd, void *buf, int size)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (fd < 0 || fd >= kMaxFds || !g_fds[fd].used)
        return sceMcResDeniedPermit;
    DWORD got = 0;
    if (!ReadFile(g_fds[fd].h, buf, size, &got, nullptr))
        return sceMcResDeniedPermit;
    return (int)got;
}

int write(int fd, const void *buf, int size)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (fd < 0 || fd >= kMaxFds || !g_fds[fd].used)
        return sceMcResDeniedPermit;
    DWORD pos = SetFilePointer(g_fds[fd].h, 0, nullptr, FILE_CURRENT);
    DWORD len = GetFileSize(g_fds[fd].h, nullptr);
    DWORD end = pos + size;
    if (end > len) {
        int grow = (int)((end + 1023) / 1024) - (int)((len + 1023) / 1024);
        int freeNow = free_clusters(g_fds[fd].port);
        if (grow > freeNow) {
            /* nothing written: the game reports the card full (8000 clusters
             * is the PS2 card's size, the free space it shows) */
            pclog::write("memcard: card %d full: a write of %d bytes needs %d more 1 KB clusters, %d free",
                         g_fds[fd].port + 1, size, grow, freeNow);
            return sceMcResFullDevice;
        }
    }
    DWORD put = 0;
    changed();
    if (!WriteFile(g_fds[fd].h, buf, size, &put, nullptr)) {
        pclog::write("memcard: card %d: write of %d bytes failed (error %lu)", g_fds[fd].port + 1, size,
                     (unsigned long)GetLastError());
        return sceMcResDeniedPermit;
    }
    if ((int)put != size)
        pclog::write("memcard: card %d: short write, %d of %d bytes", g_fds[fd].port + 1, (int)put, size);
    return (int)put;
}

int mkdir(int port, int slot, const char *name)
{
    return open(port, slot, name, sceMcFileCreateDir);
}

int remove(int port, int slot, const char *name)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!valid_card(port, slot))
        return -10;
    std::string p = card_path(port, name);
    if (is_dir(p)) {
        if (count_entries(p) > 2)
            return sceMcResNotEmpty;
        return RemoveDirectoryA(p.c_str()) ? sceMcResSucceed : sceMcResDeniedPermit;
    }
    if (!exists(p))
        return sceMcResNoEntry;
    changed();
    return DeleteFileA(p.c_str()) ? sceMcResSucceed : sceMcResDeniedPermit;
}

int rename(int port, int slot, const char *org, const char *newName)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!valid_card(port, slot))
        return -10;
    std::string from = card_path(port, org);
    if (!exists(from))
        return sceMcResNoEntry;
    std::string to = parent_of(from) + "\\" + newName;
    if (exists(to))
        return sceMcResNoEntry;
    changed();
    return MoveFileA(from.c_str(), to.c_str()) ? sceMcResSucceed : sceMcResDeniedPermit;
}

/* pattern: "dir/name*" lists the matching entries of dir; "dir/ *" a whole
 * directory, "." and ".." first.  mode 0 starts a listing, 1 goes on with
 * it.  Returns the entries written; sceMcResNoEntry when dir is not there. */
int get_dir(int port, int slot, const char *pattern, unsigned int mode, int maxent, sceMcTblGetDir *table)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!valid_card(port, slot))
        return -10;
    Listing &l = g_listing[port];
    if (mode == 0) {
        l.entries.clear();
        l.next = 0;
        std::string pat = pattern ? pattern : "*";
        while (!pat.empty() && (pat[0] == '/' || pat[0] == '\\'))
            pat.erase(0, 1);
        size_t s = pat.find_last_of("/\\");
        std::string dirPart = s == std::string::npos ? "" : pat.substr(0, s);
        std::string namePat = s == std::string::npos ? pat : pat.substr(s + 1);
        std::string dir = card_path(port, dirPart.c_str());
        if (!dir.empty() && dir.back() == '\\')
            dir.pop_back();
        /* A directory that is not there is an error, as libmc's: no entry.
         * (0 entries read as "the directory is there, the file not": the
         * game took a card without its game data for damaged data and would
         * not save a first game - MemorycardSlotMenu::makeSaveConfirm wants
         * MC_STATUS_NO_ENTRY from getMainFileKB's listing; user report.) */
        if (!is_dir(dir)) {
            std::string root = card_root(port);
            root.pop_back();
            return is_dir(root) ? sceMcResNoEntry : sceMcResNoFormat;
        }
        if (!dirPart.empty() && namePat == "*") {
            sceMcTblGetDir e;
            fill_entry(&e, ".", nullptr, 0, true);
            l.entries.push_back(e);
            fill_entry(&e, "..", nullptr, 0, true);
            l.entries.push_back(e);
        }
        WIN32_FIND_DATAA fd;
        HANDLE f = FindFirstFileA((dir + "\\*").c_str(), &fd);
        if (f != INVALID_HANDLE_VALUE) {
            do {
                if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
                    continue;
                if (!match(namePat.c_str(), fd.cFileName))
                    continue;
                bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                int size = isDir ? count_entries(dir + "\\" + fd.cFileName) : (int)fd.nFileSizeLow;
                sceMcTblGetDir e;
                fill_entry(&e, fd.cFileName, &fd, size, isDir);
                l.entries.push_back(e);
            } while (FindNextFileA(f, &fd));
            FindClose(f);
        }
    }
    int n = 0;
    while (n < maxent && l.next < l.entries.size())
        table[n++] = l.entries[l.next++];
    return n;
}

int set_file_info(int port, int slot, const char *name, const char *info, unsigned int valid)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!valid_card(port, slot))
        return -10;
    std::string p = card_path(port, name);
    if (!exists(p))
        return sceMcResNoEntry;
    changed();
    const sceMcTblGetDir *e = (const sceMcTblGetDir *)info;
    if (valid & (sceMcFileInfoCreate | sceMcFileInfoModify)) {
        HANDLE h = CreateFileA(p.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            FILETIME c, m;
            bool hc = (valid & sceMcFileInfoCreate) && card_filetime(&e->_Create, &c);
            bool hm = (valid & sceMcFileInfoModify) && card_filetime(&e->_Modify, &m);
            SetFileTime(h, hc ? &c : nullptr, nullptr, hm ? &m : nullptr);
            CloseHandle(h);
        }
    }
    if ((valid & 0x10) && e->EntryName[0]) {             /* the name */
        std::string to = parent_of(p) + "\\" + (const char *)e->EntryName;
        if (to != p && !MoveFileA(p.c_str(), to.c_str()))
            return sceMcResNoEntry;
    }
    return sceMcResSucceed;
}

int get_ent_space(int port, int slot, const char *path)
{
    (void)path;
    if (!valid_card(port, slot))
        return -10;
    return 0x20;
}

int format(int port, int slot)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!valid_card(port, slot))
        return -10;
    std::string root = card_root(port);
    root.pop_back();
    changed();
    CreateDirectoryA(root.c_str(), nullptr);
    delete_tree(root, false);
    pclog::write("memcard: card %d formatted", port + 1);
    return sceMcResSucceed;
}

int unformat(int port, int slot)
{
    return format(port, slot);
}

bool busy()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const Fd &f : g_fds)
        if (f.used && f.writer)
            return true;
    return g_lastChange != 0 && GetTickCount64() - g_lastChange < kBusyMs;
}

} // namespace memcard
