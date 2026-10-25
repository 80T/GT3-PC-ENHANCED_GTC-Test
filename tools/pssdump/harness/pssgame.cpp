/* pssgame.cpp - pssgame's native side: the platform services the game's
 * movie path runs on (the game scheduler and kernel, the volume, the
 * native sound and its WASAPI output, the log) and the harness helpers.
 *
 *   pssgame [--vol gt3.vol] [--out DIR] SCENARIO ARGS...
 *     play FILE SECONDS SOUND ONESHOT W H
 *     switch FILE1 FILE2 SECONDS
 *     preload FILE SECONDS W H
 *     missing FILE
 *     ipic
 * The table of gt3.vol comes from `python tools/gt3vol.py list` (run by the
 * harness itself).  Frames are dumped as PNG into --out. */
#include "harness.h"

#include "engine/media/native/ipu_csc.h"
#include "platform/audio.h"
#include "platform/disc.h"
#include "platform/log.h"
#include "platform/scheduler.h"
#include "../png_write.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Entry {
    unsigned lsn;
    unsigned size;
};

std::map<std::string, Entry> g_table;
std::string g_out = ".";
std::vector<char *> g_args;
std::atomic<int> g_result{-1};

bool load_table(const std::string &vol)
{
    std::string cmd = "python tools/gt3vol.py --vol \"" + vol + "\" list";
    FILE *p = _popen(cmd.c_str(), "r");
    if (!p)
        return false;
    char line[512];
    while (fgets(line, sizeof(line), p)) {
        char path[256];
        unsigned lsn, size;
        const char *l = std::strstr(line, " lsn ");
        if (!l || sscanf(line, "%255s", path) != 1)
            continue;
        if (sscanf(l, " lsn %u size %u", &lsn, &size) == 2)
            g_table[path] = {lsn, size};
    }
    _pclose(p);
    return !g_table.empty();
}

void game_thread(void *)
{
    int r = hg_run((int)g_args.size(), g_args.data());
    g_result = r;
    for (;;)
        sched::sleep_thread();
}

} // namespace

/* ---- the decompiled side's libc (pc_prelude.h maps the names) ---------- */

extern "C" void *pc_malloc(size_t n) { return _aligned_malloc(n ? n : 1, 16); }
extern "C" void pc_free(void *p) { _aligned_free(p); }
extern "C" void *pc_memalign(size_t a, size_t n) { return _aligned_malloc(n ? n : 1, a < 16 ? 16 : a); }
extern "C" void *pc_realloc(void *p, size_t n) { return _aligned_realloc(p, n ? n : 1, 16); }
extern "C" void *pc_calloc(size_t a, size_t b)
{
    void *p = pc_malloc(a * b);
    std::memset(p, 0, a * b);
    return p;
}
extern "C" int pc_printf(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fputs(buf, stdout);
    fflush(stdout);
    pclog::raw(buf, (int)strlen(buf));
    return n;
}
extern "C" int pc_sprintf(char *out, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsprintf(out, fmt, ap);
    va_end(ap);
    return n;
}

/* ---- harness helpers ---------------------------------------------------- */

extern "C" int hn_lookup(const char *path, int *lsn, unsigned *size)
{
    auto it = g_table.find(path);
    if (it == g_table.end())
        return 0;
    *lsn = (int)it->second.lsn;
    *size = it->second.size;
    return 1;
}

extern "C" int hn_read(const char *path, void **data, unsigned *size)
{
    int lsn;
    if (!hn_lookup(path, &lsn, size))
        return 0;
    *data = pc_malloc(*size);
    return disc::read_sectors((uint32_t)lsn, *data, *size) ? 1 : 0;
}

extern "C" void hn_dump_frame(const void *blocks, int w, int h, int columnMajor, const char *name)
{
    std::vector<uint8_t> rgba((size_t)w * h * 4);
    gt3media::ps2_blocks_to_rgba((const uint8_t *)blocks, w, h, rgba.data(), w * 4, columnMajor != 0);
    pngw::write(g_out + "/" + name + ".png", rgba.data(), w, h, w * 4);
}

extern "C" unsigned hn_frame_crc(const void *blocks, int bytes)
{
    return pngw::crc32((const uint8_t *)blocks, (size_t)bytes);
}

extern "C" double hn_now(void)
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

extern "C" const char *hn_out_dir(void)
{
    return g_out.c_str();
}

int main(int argc, char **argv)
{
    std::string vol = "gt3_disc/files/gt3.vol";
    int i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "--vol") && i + 1 < argc)
            vol = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)
            g_out = argv[++i];
        else
            break;
    }
    for (; i < argc; i++)
        g_args.push_back(argv[i]);
    g_args.push_back(nullptr);
    g_args.pop_back();
    if (g_args.empty()) {
        printf("pssgame [--vol gt3.vol] [--out DIR] play|switch|preload|missing|ipic ...\n");
        return 2;
    }
    CreateDirectoryA(g_out.c_str(), nullptr);
    pclog::open((g_out + "/pssgame.log").c_str());
    if (!load_table(vol)) {
        printf("pssgame: no table of %s\n", vol.c_str());
        return 2;
    }
    if (!disc::open(vol.c_str())) {
        printf("pssgame: cannot open %s\n", vol.c_str());
        return 2;
    }
    audio::init();
    sched::init();
    sched::start_main_thread(game_thread, nullptr, 1, 16 << 20);
    while (g_result < 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    /* let the sound end */
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    int r = g_result;
    printf("pssgame: %s\n", r == 0 ? "done" : "FAILED");
    pclog::close();
    fflush(stdout);
    ExitProcess(r);
}
