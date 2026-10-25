/* pc_units.cpp - the units the game shows (pc_units.h).  No original: new
 * PC code. */
#include "game/app/pc_units.h"
#include "libc.h"                               /* printf */
#include "platform/settings.h"

UnitSettings g_pcGameUnits;                     /* (the US preset until set_country) */

static int s_gen = -1;                          /* the settings' generation applied */

/* g_002F59D8: the preset's units with the settings' over them. */
static void apply(void)
{
    g_002F59D8 = g_pcGameUnits;
    if (settings::speed_unit() != settings::kUnitAuto)
        g_002F59D8.system = settings::speed_unit();
    if (settings::power_unit() != settings::kUnitAuto)
        g_002F59D8.power = settings::power_unit();
    if (settings::torque_unit() != settings::kUnitAuto)
        g_002F59D8.torque = settings::torque_unit();
    s_gen = settings::units_generation();
    printf("PC: units: %s, %s, %s (the language's: %s, %s, %s)\n", g_002F59D8.speedName(), g_002F59D8.powerName(),
           g_002F59D8.torqueName(), g_pcGameUnits.speedName(), g_pcGameUnits.powerName(), g_pcGameUnits.torqueName());
}

void pc_units_preset(u32 preset)
{
    g_pcGameUnits.setPreset(preset);
    apply();
}

void pc_units_sync(void)
{
    if (settings::units_generation() != s_gen)
        apply();
}
