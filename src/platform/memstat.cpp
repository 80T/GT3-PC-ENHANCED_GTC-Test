/* memstat.cpp - the process's memory, for the log (memstat.h). */
#include "platform/memstat.h"
#include "platform/gpu.h"

#define PSAPI_VERSION 2                     /* GetProcessMemoryInfo from kernel32 (Windows 7 and later) */
#include <windows.h>
#include <psapi.h>
#include <dxgi1_4.h>

#include <stdarg.h>
#include <stdio.h>

namespace memstat {
namespace {

/* text appended to out (n bytes, `at' used so far); the new length */
size_t put(char *out, size_t n, size_t at, const char *fmt, ...)
{
    if (at >= n)
        return at;
    va_list a;
    va_start(a, fmt);
    const int w = vsnprintf(out + at, n - at, fmt, a);
    va_end(a);
    if (w < 0)
        return at;
    return at + (size_t)w >= n ? n - 1 : at + (size_t)w;
}

} // namespace

uint32_t virtual_mb()
{
    MEMORYSTATUSEX ms = {};
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms))
        return 0;
    return (uint32_t)((ms.ullTotalVirtual - ms.ullAvailVirtual) >> 20);
}

void line(char *out, size_t n, bool withGpu)
{
    if (n == 0)
        return;
    out[0] = '\0';
    size_t at = 0;
    MEMORYSTATUSEX ms = {};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms))
        at = put(out, n, at, "virtual %u MB of %u", (unsigned)((ms.ullTotalVirtual - ms.ullAvailVirtual) >> 20),
                 (unsigned)(ms.ullTotalVirtual >> 20));
    PROCESS_MEMORY_COUNTERS_EX pm = {};
    pm.cb = sizeof(pm);
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof(pm)))
        at = put(out, n, at, "%sprivate %u MB, working set %u MB", at ? ", " : "",
                 (unsigned)(pm.PrivateUsage >> 20), (unsigned)(pm.WorkingSetSize >> 20));
    if (withGpu) {
        IDXGIAdapter *a = gpu::adapter();
        IDXGIAdapter3 *a3 = nullptr;
        if (a != nullptr && SUCCEEDED(a->QueryInterface(__uuidof(IDXGIAdapter3), (void **)&a3))) {
            DXGI_QUERY_VIDEO_MEMORY_INFO local = {}, shared = {};
            if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) &&
                SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared)))
                at = put(out, n, at, "%sGPU local %u MB of %u, non-local %u MB", at ? "; " : "",
                         (unsigned)(local.CurrentUsage >> 20), (unsigned)(local.Budget >> 20),
                         (unsigned)(shared.CurrentUsage >> 20));
            a3->Release();
        }
    }
    if (at == 0)
        put(out, n, 0, "not known");
}

} // namespace memstat
