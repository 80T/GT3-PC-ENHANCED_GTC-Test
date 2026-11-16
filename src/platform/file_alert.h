/* file_alert.h - the pop-up of files in use by another program (GT3 PC;
 * game/app/pc_data_file.h).
 *
 * A Windows message box on a thread of its own: the game goes on waiting for
 * the files behind it, and it closes by itself when they are free.  Its OK
 * only hides it.  Topmost and of no window of the game's, so it stays in
 * sight over the editor and the game window can still be closed.  On a game
 * thread, one at a time.  No Windows headers. */
#pragma once

#include <string>
#include <vector>

namespace file_alert {

/* The files in use now (Windows paths; the same list again changes nothing).
 * The box is shown with them, its list kept to those still in use; a file not
 * named yet shows it again though OK hid it.  None: it is closed. */
void show(const std::vector<std::string> &files);

/* Out of exclusive fullscreen, where a box stays hidden (gpu::leave_fullscreen):
 * before the first frame, which takes it again. */
void leave_fullscreen();

} // namespace file_alert
