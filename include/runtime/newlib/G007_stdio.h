/* G007_stdio.h - the PS2 runtime's C library and kernel stubs (GT3 PC).
 *
 * The decomp's header declared newlib (as linked into CORE.GT3), the libgcc
 * helpers and every libkernl syscall stub of the no-FDE runtime region.  In
 * the port the C library is the MSVC runtime behind libc.h and the kernel
 * calls are the platform's (eekernel.h); this header just brings both in
 * for the files that include it. */
#ifndef GAME_G007_STDIO_H
#define GAME_G007_STDIO_H

#include "types.h"
#include "libc.h"
#include "eekernel.h"

#endif /* GAME_G007_STDIO_H */
