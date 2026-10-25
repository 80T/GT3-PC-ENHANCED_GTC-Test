/* frame_step.h - the tick step of GT3 PC: the game paused, advanced one
 * 59.94 Hz tick at a time (docs/renderer.md "Tick step").
 *
 *   F5   running: pause; paused: one tick more (held: its autorepeat steps)
 *   F6   run again
 * (the Pause and Resume hotkeys: input_map.h [Hotkeys]).  The indicator (top
 * right) shows for 3 s after each F5, the pause going on without it.
 *
 * Paused, the render thread runs no tick (game/app/pc_frame.cpp).  A race
 * drawing frames between ticks goes on drawing them, at the last tick, at
 * its usual rate (the GPU and the CPU kept at their clocks: idle, they fell
 * to their power-saving ones and the resume stuttered while they rose); else
 * nothing is drawn and the last picture is presented again (the F2 / F3
 * overlays and this one's indicator stay alive) until a step or F6.  A step
 * runs one tick and draws its frame at the tick (no interpolation).  The sound freezes with
 * the game: the music thread is woken only by the stepped ticks (no timer
 * flushes), and the output stops mixing (a 5 ms fade; the voices and the BGM
 * stream hold where they are) until F6.  Other game threads (loading, the
 * memory card) go on.  No Windows headers. */
#pragma once

struct ID3D11RenderTargetView;

namespace frame_step {

/* The window thread: F5 (repeat: its autorepeat), F6. */
void key_step();
void key_resume();
/* Paused (any thread). */
bool paused();
/* The render thread, paused: a step asked for (taken: counted). */
bool take_step();
/* The indicator over the picture while paused, for 3 s after each F5 (the
 * present, as the other overlays). */
void present(ID3D11RenderTargetView *backbuffer);

} // namespace frame_step
