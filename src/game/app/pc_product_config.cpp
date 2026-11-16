/* pc_product_config.cpp - PC: the product config from its SQLite database
 * (see pc_product_config.h). */
#include "game/app/pc_product_config.h"
#include "game/app/pc_data_file.h"
#include "platform/gcf_tables.h"
#include "platform/log.h"

#include <cstring>
#include <string>
#include <vector>

void *pc_product_config_load(const char *gcfPath)
{
    std::string base = gcfPath;
    if (base.size() > 4 && base.compare(base.size() - 4, 4, ".gcf") == 0)
        base.resize(base.size() - 4);
    const std::string dbPath = base + ".sqlite3";
    std::string db;
    if (!pc_data_file_read(dbPath, &db))
        return NULL;
    gcftables::Config config;
    std::vector<std::string> warnings;
    std::string error;
    std::string gcf, taken;
    const bool haveGcf = pc_data_file_read(gcfPath, &gcf);
    if (!gcftables::read_database(db.data(), db.size(), &config, &warnings, &error)) {
        /* (the launcher deletes the .gcf once its database is checked) */
        if (!haveGcf)
            pclog::fatal("The game config %s: %s.", dbPath.c_str(), error.c_str());
        printf("PC: product config: %s: %s; %s is read\n", dbPath.c_str(), error.c_str(), gcfPath);
        return NULL;
    }
    /* the tables the database lacks: the .gcf's (when it is still there) */
    if (haveGcf) {
        gcftables::Config file;
        std::vector<std::string> ignored;
        if (gcftables::read_gcf((const uint8_t *)gcf.data(), gcf.size(), &file, &error, &ignored))
            for (int s : file.sections)
                if (!config.has(s)) {
                    config.take(file, s);
                    taken += (taken.empty() ? "" : ", ") + std::string(gcftables::table_name(s));
                }
    }
    printf("PC: product config from %s: cars %d, courses %d, events %d, prize cars %d%s%s%s\n", dbPath.c_str(),
           (int)config.cars.size(), (int)config.courses.size(), (int)config.events.size(), (int)config.prizes.size(),
           taken.empty() ? "" : "; from the .gcf: ", taken.c_str(), warnings.empty() ? "" : " (warnings below)");
    for (size_t i = 0; i < warnings.size() && i < 40; i++)
        printf("PC: product config: %s\n", warnings[i].c_str());
    if (warnings.size() > 40)
        printf("PC: product config: %d more warnings\n", (int)(warnings.size() - 40));
    const std::vector<uint8_t> img = gcftables::image(config);
    void *buf = malloc(img.size());             /* (the game heap: as file_read_blocking's buffer) */
    if (buf == NULL)
        return NULL;
    std::memcpy(buf, img.data(), img.size());
    return buf;
}
