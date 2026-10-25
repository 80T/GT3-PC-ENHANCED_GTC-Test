/* log.h - the GT3 PC log: gt3pc.log next to the program, the debugger's
 * output window, and the console when there is one.  The game's printf
 * (the PS2 debug TTY) goes here too. */
#pragma once

#include <stdarg.h>

namespace pclog {

/* Opens the log file (truncates it). */
void open(const char *path);
void close();
/* A console window of its own for the log (gt3pc.ini [Debug] Console=on):
 * what was written so far, then every line as it comes.  Its close button
 * is off (closing a console ends its program: the game closes it), and so
 * is its mouse selection (a selection holds the writers). */
void open_console();

/* One message; a missing final newline is added. */
void write(const char *fmt, ...);
void vwrite(const char *fmt, va_list ap);
/* Raw text as printf would write it (no newline added: the game's printf
 * builds lines from several calls). */
void raw(const char *text, int len);

/* Writes the message and shows it in a message box, then ends the program. */
[[noreturn]] void fatal(const char *fmt, ...);

} // namespace pclog
