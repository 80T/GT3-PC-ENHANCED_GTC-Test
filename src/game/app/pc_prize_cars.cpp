/* pc_prize_cars.cpp - the prize car randomizer (pc_prize_cars.h).  No
 * original: new PC code. */
#include "game/app/pc_prize_cars.h"
#include "game/car/model/T446_car_model.h"              /* car_model_path */
#include "game/data/T481_car_data_base.h"               /* g_002EBD88, nameToCode */
#include "game/data/T482_car_equipments.h"              /* CarRecord, CarEquipments, CARDB_* */
#include "game/data/T489_car_database.h"                /* db_car_is_locked */
#include "game/menu/gtmode/T036_garage_car.h"           /* GarageCar (getCarCode) */
#include "game/race/core/T414_race_random.h"            /* raceRandomFloat */
#include "game/save/T429_game_status.h"                 /* getGameStatus (the garage) */
#include "engine/io/T502_file_resolve.h"                /* file_resolve */
#include "engine/io/T503_vol_thread.h"                  /* vol_device_object */
#include "platform/host_file.h"                         /* host_file_quiet */
#include "platform/param_db.h"                          /* the record layouts */
#include "platform/settings.h"

#include <stdio.h>
#include <string.h>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

enum { kMaxCars = 4 };                                  /* a set's (CAR_GET_MAX_CARS) */

/* The table of each CarParts slot (CarRecord + 0x08, 8 bytes a slot):
 * T482_car_equipments.h's CarParts. */
const int kPartTables[] = {
    CARDB_BRAKE, CARDB_BRAKE_CONTROLLER, CARDB_STEER, CARDB_CHASSIS, CARDB_LIGHTWEIGHT, CARDB_RACING_MODIFY,
    CARDB_ENGINE, CARDB_PORT_POLISH, CARDB_ENGINE_BALANCE, CARDB_DISPLACEMENT, CARDB_COMPUTER, CARDB_NA_TUNE,
    CARDB_TURBINE, CARDB_DRIVETRAIN, CARDB_FLYWHEEL, CARDB_CLUTCH, CARDB_PROPELLER_SHAFT, CARDB_LSD, CARDB_GEAR,
    CARDB_SUSPENSION, CARDB_INTERCOOLER, CARDB_MUFFLER, CARDB_FRONT_TIRE, CARDB_REAR_TIRE, CARDB_ASCC, CARDB_TCSC,
    CARDB_WHEEL};
constexpr int kPartSlots = sizeof(kPartTables) / sizeof(kPartTables[0]);
constexpr u32 kPartsAt = 0x08;                          /* CarRecord.parts */

/* An entry: a CAR row the randomizer can give. */
struct Entry {
    u64 code;
    std::string label;
    int car;                                            /* its car (Pool::cars) */
};

/* A car: its entries (colours aside), the unique one first. */
struct Car {
    std::vector<int> entries;
};

struct Pool {
    bool built = false;
    std::vector<Entry> entries;
    std::vector<Car> cars;
    std::unordered_map<u64, int> carOf;                 /* every CAR row's code -> its car (unavailable rows too) */
};

Pool g_pool;

/* The screen's draws: the labels of each set's cars (NULL: not drawn). */
std::vector<std::vector<const char *>> g_drawn;
std::unordered_set<int> g_drawnCars, g_drawnEntries;
bool g_screenOn = false;

const paramdb::TableDef *table_def(int table)
{
    for (const paramdb::TableDef &t : paramdb::param_tables())
        if (t.index == table)
            return &t;
    return nullptr;
}

/* A string of the database (Str: paramstr, UStr: paramunistr) as its bytes
 * up to its NUL; "" when the index is past the table. */
std::string db_text(paramdb::Type type, u16 index)
{
    CarDataBase &db = g_002EBD88;
    StringsDataBase *s = type == paramdb::Type::Str ? db.strings : db.uniStrings;
    if (s == NULL || index >= (u32)s->numOfElements)
        return std::string();
    const char *p = type == paramdb::Type::Str ? db.getString(index) : db.getMultiByteString(index);
    const u16 size = type == paramdb::Type::Str ? db.getStringSize(index) : db.getMultiByteStringSize(index);
    return std::string(p, strnlen(p, size));
}

/* The bytes of a record's columns, as `def' lays them out, but its label,
 * its pads and the columns `skip' names: texts by their bytes (with their
 * length), the rest as stored. */
