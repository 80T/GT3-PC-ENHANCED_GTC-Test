/* pc_param_db.cpp - PC: the database folder from its SQLite databases (see
 * pc_param_db.h). */
#include "libc.h"
#include "game/app/pc_param_db.h"
#include "game/app/pc_data_file.h"
#include "platform/host_file.h"
#include "platform/language.h"
#include "platform/log.h"
#include "platform/param_db.h"

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

const char kCommonPath[] = "database/common.sqlite3";

bool data_file_present(const char *path)
{
    return host_file_present(("data/" + std::string(path)).c_str()) != 0;
}

/* The warnings of a database, logged (the first 40). */
void log_warnings(const char *what, const std::vector<std::string> &warnings)
{
    for (size_t i = 0; i < warnings.size() && i < 40; i++)
        printf("PC: %s: %s\n", what, warnings[i].c_str());
    if (warnings.size() > 40)
        printf("PC: %s: %d more warnings\n", what, (int)(warnings.size() - 40));
}

/* The databases' files, built at the first call. */
std::once_flag g_regionOnce, g_commonOnce;
paramdb::RegionImages g_region;
paramdb::CommonFiles g_common;
bool g_haveRegion = false, g_haveCommon = false;

void build_region()
{
    const std::string path = language::param_database();
    if (path.empty())
        return;
    std::string bytes, error;
    std::vector<std::string> warnings;
    if (!pc_data_file_read(path, &bytes))
        error = "it cannot be read";
    else if (paramdb::read_region(bytes.data(), bytes.size(), &g_region, &warnings, &error))
        g_haveRegion = true;
    if (!g_haveRegion) {
        if (!data_file_present(language::car_database(0)))
            pclog::fatal("The car database %s: %s.", path.c_str(), error.c_str());
        printf("PC: car database: %s: %s; %s is read\n", path.c_str(), error.c_str(), language::car_database(0));
        return;
    }
    printf("PC: car database from %s: %u + %u + %u bytes%s\n", path.c_str(), (unsigned)g_region.db.size(),
           (unsigned)g_region.str.size(), (unsigned)g_region.ustr.size(),
           warnings.empty() ? "" : " (warnings below)");
    log_warnings("car database", warnings);
}

void build_common()
{
    std::string bytes, error;
    if (!pc_data_file_read(kCommonPath, &bytes))
        return;
    std::vector<std::string> warnings;
    if (!paramdb::read_common(bytes.data(), bytes.size(), &g_common, &warnings, &error)) {
        if (!data_file_present("database/carcolor.db"))
            pclog::fatal("The shared database %s: %s.", kCommonPath, error.c_str());
        printf("PC: shared database: %s: %s; the .db files are read\n", kCommonPath, error.c_str());
        return;
    }
    g_haveCommon = true;
    printf("PC: colours and race modes from %s%s\n", kCommonPath, warnings.empty() ? "" : " (warnings below)");
    log_warnings("shared database", warnings);
}

/* A copy of images in a 64-byte-aligned game-heap buffer of their size
 * rounded up to 64 (as CarDataBase::readFile's), each at the end of the
 * one before rounded up to 64. */
void *copy_out(const std::vector<uint8_t> &a, const std::vector<uint8_t> *b = nullptr)
{
    const size_t at = b != nullptr ? (a.size() + 0x3F) & ~(size_t)0x3F : a.size();
    const size_t used = at + (b != nullptr ? b->size() : 0);
    const size_t size = (used + 0x3F) & ~(size_t)0x3F;
    u8 *buf = (u8 *)memalign(0x40, size);
    if (buf == NULL)
        return NULL;
    std::memset(buf, 0, size);
    std::memcpy(buf, a.data(), a.size());
    if (b != nullptr)
        std::memcpy(buf + at, b->data(), b->size());
    return buf;
}

/* An extra column's value (pc_param_db_number / _text). */
const paramdb::Value *extra(const char *table, u64 code, const char *column)
{
    std::call_once(g_regionOnce, build_region);
    std::call_once(g_commonOnce, build_common);
    for (const paramdb::Extras *extras : {&g_region.extras, &g_common.extras})
        for (const paramdb::ExtraTable &t : *extras) {
            if (_stricmp(t.name.c_str(), table) != 0)
                continue;
            for (size_t c = 0; c < t.columns.size(); c++) {
                if (_stricmp(t.columns[c].c_str(), column) != 0)
                    continue;
                auto it = t.rows.find(code);
                return it != t.rows.end() ? &it->second[c] : nullptr;
            }
        }
    return nullptr;
}

} // namespace

void *pc_param_db_file(int kind)
{
    std::call_once(g_regionOnce, build_region);
    if (!g_haveRegion)
        return NULL;
    return copy_out(kind == 1 ? g_region.str : kind == 2 ? g_region.ustr : g_region.db);
}

void *pc_common_db_file(int which)
{
    std::call_once(g_commonOnce, build_common);
    if (!g_haveCommon)
        return NULL;
    switch (which) {
    case PC_COMMON_CAR_COLORS: return copy_out(g_common.carColor, &g_common.carColorStrings);
    case PC_COMMON_RACE_MODES: return copy_out(g_common.raceModes);
    case PC_COMMON_RACE_DETAILS: return copy_out(g_common.raceDetails);
    }
    return NULL;
}

void *pc_realtime_db_file(void)
{
    static const char kPath[] = "text/realtime.sqlite3";
    std::string bytes, error;
    if (!pc_data_file_read(kPath, &bytes))
        return NULL;
    std::vector<uint8_t> image;
    std::vector<std::string> warnings;
    if (!paramdb::read_texts(bytes.data(), bytes.size(), &image, &warnings, &error)) {
        if (!data_file_present("text/realtime.dat"))
            pclog::fatal("The texts' database %s: %s.", kPath, error.c_str());
        printf("PC: texts: %s: %s; text/realtime.dat is read\n", kPath, error.c_str());
        return NULL;
    }
    printf("PC: texts from %s: %u bytes%s\n", kPath, (unsigned)image.size(), warnings.empty() ? "" : " (warnings below)");
    log_warnings("texts", warnings);
    return copy_out(image);
}

int pc_param_db_number(const char *table, u64 code, const char *column, double *value)
{
    const paramdb::Value *v = extra(table, code, column);
    if (v == nullptr || (v->kind != paramdb::Value::Integer && v->kind != paramdb::Value::Real))
        return 0;
    *value = v->r;
    return 1;
}

const char *pc_param_db_text(const char *table, u64 code, const char *column)
{
    const paramdb::Value *v = extra(table, code, column);
    return v != nullptr && v->kind == paramdb::Value::Text ? v->s.c_str() : NULL;
}
