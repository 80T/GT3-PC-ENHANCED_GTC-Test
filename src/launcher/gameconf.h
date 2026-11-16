/* gameconf.h - the product configs converted for the PC (docs/modding.md
 * "product.gcf").
 *
 * Each gameconf\<config>.gcf of a data folder is written out as the SQLite
 * database gameconf\<config>.sqlite3 beside it, a table a section
 * (platform/gcf_tables.h), which the game reads instead (game/app/
 * pc_product_config.h); once the database is read back and checked the .gcf
 * is deleted.  A .gcf beside its database (an earlier launcher kept them) is
 * deleted when the database reads as it, or when it is the file the
 * database was made of (older than it: the database edited since) and the
 * database has all its tables; a newer one (put there after the conversion)
 * is kept and said - the game reads the database.  A database already there
 * is never replaced.  Section types the game does not read (demoplay.gcf's
 * 3, 7, 9: a demo disc's) have no table: the database keeps such a .gcf
 * whole (gcf_file) before it goes.  An install converts as one of its last
 * steps; the launcher converts an installed version that still needs it
 * when it starts. */
#pragma once

#include <string>
#include <vector>

namespace gameconf {

/* The data folder has a gameconf\*.gcf. */
bool needs_conversion(const std::wstring &data);

/* Converts or deletes each .gcf (gameconf.h).  message: a one-line summary,
 * with what could not be done; done / problems / removed (if given): the
 * configs converted ("product"), the .gcf kept ("x.gcf: why"), the .gcf
 * deleted beside their databases; false when one is kept. */
bool convert(const std::wstring &data, std::wstring *message, std::vector<std::wstring> *done = nullptr,
             std::vector<std::wstring> *problems = nullptr, std::vector<std::wstring> *removed = nullptr);

} // namespace gameconf
