/* G000_collect2_ctors.h - the PS2 start-up code (crt0, collect2's lists)
 * (GT3 PC).
 *
 * On the PS2, crt0 cleared .bss, set $gp, made the main thread and ran
 * main; collect2's lists ran the static constructors.  In the port the C++
 * runtime does all of that and src/main starts the game thread.  What is
 * left is $gp: the threads were created with &_gp as their gpReg, which the
 * port's CreateThread ignores; the symbol exists so that code still links. */
#ifndef GAME_G000_COLLECT2_CTORS_H
#define GAME_G000_COLLECT2_CTORS_H

#include "game/game.h"

#ifdef __cplusplus
extern "C" {
#endif

extern char _gp[];                              /* 0x003579F0 on the PS2 */

#ifdef __cplusplus
}
#endif

#endif /* GAME_G000_COLLECT2_CTORS_H */
