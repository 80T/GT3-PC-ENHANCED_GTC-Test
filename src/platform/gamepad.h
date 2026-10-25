/* gamepad.h - game controllers and wheels of GT3 PC, through SDL3.
 *
 * A DualShock 4 (or any controller SDL3 knows as a gamepad: DualSense,
 * Xbox, Switch Pro...) stands for a DualShock 2: the first one connected
 * plugs into port 1, the second into port 2; unplugged, its port is free for
 * the next.  A wheel (a joystick SDL3 types as a wheel, or the one named by
 * gt3pc.ini [Wheel1] / [Wheel2] Device) is Wheel 1 / 2 instead, never a
 * pad.  SDL3.dll is loaded at start-up when it is next to the game (it is
 * delay-loaded); without it the game runs with the keyboard alone.  A
 * thread of its own polls SDL (every 2 ms), so the pads read a recent state
 * at any time; the controllers work while the window is in the background,
 * as in PCSX2.  platform/pad_pc.cpp maps the raw state to libpad's answers
 * and platform/wheel.cpp the wheels' to the USB wheel driver's, both through
 * the mappings of platform/input_map.h.
 *
 * The Guide button (PS / Xbox / Home) of any controller is the menus': a
 * tap shows or hides the settings menu (F2), a hold of 1.5 s the Esc menu.
 *
 * The DualShock 2 of State (the menus read it: the physical buttons, not
 * the mapping): the buttons by position (Cross = the bottom face button),
 * D-pad, Start = Options / Menu, Select = Share / View, L3 / R3 the stick
 * clicks; L2 / R2 from the triggers.  The two motors of the game drive the
 * controller's rumble (the large one the low-frequency motor). */
#pragma once

#include <stdint.h>

#include "platform/input_map.h"

namespace gamepad {

/* A DualShock 2's state.  buttons: the answer's bytes 2..3 (libpad), active
 * high: bit 0..7 = Select, L3, R3, Start, Up, Right, Down, Left; bit 8..15 =
 * L2, R2, L1, R1, Triangle, Circle, Cross, Square.  sticks: right x, right
 * y, left x, left y (0x80 centred, 0 = left / up).  pressure: Right, Left,
 * Up, Down, Triangle, Circle, Cross, Square, L1, R1, L2, R2. */
struct State {
    uint16_t buttons;
    uint8_t sticks[4];
    uint8_t pressure[12];
};

/* Loads SDL3 and starts the controller thread; false (logged) when SDL3.dll
 * is missing or does not start. */
bool init();
/* Stops the motors and the thread. */
void shutdown();

/* The state of the controller in `port` (0, 1); false when none. */
bool read(int port, State *out);
/* The raw state of the controller in `port' (SDL's buttons and axes as they
 * are); false when none. */
bool read_raw(int port, input_map::PadSource *out);
/* SDL's buttons (input_map::PadSource bits) seen down since the last call:
 * a tap between two of the game's reads (platform/pad_pc.h). */
uint32_t take_pressed_raw(int port);

/* The game's motors for `port`: small on (0 / 1), large 0..255. */
void set_motors(int port, int small, int large);

/* A wheel's joystick as SDL reads it. */
struct Joystick {
    int buttonCount, axisCount, hatCount;
    uint8_t buttons[128];
    int16_t axes[16];
    uint8_t hats[4];                        /* SDL_HAT_* bits */
};
/* The joystick of Wheel 1 / 2 (`wheel' 0, 1); false when none. */
bool read_wheel(int wheel, Joystick *out);

} // namespace gamepad
