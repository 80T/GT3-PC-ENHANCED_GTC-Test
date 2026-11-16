/* system_menu.h - the Esc menu of GT3 PC: Back, Restart, Restart as new game,
 * Exit.
 *
 * Esc shows or hides it in the middle of the picture (over the game, which
 * goes on, as under the settings menu).  While it is shown it takes the
 * keyboard and the game controllers - the game sees no input:
 *   Up / Down             choose               (D-pad)
 *   Enter / Space         do it                (Cross)
 *   Esc / Backspace       close                (Circle)
 * Back closes the menu.  Restart ends this run and starts the game again
 * from the beginning (main.cpp: a new gt3pc.exe with the same arguments,
 * which waits for this one to end).  Restart as new game is the same, and
 * that one next run skips the memory card's autoload (--new-game; a Restart
 * from it, or the next start, loads as usual).  Exit closes the window as its
 * close button does.  All three wait while a memory card is being written
 * (memcard::busy: a save is not cut short), the menu saying so; Esc then
 * cancels.  Drawn with platform/overlay.h.  No Windows headers. */
#pragma once

struct ID3D11RenderTargetView;

namespace system_menu {

/* Esc (the window thread). */
void toggle();
/* Hidden (F2: the settings menu instead), a pending action cancelled. */
void hide();
/* Shown: the game gets no input (platform/pad_pc.cpp, platform/window.cpp). */
bool shown();
/* A key went down while it is shown (the window thread). */
void key(int vk, bool repeat);
/* The output (pgl_targets' present), on the back buffer just before
 * Present: the game controllers read, a pending action done once the memory
 * cards are idle, the menu drawn when shown. */
void present(ID3D11RenderTargetView *backbuffer);
/* Restart was chosen (main.cpp, once the window has closed). */
bool restart_requested();
/* That restart was Restart as new game. */
bool restart_as_new_game();
/* This run was started by Restart as new game: main.cpp says so (--new-game)
 * before the game threads start, T000's main then skips the autoload. */
void set_new_game_run();
bool new_game_run();

} // namespace system_menu
