/* pc_wheel_shop.h - PC (2026-11-13, no original): aftermarket wheels for
 * every car ([Game] UnrestrictedWheels, the settings menu's GAME > EXTRAS >
 * UNRESTRICTED WHEELS, the launcher's Game > Extras).
 *
 * The game keeps eleven cars out of the wheel shop ("at/ws/ws", check_ws)
 * and out of the garage's wheel change ("hm/hm_wheel", check_hm_wheel), T045:
 * the cars whose record has shop flag bit 2 (CAR_SHOP_BIT_FIXED_WHEELS; the
 * US database: the six F1 cars lotus_99t, mclaren_mp4_4, mclaren_mp4_5b,
 * williams_fw11, williams_fw16a, williams_fw16b and the concept cars
 * rsx_detroitver_01, rx8_01, rx8_01_ext, viper_gtsr_00, z_detroitver_01) get
 * the message "00cmn/not_for_change_wheel".  Nothing else restricts a wheel:
 * every wheel of the shop goes on every other car, scaled to its rim
 * (TireModelBase), and a race loads whatever wheel a car has
 * (RaceEntryCar::loadWheel).
 *
 * On, the two checks let those cars in.  Their model files are laid out as
 * every car's (the wheel part last but for the 64-byte tail, checked over
 * the US data's 1,146 car files), so the menus load a wheel over their own
 * as for any car (gt2k_menu::Car, T212) and the races load it apart
 * (RaceCarModel::loadWheel).  Their tyres stay their own: an F1 car keeps
 * its F1 tyre file, front and rear sizes. */
#ifndef GAME_APP_PC_WHEEL_SHOP_H
#define GAME_APP_PC_WHEEL_SHOP_H

#include "game/game.h"

/* The setting: 1 on. */
int pc_wheels_unrestricted(void);

#endif /* GAME_APP_PC_WHEEL_SHOP_H */
