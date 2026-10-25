/* pc_units.h - the units the game shows in GT3 PC (the settings [Game]
 * SpeedUnit / PowerUnit / TorqueUnit: platform/settings.h; the settings
 * menu, the launcher's Game tab).  No original: new PC code
 * (docs/units.md).
 *
 * The game's UnitSettings (T518) has every unit - speed and distance
 * metric or imperial, power in PS, HP, BHP, CV or kW, torque in kgf.m,
 * ft.lb or Nm - and a preset per country (set_units_for_country, T050: the
 * US mph / HP / ft.lb, Japan km/h / PS / kgf.m, Britain BHP, France CV...);
 * the US build only ever had its own.  Now two sets:
 *   - g_002F59D8, the units shown: the language's preset with the
 *     settings' choices over it (the texts that print a unit's name);
 *   - g_pcGameUnits, the language's preset alone: the game's own figures
 *     whose unit is not printed - the power-to-weight ratios that pick a
 *     race class, order a grid (retail computed them in the shown units, so
 *     a kW choice would have changed the game) and the ratios the menus
 *     show beside a unit drawn into their art.
 * A change applies at the next tick (pc_units_sync); a race's speedometer
 * dial is chosen at its start. */
#ifndef GAME_APP_PC_UNITS_H
#define GAME_APP_PC_UNITS_H

#include "game/data/T518_unit_settings.h"

extern UnitSettings g_pcGameUnits;

/* The country's preset into both sets, then the settings' choices over
 * g_002F59D8 (set_units_for_country). */
void pc_units_preset(u32 preset);
/* Each tick: a changed setting applied (game thread). */
void pc_units_sync(void);

#endif /* GAME_APP_PC_UNITS_H */
