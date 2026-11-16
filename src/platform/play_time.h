/* play_time.h - the time played (gt3pc.ini [Stats] PlayTime).
 *
 * The seconds the game has run, over all its runs: start() reads the count,
 * a thread of its own writes it again each minute (a crash loses at most
 * that), stop() writes it at the close.  Time the PC sleeps is not counted.
 * The launcher shows it in hours (src/launcher/ui.cpp). */
#pragma once

namespace play_time {

void start();       /* after paths::init */
void stop();

} // namespace play_time