void add_columns(std::string *sig, const u8 *rec, const paramdb::TableDef &def,
                 bool (*skip)(const paramdb::Column &))
{
    for (int c = 0; c < def.count; c++) {
        const paramdb::Column &col = def.columns[c];
        if (col.name == nullptr || col.type == paramdb::Type::Label || col.type == paramdb::Type::Pad || skip(col))
            continue;
        if (col.type == paramdb::Type::Str || col.type == paramdb::Type::UStr) {
            u16 index;
            memcpy(&index, rec + col.offset, 2);
            const std::string text = db_text(col.type, index);
            const u32 n = (u32)text.size();
            sig->append((const char *)&n, 4);
            sig->append(text);
        } else {
            sig->append((const char *)rec + col.offset, col.size);
        }
    }
}

/* A part record's columns that do not make the car: the car it belongs to,
 * its body model (the colours). */
bool skip_part(const paramdb::Column &col)
{
    return col.type == paramdb::Type::Id && (strcmp(col.name, "Car") == 0 || strcmp(col.name, "Model") == 0);
}

/* A CAR row's columns that do not make the car: its label as text, its
 * year, shop flags and region lock; the parts (by their contents: below). */
bool skip_car(const paramdb::Column &col)
{
    return col.type == paramdb::Type::Id || strcmp(col.name, "ShortName") == 0 || strcmp(col.name, "Year") == 0 ||
           strcmp(col.name, "ShopFlags") == 0 || strcmp(col.name, "RegionLock") == 0;
}

/* What makes a CAR row's car: its columns and its parts' contents. */
std::string car_signature(const u8 *rec, const paramdb::TableDef &carDef)
{
    std::string sig;
    add_columns(&sig, rec, carDef, skip_car);
    for (int s = 0; s < kPartSlots; s++) {
        u64 code;
        memcpy(&code, rec + kPartsAt + s * 8, 8);
        const paramdb::TableDef *def = table_def(kPartTables[s]);
        const u8 *part = code != 0 ? (const u8 *)g_002EBD88.getRecord(kPartTables[s], code) : NULL;
        sig.push_back(part != NULL ? 'P' : 'C');
        if (part == NULL || def == nullptr)
            sig.append((const char *)&code, 8);         /* none, or not in its table: the code itself */
        else
            add_columns(&sig, part, *def, skip_part);
    }
    return sig;
}

/* A file of the volume is there (quietly: a probe). */
bool file_there(const char *path)
{
    FileLoc loc;
    host_file_quiet(1);
    const int found = file_resolve(path, &loc, &vol_device_object);
    host_file_quiet(0);
    return found != -1;
}

/* Why a CAR row cannot be given, NULL when it can: the region, the model
 * files (the day model the races load, the menu model the prize screen
 * shows). */
const char *unavailable(u64 code)
{
    if (db_car_is_locked((s64)code))
        return "region";
    CarEquipments equip;
    if (equip.setCar(code) != 1)
        return "not in the database";
    const u64 model = equip.getModelCode();
    if (!car_list_has(model))
        return "no day model";
    char path[0x100];
    car_model_path(model, path, 0);
    if (!file_there(path))
        return "no day model file";
    car_model_path(model, path, 2);
    if (!file_there(path))
        return "no menu model file";
    return NULL;
}

void build_pool()
{
    Pool &p = g_pool;
    p.built = true;
    const paramdb::TableDef *carDef = table_def(CARDB_CAR);
    DataBlock *block = g_002EBD88.db != NULL ? g_002EBD88.getDataBlock(CARDB_CAR) : NULL;
    if (carDef == nullptr || block == NULL || block->elementSize < carDef->rowSize) {
        printf("PC: prize car randomizer: no car table\n");
        return;
    }
    const int rows = block->numElements;
    std::unordered_map<std::string, int> carOfSig;
    int unlabelled = 0, out = 0;
    for (int i = 0; i < rows; i++) {
        const u8 *rec = (const u8 *)block->getElement(i);
        const CarRecord *car = (const CarRecord *)rec;
        const int c = carOfSig.emplace(car_signature(rec, *carDef), (int)p.cars.size()).first->second;
        if (c == (int)p.cars.size())
            p.cars.emplace_back();
        p.carOf[car->code] = c;
        const std::string label = db_text(paramdb::Type::Str, car->shortName);
        if (label.empty() || nameToCode(label.c_str()) != car->code) {
            unlabelled++;                               /* the garage takes a car by its label */
            continue;
        }
        if (const char *why = unavailable(car->code)) {
            if (strcmp(why, "region") != 0)
                printf("PC: prize car randomizer: %s left out (%s)\n", label.c_str(), why);
            out++;
            continue;
        }
        p.cars[c].entries.push_back((int)p.entries.size());
        p.entries.push_back({car->code, label, c});
    }
    /* each car's unique entry first: its shortest label (the base car: X
     * before X_ext), then the database's order */
    int cars = 0, duplicates = 0;
    for (Car &car : p.cars) {
        if (car.entries.empty())
            continue;
        size_t best = 0;
        for (size_t k = 1; k < car.entries.size(); k++)
            if (p.entries[car.entries[k]].label.size() < p.entries[car.entries[best]].label.size())
                best = k;
        std::swap(car.entries[0], car.entries[best]);
        cars++;
        duplicates += (int)car.entries.size() - 1;
    }
    printf("PC: prize car randomizer: %d cars to give (%d of the %d CAR rows, %d of them duplicates: the same "
           "car in other colours); %d rows not available here, %d without a label\n",
           cars, (int)p.entries.size(), rows, duplicates, out, unlabelled);
}

