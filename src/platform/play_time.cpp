/* play_time.cpp - the time played (play_time.h). */
#include "platform/play_time.h"
#include "platform/log.h"
#include "platform/paths.h"

#include <windows.h>
#include <realtimeapiset.h>                 /* QueryUnbiasedInterruptTime */

#include <stdio.h>
#include <stdlib.h>

#include <mutex>
#include <string>

namespace play_time {
namespace {

constexpr const char *kSection = "Stats";
constexpr const char *kKey = "PlayTime";
constexpr DWORD kWriteMs = 60 * 1000;

uint64_t g_base;                            /* the seconds of the runs before this one */
ULONGLONG g_start;                          /* QueryUnbiasedInterruptTime at start (100 ns; no sleep) */
HANDLE g_thread;
HANDLE g_stop;
std::mutex g_mutex;

std::string ini_path()
{
    return paths::ini_path();               /* PC (2026-11-14): the shared one with --shared (paths.h) */
}

void write()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    ULONGLONG now = g_start;
    QueryUnbiasedInterruptTime(&now);
    char v[32];
    snprintf(v, sizeof(v), "%llu", (unsigned long long)(g_base + (now - g_start) / 10000000ull));
    WritePrivateProfileStringA(kSection, kKey, v, ini_path().c_str());
}

DWORD WINAPI writer(void *)
{
    while (WaitForSingleObject(g_stop, kWriteMs) == WAIT_TIMEOUT)
        write();
    return 0;
}

} // namespace

void start()
{
    char v[32] = "";
    GetPrivateProfileStringA(kSection, kKey, "0", v, sizeof(v), ini_path().c_str());
    g_base = _strtoui64(v, nullptr, 10);
    QueryUnbiasedInterruptTime(&g_start);
    g_stop = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    g_thread = g_stop != nullptr ? CreateThread(nullptr, 0, writer, nullptr, 0, nullptr) : nullptr;
    pclog::write("time played: %.1f hours so far", (double)g_base / 3600.0);
}

void stop()
{
    if (g_thread != nullptr) {
        SetEvent(g_stop);
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    write();
}

} // namespace play_time
