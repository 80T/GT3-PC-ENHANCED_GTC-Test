/* pc_data_file.h - PC: a database of the data folder read whole
 * (docs/database.md "While an editor has it open").  No original: new PC code.
 *
 * The game reads a database's bytes and parses them in memory.  An SQLite
 * database (<name>.sqlite3) is read through SQLite itself (platform/
 * sqlite_file.h), under its locks: an SQLite editor connected to it does not
 * hold it up any more (PC 2026-11-07: before, the plain read's sharing
 * refused the editor's writable handle, and the user had to disconnect before
 * every start); what it reads is the last committed state, so a change still
 * open in the editor is taken at the next start once committed.  Only a
 * commit under way locks it, and that is waited for.
 *
 * Any other file (a .gcf, a still's PNG, a .db) is read as a plain file, whose
 * open refuses a file another program holds for writing.  Such a file is
 * there, and only waiting gets it: the file it replaced (realtime.dat, a .db,
 * a .gcf) is gone, and the game would wait for that for ever.  So the read
 * waits here, a pop-up (platform/file_alert.h) naming the file, closed when
 * it is free. */
#ifndef GAME_APP_PC_DATA_FILE_H
#define GAME_APP_PC_DATA_FILE_H

#include <string>

/* The file at `path' (of the data folder, or a mod's: "database/common.sqlite3")
 * into *out; false when it is not there, quietly (a probe: the older file is
 * read instead) or cannot be read (said in the log).  Waits, however long,
 * while another program holds it (a plain file) or has a transaction under
 * way on it (an SQLite database).  On a game thread. */
bool pc_data_file_read(const std::string &path, std::string *out);

#endif /* GAME_APP_PC_DATA_FILE_H */
