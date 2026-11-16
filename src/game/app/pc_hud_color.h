/* pc_hud_color.h - the race HUD's colours as set (user, 2026-10-30; the F2
 * menu's GAME > HUD > COLORS, the launcher's Game > HUD; docs/launcher.md).
 * No original: new PC code.
 *
 * The HUD's colour constants go through pc_hud_color where they are used, so
 * a colour never changed draws the game's own value, untouched. */
#ifndef GAME_APP_PC_HUD_COLOR_H
#define GAME_APP_PC_HUD_COLOR_H

#include "game/game.h"
#include "platform/hud_colors.h"                /* HudColor */

/* `original' (0xAABBGGRR), its red, green and blue the setting's once colour
 * `id' is set.  The alpha is always the original's. */
u32 pc_hud_color(int id, u32 original);

#endif /* GAME_APP_PC_HUD_COLOR_H */
