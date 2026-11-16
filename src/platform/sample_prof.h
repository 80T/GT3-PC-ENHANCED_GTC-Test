/* sample_prof.h - PC (no original; docs/renderer.md's debug table): a
 * sampling profiler of the renderer's threads, for the renderer's CPU time
 * by function (the executor's PROF_* kinds say what kind of work, not which
 * code).
 *
 * GT3PC_SAMPLE=N: a thread of its own looks at each registered thread about
 * a thousand times a second (suspended for the moment of a GetThreadContext:
 * its instruction pointer); every N seconds each one's hottest functions
 * are logged ("sample prof ...": the share of its samples, the hottest line
 * in each), from gt3pc.pdb beside the exe (dbghelp), other modules by name.
 * The samples count the thread's own time where it was (the leaf), not its
 * callers'; a waiting thread's samples are in the module it waits in.
 * Without the variable nothing runs.  diag\gpu_sections.cmd sets it. */
#pragma once

namespace sample_prof {

/* The calling thread sampled from now on under `name' (once; nothing
 * without GT3PC_SAMPLE). */
void register_thread(const char *name);

} // namespace sample_prof
