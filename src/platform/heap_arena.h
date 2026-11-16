/* heap_arena.h - the memory of the game heap.
 *
 * The PS2 game heap (pdistd T532: malloc, memalign, free...) took the RAM
 * from the end of the program to 0x01F00000, about 24 MB.  The port gives
 * it one block reserved at start, zeroed, so code that sizes its pools from
 * the free heap still works and has room: PC (ROADMAP 18, no memory limits)
 * 768 MB, else the largest of 512 .. 128 MB that can be had, below 2 GB if
 * it fits there (the decomp's code is not proven for addresses with the top
 * bit set); GT3PC_HEAP_MB=N (128 .. 2048) asks for a size first.  (PC
 * 2026-11-07: 768, was 1536 - the whole 4 GB address space ran out in a
 * long session, platform/memstat.h; a 32-car race uses some 30 MB of the
 * arena.)  Reserved first thing in WinMain, before the libraries split the
 * address space up. */
#pragma once

#include <stdint.h>

/* The arena (reserved on the first call); *size receives its size. */
void *heap_arena(uint32_t *size);