/* A random index below n (n > 0). */
int random_below(int n)
{
    int k = (int)(raceRandomFloat(1) * (f32)n);
    return k < 0 ? 0 : k >= n ? n - 1 : k;
}

/* One draw (pc_prize_cars.h's order); the entry's index, -1 if none.
 * owned: the cars the garage has. */
int draw(const std::unordered_set<int> &owned, const char **how)
{
    const Pool &p = g_pool;
    std::vector<int> pick;
    for (size_t c = 0; c < p.cars.size(); c++)
        if (!p.cars[c].entries.empty() && owned.count((int)c) == 0 && g_drawnCars.count((int)c) == 0)
            pick.push_back(p.cars[c].entries[0]);
    *how = "unique, not in the garage";
    if (pick.empty()) {
        for (size_t e = 0; e < p.entries.size(); e++)
            if (g_drawnCars.count(p.entries[e].car) == 0)
                pick.push_back((int)e);
        *how = "fallback: in the garage, or a duplicate";
    }
    if (pick.empty()) {
        for (size_t e = 0; e < p.entries.size(); e++)
            if (g_drawnEntries.count((int)e) == 0)
                pick.push_back((int)e);
        *how = "fallback: a car drawn on this screen";
    }
    if (pick.empty()) {
        for (size_t e = 0; e < p.entries.size(); e++)
            pick.push_back((int)e);
        *how = "fallback: an entry drawn on this screen";
    }
    if (pick.empty())
        return -1;
    return pick[random_below((int)pick.size())];
}

} // namespace

int pc_prize_draw_begin(void)
{
    g_drawn.clear();
    g_drawnCars.clear();
    g_drawnEntries.clear();
    g_screenOn = false;
    if (!settings::prize_car_randomizer())
        return 0;
    if (!g_pool.built)
        build_pool();
    if (g_pool.entries.empty()) {
        printf("PC: prize car randomizer: no car to give; the prize sets' cars\n");
        return 0;
    }
    g_screenOn = true;
    return 1;
}

void pc_prize_draw_set(int i, const char *set, int count)
{
    if (!g_screenOn || i < 0)
        return;
    if ((int)g_drawn.size() <= i)
        g_drawn.resize(i + 1);
    std::unordered_set<int> owned;
    gt2k_game::GameStatus *status = getGameStatus();
    for (int k = 0; k < status->getCarNum(); k++) {
        GarageCar *car = status->getCar(k);
        if (car == NULL)
            continue;
        auto c = g_pool.carOf.find(car->getCarCode());
        if (c != g_pool.carOf.end())
            owned.insert(c->second);
    }
    std::vector<const char *> &cars = g_drawn[i];
    cars.clear();
    for (int j = 0; j < count && j < kMaxCars; j++) {
        const char *how = "";
        const int e = draw(owned, &how);
        if (e < 0)
            break;
        const Entry &entry = g_pool.entries[e];
        g_drawnCars.insert(entry.car);
        g_drawnEntries.insert(e);
        cars.push_back(entry.label.c_str());
        printf("PC: prize car randomizer: %s car %d of %d: %s (%s)\n", set != NULL ? set : "?", j + 1, count,
               entry.label.c_str(), how);
    }
}

const char *pc_prize_car(int i, int j)
{
    if (!g_screenOn || i < 0 || i >= (int)g_drawn.size() || j < 0 || j >= (int)g_drawn[i].size())
        return NULL;
    return g_drawn[i][j];
}
