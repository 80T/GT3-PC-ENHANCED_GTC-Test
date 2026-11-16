/* memstat.h - PC (2026-11-07): the process's memory, for the log.
 *
 * The game is a 32-bit program: large-address-aware, it has 4 GB of address
 * space on 64-bit Windows, and everything shares it - the game heap's
 * arena (platform/heap_arena.h), the renderer's decoded textures and
 * meshes and the Direct3D runtime's and driver's own memory for them, the
 * libraries, the threads' stacks.  A crash of 2026-11-07 (user: a 32-car
 * race after six others) was a std::bad_alloc from a texture decode on the
 * decoding worker with 4071 MB of the 4096 in use.  The line names what is
 * in use: the address space (virtual), the committed private bytes, the
 * working set, and the GPU's memory as DXGI counts it for this process
 * (local: on the card; non-local: system memory the driver holds for it).
 * The renderer logs it after each run of decodes (pgl_prewarm.cpp), the
 * crash report before its dump (crash.cpp).  No allocation. */
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace memstat {

/* "virtual N MB of M, private N MB, working set N MB; GPU local N MB of M,
 * non-local N MB" (the GPU part with withGpu, when the adapter answers). */
void line(char *out, size_t n, bool withGpu);

/* The address space in use, MB (0 when not known). */
uint32_t virtual_mb();

} // namespace memstat
