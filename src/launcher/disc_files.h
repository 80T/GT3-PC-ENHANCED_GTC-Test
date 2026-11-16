/* disc_files.h - the launcher's view of a disc image's data folder.
 *
 * PC (2026-11-15, user request: "If the user only links a disc with no
 * installation, the song manager needs to still work").  A disc image added
 * on the Play tab's Disc sub-tab has a small data folder of its own
 * (discs\<boot file>_<id>: install.h prepare_disc) whose marker names the
 * image; the game reads the disc's files from the image under that folder's
 * own (src/platform/disc_image.h).  The launcher reads them the same way
 * where it needs them - the music's list (bgm\ads.inf) and the songs
 * (bgm\*.ads) for the song manager (music.h) - with its own readers of the
 * image and its volume (iso9660.h, volume.h, inflate.h), in the same unified
 * structure.  The image is opened for reading only.
 *
 * Kept open once opened, until release_all (the song manager's window
 * closed, the counts of the Songs page taken): the image can be moved or
 * deleted while the launcher shows its window.  Reads are positional and
 * thread-safe (the song manager's threads). */
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace disc_files {

/* A file of the image's volume. */
struct File {
    std::string rel;            /* its path in the data folder, the volume's case: "bgm\race_a01.ads" */
    uint64_t offset = 0;        /* its stored bytes' place in the volume */
    uint32_t size = 0;          /* its bytes once unpacked */
    uint32_t stored = 0;        /* its bytes in the volume */
    bool packed = false;        /* gzip-packed */
};

class Disc;

/* The disc of the data folder `data' (its marker's "image" line), opened and
 * kept; nullptr for an installed version's folder, or when the image cannot
 * be used (missing, another disc there now). */
std::shared_ptr<const Disc> of(const std::wstring &data);
/* Every disc let go of (those still in use stay open until their last user is
 * done). */
void release_all();

class Disc {
public:
    /* The file at `rel', any case, '\' or '/' separated ("bgm\ads.inf"). */
    const File *find(const std::string &rel) const;
    /* The files directly in the folder `rel' ("bgm"), by name. */
    std::vector<const File *> folder(const std::string &rel) const;
    /* `bytes' of f once unpacked, from `offset' on; false on a read error or
     * past its end (a packed file is unpacked whole for it). */
    bool read(const File &f, uint64_t offset, void *buf, uint32_t bytes) const;
    /* f whole, unpacked and checked; false with a line in the log otherwise. */
    bool whole(const File &f, std::vector<uint8_t> *out) const;
    const std::wstring &image() const { return image_; }

    Disc();
    ~Disc();
    Disc(const Disc &) = delete;
    Disc &operator=(const Disc &) = delete;

private:
    friend std::shared_ptr<const Disc> of(const std::wstring &data);
    bool open(const std::wstring &image, const std::string &volume, const std::string &language);

    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::wstring image_;
    std::vector<File> files_;
};

} // namespace disc_files
