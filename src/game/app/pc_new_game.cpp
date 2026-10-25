/* pc_new_game.cpp - what a new game and a loaded one get (pc_new_game.h).
 * No original: new PC code. */
#include "game/app/pc_new_game.h"
#include "game/save/T429_game_status.h"          /* GameStatus, GarageCar, LicenseRecords */
#include "game/data/T435_product_config.h"       /* product_config, pcHighestCourseLevel */
#include "game/data/T483_car_information.h"      /* CarInformation (the probe) */
#include "game/data/T485_car_color.h"            /* g_00352D98 (the colours) */
#include "game/data/T489_car_database.h"         /* db_car_count, db_car_code */
#include "game/data/T516_country.h"              /* g_00352FD8, COUNTRY_JP */
#include "game/car/model/T446_car_model.h"       /* car_list_has */
#include "game/menu/arcade/T010_arcade_control.h" /* g_002944A8 (the arcade modes' course lists) */
#include "platform/settings.h"

/* Prices are x 100 in Japanese (yen). */
static s64 pc_price_scale(void)
{
    return g_00352FD8.value == COUNTRY_JP ? 100 : 1;
}

s64 pc_new_game_money(s64 initial)
{
    s64 money = initial;
    if (settings::starting_credits() != 0)
        money = (s64)settings::starting_credits() * pc_price_scale();
    if (settings::debug_fill_money())
        money = (s64)100000000 * pc_price_scale();
    return money;
}

/* The tables of a car's stock parts, in CarParts order, and whether the game
 * reads the part whatever its code (setCar's reset* and the spec sheet,
 * T482: these have a record for every car) or only for a code other than 0
 * (the tuning parts a car may lack). */
static const struct { int table; int always; const char *name; } kCarPartTables[27] = {
    { CARDB_BRAKE, 1, "brake" },                { CARDB_BRAKE_CONTROLLER, 1, "brake controller" },
    { CARDB_STEER, 1, "steering" },             { CARDB_CHASSIS, 1, "chassis" },
    { CARDB_LIGHTWEIGHT, 0, "lightweight" },    { CARDB_RACING_MODIFY, 1, "racing modify" },
    { CARDB_ENGINE, 1, "engine" },              { CARDB_PORT_POLISH, 0, "port polish" },
    { CARDB_ENGINE_BALANCE, 0, "engine balance" }, { CARDB_DISPLACEMENT, 0, "displacement" },
    { CARDB_COMPUTER, 0, "computer" },          { CARDB_NA_TUNE, 0, "NA tune" },
    { CARDB_TURBINE, 1, "turbine" },            { CARDB_DRIVETRAIN, 1, "drivetrain" },
    { CARDB_FLYWHEEL, 0, "flywheel" },          { CARDB_CLUTCH, 0, "clutch" },
    { CARDB_PROPELLER_SHAFT, 0, "propeller shaft" }, { CARDB_LSD, 1, "LSD" },
    { CARDB_GEAR, 1, "gear" },                  { CARDB_SUSPENSION, 1, "suspension" },
    { CARDB_INTERCOOLER, 0, "intercooler" },    { CARDB_MUFFLER, 0, "muffler" },
    { CARDB_FRONT_TIRE, 1, "front tyre" },      { CARDB_REAR_TIRE, 1, "rear tyre" },
    { CARDB_ASCC, 1, "ASCC" },                  { CARDB_TCSC, 1, "TCSC" },
    { CARDB_WHEEL, 0, "wheel" },
};

/* Whether every stock part of car `code' is in the database: the game reads
 * the part records without a test, so a modded database's car naming a part
 * that is not there would crash its loadCar.  The parts are the preset's when
 * the car has one (setCar takes them first), else the car record's.  A broken
 * car is logged with its part. */
static int car_parts_present(CarInformation &probe, u64 code)
{
    const PresetCarRecord *preset = (const PresetCarRecord *)probe.getRecord(CARDB_PRESET_CAR, code);
    const CarRecord *car = (const CarRecord *)probe.getRecord(CARDB_CAR, code);
    const CarParts *parts = preset != NULL ? &preset->parts : car != NULL ? &car->parts : NULL;
    if (parts == NULL)
        return 0;
    const u64 *codes = (const u64 *)parts;
    for (int p = 0; p < 27; p++) {
        if (probe.getRecord(kCarPartTables[p].table, codes[p]) != NULL)
            continue;
        if (codes[p] == 0 && !kCarPartTables[p].always)
            continue;                       /* (no such part on this car) */
        const char *name = db_car_short_name((s64)code);
        printf("PC: new game: car %s (%016llX) left out: its %s part %016llX is not in the database\n",
               name != NULL ? name : "?", (unsigned long long)code, kCarPartTables[p].name,
               (unsigned long long)codes[p]);
        return 0;
    }
    return 1;
}

/* One of every car of the database with a model, in each of its colours. */
static void fill_garage(gt2k_game::GameStatus *gs)
{
    int cars = 0, added = 0, broken = 0;
    const int n = db_car_count();
    for (int i = 0; i < n; i++) {
        const u64 code = (u64)db_car_code(i);
        u64 model;
        {
            CarInformation probe;
            if (car_parts_present(probe, code) == 0) {
                broken++;
                continue;
            }
            if (probe.loadCar(code) == 0)
                continue;
            model = probe.getModelCode();
        }
        if (!car_list_has(model))
            continue;                       /* (no model in the data: not a car of the game) */
        int colors = g_00352D98.getNumColors(model);
        if (colors <= 0)
            colors = 1;
        cars++;
        for (int c = 0; c < colors; c++) {
            GarageCar *car = gs->findFreeCar();
            if (car == NULL) {
                printf("PC: new game: the garage is full after %d cars\n", added);
                return;
            }
            car->loadCar(code);
            car->valid = 1;
            car->setColorIndex(c);
            gs->addCar(car);
            added++;
        }
    }
    printf("PC: new game: the garage filled: %d cars of the database's %d, %d in all their colours"
           " (%d left out: a part not in the database)\n", cars, n, added, broken);
}

static void unlocks(gt2k_game::GameStatus *gs)
{
    if (settings::debug_unlock_arcade() && product_config() != NULL) {
        int highest = gs->arcadeLevel;
        for (int m = 0; m < (int)(sizeof(g_002944A8) / sizeof(g_002944A8[0])); m++) {
            const int level = product_config()->pcHighestCourseLevel(g_002944A8[m]);
            if (level > highest)
                highest = level;
        }
        if (highest > gs->arcadeLevel) {
            printf("PC: the arcade courses unlocked: progress level %d -> %d\n", gs->arcadeLevel, highest);
            gs->arcadeLevel = highest;
        }
    }
    if (settings::debug_unlock_licenses()) {
        for (int lic = 0; lic < LICENSE_COUNT; lic++)
            gs->licences.passAll(lic);
        printf("PC: every licence unlocked\n");
    }
}

void pc_new_game(gt2k_game::GameStatus *gs)
{
    if (settings::debug_fill_garage())
        fill_garage(gs);
    unlocks(gs);
}

void pc_loaded_game(gt2k_game::GameStatus *gs)
{
    if (gs != NULL)
        unlocks(gs);
}
