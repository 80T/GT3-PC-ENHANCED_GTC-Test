/* screenshot.h - screenshots of GT3 PC (PC 2026-11-15, user request: "Screenshot
 * hotkey that takes a snapshot of the internal resolution and saves it to a
 * screenshots folder.  Also an option to take a screenshot in the F2 menu";
 * docs/renderer.md "Screenshots").
 *
 * The picture at the game's own resolution: the shown part of the displayed
 * buffer at its native size - the window's picture times the resolution scale
 * (pgl_targets.cpp picture_scales: square pixels, the race view's aspect
 * included), the anti-aliasing resolved (MSAA, AGSS) or applied (FXAA) - not
 * the window's copy of it: no black bars, none of the overlays drawn over it at
 * the present (the settings menu, the frame monitor, the messages, the panels).
 * A PNG in the screenshots folder beside gt3pc.ini (paths::screenshots_dir),
 * gt3pc_<date>_<time>.png.
 *
 * Asked for by the Screenshot hotkey (F1, input_map.h) or the settings menu's
 * TAKE SCREENSHOT; taken by the renderer at its next present (engine/pgl/
 * native/pgl_targets.cpp: a copy on the GPU, read a present or two later
 * without waiting for the GPU), written by a thread of its own; a message
 * (osd.h) tells where.  No Windows headers: game code can include it. */
#pragma once

#include <cstdint>
#include <vector>

namespace screenshot {

/* Asks for one (any thread): the next picture presented.  Refused, with a
 * message, while two are still being written. */
void request();
/* The renderer, at a present: a screenshot is asked for (the request taken). */
bool take_request();
/* The renderer: the one asked for could not be taken (why: the message). */
void failed(const char *why);
/* The renderer: the picture taken, w x h rows of R, G, B, A bytes, top down
 * (the alpha not written): moved to a thread of its own, written there as a
 * PNG, a message when it is (or why it is not). */
void save(std::vector<uint8_t> &&rgba, int w, int h);
/* Screenshots asked for or taken and not yet written (the settings menu's
 * TAKE SCREENSHOT shows SAVING...). */
int pending();

} // namespace screenshot
