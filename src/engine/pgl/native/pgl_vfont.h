/* pgl_vfont.h - PC (ROADMAP 36, no original): vector fonts, TrueType /
 * OpenType replacements of the bitmap fonts (docs/fonts.md).
 *
 * A font-set file (.fnt: faces of pre-rasterized glyphs, T496 Font::loadFile)
 * and the kanji font file (.kf, Font::loadKanjiFont) may have a folder of the
 * same name beside them - font/system.fnt and font/system.kf: font/system/ -
 * read through the data folder and the mod folders (platform/host_file.h).
 * In it:
 *   <face>.ttf / .otf / .ttc   the face <face> of the .fnt ("sw_12", "rank")
 *   default.ttf / .otf / .ttc  the faces without a file of their own
 *   kanji.ttf / .otf / .ttc    the .kf's characters (the two-byte JIS codes)
 * A face with a font is drawn from it: at the height of the original (its
 * "H", or "0" for a face of digits, as tall as the original's - the white
 * fill, the outline aside - and on the same baseline), each character as
 * wide as the font makes it, the font's own kerning pairs; the lines, the
 * alignment and the kanji cells keep the original's metrics.  The digits
 * and the time display's dash (0x7F, race_format_time's "-'--.---") are
 * one cell wide instead: the font's widest digit, made whole output pixels
 * where they are drawn, the glyph in its middle, never kerned - a time
 * keeps its width as it counts.  Its two
 * palettes are made from the one font: #0 white with the original's black
 * outline of one GS pixel, #1 plain white.  A character the font lacks
 * falls back to the original glyph (the PlayStation buttons, ...).  Without
 * the folder, or a file in it, nothing changes.
 *
 * The glyphs are rasterized when first used (DirectWrite's outlines, a
 * coverage rasterizer) at the output scale - native pixels per GS pixel of
 * the frames' height, pglr_text_scale; again when it changes - into pages of
 * 256 x 1024 GS texels per face and palette, host images (pgl_bridge.h) the
 * renderer samples bilinear.  The game thread only (the renderer reads the
 * pages under their locks).
 *
 * Plain C types: the decompiled side (pgl_font.cpp, T496, T498) calls it. */
#ifndef PGL_NATIVE_PGL_VFONT_H
#define PGL_NATIVE_PGL_VFONT_H

#include <stdint.h>

extern "C" {

/* A glyph of a vector face, GS pixels at scale 1, from the pen (the point
 * the game's pen is at: the original's baseline reference): its cell (x0, y0;
 * w x h, w 0: nothing drawn), the pen's move, the cell's first texel (u, v)
 * in its page and the page's TEX0 in the palette selected; id: its kerning
 * (pgl_vfont_kern), never 0; tab: a digit or the time's dash, its advance
 * the digits' cell (pgl_vfont_advance). */
typedef struct PglVGlyph {
    float    x0, y0;
    int32_t  w, h;
    float    advance;
    int32_t  u, v;
    uint64_t tex0;
    uint32_t id;
    int32_t  tab;
} PglVGlyph;

/* Font::loadFile: the font-set file at `path' (the game's: "font/menu.fnt")
 * is loaded and relocated at fontSet (G001 FontSet). */
void pgl_vfont_set_loaded(const char *path, const void *fontSet);
/* Font::releaseFile: its faces are gone. */
void pgl_vfont_set_released(const void *fontSet);
/* Font::loadKanjiFont: the kanji font file at `path' is the Font's (NULL:
 * released). */
void pgl_vfont_kanji_loaded(const char *path);
/* font_set: the vector side of the face (G001 PglFont), NULL when it has
 * none - neither a font of its own nor the kanji font. */
void *pgl_vfont_face(const void *pglFont);
/* Font::begin: the palette (the style's "#n") the glyphs' TEX0 are of. */
void pgl_vfont_palette(void *vf, int palette);
/* The glyph of Unicode `code' (font_putc's code: ASCII, ISO 8859-n from the
 * kana pages, 0x80..0x9F as Windows-1252); 0 when the face's font lacks it
 * (or it has none): the original glyph is drawn. */
int pgl_vfont_glyph(void *vf, int code, PglVGlyph *out);
/* The glyph of the JIS code (row / cell, font_put_kanji's): from the kanji
 * font in the face's kanji cell, else the face's font; 0: neither has it. */
int pgl_vfont_kanji(void *vf, int jis, PglVGlyph *out);
/* The kerning between two glyphs drawn one after the other (GS pixels,
 * added to the first one's advance); 0 when they are of different fonts, or
 * either is a tab glyph. */
float pgl_vfont_kern(void *vf, uint32_t prev, uint32_t next);
/* The pen's move for glyph g drawn at xScale with spacingX after it (as
 * font_putc moves it: advance * xScale + spacingX): its advance, else for a
 * tab glyph the cell widened so that the move is whole output pixels
 * (pglr_text_scale_x); *shift: the glyph's move to the middle of that (GS
 * pixels at scale 1, whole output pixels too). */
float pgl_vfont_advance(const PglVGlyph *g, float xScale, float spacingX, float *shift);

}  /* extern "C" */

#endif /* PGL_NATIVE_PGL_VFONT_H */
