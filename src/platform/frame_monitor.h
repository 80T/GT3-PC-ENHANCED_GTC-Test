/* frame_monitor.h - the frame time monitor of GT3 PC.
 *
 * F3 cycles it off, simple, detailed, verbose (the settings menu sets it
 * too), over the top left corner of the picture.  Simple: the frame rate, the mean
 * frame time and the rolling graph of the time between the frames presented
 * (CPU side, when Present returns) over the last 300 frames, a line at the
 * display's refresh period, a frame over twice the average red.  Detailed:
 * also the extremes and the 99th percentile, the display's state (refresh
 * rate, vsync, tearing, exclusive fullscreen, frames between the game's
 * ticks), the graph coloured by tick (a frame that followed a game tick
 * orange, one between two ticks green) with a line at the tick (16.7 ms).
 * The graph's height follows the frames (the smallest of 5 / 10 / 20 ms and
 * 2, 4, 8 ticks that holds 1.5 x their 99th percentile).
 *
 * Detailed, below: the frames' breakdown (the render thread's time between two
 * Presents, by part, AVG / MAX over the graph's frames) and its graph, the
 * parts stacked per frame: UPDATE the game's ticks (the pads, the object's
 * update, the sound's flush), RECORD the game's draw code (pgl's recording),
 * EXECUTE the renderer's work on the CPU (the command list into Direct3D),
 * GPU WAIT the part of the execution the render thread did not run (its
 * CPU cycles against the time: the driver holding a call while the GPU is
 * behind, a readback), shown apart from EXECUTE, OUTPUT the
 * picture into the back buffer and the overlays, PRESENT the Present call
 * (the display, the GPU or a limiter), PACE the frame cap's (or the
 * vblank's) wait, OTHER the rest (the game's other threads, the scheduler);
 * GPU, the GPU's time of the frame's commands (timestamp queries, known a
 * few frames later), a white mark on the graph.
 *
 * Verbose: the detailed view, and a panel at its right: the GPU's time by
 * section (AVG / MAX): the renderer's sections (pgl's GT3PC_GPU_SECTIONS,
 * timed while this view is shown: a race view's scene, its effects, the
 * copy, the replay blur, the weather, the smoke, the flare, the post
 * effects, the rest); EXECUTE's CPU time by kind of work (AVG / MAX: the
 * renderer's profile, pgl_render.h ProfCat, each kind's own time; the D3D
 * thread's busy time apart, it runs alongside) and what
 * a frame executed (draw calls, draw states, texture binds and decodes,
 * model parts, GS commands, uploads, target copies, GS vertices and
 * batches, CLUT loads and memo misses, composed texture spaces; AVG).
 * Frames are recorded while it is hidden too.  No Windows headers: the
 * game's frame loop includes it. */
#pragma once

#include <stdint.h>

struct ID3D11RenderTargetView;

namespace frame_monitor {

/* The parts of a frame's time (the breakdown, above). */
enum Part { PART_UPDATE, PART_RECORD, PART_EXECUTE, PART_WAIT, PART_OUTPUT, PART_PRESENT, PART_PACE, kParts };
/* ms of part p in the frame being timed (shown at the next Present; WAIT
 * is a part of EXECUTE). */
void add(Part p, double ms);
/* The frame being timed (gpu_time's key). */
uint32_t frame_seq();
/* The GPU time of frame seq's commands, once its queries are read. */
void gpu_time(uint32_t seq, double ms);
/* The output begins (pgl's presentation: OUTPUT runs to Present). */
void output_begin();
/* gpu::present, just before Present (OUTPUT ends). */
void present_begin();
/* The GPU sections (verbose): their names (n at most kGpuSections), and
 * frame seq's ms in each, once its queries are read. */
constexpr int kGpuSections = 12;
void gpu_section_names(const char *const *names, int n);
void gpu_sections(uint32_t seq, const float *ms, int n);
/* The renderer's CPU profile of the execution (verbose): the kinds' and
 * the counts' names, and the frame being timed's ms by kind and counts. */
constexpr int kExecKinds = 20, kExecCounts = 18;
void exec_profile_names(const char *const *kinds, int nKinds, const char *const *counts, int nCounts);
void exec_profile(const float *ms, int nKinds, const uint32_t *counts, int nCounts);
/* The renderer's D3D thread's busy time in the frame being timed (ms; below
 * 0: no D3D thread) and the game thread's time waiting for it (syncs: in the
 * execution or out of it, the presentation's), shown apart from EXECUTE's
 * profile (it runs alongside). */
void exec_d3d_thread(float busyMs, float waitMs);
/* The renderer's executor thread's busy time (ms) for the frame being
 * timed: it runs the frame's list while the game thread runs the next
 * ticks and records the next frame (EXECUTE is then the game thread's wait
 * for it); shown apart. */
void exec_thread(float ms);

enum Mode { MODE_OFF, MODE_SIMPLE, MODE_DETAILED, MODE_VERBOSE, kModes };
/* F3 (the window thread): the next mode (off, simple, detailed, verbose). */
void toggle();
/* The settings menu. */
Mode mode();
void set_mode(Mode m);
bool shown();
/* The game's frame loop (game/app/pc_frame.cpp), before a frame is drawn:
 * whether it follows a tick (else it lies between two) and whether frames
 * are drawn between the ticks. */
void note_frame(int tick, int between);
/* The output (pgl_targets' present), on the back buffer just before
 * Present: draws the monitor when shown (its own D3D state; the renderer
 * forgets its bindings after). */
void present(ID3D11RenderTargetView *backbuffer);
/* gpu::present, once Present returned: the frame's time is recorded (the
 * cadence the display, the GPU or an outside limiter gives). */
void presented();

} // namespace frame_monitor
