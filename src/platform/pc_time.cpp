/* pc_time.cpp - the clock as the game saw it on the PS2 (pc_time.h). */
#include "platform/pc_time.h"

#include <windows.h>

namespace pctime {
namespace {

constexpr int64_t kJstOffset = 9 * 3600;

/* Seconds between 1601-01-01 (FILETIME) and 2000-01-01, both UTC. */
constexpr int64_t kEpoch2000 = 12591158400LL;

int bcd(uint8_t v) { return (v >> 4) * 10 + (v & 15); }
uint8_t to_bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

int64_t utc_now_seconds_since_2000()
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    int64_t t = ((int64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return t / 10000000 - kEpoch2000;
}

/* BCD clock <-> seconds since 2000 in the clock's own zone. */
int64_t bcd_to_seconds(const uint8_t *c)
{
    SYSTEMTIME st = {};
    st.wYear = (WORD)(2000 + bcd(c[7]));
    st.wMonth = (WORD)bcd(c[6]);
    st.wDay = (WORD)bcd(c[5]);
    st.wHour = (WORD)bcd(c[3]);
    st.wMinute = (WORD)bcd(c[2]);
    st.wSecond = (WORD)bcd(c[1]);
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft))
        return 0;
    int64_t t = ((int64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return t / 10000000 - kEpoch2000;
}

void seconds_to_bcd(int64_t s, uint8_t *c)
{
    int64_t t = (s + kEpoch2000) * 10000000;
    FILETIME ft;
    ft.dwLowDateTime = (DWORD)t;
    ft.dwHighDateTime = (DWORD)(t >> 32);
    SYSTEMTIME st;
    FileTimeToSystemTime(&ft, &st);
    c[1] = to_bcd(st.wSecond);
    c[2] = to_bcd(st.wMinute);
    c[3] = to_bcd(st.wHour);
    c[4] = 0;
    c[5] = to_bcd(st.wDay);
    c[6] = to_bcd(st.wMonth);
    c[7] = to_bcd(st.wYear % 100);
}

} // namespace

uint32_t rtc_seconds_since_2000_jst()
{
    return (uint32_t)(utc_now_seconds_since_2000() + kJstOffset);
}

void bcd_jst_to_gmt(uint8_t *c)
{
    seconds_to_bcd(bcd_to_seconds(c) - kJstOffset, c);
}

void bcd_jst_to_local(uint8_t *c)
{
    int64_t utc = bcd_to_seconds(c) - kJstOffset;
    /* UTC -> local with the PC's time zone rules (daylight saving included). */
    int64_t t = (utc + kEpoch2000) * 10000000;
    FILETIME ft, lft;
    ft.dwLowDateTime = (DWORD)t;
    ft.dwHighDateTime = (DWORD)(t >> 32);
    SYSTEMTIME st, lst;
    FileTimeToSystemTime(&ft, &st);
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &st, &lst))
        lst = st;
    SystemTimeToFileTime(&lst, &lft);
    int64_t l = ((int64_t)lft.dwHighDateTime << 32) | lft.dwLowDateTime;
    seconds_to_bcd(l / 10000000 - kEpoch2000, c);
}

} // namespace pctime
