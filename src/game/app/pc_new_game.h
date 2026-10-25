/* pc_new_game.h - what a new game and a loaded one get in GT3 PC (the
 * settings: platform/settings.h [Game] StartingCredits, the launcher's
 * Debug tab's [Debug] FillGarage / FillMoney / UnlockArcade /
 * UnlockLicenses).  No original: new PC code (docs/game_start.md).
 *
 * A new game is GameStatus::init on the game's own status (createGameStatus
 * at boot; the guest garage's status is left alone): its money - the
 * setting's credits instead of the product config's "initial" (USA /
 * Europe 18,000, Japan 15,000), FillMoney's 100,000,000; in Japanese x 100,
 * yen, as the code prices everything (ProductConfig::servicePrice,
 * CarInformation::getPrice) - its garage (FillGarage: one of every car of
 * the car database whose model is in the car list, in each of its colours:
 * the cars of the data, whatever they are), and the unlocks.  A loaded game
 * (main, T000) gets the unlocks: UnlockArcade raises the arcade progress
 * level to the highest course level of the arcade modes' course lists
 * (every course open; the car levels and the other rewards stay as won),
 * UnlockLicenses gives every licence test at least a bronze medal
 * (LicenseRecords::passAll: every licence held; the licence trophies, won
 * at the licence ceremony, stay as they were). */
#ifndef GAME_APP_PC_NEW_GAME_H
#define GAME_APP_PC_NEW_GAME_H

#include "game/game.h"

namespace gt2k_game {
class GameStatus;
}

/* The money of a new game: the setting's (0: auto, `initial' the product
 * config's, already in yen in Japanese), FillMoney's. */
s64 pc_new_game_money(s64 initial);
/* The rest of a new game (end of GameStatus::init, the game's own status). */
void pc_new_game(gt2k_game::GameStatus *gs);
/* A loaded game (main, after a load). */
void pc_loaded_game(gt2k_game::GameStatus *gs);

#endif /* GAME_APP_PC_NEW_GAME_H */
