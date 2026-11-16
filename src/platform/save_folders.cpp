/* save_folders.cpp - where an engine's memory cards are (save_folders.h).
 * PC (2026-11-15). */
#include "platform/save_folders.h"

#include <windows.h>

#include <algorithm>

namespace save_folders {
namespace {

std::wstring with_slash(std::wstring s)
{
    if (!s.empty() && s.back() != L'\\' && s.back() != L'/')
        s += L'\\';
    return s;
}

std::wstring no_slash(std::wstring s)
{
    while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/'))
        s.pop_back();
    return s;
}

std::wstring from_utf8(const std::string &s)
{
    if (s.empty())
        return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)std::max(n, 0), L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

/* A file name cannot have it. */
bool bad_char(wchar_t c)
{
    return c < 32 || wcschr(L"<>:\"/\\|?*", c) != nullptr;
}

/* The ANSI code page keeps it as it is. */
bool ansi_keeps(const std::wstring &s)
{
    BOOL lossy = FALSE;
    WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, s.c_str(), (int)s.size(), nullptr, 0, nullptr, &lossy);
    return !lossy;
}

/* CON, PRN, AUX, NUL, COM1..9, LPT1..9, alone or before a dot. */
bool device_name(const std::wstring &name)
{
    std::wstring base = name.substr(0, name.find(L'.'));
    while (!base.empty() && base.back() == L' ')
        base.pop_back();
    for (const wchar_t *d : {L"CON", L"PRN", L"AUX", L"NUL"})
        if (_wcsicmp(base.c_str(), d) == 0)
            return true;
    if (base.size() == 4 && base[3] >= L'1' && base[3] <= L'9')
        for (const wchar_t *d : {L"COM", L"LPT"})
            if (_wcsnicmp(base.c_str(), d, 3) == 0)
                return true;
    return false;
}

} // namespace

std::wstring from_ansi(const std::string &s)
{
    if (s.empty())
        return std::wstring();
    const int n = MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)std::max(n, 0), L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string to_ansi(const std::wstring &s)
{
    if (s.empty())
        return std::string();
    const int n = WideCharToMultiByte(CP_ACP, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string a((size_t)std::max(n, 0), '\0');
    if (n > 0)
        WideCharToMultiByte(CP_ACP, 0, s.data(), (int)s.size(), &a[0], n, nullptr, nullptr);
    return a;
}

std::wstring engine_name(const std::wstring &engineFolder, bool shared)
{
    /* (engines.cpp name_of's reading: UTF-8, a BOM skipped, the first line
     * without its trailing spaces, at most 80 characters) */
    const std::wstring folder = no_slash(engineFolder);
    HANDLE h = CreateFileW((folder + L"\\engine.txt").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        char buf[512];
        DWORD got = 0;
        const bool ok = ReadFile(h, buf, sizeof(buf), &got, nullptr) != 0;
        CloseHandle(h);
        if (ok && got != 0) {
            std::string line(buf, got);
            if (line.compare(0, 3, "\xEF\xBB\xBF") == 0)
                line.erase(0, 3);
            line = line.substr(0, line.find_first_of("\r\n"));
            while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
                line.pop_back();
            const std::wstring name = from_utf8(line);
            if (!name.empty())
                return name.substr(0, 80);
        }
    }
    if (!shared)
        return kOwnName;
    const size_t slash = folder.find_last_of(L"\\/");
    return slash == std::wstring::npos ? folder : folder.substr(slash + 1);
}

std::wstring saves_dir(const std::wstring &sharedFolder, const std::wstring &engineFolder)
{
    const bool shared = _wcsicmp(with_slash(sharedFolder).c_str(), with_slash(engineFolder).c_str()) != 0;
    std::wstring name = engine_name(engineFolder, shared), folder;
    for (wchar_t c : name) {
        const std::wstring one(1, c);
        folder += bad_char(c) || !ansi_keeps(one) ? L'_' : c;
    }
    while (!folder.empty() && (folder.back() == L'.' || folder.back() == L' '))
        folder.pop_back();
    if (folder.empty() || device_name(folder))
        folder = L"_" + folder;
    return with_slash(sharedFolder) + kFolder + L"\\" + folder + L"\\";
}

const wchar_t *name_problem(const std::wstring &name)
{
    if (name.empty())
        return L"A save folder needs a name.";
    if ((int)name.size() > kMaxName)
        return L"That name is too long (64 characters at most).";
    for (wchar_t c : name)
        if (bad_char(c))
            return L"A folder's name cannot have any of these characters:  < > : \" / \\ | ? *";
    if (name == L"." || name == L"..")
        return L"That name is not a folder's name.";
    if (name.back() == L'.' || name.back() == L' ' || name.front() == L' ')
        return L"A folder's name cannot start with a space, nor end with a space or a dot.";
    if (device_name(name))
        return L"That name is one Windows keeps for a device (CON, NUL, COM1 ...).";
    if (!ansi_keeps(name))
        return L"That name has characters gt3pc_engine.ini cannot keep: use plainer letters.";
    return nullptr;
}

std::vector<std::wstring> list(const std::wstring &savesDir)
{
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW((with_slash(savesDir) + L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                nullptr, 0);
    if (f == INVALID_HANDLE_VALUE)
        return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        out.push_back(fd.cFileName);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    std::sort(out.begin(), out.end(),
              [](const std::wstring &a, const std::wstring &b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    return out;
}

bool make_defaults(const std::wstring &savesDir, const std::wstring &engineIni)
{
    if (savesDir.empty() || !list(savesDir).empty())
        return false;
    /* the folder and its parents (saves\, saves\<engine>\) */
    const std::wstring dir = with_slash(savesDir);
    for (size_t at = dir.find(L'\\', 3); at != std::wstring::npos; at = dir.find(L'\\', at + 1))
        CreateDirectoryW(dir.substr(0, at).c_str(), nullptr);
    bool made = false;
    for (int s = 0; s < kSlots; s++) {
        const std::wstring f = dir + kDefaults[s];
        if (!CreateDirectoryW(f.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
            continue;
        /* (no folder was there: whatever the slot named is not, so it takes
         * this one - a slot naming "default2" before, now default1's slot) */
        made = true;
        set_slot(engineIni, s, kDefaults[s]);
    }
    return made;
}

std::wstring slot(const std::wstring &engineIni, int slot)
{
    wchar_t v[256] = L"";
    const std::wstring key = L"Slot" + std::to_wstring(slot + 1);
    GetPrivateProfileStringW(kSection, key.c_str(), L"", v, 256, engineIni.c_str());
    return v;
}

void set_slot(const std::wstring &engineIni, int slot, const std::wstring &name)
{
    const std::wstring key = L"Slot" + std::to_wstring(slot + 1);
    WritePrivateProfileStringW(kSection, key.c_str(), name.empty() ? nullptr : name.c_str(), engineIni.c_str());
}

} // namespace save_folders
