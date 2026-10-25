/* volume.h - the files of Gran Turismo 3's GT3.VOL ("RoFS").
 *
 * The layout engine/io/T500_vol_toc.cpp and T510_vol_device.cpp read (and
 * tools/gt3vol.py): the table of contents at the start of the volume, header
 * word 3 = header size, word 4 = body size; a TOC whose magic is ~"RoFS" has
 * its body bytes inverted.  Directory record at offset `off`: +0 name offset
 * (low 24 bits), +4 entry count + 1, +0xC entry record offsets; root record
 * 0x14.  Entry record: +3 type (1 directory, 2 gzip-packed file, else a plain
 * file), +4 first sector (relative to the volume, 0x800 bytes), +8 size, +0xC
 * packed size.  The volume is read through a callback, so the same code
 * walks GT3.VOL inside a disc image or an installed gt3.vol file. */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace volume {

constexpr uint32_t kSectorSize = 0x800;

struct File {
    std::string path;       /* "cars/day/am0006": '/' separated, as the game names it */
    uint32_t lsn = 0;       /* first sector, relative to the volume */
    uint32_t size = 0;      /* bytes once unpacked */
    uint32_t stored = 0;    /* bytes in the volume (the packed size of a packed file) */
    bool packed = false;    /* gzip-packed */
};

/* Reads `size` bytes at byte `offset` of the volume. */
using Reader = std::function<bool(uint64_t offset, void *buf, uint32_t size)>;

/* Lists every file of a volume of `volume_size` bytes.  False with a
 * message when the table of contents is not a RoFS one or does not fit. */
bool list_files(const Reader &read, uint64_t volume_size, std::vector<File> *files, std::wstring *error);

} // namespace volume
