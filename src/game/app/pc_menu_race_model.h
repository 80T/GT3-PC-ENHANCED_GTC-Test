/* pc_menu_race_model.h - PC (2026-11-12, no original): the menus' cars with
 * their race models ([Debug] MenuRaceModel / MenuRaceModelLod, the settings
 * menu's DEBUG > MENU RACE MODEL, the launcher's Debug tab).
 *
 * A menu car (gt2k_menu::Car, T212: the showroom, the garage, the dealers,
 * the arcade's car select, the car wash; the prize cars, T061 / T107) is
 * read from "menu/cars/<car>"; with the setting on from "cars/day/<car>"
 * (car_model_path kind 0), the file a race reads - the same CarModelFile
 * layout: the body, its race wheel ("wheel/<name>" for a fitted wheel), the
 * tyre file.  The menus draw it as they draw their own:
 *   - the body's LOD commands (0x07) choose as the game does in a menu (the
 *     CarModel's callback, by projected size, or level 0 with [Graphics]
 *     Lod off), or the level set here (modelset_pc_force_lod, T451) while
 *     the car is drawn: race car bodies have three levels, 0 the finest;
 *   - the race wheel's face is not in the wheel model: the race fills a
 *     slot of the models' second texture table each frame
 *     (RaceTirePacket::drawWheelTexture, TEX2_WHEEL_FACE); the menu tyre
 *     (T450 MenuTireModel::draw) does it for a race wheel here, still, from
 *     the face of the wheel's texture set for the level (0 when auto), in a
 *     table of its own (the menus' table is the environment image's own
 *     register blocks);
 *   - not drawn: the race's brake discs and callipers (race/brake.bin,
 *     RaceTirePacket; the menu models carry theirs as body parts 2 / 3,
 *     which a race body leaves empty), the race tyres' levels (the menu
 *     tyre is built with 30 segments), the race's motion blur.
 * The race body's own shadow parts (1, 6) are drawn as the menus draw a
 * menu body's. */
#ifndef GAME_APP_PC_MENU_RACE_MODEL_H
#define GAME_APP_PC_MENU_RACE_MODEL_H

#include "game/game.h"

/* The setting: 1 on. */
int pc_menu_race_model(void);
/* car_model_path's kind for a menu car now: 0 (cars/day) on, 2 (menu/cars)
 * off. */
int pc_menu_race_model_kind(void);
/* The prize cars' models are race models (T061 car_get_prizes records it as
 * it loads them; T107 draws them). */
void pc_menu_race_model_set_prizes(int race);
int pc_menu_race_model_prizes(void);
/* Around a menu car's draw (the render thread): raceModel 1 when the model
 * drawn is a race model - its level of detail forced (the setting's level,
 * none when auto) and its race wheels' faces given (pc_menu_race_wheel_level).
 * raceModel 0: nothing. */
void pc_menu_race_model_begin(int raceModel);
void pc_menu_race_model_end(void);
/* Inside begin(1) / end: the level of the race wheels' faces (the forced one,
 * 0 when auto); -1 outside, or for a menu model. */
int pc_menu_race_wheel_level(void);

#endif /* GAME_APP_PC_MENU_RACE_MODEL_H */
