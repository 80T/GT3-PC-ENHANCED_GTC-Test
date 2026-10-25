/* disc.cpp - the game's "disc" in GT3 PC (disc.h). */
#include "platform/disc.h"
#include "platform/log.h"

#include <windows.h>

namespace disc {
namespace {

HANDLE   g_file = INVALID_HANDLE_VALUE;
uint64_t g_size = 0;

} // namespace

bool open(const char *volumePath)
{
    g_file = CreateFileA(volumePath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (g_file == INVALID_HANDLE_VALUE) {
        pclog::write("disc: cannot open %s (error %lu)", volumePath, GetLastError());
        return false;
    }
    LARGE_INTEGER sz;
    GetFileSizeEx(g_file, &sz);
    g_size = (uint64_t)sz.QuadPart;
    pclog::write("disc: %s, %llu bytes", volumePath, (unsigned long long)g_size);
    return true;
}

uint64_t size()
{
    return g_size;
}

bool read_bytes(uint64_t offset, void *buf, uint32_t bytes)
{
    if (g_file == INVALID_HANDLE_VALUE)
        return false;
    uint8_t *dst = (uint8_t *)buf;
    while (bytes > 0) {
        OVERLAPPED ov = {};
        ov.Offset = (DWORD)offset;
        ov.OffsetHigh = (DWORD)(offset >> 32);
        DWORD got = 0;
        if (!ReadFile(g_file, dst, bytes, &got, &ov) || got == 0) {
            pclog::write("disc: read of %u bytes at 0x%llX failed", bytes, (unsigned long long)offset);
            return false;
        }
        dst += got;
        offset += got;
        bytes -= got;
    }
    return true;
}

} // namespace disc
