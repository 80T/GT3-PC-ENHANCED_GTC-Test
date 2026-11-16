/* paths.cpp - where GT3 PC finds its files (paths.h). */
#include "platform/paths.h"
#include "platform/disc_image.h"
#include "platform/log.h"
#include "platform/save_folders.h"             /* PC (2026-11-15) */

#include <windows.h>

#include <stdio.h>
#include <string.h>

#include <utility>
#include <vector>

namespace paths {
namespace {

std::string g_gameDir, g_dataDir, g_dataFolder, g_initError;
std::string g_ini, g_engineIni;                 /* PC (2026-11-14): paths.h ini_path, engine_ini_path */
std::string g_savesDir;                         /* PC (2026-11-15): paths.h saves_dir */
std::string g_screenshotsDir;                   /* PC (2026-11-15): paths.h screenshots_dir */
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

    /* PC (2026-11-14): the settings file - another launcher's with --shared
     * (this engine was started from its selector) - and the engine's own */
    std::string iniDir = g_gameDir;
    for (int i = 1; i + 1 < argc; i++)
        if (strcmp(argv[i], "--shared") == 0)
            iniDir = with_slash(full_path(argv[i + 1]));
    const bool shared = _stricmp(iniDir.c_str(), g_gameDir.c_str()) != 0;
    g_ini = iniDir + "gt3pc.ini";
    g_engineIni = g_gameDir + kEngineIni;
    if (!shared && !file_exists(g_engineIni))
        g_engineIni = g_ini;                /* (the launcher has not moved them yet) */
    if (shared)
        pclog::write("settings: %s (shared: --shared); this engine's own: %s", g_ini.c_str(), g_engineIni.c_str());
    /* PC (2026-11-15): the memory cards, the engine's save folders
     * (save_folders.h) beside the gt3pc.ini in use */
    g_savesDir = save_folders::to_ansi(
        save_folders::saves_dir(save_folders::from_ansi(iniDir), save_folders::from_ansi(g_gameDir)));
    /* PC (2026-11-15): the screenshots, beside it too (platform/screenshot.h) */
    g_screenshotsDir = iniDir + kScreenshotsFolder + "\\";

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
    /* PC (2026-11-14): the disc image last played from the launcher (its
     * folder in discs\) */
    char disc[128] = "";
    GetPrivateProfileStringA("Game", "Disc", "", disc, sizeof(disc), g_engineIni.c_str());
    if (disc[0] != 0 && strpbrk(disc, "\\/:") == nullptr)
        candidates.push_back(g_gameDir + kDiscsFolder + "\\" + disc + "\\");
    char version[16] = "";
    GetPrivateProfileStringA("Game", "Version", "", version, sizeof(version), g_engineIni.c_str());
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
            /* PC (2026-11-14): a disc image's folder: its files are the
             * image's volume's, read in place (platform/disc_image.h) */
            const std::string image = install_info("image");
            if (!image.empty()) {
                std::string error;
                if (!disc_image::mount(image, install_info("volume"), install_info("language"), &error)) {
                    pclog::write("data: the disc image %s cannot be used: %s", image.c_str(), error.c_str());
                    g_initError = "The disc image\n\n" + image + "\n\ncannot be used: " + error +
                                  ".\n\nStart it again from the launcher's Play tab (Disc), or play an installed "
                                  "version there.";
                    return false;
                }
            }
            return true;
        }
    pclog::write("data: no installed game files (%s\\%s_US, _EU, _JP with %s) in %s (run the launcher to install "
                 "them)", g_gameDir.c_str(), kDataFolder, kInstallMarker, g_gameDir.c_str());
    return false;
}

const std::string &init_error() { return g_initError; }
const std::string &ini_path() { return g_ini; }
const std::string &engine_ini_path() { return g_engineIni; }
const std::string &game_dir() { return g_gameDir; }
const std::string &saves_dir() { return g_savesDir; }  /* PC (2026-11-15) */
const std::string &screenshots_dir() { return g_screenshotsDir; }  /* PC (2026-11-15) */
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
