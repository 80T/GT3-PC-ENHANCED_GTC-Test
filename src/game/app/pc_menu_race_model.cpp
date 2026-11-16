/* pc_menu_race_model.cpp - PC (2026-11-12, no original): the menus' cars
 * with their race models (pc_menu_race_model.h). */
#include "game/app/pc_menu_race_model.h"
#include "engine/gfx/T451_model_set.h"      /* modelset_pc_force_lod */
#include "platform/settings.h"

static int g_prizes;                        /* the prize cars' models are race models */
static int g_wheelLevel = -1;               /* inside begin(1) / end: the wheels' face level */

int pc_menu_race_model(void)
{
    return settings::menu_race_model() ? 1 : 0;
}

int pc_menu_race_model_kind(void)
{
    return pc_menu_race_model() ? 0 : 2;
}

void pc_menu_race_model_set_prizes(int race)
{
    g_prizes = race != 0;
}

int pc_menu_race_model_prizes(void)
{
    return g_prizes;
}

void pc_menu_race_model_begin(int raceModel)
{
    if (raceModel == 0)
        return;
    const int level = settings::menu_race_model_lod();
    modelset_pc_force_lod(level);
    g_wheelLevel = level >= 0 ? level : 0;
}

void pc_menu_race_model_end(void)
{
    if (g_wheelLevel < 0)
        return;
    modelset_pc_force_lod(-1);
    g_wheelLevel = -1;
}

int pc_menu_race_wheel_level(void)
{
    return g_wheelLevel;
}
