/* pc_bgm_list.cpp - PC: bgm/ads.txt, the plain-text list of the streamed
 * music (see pc_bgm_list.h). */
#include "game/app/pc_bgm_list.h"
#include "engine/io/T502_file_resolve.h"        /* file_locate_n */
#include "engine/io/T503_vol_thread.h"          /* vol_device_object */
#include "engine/sound/native/bgm_files.h"
#include "platform/host_file.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace spu2;

constexpr int kHostName = 0x200;

/* A host file of the data folder (bgm/...): its handle, -1 when it is not
 * there (quietly: a probe). */
int open_quiet(const std::string &path)
{
    char host[kHostName];
    int lsn = 0;
    u32 size = 0;
    host_file_quiet(1);
    int fd = -1;
    if (file_locate_n(path.c_str(), host, sizeof(host), &lsn, &size, &vol_device_object) != -1 && host[0] != '\0')
        fd = host_file_open(host);
    host_file_quiet(0);
    return fd;
}

/* A track's file: bgm/<name>, or where its full path says. */
int open_track(const std::string &name)
{
    if (!bgm_system_path(name))
        return open_quiet("bgm/" + name);
    host_file_quiet(1);
    const int fd = host_file_open(name.c_str());
    host_file_quiet(0);
    return fd;
}

int read_fd(void *user, uint32_t offset, void *dst, uint32_t size)
{
    return host_file_read_at((int)(intptr_t)user, offset, dst, size);
}

/* What a track's file (or its other format's) is. */
bool probe(const std::string &file, BgmFileInfo *info, std::string *found)
{
    for (const std::string &name : {file, bgm_other_name(file)}) {
        if (name.empty())
            continue;
        const int fd = open_track(name);
        if (fd < 0)
            continue;
        const int64_t size = host_file_size(fd);
        const bool ok = bgm_probe(read_fd, (void *)(intptr_t)fd, size > 0xFFFFFFFFll ? 0xFFFFFFFFu : (uint32_t)size,
                                  info);
        host_file_close(fd);
        if (ok) {
            *found = name;
            return true;
        }
    }
    return false;
}

} // namespace

extern "C" void *pc_ads_list_load(void)
{
    const int fd = open_quiet("bgm/ads.txt");
    if (fd < 0)
        return NULL;
    const int64_t size = host_file_size(fd);
    std::string text;
    if (size > 0 && size < (64 << 20)) {
        text.resize((size_t)size);
        if (host_file_read_at(fd, 0, &text[0], (uint32_t)size) != (int)size)
            text.clear();
    }
    host_file_close(fd);
    if (text.empty()) {
        printf("PC: bgm/ads.txt could not be read: bgm/ads.inf is used\n");
        return NULL;
    }
    AdsList list;
    std::vector<std::string> warnings;
    ads_list_parse(text, &list, &warnings);
    const std::vector<uint8_t> img = ads_list_image(list, probe, &warnings);
    int tracks = 0, groups = 0;
    for (int g = 0; g < ADS_GROUPS; g++) {
        tracks += (int)list.group[g].size();
        groups += list.group[g].empty() ? 0 : 1;
    }
    printf("PC: the music list from bgm/ads.txt: %d tracks in %d groups%s\n", tracks, groups,
           warnings.empty() ? "" : " (warnings below)");
    for (size_t i = 0; i < warnings.size() && i < 40; i++)
        printf("PC: bgm/ads.txt %s\n", warnings[i].c_str());
    if (warnings.size() > 40)
        printf("PC: bgm/ads.txt: %d more warnings\n", (int)(warnings.size() - 40));
    void *inf = malloc(img.size());             /* (the game heap: ads_term frees it) */
    if (inf == NULL)
        return NULL;
    std::memcpy(inf, img.data(), img.size());
    return inf;
}

extern "C" int pc_bgm_other_format(char *path, int size)
{
    const std::string other = bgm_other_name(path);
    if (other.empty() || (int)other.size() >= size)
        return 0;
    std::memcpy(path, other.c_str(), other.size() + 1);
    return 1;
}

extern "C" int pc_bgm_system_path(const char *name)
{
    return bgm_system_path(name) ? 1 : 0;
}

extern "C" int pc_bgm_file_size(const char *path, unsigned *size)
{
    const int fd = open_track(path);
    if (fd < 0)
        return 0;
    const int64_t bytes = host_file_size(fd);
    host_file_close(fd);
    if (bytes < 0)
        return 0;
    *size = bytes > 0xFFFFF800ll ? 0xFFFFF800u : (unsigned)bytes;
    return 1;
}

extern "C" int pc_bgm_song_there(const char *name)
{
    return !bgm_system_path(name) || host_file_present(name) ? 1 : 0;
}
