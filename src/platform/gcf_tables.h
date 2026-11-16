/* gcf_tables.h - the product configs (gameconf/*.gcf, T435 ProductConfig) as
 * the tables of an SQLite database, gameconf/<config>.sqlite3: read from a
 * .gcf, written as a database, read back, and built into the .gcf's layout
 * for the game (docs/modding.md "product.gcf").  Shared by the game
 * (game/app/pc_product_config.h) and the launcher (its conversion); standard
 * C++ and SQLite (cmake/sqlite.cmake).
 *
 * A .gcf: a header {count, sections} and its sections {type, data}; each
 * pointer an offset from the record that holds it (T436..T445 relocate
 * them in place).  Its tables (game/data/gcf_internal.h, the readers in
 * T435), the database's (their columns' meanings in its schema, as
 * comments):
 *   general     setting, value      type 0x0: idle_seconds (the title page's
 *               idle time), demo_time_limit (the attract demo's race, s)
 *   cars        position, category, car, race, selectable, level, race_step
 *               type 0xE: the arcade car lists by category
 *   courses     position, list, course, available, flag, level, length
 *               type 0x8: the course lists by game mode
 *   events      event, requirement  type 0x4 (sorted by the game)
 *   best_times  course, value       type 0x6: the time trial best times'
 *               courses (sorted by the game; the value is never read)
 *   services    service, price      type 0xA (sorted by the game)
 *   demos       position, race, course, shop_car, shop_car_position,
 *               requirement         type 0xC: the attract demos
 *   card_demos  position, demo, level   type 0xD: the memory card demos
 *   prizes      position, prize_set, car, color, weight, unknown   type 0xF:
 *               the prize car sets (sorted by the game), each set's cars
 * position: the rows' order (a category's / list's / set's rows, and the
 * categories and lists, in it; ties by the rows' order in the table).  A
 * table missing: its section is not in the config (as a .gcf without it). */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gcftables {

struct Car {
    std::string category, code, race;
    int selectable = 0, level = 0, raceStep = 0;
};
struct Course {
    std::string list, name;
    int available = 0, flag = 0, level = 0, length = 0;
};
struct Named {
    std::string name;
    int value = 0;
};
struct Demo {
    std::string race, course, shopCar;
    int shopCarPos = 0, requirement = 0;
};
struct PrizeCar {
    std::string set, code;
    int color = 0, weight = 0, unknown = 0;
};

/* The section types (game/data/gcf_internal.h GCF_*). */
enum Section { GENERAL = 0x0, EVENTS = 0x4, BEST_TIMES = 0x6, COURSES = 0x8, SERVICES = 0xA, DEMOS = 0xC,
               CARD_DEMOS = 0xD, CARS = 0xE, PRIZES = 0xF };

struct Config {
    std::vector<int> sections;          /* the sections it has, in its order */
    int idleSeconds = 0, demoTimeLimit = 0;
    std::vector<Car> cars;
    std::vector<Course> courses;
    std::vector<Named> events, bestTimes, services, cardDemos;
    std::vector<Demo> demos;
    std::vector<PrizeCar> prizes;
    bool has(int section) const;
    /* section s (a type `from' has) taken from `from' */
    void take(const Config &from, int section);
};

/* The table of section type s ("cars"), "" for none. */
const char *table_name(int section);

/* A .gcf into its tables; false with the reason when it is not one (a
 * section type the game does not know: left out, a warning). */
bool read_gcf(const uint8_t *data, size_t size, Config *out, std::string *error, std::vector<std::string> *warnings);

/* The tables as a new SQLite database at `path' (UTF-8; replaced); false
 * with the reason. */
bool write_database(const Config &config, const std::string &path, std::string *error);

/* The tables of a database file's bytes (read in memory).  A table missing:
 * its section is not in the config; a row without its name (a car's code,
 * an event's...) is skipped, a value that is not a whole number taken as 0
 * (the row kept: the game finds them by name unchecked), each a line in
 * warnings ("cars row 4: ..."); a name twice in a sorted table: the first
 * kept.  False with the reason when it is not a database or has none of the
 * tables. */
bool read_database(const void *bytes, size_t size, Config *out, std::vector<std::string> *warnings,
                   std::string *error);

/* The config in the .gcf's layout (built at 0: offsets), as the game reads
 * it: the sorted tables sorted by name (the game's binary searches: signed
 * bytes), the rest in their order. */
std::vector<uint8_t> image(const Config &config);

/* A .gcf with sections no table holds (read_gcf's "not one the game reads":
 * demoplay.gcf's types 3, 7, 9) kept whole in the database at `path' (the
 * table gcf_file, one row: the file's bytes; replaced), so that the .gcf can
 * go without losing them.  The game does not read the table.  False with the
 * reason. */
bool keep_file(const std::vector<uint8_t> &gcf, const std::string &path, std::string *error);

} // namespace gcftables
