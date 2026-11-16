/* install.h - installs the game files from the user's disc image.
 *
 * The game reads its files loose from a data folder next to it (the game's
 * own loose-file device, "host:data/": src/engine/io/T502_file_resolve.cpp,
 * type 3; platform/paths.h), one per version: data_US (Gran Turismo 3
 * A-Spec USA, SCUS-97102, and its bundle release PBPX-95503: the same
 * files), data_EU (Europe, SCES-50294), data_JP (Japan, SCPS-15009).  Any
 * other Gran Turismo 3 engine disc - GT Concept, a demo: a disc with
 * CORE.GT3 - is extracted as data only, into data_<boot file>
 * (data_SCES_508.58), which the game does not play.
 *
 * The installer checks that the disc image is a Gran Turismo 3 engine disc
 * - it has CORE.GT3, the engine's main program, at its root or in a folder
 * of the root (a demo disc's game), and a RoFS volume beside it (GT3.VOL;
 * docs/languages.md) - then unpacks every file of the volume - the
 * gzip-packed ones inflated and checked against their CRC - into
 * <data folder>.part\, writes the marker file last and puts the folder in
 * place, so an interrupted install never leaves a partial data folder.  A
 * new install of a version replaces its files.
 *
 * The files go into one unified structure, the volume's own where the discs
 * agree ("cars/day/am0006" -> cars\day\am0006): the courses are always in
 * a region's folder, courses\<US|JP|UK|EU>\ - the Japanese disc's volume
 * has them in courses\ itself, so they go into courses\JP\ (unified_path;
 * docs/languages.md "The installed files").
 *
 * A version the game plays also has its arcade stills written as PNG files
 * (stills.h), and gets the demo replays of the licence tests and the arcade
 * time trials over the disc's: this tree's demo\, built into the launcher
 * (write_demos), so every install and region has the same set; an installed
 * version gets the set's files that differ when the launcher starts
 * (refresh_demos), so a set that grew reaches it without a new install.
 *
 * The marker (kMarkerFile) says which disc the files are from, in "key
 * value" lines the game reads (platform/paths.h install_info): release (the
 * disc's name), boot (SYSTEM.CNF's boot file), language (the disc's own:
 * JP, US, BR - English (UK) - ...), and database, strings, unistrings (the
 * car database files its CORE.GT3 reads: each release's code names its
 * region's, database/paramdb_us.db, _eu, ...), then files, bytes, source.
 * An earlier launcher's data\ (one disc at a time) is moved into its
 * version's folder (migrate_earlier_install).  A gt3.vol installed by an
 * earlier version (pass 1 read the volume itself) can be unpacked the same
 * way without the disc image (the release from a CORE.GT3 / SYSTEM.CNF
 * beside it).  It runs on whatever thread calls it; the Listener reports
 * the progress and asks for cancellation. */
#pragma once

#include "iso9660.h"

#include <cstdint>
#include <string>
#include <vector>

