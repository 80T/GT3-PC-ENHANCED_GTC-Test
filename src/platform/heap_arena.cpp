/* heap_arena.cpp - the memory of the game heap (heap_arena.h). */
#include "platform/heap_arena.h"
#include "platform/log.h"

#include <windows.h>

namespace {

/* The sizes tried, largest first (MB): a place below 2 GB first, then any
 * (the process is large-address aware). */
constexpr uint32_t kSizesMb[] = {1536, 1024, 768, 512, 384, 256, 128};
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
        for (int pass = 0; pass < 2 && !g_arena; pass++)
            for (uint32_t mb : kSizesMb) {
                g_arena = reserve(mb << 20, pass == 0);
                if (g_arena) {
                    g_size = mb << 20;
                    break;
                }
            }
        if (!g_arena)
            pclog::fatal("could not reserve the game heap (%u MB)", kSizesMb[sizeof(kSizesMb) / sizeof(kSizesMb[0]) - 1]);
        pclog::write("heap: %u MB at %p", g_size >> 20, g_arena);
    }
    *size = g_size;
    return g_arena;
}
