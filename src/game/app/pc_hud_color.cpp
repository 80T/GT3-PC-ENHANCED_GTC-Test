/* pc_hud_color.cpp - the race HUD's colours as set (pc_hud_color.h).  No
 * original: new PC code. */
#include "game/app/pc_hud_color.h"
#include "platform/settings.h"

u32 pc_hud_color(int id, u32 original)
{
    const uint32_t rgb = settings::hud_color_set(id);
    if (rgb == settings::kHudColorGame)
        return original;
    return (original & 0xFF000000u) | ((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF);
}
