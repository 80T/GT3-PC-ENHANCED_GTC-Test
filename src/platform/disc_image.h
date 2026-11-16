/* disc_image.h - the game files read straight from a disc image.
 *
 * PC (2026-11-14, user request: "implement direct .iso loading for the Play
 * from disc... feature"): the launcher's Play tab > Disc starts the game on a
 * disc image without installing it.  The data folder is then a small one the
 * launcher makes for that disc (discs\<boot file>_<id>: src/launcher/
 * install.h prepare_disc), whose marker names the image ("image <path>",
 * "volume <its volume in the image>"; platform/paths.h mounts it), and the
 * disc's own files come from the volume inside the image (GT3.VOL, "RoFS":
 * src/launcher/volume.h) through the game's ordinary file layer
 * (platform/host_file.h): a file the data folder or a mod folder does not
 * have is the volume's.
 *
 * The volume's files are seen as an install lays them out (the unified
 * structure, src/launcher/install.h: the Japanese disc's courses in
 * courses\JP\) and unpacked: a gzip-packed file (a third of the volume: the
 * cars, the courses, the menus) is inflated whole at its first read and
 * checked against its CRC and size, as the installer does; a plain one (the
 * movies, the music, the sounds) is read in place.  Nothing is written to
 * the image, which is opened for reading only.
 *
 * Reads are positional and thread-safe (the game's loader, the streaming
 * threads). */
#pragma once

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

namespace disc_image {

/* A file of the volume. */
struct File {
    std::string rel;        /* its path in the data folder, the volume's case: "cars\day\am0006" */
    uint64_t offset = 0;    /* its stored bytes' place in the volume */
    uint32_t size = 0;      /* its bytes once unpacked */
    uint32_t stored = 0;    /* its bytes in the volume (a packed file's packed size) */
    bool packed = false;    /* gzip-packed */
};

/* Opens the image at `image' (UTF-8) and lists the files of its volume
 * `volume' ("GT3.VOL", "FOLDER/GT3.VOL"; "": GT3.VOL); `language': the
 * disc's own ("JP", "US", "BR"...: its courses' region folder).  False with
 * the reason in *error.  Once, before the first file is opened. */
bool mount(const std::string &image, const std::string &volume, const std::string &language, std::string *error);
bool mounted();
/* The image's path as given to mount (UTF-8), for the log. */
const std::string &image_path();

/* The file at `rel', lower case, '\' separated ("cars\day\am0006"); NULL: not
 * the volume's. */
const File *find(const std::string &rel);
/* Every file of the volume. */
const std::vector<File> &files();

/* `bytes' of f's stored bytes from `offset' on; false on a read error or
 * past its end. */
bool read(const File &f, uint64_t offset, void *buf, uint32_t bytes);
/* A packed file's bytes, unpacked and checked (the last few are kept: a file
 * opened again soon is not inflated again); NULL with a line in the log when
 * it cannot be read or is damaged. */
std::shared_ptr<const std::vector<uint8_t>> unpacked(const File &f);

} // namespace disc_image
