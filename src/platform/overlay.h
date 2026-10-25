/* overlay.h - drawing over the picture for GT3 PC's own panels (the frame
 * time monitor, the settings menu): coloured rectangles and a 5 x 7 pixel
 * font, in back buffer pixels, drawn on the back buffer just before Present
 * with its own D3D objects (the renderer forgets its bindings after).  No
 * Windows headers. */
#pragma once

#include <stdint.h>

struct ID3D11RenderTargetView;

namespace overlay {

/* R8G8B8A8_UNORM: 0xAABBGGRR */
constexpr uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    return r | (g << 8) | (b << 16) | (a << 24);
}

/* Starts a panel's drawing (the D3D objects made on the first use); false
 * when there is nothing to draw on (then no end). */
bool begin();
/* The back buffer's size (during a panel's drawing). */
void size(int *w, int *h);
void rect(float x, float y, float w, float h, uint32_t color);
/* A character cell is 6 x 9 font pixels of s screen pixels. */
float text_width(const char *str, int s);
/* The text at (x, y), upper case (lower case shown so, others as blanks).
 * Returns the x after it. */
float text(float x, float y, int s, const char *str, uint32_t color);
/* Draws what was built on the back buffer. */
void end(ID3D11RenderTargetView *backbuffer);

} // namespace overlay
