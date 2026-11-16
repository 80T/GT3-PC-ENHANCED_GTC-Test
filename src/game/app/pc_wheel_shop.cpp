/* pc_wheel_shop.cpp - PC (2026-11-13, no original): aftermarket wheels for
 * every car (pc_wheel_shop.h). */
#include "game/app/pc_wheel_shop.h"
#include "platform/settings.h"

int pc_wheels_unrestricted(void)
{
    return settings::unrestricted_wheels() ? 1 : 0;
}
