/* disc_list.cpp - the disc images played from the launcher (disc_list.h). */
#include "disc_list.h"

#include <windows.h>

namespace disc_list {
namespace {

const wchar_t kSection[] = L"Discs";

} // namespace

std::vector<Disc> load(const std::wstring &ini)
{
    std::vector<Disc> out;
    const int count = (int)GetPrivateProfileIntW(kSection, L"Count", 0, ini.c_str());
    for (int i = 1; i <= count && (int)out.size() < kMaxDiscs; i++) {
        wchar_t path[2048] = L"", name[256] = L"", folder[256] = L"";
        const std::wstring n = std::to_wstring(i);
        GetPrivateProfileStringW(kSection, (L"Disc" + n).c_str(), L"", path, 2048, ini.c_str());
        GetPrivateProfileStringW(kSection, (L"Disc" + n + L"Name").c_str(), L"", name, 256, ini.c_str());
        GetPrivateProfileStringW(kSection, (L"Disc" + n + L"Folder").c_str(), L"", folder, 256, ini.c_str());
        if (path[0] != 0)
            out.push_back(Disc{path, name, folder});
    }
    return out;
}

void save(const std::wstring &ini, const std::vector<Disc> &discs)
{
    WritePrivateProfileStringW(kSection, nullptr, nullptr, ini.c_str());    /* (the section written anew) */
    WritePrivateProfileStringW(kSection, L"Count", std::to_wstring(discs.size()).c_str(), ini.c_str());
    for (size_t i = 0; i < discs.size(); i++) {
        const std::wstring n = std::to_wstring(i + 1);
        WritePrivateProfileStringW(kSection, (L"Disc" + n).c_str(), discs[i].path.c_str(), ini.c_str());
        WritePrivateProfileStringW(kSection, (L"Disc" + n + L"Name").c_str(), discs[i].name.c_str(), ini.c_str());
        if (!discs[i].folder.empty())
            WritePrivateProfileStringW(kSection, (L"Disc" + n + L"Folder").c_str(), discs[i].folder.c_str(), ini.c_str());
    }
}

void remember(const std::wstring &ini, const std::wstring &path, const std::wstring &name, const std::wstring &folder)
{
    std::vector<Disc> discs = load(ini);
    if (!discs.empty() && discs[0].path == path && discs[0].name == name && discs[0].folder == folder)
        return;                             /* (the first already) */
    for (size_t i = 0; i < discs.size(); i++)
        if (_wcsicmp(discs[i].path.c_str(), path.c_str()) == 0) {
            discs.erase(discs.begin() + (std::ptrdiff_t)i);
            break;
        }
    if ((int)discs.size() >= kMaxDiscs)
        discs.pop_back();                   /* (the one played longest ago makes room) */
    discs.insert(discs.begin(), Disc{path, name, folder});
    save(ini, discs);
}

void set_folder(const std::wstring &ini, size_t index, const std::wstring &folder)
{
    std::vector<Disc> discs = load(ini);
    if (index >= discs.size() || discs[index].folder == folder)
        return;
    discs[index].folder = folder;
    save(ini, discs);
}

void remove(const std::wstring &ini, size_t index)
{
    std::vector<Disc> discs = load(ini);
    if (index >= discs.size())
        return;
    discs.erase(discs.begin() + (std::ptrdiff_t)index);
    save(ini, discs);
}

} // namespace disc_list
