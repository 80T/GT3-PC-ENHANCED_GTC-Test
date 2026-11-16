/* pc_menu_tires.h - PC (2026-11-13, no original): the race cars' tyres as
 * the menus' (user: "A graphics setting to use the tire's menu model instead
 * of the race one").
 *
 * A tyre has no model file: TirePacket (T462) builds its strips from the
 * tyre's size, and the tyre file ("GTTR") only carries the texture of its
 * tread and sidewall, how often it repeats round the tyre, the F1 shape's
 * flag and the material's power.  What a menu car's tyre has over a race
 * car's:
 *   - its file: "menu/tire/n<k>" - the texture 256 x 128 where the race
 *     file's ("tire/n<k>") is 128 x 64, everything else of the 39 pairs the
 *     same;
 *   - its build (MenuTireModel::build, T450): 30 segments round, bevelled,
 *     the sidewall 1 mm inside the wheel's face - a race tyre's levels are
 *     18, 12 and 8 segments, 4 mm outside it.
 *
 * [Graphics] MenuTires=on (platform/settings.h; the F2 menu's GRAPHICS >
 * MENU TIRES, the launcher's Graphics tab): every race car reads the menu
 * file of its tyre (RaceCarModel::pcLoadMenuTire, T180: with the race, or
 * when the setting is turned on in one) and its four tyres get a second set
 * of packets for their levels 0 and 1, built the menu's way with that file
 * (RaceTireModel::pcBuildMenu, T449: the brake's disc, caliper and glow in
 * them as in the race's), in a work space of their own.  RaceTireModel::draw
 * takes them and the menu file while the setting is on; level 2 (the far
 * one, its wheel a hexagon) and the plain cylinder stay the race's.  The
 * race tyre's packets are kept: the setting switches in a race at once, and
 * nothing a frame is drawing from is written.
 *
 * The tread's motion blur (RaceTirePacket::drawTreadBlur) renders the tread
 * texture into a 128 x 64 GS buffer and shifts it along the tyre: the first
 * sprite takes all of the texture now, whatever its size (it took 128 x 64
 * texels: a quarter of a menu tyre's).  The buffer keeps its size - see
 * there: a 256 x 128 one moved about the VRAM and stalled the race.
 *
 * VRAM: the cars with the same tyre share one menu file
 * (pc_menu_tire_acquire), one texture set of 3 allocator pages for all of
 * them - a race tyre's file is each car's own, a page each. */
#ifndef GAME_APP_PC_MENU_TIRES_H
#define GAME_APP_PC_MENU_TIRES_H

#include "game/game.h"

struct TireFile2;                           /* T447 */

/* [Graphics] MenuTires: 1 on. */
int pc_menu_tires(void);

/* The menu file of tyre `number` ("menu/tire/n<number>"), read whole and
 * relocated the first time (blocking), then shared: one more user.  NULL
 * (logged): not there, not a tyre file.  *size (may be NULL): its bytes.
 * _addref: one more user of a file already had (RaceCarModel::copyFrom);
 * _release: one less, the file freed with its last.  The game thread's. */
TireFile2 *pc_menu_tire_acquire(int number, u32 *size);
void pc_menu_tire_addref(TireFile2 *t);
void pc_menu_tire_release(TireFile2 *t);

#endif /* GAME_APP_PC_MENU_TIRES_H */
