/* disc.h - the game's "disc" in GT3 PC.
 *
 * On the PS2 the IOP read 2048-byte sectors of the DVD.  Everything the game
 * reads from the disc lives in gt3.vol, so in the port the disc is gt3.vol
 * itself: sector N is gt3.vol's bytes N * 2048 onwards (the ISO 9660 lookup
 * of GT3.VOL answers sector 0, engine/io/T526_iso9660.cpp).  Reads are
 * positional and thread-safe (game threads and native streaming threads
 * read at the same time).
 *
 * Since the loose-file pass the game does not open it: it mounts the loose
 * host:data/ files (engine/io/T503 vol_init), so the disc branches of the
 * readers (pdipipe, pdibgm, pdicdvd, mpg_service) are not reached, and a
 * read of the unopened disc fails.  Tools that read gt3.vol itself
 * (tools/pssdump) still open it. */
#pragma once

#include <stdint.h>

namespace disc {

constexpr uint32_t kSectorSize = 0x800;

bool     open(const char *volumePath);
uint64_t size();                                    /* gt3.vol's size in bytes */
/* Reads `bytes' at byte offset `offset'; false on a read error or past the end. */
bool     read_bytes(uint64_t offset, void *buf, uint32_t bytes);
inline bool read_sectors(uint32_t lsn, void *buf, uint32_t bytes)
{
    return read_bytes((uint64_t)lsn * kSectorSize, buf, bytes);
}

} // namespace disc
