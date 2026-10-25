/* paths.cpp - where GT3 PC finds its files (paths.h). */
#include "platform/paths.h"
#include "platform/log.h"

#include <windows.h>

#include <stdio.h>
#include <string.h>

#include <utility>
#include <vector>

namespace paths {
namespace {

std::string g_gameDir, g_dataDir, g_dataFolder;
std::vector<std::pair<std::string, std::string>> g_marker;     /* the install marker's "key value" lines */

/* The marker's lines with a key and a value (the first line, the release's
 * description, has spaces too: its "key" is just never asked for). */
void read_marker(const std::string &path)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (f == nullptr)
        return;
    char line[1024];
    while (fgets(line, sizeof(line), f) != nullptr) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' '))
            line[--n] = 0;
        const char *space = strchr(line, ' ');
        if (space == nullptr || space == line)
            continue;
        g_marker.emplace_back(std::string(line, space - line), std::string(space + 1));
    }
    fclose(f);
}

bool file_exists(const std::string &p)
{
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string with_slash(std::string s)
{
    if (!s.empty() && s.back() != '\\' && s.back() != '/')
        s += '\\';
    return s;
}

std::string full_path(const std::string &p)
{
    char buf[MAX_PATH * 2];
    DWORD n = GetFullPathNameA(p.c_str(), sizeof(buf), buf, nullptr);
    return n ? std::string(buf, n) : p;
}

} // namespace

bool init(int argc, char **argv)
{
    char exe[MAX_PATH * 2];
    GetModuleFileNameA(nullptr, exe, sizeof(exe));
    char *slash = strrchr(exe, '\\');
    if (slash)
        slash[1] = 0;
    g_gameDir = exe;

    /* the data folders tried, each a folder with the marker: --data's (the
     * data folder, or a folder holding data\), the [Game] Version's
     * (data_US ...: the launcher's last Play), the versions', an earlier
     * launcher's data\, the source tree's extracted disc */
    std::vector<std::string> candidates;
    for (int i = 1; i + 1 < argc; i++)
        if (strcmp(argv[i], "--data") == 0) {
            candidates.push_back(with_slash(argv[i + 1]));
            candidates.push_back(with_slash(argv[i + 1]) + kDataFolder + "\\");
        }
    char version[16] = "";
    GetPrivateProfileStringA("Game", "Version", "", version, sizeof(version), (g_gameDir + "gt3pc.ini").c_str());
    if (version[0] != 0)
        candidates.push_back(g_gameDir + kDataFolder + "_" + version + "\\");
    for (const char *v : {"US", "EU", "JP"})
        candidates.push_back(g_gameDir + kDataFolder + "_" + v + "\\");
    candidates.push_back(g_gameDir + kDataFolder + "\\");
    candidates.push_back(with_slash(full_path(g_gameDir + "..\\..\\gt3_disc\\files\\data")));
    for (const std::string &c : candidates)
        if (file_exists(c + kInstallMarker)) {
            g_dataFolder = with_slash(full_path(c));
            g_dataDir = g_dataFolder.substr(0, g_dataFolder.find_last_of('\\', g_dataFolder.size() - 2) + 1);
            pclog::write("data: %s", g_dataFolder.c_str());
            read_marker(g_dataFolder + kInstallMarker);
            const std::string release = install_info("release");
            pclog::write("data: %s", release.empty() ? "Gran Turismo 3 A-Spec (USA) (an earlier launcher's install)"
                                                      : release.c_str());
            return true;
        }
    pclog::write("data: no installed game files (%s\\%s_US, _EU, _JP with %s) in %s (run the launcher to install "
                 "them)", g_gameDir.c_str(), kDataFolder, kInstallMarker, g_gameDir.c_str());
    return false;
}

const std::string &game_dir() { return g_gameDir; }
const std::string &data_dir() { return g_dataDir; }
const std::string &data_folder() { return g_dataFolder; }

std::string install_info(const char *key)
{
    for (const auto &kv : g_marker)
        if (kv.first == key)
            return kv.second;
    return std::string();
}

} // namespace paths
