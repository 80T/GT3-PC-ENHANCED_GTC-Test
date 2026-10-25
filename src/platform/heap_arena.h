/* heap_arena.h - the memory of the game heap.
 *
 * The PS2 game heap (pdistd T532: malloc, memalign, free...) took the RAM
 * from the end of the program to 0x01F00000, about 24 MB.  The port gives
 * it one block reserved at start, zeroed, so code that sizes its pools from
 * the free heap still works and has room: PC (ROADMAP 18, no memory limits)
 * as much as the 32-bit address space spares - 1.5 GB, else the largest of
 * 1 GB .. 128 MB that can be had, below 2 GB if it fits there (the decomp's
 * code is not proven for addresses with the top bit set).  Reserved first
 * thing in WinMain, before the libraries split the address space up. */
#pragma once

#include <stdint.h>

/* The arena (reserved on the first call); *size receives its size. */
void *heap_arena(uint32_t *size);
