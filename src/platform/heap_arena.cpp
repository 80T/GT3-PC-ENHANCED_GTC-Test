/* heap_arena.cpp - the memory of the game heap (heap_arena.h). */
#include "platform/heap_arena.h"
#include "platform/log.h"

#include <windows.h>

#include <stdlib.h>                         /* PC (2026-11-07): getenv (GT3PC_HEAP_MB) */

namespace {

/* The sizes tried, largest first (MB): a place below 2 GB first, then any
 * (the process is large-address aware).  PC (2026-11-07): 768 MB, was 1536 -
 * the 4 GB address space ran out in a 32-car race after six others (a
 * texture decode's new threw bad_alloc at 4071 MB in use, platform/
 * memstat.h), the arena, committed whole, holding 1.5 GB of it with 29 MB
 * in use; GT3PC_HEAP_MB=N (128 .. 2048) asks for a size of its own first. */
constexpr uint32_t kSizesMb[] = {768, 512, 384, 256, 128};
constexpr uint32_t kEnvMinMb = 128, kEnvMaxMb = 2048;
void *g_arena;
uint32_t g_size;

void *reserve(uint32_t bytes, bool low)
{
    void *p = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (p != nullptr && low && (uintptr_t)p + bytes > 0x80000000u) {
        VirtualFree(p, 0, MEM_RELEASE);
        return nullptr;
    }
    return p;
}

} // namespace

void *heap_arena(uint32_t *size)
{
    if (!g_arena) {
        /* PC (2026-11-07): GT3PC_HEAP_MB first */
        uint32_t askedMb = 0;
        if (const char *e = getenv("GT3PC_HEAP_MB")) {
            const int v = atoi(e);
            askedMb = v < (int)kEnvMinMb ? kEnvMinMb : v > (int)kEnvMaxMb ? kEnvMaxMb : (uint32_t)v;
        }
        for (int pass = 0; pass < 2 && !g_arena; pass++) {
            if (askedMb != 0 && !g_arena) {
                g_arena = reserve(askedMb << 20, pass == 0);
                if (g_arena)
                    g_size = askedMb << 20;
            }
            for (uint32_t mb : kSizesMb) {
                if (g_arena)
                    break;
                g_arena = reserve(mb << 20, pass == 0);
                if (g_arena)
                    g_size = mb << 20;
            }
        }
        if (!g_arena)
            pclog::fatal("could not reserve the game heap (%u MB)", kSizesMb[sizeof(kSizesMb) / sizeof(kSizesMb[0]) - 1]);
        pclog::write("heap: %u MB at %p%s", g_size >> 20, g_arena,
                     askedMb != 0 ? (g_size == askedMb << 20 ? " (GT3PC_HEAP_MB)" : " (GT3PC_HEAP_MB's size could not be had)")
                                  : "");
    }
    *size = g_size;
    return g_arena;
}
