/* pc_menu_wheels.h - PC (2026-11-12, no original): the race cars' wheels
 * drawn with their menu wheel models (user: "A graphics setting that loads
 * the menu wheel models instead of the race wheels").
 *
 * [Graphics] MenuWheels=on (platform/settings.h; the F2 menu's GRAPHICS >
 * MENU WHEELS, the launcher's Graphics tab): every race car keeps its menu
 * wheel file - the showroom's 3D wheel (~1,300 triangles, open spokes) where
 * the race wheel is a shallow dish painted with the wheel's face - loaded
 * with the race (RaceCarModel::pcLoadMenuWheel: an aftermarket wheel's
 * "menu/wheel/<name>", else the wheel part of the car's own menu file).
 * RaceTirePacket::draw (T449) draws it in place of the race wheel at the
 * tyre's levels 0 and 1 (level 2 keeps its hexagon): in the same matrix,
 * spun with the tyre, the race tyre, brake disc and caliper round it.
 *
 * Motion: the race wheel's blur is a picture of its face turned in a small
 * buffer; a 3D wheel cannot use it.  From MenuWheelsSpeed (km/h at the tyre,
 * 20 by default; 0: never) the race wheel and its blur take over, back below
 * 85 % of it (RaceCarModel::drawTires, per wheel).
 *
 * The lighting needs nothing of its own: pgl puts the lights into the
 * model's space at every model draw (pgl_update_eye_lights, from the
 * modelview with the tyre's spin), the reflection pass's matrix too
 * (pgl_build_env_matrix), and the wheel's command stream sets its own blend
 * and depth states. */
#ifndef GAME_APP_PC_MENU_WHEELS_H
#define GAME_APP_PC_MENU_WHEELS_H

#include "game/game.h"

struct WheelFile2;                          /* T447 */

/* [Graphics] MenuWheels: 1 on. */
int pc_menu_wheels(void);
/* [Graphics] MenuWheelsSpeed, km/h at the tyre; 0: the menu wheel at every
 * speed. */
int pc_menu_wheels_speed(void);

/* RaceCarModel::drawTires: the menu wheel of the tyre drawn next (NULL: its
 * race wheel); RaceTirePacket::draw reads it.  The game thread's (the draws). */
void pc_menu_wheel_set(WheelFile2 *w);
WheelFile2 *pc_menu_wheel_now(void);

/* Per wheel, whether its race wheel has taken over (the switch's hysteresis:
 * fast at the speed, slow again below 85 % of it).  kmh the wheel's speed. */
int pc_menu_wheel_fast(int wasFast, f32 kmh);

#endif /* GAME_APP_PC_MENU_WHEELS_H */
