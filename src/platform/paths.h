/* paths.h - where GT3 PC finds its files.
 *
 * The game folder is the folder of the executable.  The game files are
 * loose in a data folder of it, one per version - data_US (the USA disc and
 * the bundle), data_EU, data_JP - which the launcher unpacks there from the
 * user's disc image (the volume GT3.VOL, src/launcher/install.h) and marks
 * whole with gt3pc_install.txt, written last.  The game reads them as the
 * original's loose host:data/ files (platform/host_file.h: "data/" is the
 * data folder).  The folder: --data <folder> (the launcher's Play: the data
 * folder itself, or a folder holding data\), else [Game] Version's (the
 * launcher's last Play), else data_US, data_EU, data_JP, an earlier
 * launcher's data\, and for development the extracted disc of the source
 * tree (pc_enhanced/gt3_disc/files/data, unpacked by the launcher's
 * --unpack). */
#pragma once

#include <string>

namespace paths {

/* The folder of the game files, and the file the launcher writes into it
 * last (a data folder without it is an install that did not finish). */
constexpr const char *kDataFolder = "data";
constexpr const char *kInstallMarker = "gt3pc_install.txt";

/* Resolves the folders from the command line; false (with a message in
 * the log) when no installed data folder is found. */
bool init(int argc, char **argv);

const std::string &game_dir();      /* the executable's folder, with a trailing '\' */
const std::string &data_folder();   /* the data folder (data_US\ ...), with a trailing '\' */
const std::string &data_dir();      /* the folder holding it, with a trailing '\' */
/* A line of the install marker, "<key> <value>": the value ("language BR"
 * -> "BR"; the launcher's keys: src/launcher/install.h), or "" when it has
 * no such line (an earlier launcher's marker: Gran Turismo 3 A-Spec (USA)). */
std::string install_info(const char *key);
/* The memory cards are game_dir() + "memcard1\" and "memcard2\"
 * (platform/memcard.cpp). */

} // namespace paths
