/* pc_volume.cpp - the player's volumes of the sound effects (pc_volume.h). */
#include "game/app/pc_volume.h"

#include "platform/settings.h"

thread_local f32 g_pcSeOutLevel = 1.0f;
f32 g_pcRaceSeLevel = 1.0f;

f32 pc_menu_se_volume(void)
{
    return settings::volume(settings::Volume::menuSe) / 100.0f;
}

f32 pc_race_se_volume(int replay)
{
    return settings::volume(replay ? settings::Volume::replaySe : settings::Volume::raceSe) / 100.0f;
}
