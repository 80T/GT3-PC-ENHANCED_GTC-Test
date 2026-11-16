/* pc_menu_car.cpp - the right stick on the 3D cars of the menus (pc_menu_car.h). */
#include "game/app/pc_menu_car.h"
#include "platform/debug_camera.h"
#include "platform/pad_pc.h"

#include <math.h>

/* 10 s of 60 Hz ticks; the speed at the stop (the automatic spins are 10 .. 20
 * degrees a second), squared so that a small tilt is fine; the share of the
 * way to that speed taken in a tick: a turn starts and stops softly. */
enum { MENU_CAR_HOLD_TICKS = 600 };
static const f32 MENU_CAR_STICK_SPEED = 180.0f;
static const f32 MENU_CAR_STICK_EASE = 0.25f;

int pc_menu_car_spin(int player, int *hold, f32 *velocity)
{
    float x = 0.0f, y = 0.0f;
    if (!debug_camera::holds_input())
        pad_pc::camera(player, false, false, false, &x, &y);
    if (x != 0.0f)
        *hold = MENU_CAR_HOLD_TICKS;
    else if (*hold > 0)
        --*hold;
    if (*hold <= 0)
        return 0;
    /* right: the car's near side goes right, so the eye (tumbleY) goes left */
    const f32 target = -x * fabsf(x) * MENU_CAR_STICK_SPEED;
    *velocity += (target - *velocity) * MENU_CAR_STICK_EASE;
    return 1;
}
