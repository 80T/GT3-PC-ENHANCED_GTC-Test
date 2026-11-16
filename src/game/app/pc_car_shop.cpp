/* pc_car_shop.cpp - PC (2026-11-14, no original): the cars the game does not
 * sell bought as any other (pc_car_shop.h). */
#include "game/app/pc_car_shop.h"
#include "platform/settings.h"

int pc_buy_not_for_sale(void)
{
    return settings::buy_not_for_sale() ? 1 : 0;
}

int pc_buy_flags(int shopFlags)
{
    return pc_buy_not_for_sale() ? (shopFlags & ~3) : shopFlags;
}
