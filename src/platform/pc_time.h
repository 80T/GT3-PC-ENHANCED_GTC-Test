/* pc_time.h - the clock as the game saw it on the PS2.
 *
 * The PS2's real-time clock counts Japan time (UTC+9) whatever the
 * console's region; libscf turned its readings into GMT or into the local
 * time of the console's settings.  The port reads the PC's clock and gives
 * the game the same: Japan time from the RTC, GMT and local time from the
 * conversions. */
#pragma once

#include <stdint.h>

namespace pctime {

/* Seconds since 2000-01-01 00:00 Japan time (pdicdvd's clock word). */
uint32_t rtc_seconds_since_2000_jst();

/* A BCD date (sceCdCLOCK layout: stat, second, minute, hour, pad, day,
 * month, year since 2000), converted in place from Japan time. */
void bcd_jst_to_gmt(uint8_t *clock8);
void bcd_jst_to_local(uint8_t *clock8);

} // namespace pctime
