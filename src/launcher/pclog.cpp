/* pclog.cpp - the game's log (platform/log.h) for the game code the
 * launcher shares (engine/media/native/mp4_movie.cpp, the song manager's
 * .mp3 reader): its lines to the debugger's output - the launcher has no
 * log file; what fails is said where it is used. */
#include "platform/log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace pclog {

void vwrite(const char *fmt, va_list ap)
{
    char line[1024];
    std::vsnprintf(line, sizeof(line), fmt, ap);
    OutputDebugStringA(line);
    OutputDebugStringA("\n");
}

void write(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vwrite(fmt, ap);
    va_end(ap);
}

} // namespace pclog
