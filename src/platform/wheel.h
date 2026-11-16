/* wheel.h - the USB steering wheels of GT3 PC (no Windows headers: the
 * engine's USB wheel driver, engine/input/usb_pc.cpp, includes it).
 *
 * Wheel 1 / 2 (the game's USB wheel ports 1 / 2, players 1 / 2) is a
 * Logitech Driving Force (USB 0x46D:0xC294, which GT3 knows) built through
 * the mapping of gt3pc.ini [Wheel1] / [Wheel2] (platform/input_map.h) from
 * the wheel's SDL joystick (platform/gamepad.h), the devices its bindings
 * name (pedals of their own, a shifter...) and the keyboard and mouse.
 * It is connected while its joystick is.  The steering and pedal dead zones
 * apply to the virtual steering (right - left) and pedals.  No force
 * feedback yet: the game's commands are taken and dropped. */
#pragma once

namespace wheel {

struct State {
    int x;                                  /* the steering, 0..0x3FF (0x200 centred, 0 full left) */
    int gas, brake;                         /* the pedals pressed, 0..0xFF */
    unsigned char buttons[12];              /* the Driving Force's, in its report's order (1 pressed) */
    int hat;                                /* the D-pad: 0 up .. 7 up-left clockwise, 8 none */
};

/* The wheel of `port' (0, 1); false when it is not connected. */
bool read(int port, State *out);
/* The same without reading it. */
bool connected(int port);
/* Player `port' + 1 plays with the wheel while both it and the controller
 * are connected ([WheelN] Input=wheel, the default); false: with the
 * controller (Input=controller). */
bool preferred(int port);

} // namespace wheel
