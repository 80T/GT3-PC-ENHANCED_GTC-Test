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

} // namespace install
