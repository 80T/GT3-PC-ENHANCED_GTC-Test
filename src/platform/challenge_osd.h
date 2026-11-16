/* challenge_osd.h - PC (2026-11-14, no original): the challenge modes' panel
 * (game/app/pc_challenge.h): over the bottom left corner of the picture, in
 * the GT mode's menus and races, while a challenge is on - each challenge on
 * by its name (that it is active) and its values.  Drawn with the panels
 * (platform/overlay.h) in their 5 x 7 font, before the settings menu (which
 * covers it).  Whether a challenge is on is read when drawn
 * (settings::challenge): the settings menu's change shows at once.  No
 * Windows headers: game code can include it. */
#pragma once

struct ID3D11RenderTargetView;

namespace challenge_osd {

constexpr int kStatusLines = 2;

/* The game's thread: the panel's title (the day) and challenge c's values
 * (settings::Challenge order; lines nullptr or "": none), kept until set
 * again. */
void set_title(const char *title);
void set_status(int c, const char *const lines[kStatusLines]);
/* The game's thread: whether what runs shows the panel (the GT mode's menus
 * and races). */
void set_shown(bool shown);

/* The panel over the picture (the present, as the other overlays). */
void present(ID3D11RenderTargetView *backbuffer);

} // namespace challenge_osd
