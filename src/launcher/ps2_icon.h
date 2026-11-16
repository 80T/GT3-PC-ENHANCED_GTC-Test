/* ps2_icon.h - a PS2 save's 3D icon (its .ico file, lit by its icon.sys) as a
 * picture, for the save manager's list (save_manager.h).
 *
 * PC (2026-11-15).  The game's file screens draw it (T077 icon_model_render /
 * icon_model_draw): the mesh morphed by its first animation, its vertex
 * colours lit by icon.sys's three lights and ambient (icon_light_vertex),
 * times its 128 x 128 16-bit texture (MODULATE), seen through a frustum of
 * +-11 at 50 with the model at (0, -10, -50) and 1/1024 to a unit, y and z
 * flipped.  Here the same at time 0 (not turning), drawn in software at 4 x 4
 * samples a pixel, on a clear background (alpha), premultiplied BGRA. */
#pragma once

#include <cstdint>
#include <vector>

namespace ps2_icon {

/* `ico`: the .ico file; `iconSys`: its icon.sys (0x3C4 bytes; nullptr or
 * shorter: the vertex colours as they are).  size x size pixels into *bgra;
 * false (nothing written) when it is not an icon the game would draw. */
bool render(const std::vector<uint8_t> &ico, const std::vector<uint8_t> *iconSys, int size,
            std::vector<uint32_t> *bgra);

} // namespace ps2_icon
