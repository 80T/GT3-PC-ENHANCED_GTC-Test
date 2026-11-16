/* osd.h - a short message over the picture of GT3 PC (docs/renderer.md "Replay
 * speed"): at the top centre, for 2 s of real time, the latest in place of the
 * last.  Drawn with the panels (platform/overlay.h), over whatever the game
 * shows.  No Windows headers: game code can include it. */
#pragma once

struct ID3D11RenderTargetView;

namespace osd {

/* Shows `text` (any thread). */
void show(const char *text);
/* The message over the picture, while it lasts (the present, as the other
 * overlays). */
void present(ID3D11RenderTargetView *backbuffer);

} // namespace osd
