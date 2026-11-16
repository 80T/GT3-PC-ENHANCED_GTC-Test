/* pc_menu_car.h - the right stick on the 3D cars of the menus (docs/ROADMAP.md
 * 45; user, 2026-11-01).
 *
 * A menu car (gt2k_menu::Car, T212) is turned by its camera's orbit, some by
 * themselves (the showroom record's rotSpeed: single, dealer, garage,
 * carwash).  The chase camera's controls of the car's player (pad_pc::camera:
 * by default the right stick, and J / L) turn it instead: tilted, the
 * automatic spin stops and the deflection is the speed; it comes back 10 s
 * after the last tilt.  A new car on show (Car::setCar) starts on its own
 * spin again.  The horizontal axis only. */
#pragma once

#include "game/game.h"

/* One tick of a menu car, before its camera's (Car::virtual_1): player the
 * pad, *hold the car's ticks left of the stick's turn.  1 while the stick has
 * the car: its yaw *velocity (degrees a second) was set and the automatic
 * spin must not push it. */
int pc_menu_car_spin(int player, int *hold, f32 *velocity);
