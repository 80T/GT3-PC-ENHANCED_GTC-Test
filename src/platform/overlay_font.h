/* overlay_font.h - the overlay's own font (platform/overlay.h): the 5 x 7
 * capitals (rows top down, bit 4 the left column), the accented ones of the
 * interface's languages and Cyrillic in the same style (an accent in rows -3
 * and -2, a cedilla or ogonek in 7 and 8).  Every other character the overlay
 * draws from MS Gothic's 12 pixel bitmaps (overlay.cpp: GDI).  No Windows or
 * D3D headers: tools/lang_sync links it too, to tell which characters a
 * panel's translation may use.  PC (2026-11-16): moved out of overlay.cpp. */
#pragma once

#include <stdint.h>

namespace overlay_font {

/* A narrow glyph: rows -3 .. 8 (index 0 .. 11), 5 columns. */
struct Glyph {
    uint8_t rows[12];
};

/* The upper case of a lower case letter (Latin-1, Latin Extended-A,
 * Cyrillic); others as they are. */
uint32_t upper(uint32_t c);
/* The font's glyph of an (upper case) character; false: it has none (an
 * ASCII one is drawn as a blank, any other from MS Gothic). */
bool narrow_glyph(uint32_t c, Glyph *g);
/* The ranges MS Gothic's bitmaps cover for the interface's languages - kana,
 * kanji, the full-width forms: two cells wide (the half-width katakana one). */
bool wide_range(uint32_t c);

} // namespace overlay_font
