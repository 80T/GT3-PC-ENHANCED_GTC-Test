/* util.cpp - small helpers shared by the launcher's files (util.h). */
#include "util.h"

#include <windows.h>
#include <shobjidl.h>

#include <stdio.h>

namespace util {

std::wstring system_error_text(unsigned long code)
{
    wchar_t *text = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, code, 0, reinterpret_cast<wchar_t *>(&text), 0, nullptr);
    std::wstring s;
    if (n && text)
        s.assign(text, n);
    if (text)
        LocalFree(text);
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' ' || s.back() == L'.'))
        s.pop_back();
    if (s.empty()) {
        wchar_t buf[32];
        swprintf(buf, 32, L"error %lu", code);
        s = buf;
    }
    return s;
}

std::wstring format_size(uint64_t bytes)
{
    wchar_t buf[64];
    const double kb = 1024.0, mb = kb * 1024.0, gb = mb * 1024.0;
    if (bytes >= (uint64_t)gb)
        swprintf(buf, 64, L"%.2f GB", bytes / gb);
    else if (bytes >= (uint64_t)mb)
        swprintf(buf, 64, L"%.0f MB", bytes / mb);
    else if (bytes >= (uint64_t)kb)
        swprintf(buf, 64, L"%.0f KB", bytes / kb);
    else
        swprintf(buf, 64, L"%u bytes", (unsigned)bytes);
    return buf;
}

std::wstring exe_folder()
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, &path[0], (DWORD)path.size());
        if (n == 0)
            return L".\\";
        if (n < path.size()) {
            path.resize(n);
            break;
        }
        path.resize(path.size() * 2);
    }
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L".\\" : path.substr(0, slash + 1);
}

std::wstring with_slash(std::wstring folder)
{
    if (!folder.empty() && folder.back() != L'\\' && folder.back() != L'/')
        folder += L'\\';
    return folder;
}

bool make_folders(const std::wstring &folder)
{
    if (folder.empty())
        return false;
    DWORD a = GetFileAttributesW(folder.c_str());
    if (a != INVALID_FILE_ATTRIBUTES)
        return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    /* Create the parent first, then this folder. */
    std::wstring path = folder;
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/'))
        path.pop_back();
    size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 0 && path[slash - 1] != L':' && path[slash - 1] != L'\\')
        make_folders(path.substr(0, slash));
    if (CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS)
        return true;
    return false;
}

std::wstring widen(const std::string &s)
{
    if (s.empty())
        return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string narrow(const std::wstring &w)
{
    if (w.empty())
        return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring choose_folder(void *owner, const wchar_t *title, const wchar_t *ok, const std::wstring &start)
{
    std::wstring out;
    IFileOpenDialog *d = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d))))
        return out;
    DWORD o = 0;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
    d->SetTitle(title);
    d->SetOkButtonLabel(ok);
    IShellItem *at = nullptr;
    if (!start.empty() && SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&at)))) {
        d->SetFolder(at);
        at->Release();
    }
    IShellItem *it = nullptr;
    if (SUCCEEDED(d->Show((HWND)owner)) && SUCCEEDED(d->GetResult(&it))) {
        PWSTR p = nullptr;
        if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
            out = p;
            CoTaskMemFree(p);
        }
        it->Release();
    }
    d->Release();
    return out;
}

} // namespace util
