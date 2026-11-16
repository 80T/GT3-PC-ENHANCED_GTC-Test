/* databases.h - the database folder and text\realtime.dat converted for the
 * PC (docs/database.md).
 *
 * The car database of each region in a data folder's database\ - paramdb<r>.db,
 * paramstr<r>.db, paramunistr<r>.db and the labels .id_db_idx<r>.db /
 * .id_db_str<r>.db - is written out as the SQLite database
 * paramdb<r>.sqlite3, the colours and race modes (carcolor.db / .sdb,
 * racemode.db, racedetail.db) as common.sqlite3, and text\realtime.dat (the
 * texts the game prints in each language) as text\realtime.sqlite3
 * (platform/param_db.h), which the game reads instead
 * (game/app/pc_param_db.h).  Each database is read
 * back and checked first - the game must get from it what it got from the
 * files - and only then are the files deleted.  A database already there
 * (edited, a mod's) is never replaced, and its files are left alone.  An
 * install converts as one of its last steps; the launcher converts an
 * installed version that still needs it when it starts. */
#pragma once

#include <string>
#include <vector>

namespace databases {

/* The data folder has database files without their database, or a
 * common.sqlite3 whose race display rows have no names yet (the first
 * conversion's, 2026-10-26: platform/param_db.h name_race_rows). */
bool needs_conversion(const std::wstring &data);

/* Writes each such database (as <name>.sqlite3.part, then put in place) and
 * deletes its files; names common.sqlite3's race display rows (on a copy).
 * message: a one-line summary, with what could not be converted; done /
 * problems / updated (if given): the databases written
 * ("database\paramdb_us", "text\realtime"), those that could not be
 * ("database\paramdb_us: why"), the databases changed ("database\common:
 * 229 race display rows named"); false when one could not be (the rest
 * were; its files stay, which the game reads). */
bool convert(const std::wstring &data, std::wstring *message, std::vector<std::wstring> *done = nullptr,
             std::vector<std::wstring> *problems = nullptr, std::vector<std::wstring> *updated = nullptr);

} // namespace databases
