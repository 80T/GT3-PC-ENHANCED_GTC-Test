/* pad_pc.h - the PC side of the controller reads (pad_pc.cpp implements
 * libpad over the keyboard and SDL3; no Windows headers: the engine's pad
 * code includes it).
 *
 * The game reads the pads once per tick (engine/input T477 padReadAll, from
 * PadManager::update).  When the frames are slower than the ticks, one
 * frame runs several ticks back to back (game/app/T002): their reads would
 * come within microseconds of each other, so a press would show on the first
 * tick only and code that looks for it later in the frame (the draw, the
 * post-draw slot) missed it.  Those ticks repeat the last read instead
 * (hold_reads); the frame's last tick reads the pads.  And a press released
 * again before the next read (a tap shorter than the time between two reads)
 * still counts: the buttons pressed since the last read are reported
 * pressed for one read (take_taps). */
#pragma once

namespace pad_pc {

/* true while the ticks caught up read: padReadAll keeps the last read */
void hold_reads(bool on);
bool holding();
/* By padReadAll before it reads: the buttons pressed since the last read
 * (keyboard and controllers) are reported pressed in this read's answers. */
void take_taps();

/* The chase camera's controls of player `port' (0, 1; input_map's PAD_CAM_*
 * of Controller N, or WHEEL_CAM_* of Wheel N when `wheel': the wheel is the
 * player's device), each slot's binding read as the pad's controls are (the
 * dead zones first): *x right - left, *y down - up, -1 .. 1; 0 while a menu
 * has the input.  skipX / skipY: the game's controller settings use the
 * right stick's x / y, so a camera binding that the virtual right stick's
 * control on that axis has too is left out (the pad's stick goes to the
 * game, not the camera). */
void camera(int port, bool wheel, bool skipX, bool skipY, float *x, float *y);

} // namespace pad_pc
