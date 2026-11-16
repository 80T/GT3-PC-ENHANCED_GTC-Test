/* pc_param_db.h - PC: the database folder from its SQLite databases
 * (platform/param_db.h; docs/database.md).  No original: new PC code.
 *
 * database/paramdb<r>.sqlite3 (the run's: platform/language.h
 * param_database) is read in place of the region's paramdb<r>.db,
 * paramstr<r>.db and paramunistr<r>.db, database/common.sqlite3 in place of
 * carcolor.db / .sdb, racemode.db and racedetail.db, text/realtime.sqlite3
 * in place of text/realtime.dat: the files are built from them once, at the
 * first load, in their layouts, which CarDataBase (T481), CarColorDataBase
 * (T485), RaceModeDatabase (T487) and realtime_load (T517) read as ever.
 * The log: "PC: car database from database/paramdb_us.sqlite3: ..." and a
 * line for each value it could not take.  A database is read through SQLite
 * itself, so an editor connected to it does not hold the game up; only a
 * transaction under way on it is waited for (pc_data_file.h).
 *
 * A column the schema does not know (one added to a database, docs/database.md
 * "Columns of your own") is not in the records: code reads it by its row's
 * code with pc_param_db_number / pc_param_db_text. */
#ifndef GAME_APP_PC_PARAM_DB_H
#define GAME_APP_PC_PARAM_DB_H

#include "types.h"

/* File `kind' (0 paramdb, 1 paramstr, 2 paramunistr) built from the run's
 * database: a copy in a 64-byte-aligned buffer of the game heap (CarDataBase
 * frees it as the file's buffer); NULL when the run has no database (its
 * .db files are read).  A database that cannot be read: said, then its .db
 * files are read when they are there, else the game stops with the reason. */
void *pc_param_db_file(int kind);

/* The shared files built from database/common.sqlite3, as pc_param_db_file:
 * carcolor.db with carcolor.sdb after it at its size rounded up to 64 (the
 * layout CarColorDataBase::loadFiles reads them into), racemode.db,
 * racedetail.db. */
enum { PC_COMMON_CAR_COLORS, PC_COMMON_RACE_MODES, PC_COMMON_RACE_DETAILS };
void *pc_common_db_file(int which);

/* text/realtime.dat built from text/realtime.sqlite3 (the texts the game
 * prints in each language, T517), as pc_param_db_file; NULL when there is
 * none (the .dat is read). */
void *pc_realtime_db_file(void);

/* A column the schema does not know of the row of `code' in table `table'
 * ("CAR"; the shared database's too, COLOR's rows by their colour code), by
 * the column's name: 1 with its number, 0 when the database, table, column
 * or row is not there or the value is no number. */
int pc_param_db_number(const char *table, u64 code, const char *column, double *value);
/* ... its text (UTF-8), NULL when it is not there or is no text. */
const char *pc_param_db_text(const char *table, u64 code, const char *column);

#endif /* GAME_APP_PC_PARAM_DB_H */