namespace install {

/* The data folders' names start so ("data_US"; an earlier launcher's:
 * "data" itself). */
constexpr const wchar_t *kDataFolder = L"data";
/* Written last into the data folder: its presence says the install is whole. */
constexpr const wchar_t *kMarkerFile = L"gt3pc_install.txt";
/* The volume an earlier version installed (unpackable, then not needed). */
constexpr const wchar_t *kVolumeFile = L"gt3.vol";

/* The playable versions, and the rest (data only). */
enum class Version { us, eu, jp, data_only };
constexpr int kVersions = 3;
/* "Gran Turismo 3 A-Spec (USA)"; its data folder "data_US"; its code "US"
 * (gt3pc.ini [Game] Version). */
const wchar_t *version_name(Version v);
std::wstring version_folder(Version v);
const char *version_code(Version v);

/* What a data folder of `folder` (`data`: "data_US" ...) holds. */
enum class InstallState {
    missing,   /* no data folder */
    ok,        /* a data folder with its marker */
    damaged,   /* a data folder without the marker (an install that did not finish) */
};
InstallState install_state(const std::wstring &folder, const std::wstring &data);

/* True when the folder holds a gt3.vol of an earlier version with a RoFS
 * table of contents (the game files can be unpacked from it). */
bool has_volume(const std::wstring &folder);

/* A data folder's disc (the marker's release; an earlier launcher's marker:
 * its first line's), empty when nothing is installed there. */
std::wstring installed_release(const std::wstring &folder, const std::wstring &data);
/* Its disc's own language ("US", "BR", "JP"...; "US" for an earlier
 * launcher's install). */
std::string installed_language(const std::wstring &folder, const std::wstring &data);
/* PC (2026-11-15): the value of the marker's line "<key> <value>" of the data
 * folder at the path `data' ("" without one): a disc image's folder's
 * "image" and "volume" (prepare_disc; disc_files.h). */
std::string marker_info(const std::wstring &data, const char *key);
/* It has the menus of the language with the country code `code`
 * (menu\<code>\gt_mode301.pmb; platform/language.h). */
bool language_installed(const std::wstring &folder, const std::wstring &data, const char *code);

/* The data-only extractions in `folder`: their data folders and discs. */
struct Extraction {
    std::wstring data, release;
};
std::vector<Extraction> extractions(const std::wstring &folder);

/* An earlier launcher's data\ (with its marker) moved to its version's data
 * folder (data_US for a marker without a boot file: that launcher took the
 * US disc only), when that one is not there.  A note for the user, or
 * empty when there was nothing to move (or it could not be: the game
 * running; tried again at the next start). */
std::wstring migrate_earlier_install(const std::wstring &folder);

/* A Gran Turismo 3 engine disc. */
struct DiscInfo {
    std::string boot;        /* the boot file of SYSTEM.CNF, e.g. "SCUS_971.02" ("" without one) */
    std::string folder;      /* CORE.GT3's folder ("" the root, else a folder of the root) */
    std::wstring release;    /* its name, "Gran Turismo 3 A-Spec (Europe, SCES-50294)" */
    Version version = Version::data_only;
    std::wstring data;       /* its data folder: "data_EU", "data_SCES_508.58" */
    iso9660::Entry core;     /* CORE.GT3 */
    iso9660::Entry volume;   /* GT3.VOL, or the folder's other RoFS volume */
};

/* Checks that `image` is a Gran Turismo 3 engine disc and that its volume
 * is whole and starts with a RoFS table of contents.  False with a message
 * for the user otherwise; `wrong_disc` tells a different game apart from a
 * damaged image. */
bool check_disc(const iso9660::Image &image, DiscInfo *info, std::wstring *message, bool *wrong_disc);

class Listener {
public:
    virtual ~Listener() = default;
    virtual void phase(const std::wstring &text) { (void)text; }   /* "Unpacking the game files..." */
    virtual void progress(uint64_t done, uint64_t total) { (void)done; (void)total; }
    virtual bool cancel_requested() { return false; }
};

enum class Result {
    ok,
    cancelled,
    wrong_disc,  /* not a Gran Turismo 3 engine disc */
    failed,      /* unreadable image, no space, write error... */
};

/* Unpacks the volume of the image at `iso_path` into its data folder of
 * `folder` (created when missing).  `message` gets the reason for anything
 * but Result::ok, or a one-line summary on success. */
Result install_from_iso(const std::wstring &iso_path, const std::wstring &folder, Listener &listener,
                        std::wstring *message);

/* The same from the volume file `volume_path` (an earlier version's
 * gt3.vol). */
Result install_from_volume(const std::wstring &volume_path, const std::wstring &folder, Listener &listener,
                           std::wstring *message);

/* The demo replays built into the launcher (this tree's demo\) written into
 * the installed data folder `data` (its full path) where a file is missing
 * or differs from the launcher's, over the disc's copies too (<name>_*): the
 * launcher does this for every installed version when it starts.  *written:
 * how many demos had a file written (0: the install already had the set);
 * false with the reason in *message when one could not be written. */
bool refresh_demos(const std::wstring &data, int *written, std::wstring *message);

/* ---- playing straight from a disc image --------------------------------------
 * PC (2026-11-14, user request: "implement direct .iso loading for the Play
 * from disc... feature").  The game reads a version's disc image in place
 * (src/platform/disc_image.h): its data folder is then a small one of the
 * image's own, <folder>\discs\<boot file>_<id> (id: the CRC-32 of its
 * volume's table of contents, so two discs of one boot file - a retail disc
 * and a prototype - have a folder each, and an image moved keeps its own),
 * holding the marker - an install's lines, then "image <its path>" (UTF-8)
 * and "volume <its volume in the image>" - and the launcher's demo replays
 * (write_demos), nothing of the disc's.  The game reads a file of that folder
 * in place of the disc's of the same path, so it is also where a changed
 * file of that disc goes.  A version's variant - its number under other
 * letters, a prototype's SCEF_502.94 for Europe's SCES_502.94 - is played as
 * that version (the marker's boot line the version's, "disc <its own>"
 * after it); any other Gran Turismo 3 engine disc is refused, as the Play tab
 * has no Play for a data-only extraction. */
constexpr const wchar_t *kDiscsFolder = L"discs";

struct DiscFolder {
    std::wstring name;      /* its folder in discs\: "SCUS_971.02_5F3A9C10" */
    std::wstring release;   /* "Gran Turismo 3 A-Spec (USA, SCUS-97102)" */
    Version version = Version::data_only;
};

/* Checks the image at `iso_path` (check_disc; a disc the game does not play
 * is Result::wrong_disc) and readies its data folder in `folder` - made, its
 * demo replays written where they are missing or differ, its marker written
 * last, with the image's path as it is now.  Quick: nothing of the volume is
 * copied.  `message`: the reason for anything but Result::ok. */
Result prepare_disc(const std::wstring &iso_path, const std::wstring &folder, DiscFolder *out, std::wstring *message);

} // namespace install
