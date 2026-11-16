/* ui.h - the launcher window. */
#pragma once

#include <string>

namespace ui {

/* Shows the launcher window and runs it until it closes; the exit code.
 * shared (PC 2026-11-14, engines.h): the folder whose gt3pc.ini this launcher
 * reads and writes, and its game with it - another launcher's, whose engine
 * selector started this one as an engine; "": its own. */
int run(int show_command, const std::wstring &shared = std::wstring());

} // namespace ui
