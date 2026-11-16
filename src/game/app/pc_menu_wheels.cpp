/* pc_menu_wheels.cpp - the race cars' menu wheels (pc_menu_wheels.h).
 * No original: new PC code. */
#include "game/app/pc_menu_wheels.h"
#include "platform/settings.h"

namespace {
WheelFile2 *g_now;                          /* the tyre drawn next (drawTires) */
}

int pc_menu_wheels(void)
{
    return settings::menu_wheels() ? 1 : 0;
}

int pc_menu_wheels_speed(void)
{
    return settings::menu_wheels_speed();
}

void pc_menu_wheel_set(WheelFile2 *w)
{
    g_now = w;
}

WheelFile2 *pc_menu_wheel_now(void)
{
    return g_now;
}

int pc_menu_wheel_fast(int wasFast, f32 kmh)
{
    const int limit = pc_menu_wheels_speed();
    if (limit <= 0)
        return 0;
    if (wasFast)
        return kmh >= (f32)limit * 0.85f ? 1 : 0;
    return kmh >= (f32)limit ? 1 : 0;
}
