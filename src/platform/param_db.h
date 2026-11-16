/* param_db.h - GT3's database folder as SQLite (docs/database.md).
 *
 * The parameter database of a region - paramdb<r>.db (36 tables of records,
 * each sorted by the 64-bit code of its row's label), paramstr<r>.db and
 * paramunistr<r>.db (the strings the records point at), .id_db_idx<r>.db /
 * .id_db_str<r>.db (the labels behind the codes: the game does not read
 * them) - becomes one database, database/paramdb<r>.sqlite3 (<r>: ""
 * Japan, "_us", "_eu"): a table a record table, a row a record, its label in
 * the column Label, the strings and the codes of other rows in their columns
 * as text.  What the files derived from that - the codes, the order, the
 * string files, the label files - the game derives again when it loads the
 * database (read_region): there is nothing else to keep in step.  The
 * colours (carcolor.db / .sdb) and the race modes (racemode.db,
 * racedetail.db), one set for every region, become database/common.sqlite3,
 * the texts the game prints in each language (text/realtime.dat)
 * text/realtime.sqlite3.  The files' magic and version, which the game does
 * not read, are kept in the database's header (application_id,
 * user_version).
 *
 * The record layouts are the schema's (param_schema.cpp: a column's name,
 * type and place in the record, and its meaning, which the database's schema
 * keeps as comments).  A column or table the schema does not know is left
 * alone: a database may carry more, which the game's code can read by a
 * row's code (Extras; game/app/pc_param_db.h).  Shared by the game and the
 * launcher (its conversion); standard C++, SQLite, Windows' code page 20932
 * for the EUC-JP strings. */
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace paramdb {

/* ---- the schema ---------------------------------------------------------- */

enum class Type : uint8_t {
    Label,          /* u64 at 0: the row's code, the hash of its label (text) */
    Id,             /* u64: another row's code, as its label (text; empty: 0) */
    U8, S8, U16, S16, U32, S32, F32,
    Str,            /* u16: a string of paramstr (text) */
    UStr,           /* u16: a string of paramunistr (text, EUC-JP in the game) */
    Pad             /* bytes 0 in every record (no column) */
};

struct Column {
    const char *name;
    Type type;
    uint16_t offset;
    uint16_t size;              /* (Pad: its length; the others: their type's) */
    const char *doc;
};

struct TableDef {
    const char *name;
    int index;                  /* the table's place in paramdb (its GTDT id); -1 a file of its own */
    uint16_t rowSize;
    const char *doc;
    const Column *columns;
    int count;
};

/* paramdb's 36 tables, by index; racemode.db's and racedetail.db's
 * (param_schema.cpp). */
const std::vector<TableDef> &param_tables();
const std::vector<TableDef> &race_tables();

/* The code of a label (the game's nameToCode, T481): the sum of its bytes'
 * low 16 bits, then for each byte rotate left 7 and add it. */
uint64_t name_to_code(const std::string &label);

/* ---- what the schema does not know ------------------------------------------ */

/* A value as SQLite holds it. */
struct Value {
    enum Kind : uint8_t { Null, Integer, Real, Text, Blob } kind = Null;
    int64_t i = 0;
    double r = 0;
    std::string s;              /* Text (UTF-8), Blob */
};

/* A table's columns the schema does not know (a table it does not know: all
 * its columns but the key), by the row's code - its Label's, or its Code
 * (COLOR) - the first row of a code. */
struct ExtraTable {
    std::string name;
    std::vector<std::string> columns;
    std::unordered_map<uint64_t, std::vector<Value>> rows;
};
using Extras = std::vector<ExtraTable>;

/* ---- a region ------------------------------------------------------------ */

/* A region's files as read (any may be empty but db). */
struct RegionFiles {
    std::vector<uint8_t> db, str, ustr, idIdx, idStr;
};

/* What the game reads of a region, rebuilt from its database. */
struct RegionImages {
    std::vector<uint8_t> db, str, ustr;
    Extras extras;
};

/* The region's files as a new database at `path' (UTF-8; replaced): false
 * with the reason.  notes: what was found that a reader should know (codes
 * without a label, strings no record uses...). */
bool write_region(const RegionFiles &files, const std::string &path, std::string *error,
                  std::vector<std::string> *notes);

/* The game's files from a database's bytes; false with the reason when it
 * is not one.  warnings: tables, rows, values the game could not take
 * (empty, skipped, 0...); extra columns are not warnings. */
bool read_region(const void *bytes, size_t size, RegionImages *out, std::vector<std::string> *warnings,
                 std::string *error);

/* What the game reads from the images equals what it read from the files:
 * every table's rows (in order), every column's value, every string a row
 * points at (by its bytes).  false with the first difference. */
bool same_region(const RegionFiles &files, const RegionImages &images, std::string *why);

/* ---- the shared database (database/common.sqlite3) ------------------------ */

/* The labels of a region's label files (.id_db_idx<r>.db, .id_db_str<r>.db)
 * added to `labels' (code, label): the common database's cars are written
 * by them. */
void add_labels(const std::vector<uint8_t> &idIdx, const std::vector<uint8_t> &idStr,
                std::vector<std::pair<uint64_t, std::string>> *labels);

/* The colours and race modes, as read / as rebuilt. */
struct CommonFiles {
    std::vector<uint8_t> carColor, carColorStrings;     /* carcolor.db, carcolor.sdb */
    std::vector<uint8_t> raceModes, raceDetails;        /* racemode.db, racedetail.db */
    Extras extras;                                      /* (read_common's) */
};

/* As write_region / read_region / same_region, for the shared files. */
bool write_common(const CommonFiles &files, const std::vector<std::pair<uint64_t, std::string>> &labels,
                  const std::string &path, std::string *error, std::vector<std::string> *notes);
bool read_common(const void *bytes, size_t size, CommonFiles *out, std::vector<std::string> *warnings,
                 std::string *error);
bool same_common(const CommonFiles &files, const CommonFiles &images, std::string *why);

/* Names for the race display's rows no label is known for (the discs':
 * every RACE_DETAIL row, the RACE_MODE row of id 10) - a detail by the mode that
 * lists it, what it shows and its view ("REPLAY_GEAR_CHASE"; "UNUSED_..."
 * in no mode's list), a mode by its id ("RACE_MODE_ID10") - put in the
 * files: their codes become the names' (the records re-sorted, the modes'
 * lists with them), the names added to `labels'.  The game finds a detail
 * only through the modes' lists, and that mode only by its id: it plays as
 * before.  The number named. */
int name_race_rows(CommonFiles *files, std::vector<std::pair<uint64_t, std::string>> *labels);

/* The race display's rows of a shared database (its bytes) without a name
 * (#<code>). */
int unnamed_race_rows(const void *bytes, size_t size);

/* name_race_rows in the shared database at `path' (UTF-8), changed in place
 * by SQL - the other columns, tables and rows keep what they have - with
 * RACE_MODE's and RACE_DETAIL's schema comments made the current ones, then
 * checked: the game reads what it read but the names' codes.  named: how
 * many; false with the reason (the file is changed by then: give it a copy). */
bool name_race_database(const std::string &path, int *named, std::string *error);

/* ---- the real-time texts (text/realtime.sqlite3) ---------------------------- */

/* text/realtime.dat ("MLTX": the texts the game prints in each language,
 * T517 GT2KRealTimeText) as the database at `path' (UTF-8; replaced): a
 * row a text (its name "<table>_<key>"), a column a language (Japanese ..
 * Spanish, the game's countries 0..6; Block7...: a file's further blocks,
 * which the game does not read, kept as they are).  false with the reason. */
bool write_texts(const std::vector<uint8_t> &file, const std::string &path, std::string *error);

/* The file built from a database's bytes (the names sorted, each text a
 * slot of its longest language's size in every block, as the discs'). */
bool read_texts(const void *bytes, size_t size, std::vector<uint8_t> *out, std::vector<std::string> *warnings,
                std::string *error);

/* What the game reads of two such files is the same: every name, every
 * language's text of it. */
bool same_texts(const std::vector<uint8_t> &file, const std::vector<uint8_t> &image, std::string *why);

} // namespace paramdb
