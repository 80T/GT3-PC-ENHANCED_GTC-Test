/* screenshot.cpp - screenshots (screenshot.h). */
#include "platform/screenshot.h"
#include "platform/log.h"
#include "platform/osd.h"
#include "platform/paths.h"
#include "platform/png_image.h"

#include <windows.h>

#include <atomic>
#include <mutex>
#include <stdio.h>
#include <string>
#include <thread>

namespace screenshot {
namespace {

/* At most this many written at once (each holds its picture: up to 256 MB
 * at the largest resolution, in a 32-bit process) */
constexpr int kMaxWriting = 2;

std::atomic<bool> g_asked{false};           /* asked for, not yet taken by the renderer */
std::atomic<int> g_taking{0};               /* taken, its picture not yet handed over */
std::atomic<int> g_writing{0};              /* handed over, being written */
std::mutex g_write;                         /* one written at a time: the names chosen in order */

std::wstring wide(const std::string &s)
{
    const int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

/* The picture as screenshots\gt3pc_<date>_<time>[_N].png (the time it was
 * taken; _2 .. _99 after one of the same second); a message either way. */
void write(const std::vector<uint8_t> &px, int w, int h, const SYSTEMTIME &at)
{
    std::lock_guard<std::mutex> hold(g_write);
    const std::string &dir = paths::screenshots_dir();
    const std::wstring wdir = wide(dir);
    CreateDirectoryW(wdir.c_str(), nullptr);    /* (there already: nothing) */
    char stem[48], file[64] = "";
    snprintf(stem, sizeof(stem), "gt3pc_%04u-%02u-%02u_%02u-%02u-%02u", at.wYear, at.wMonth, at.wDay, at.wHour,
             at.wMinute, at.wSecond);
    for (int n = 1; n < 100; n++) {
        if (n == 1)
            snprintf(file, sizeof(file), "%s.png", stem);
        else
            snprintf(file, sizeof(file), "%s_%d.png", stem, n);
        if (GetFileAttributesW((wdir + wide(file)).c_str()) == INVALID_FILE_ATTRIBUTES)
            break;
    }
    const bool ok = png_image::encode_file(wdir + wide(file), px.data(), w, h);
    char msg[64];
    if (ok) {
        pclog::write("screenshot: %s%s (%d x %d)", dir.c_str(), file, w, h);
        snprintf(msg, sizeof(msg), "SCREENSHOT: %s %d X %d", file, w, h);
    } else {
        pclog::write("screenshot: %s%s (%d x %d) could not be written", dir.c_str(), file, w, h);
        snprintf(msg, sizeof(msg), "SCREENSHOT COULD NOT BE WRITTEN (SEE THE LOG)");
    }
    osd::show(msg);
}

} // namespace

void request()
{
    if (g_writing.load() >= kMaxWriting) {
        osd::show("SCREENSHOT: STILL WRITING THE LAST ONES");
        return;
    }
    g_asked = true;
}

bool take_request()
{
    if (!g_asked.exchange(false))
        return false;
    g_taking = 1;
    return true;
}

void failed(const char *why)
{
    pclog::write("screenshot: not taken: %s", why);
    char msg[64];
    snprintf(msg, sizeof(msg), "NO SCREENSHOT: %s", why);
    osd::show(msg);
    g_taking = 0;
}

void save(std::vector<uint8_t> &&rgba, int w, int h)
{
    SYSTEMTIME at;
    GetLocalTime(&at);
    g_writing++;                            /* (before: pending() never 0 meanwhile) */
    g_taking = 0;
    try {
        std::thread([px = std::move(rgba), w, h, at] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);   /* (the game first) */
            write(px, w, h, at);
            g_writing--;
        }).detach();
    } catch (...) {
        g_writing--;
        failed("NO THREAD TO WRITE IT");
    }
}

int pending()
{
    return (g_asked.load() ? 1 : 0) + g_taking.load() + g_writing.load();
}

} // namespace screenshot
