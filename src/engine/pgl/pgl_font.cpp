/* pgl_font.cpp - pgl's bitmap text renderer and printf engine: the
 * decompiled font.c (unit u114, 0x00249B28..0x0024B0D8), fontprint.c
 * (0x0024B0D8..0x0024B5B0), fontfmt.c (0x0024D128..0x0024D4F8),
 * format_part1.c (0x0024D4F8..0x0024D8C0) and format_part2.c
 * (0x0024D8C0..0x0024DF28), ported.
 *
 * The text renderer writes its GIF packets into the packet buffer as the
 * decomp does (sprites in REGLIST runs, the font texture's registers by
 * A+D, the kanji glyphs uploaded one at a time and drawn through GS context
 * 2).  PC: font_flush hands each closed packet to the renderer (pglr_gif:
 * decoded when the frame executes); the kanji uploads are registry records;
 * the MMI kerning loop and the VU0 projection of font_locate_3d are C++.
 *
 * PC: the printf engine reads its arguments with va_arg.  The PS2 build
 * walked 8-byte EE argument slots (format_part2.c); MSVC's x86 va_list
 * passes ints and pointers in 4 bytes and floats as doubles, so the slot
 * walk is replaced, conversion by conversion (docs/runtime.md). */
#include "engine/pgl/pgl_port.h"
#include "engine/pgl/native/pgl_vfont.h"        /* PC: vector fonts (ROADMAP 36) */
#include "engine/pgl/native/pgl_state_prim.h"   /* PC: fullColor: text in pgl_color's colour whole */

/* EE COP1 max.s */
static inline f32 u114_maxf(f32 a, f32 b) { return a > b ? a : b; }

/* The kerning between two glyphs: the smallest gap between the previous
 * glyph's right profile and this glyph's left profile, as the MMI loop of
 * the original computes it (pgl_font_internal.h u114_glyph_kerning):
 * per 16-row block the saturated byte sums right + left, rows past the
 * profile end saturated up by 0x7F; even bytes taken & 0x7F, odd bytes as
 * unsigned; the minimum (from 0x80) & 0x7F. */
static int glyph_kerning(const s8 *left, const s8 *right, s32 rows)
{
    int k = 0x80;

    while (rows > 0) {
        for (int i = 0; i < 16; i++) {
            int s = (int)left[i] + (int)right[i];
            int h;

            if (s > 127)
                s = 127;
            if (s < -128)
                s = -128;
            if (rows <= 16 && i >= rows) {
                s += 0x7F;
                if (s > 127)
                    s = 127;
            }
            h = (i & 1) ? (int)(u8)(s8)s : ((int)(u8)(s8)s & 0x7F);
            if (h < k)
                k = h;
        }
        left += 16;
        right += 16;
        rows -= 16;
    }
    return k & 0x7F;
}

/* ======================================================================== */
/* Data                                                                     */
/* ======================================================================== */

/* The code pages of the two-byte codes 0x80xx..0x8Fxx (kana_page_lookup):
 * byte 2 - 0x20 -> the code of the bitmap font (Unicode), 0xFFFF none.
 * Page 0x80 + n is ISO 8859-n (n >= 1).  Only g_kanaPages points at them,
 * so the file's .data starts unreferenced here (the anchor). */
// DATA_START 0x002F5D60

/* page 0x80: Latin-1 with the C1 range (0x81, 0x8D, 0x8F, 0x90, 0x9D unmapped) */
static u16 g_002F5D60[224] = {   /* 0x002F5D60 */
    0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027, 0x0028, 0x0029, 0x002A, 0x002B,
    0x002C, 0x002D, 0x002E, 0x002F, 0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F, 0x0040, 0x0041, 0x0042, 0x0043,
    0x0044, 0x0045, 0x0046, 0x0047, 0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057, 0x0058, 0x0059, 0x005A, 0x005B,
    0x005C, 0x005D, 0x005E, 0x005F, 0x0060, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F, 0x0070, 0x0071, 0x0072, 0x0073,
    0x0074, 0x0075, 0x0076, 0x0077, 0x0078, 0x0079, 0x007A, 0x007B, 0x007C, 0x007D, 0x007E, 0xFFFF,
    0x0080, 0xFFFF, 0x0082, 0x0083, 0x0084, 0x0085, 0x0086, 0x0087, 0x0088, 0x0089, 0x008A, 0x008B,
    0x008C, 0xFFFF, 0x008E, 0xFFFF, 0xFFFF, 0x0091, 0x0092, 0x0093, 0x0094, 0x0095, 0x0096, 0x0097,
    0x0098, 0x0099, 0x009A, 0x009B, 0x009C, 0xFFFF, 0x009E, 0x009F, 0x00A0, 0x00A1, 0x00A2, 0x00A3,
    0x00A4, 0x00A5, 0x00A6, 0x00A7, 0x00A8, 0x00A9, 0x00AA, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF,
    0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7, 0x00B8, 0x00B9, 0x00BA, 0x00BB,
    0x00BC, 0x00BD, 0x00BE, 0x00BF, 0x00C0, 0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x00C7,
    0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF, 0x00D0, 0x00D1, 0x00D2, 0x00D3,
    0x00D4, 0x00D5, 0x00D6, 0x00D7, 0x00D8, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x00DD, 0x00DE, 0x00DF,
    0x00E0, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x00E7, 0x00E8, 0x00E9, 0x00EA, 0x00EB,
    0x00EC, 0x00ED, 0x00EE, 0x00EF, 0x00F0, 0x00F1, 0x00F2, 0x00F3, 0x00F4, 0x00F5, 0x00F6, 0x00F7,
    0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x00FD, 0x00FE, 0x00FF,
};
/* page 0x81: ISO 8859-1 (also pages 0x8A..0x8D: 8859-10..13 are not supported) */
static u16 g_002F5F20[224] = {   /* 0x002F5F20 */
    0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027, 0x0028, 0x0029, 0x002A, 0x002B,
    0x002C, 0x002D, 0x002E, 0x002F, 0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F, 0x0040, 0x0041, 0x0042, 0x0043,
    0x0044, 0x0045, 0x0046, 0x0047, 0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057, 0x0058, 0x0059, 0x005A, 0x005B,
    0x005C, 0x005D, 0x005E, 0x005F, 0x0060, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F, 0x0070, 0x0071, 0x0072, 0x0073,
    0x0074, 0x0075, 0x0076, 0x0077, 0x0078, 0x0079, 0x007A, 0x007B, 0x007C, 0x007D, 0x007E, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x00A0, 0x00A1, 0x00A2, 0x00A3,
    0x00A4, 0x00A5, 0x00A6, 0x00A7, 0x00A8, 0x00A9, 0x00AA, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF,
    0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7, 0x00B8, 0x00B9, 0x00BA, 0x00BB,
    0x00BC, 0x00BD, 0x00BE, 0x00BF, 0x00C0, 0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x00C7,
    0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF, 0x00D0, 0x00D1, 0x00D2, 0x00D3,
    0x00D4, 0x00D5, 0x00D6, 0x00D7, 0x00D8, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x00DD, 0x00DE, 0x00DF,
    0x00E0, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x00E7, 0x00E8, 0x00E9, 0x00EA, 0x00EB,
    0x00EC, 0x00ED, 0x00EE, 0x00EF, 0x00F0, 0x00F1, 0x00F2, 0x00F3, 0x00F4, 0x00F5, 0x00F6, 0x00F7,
    0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x00FD, 0x00FE, 0x00FF,
};
/* page 0x82: ISO 8859-2 */
static u16 g_002F60E0[224] = {   /* 0x002F60E0 */
    0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027, 0x0028, 0x0029, 0x002A, 0x002B,
    0x002C, 0x002D, 0x002E, 0x002F, 0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F, 0x0040, 0x0041, 0x0042, 0x0043,
    0x0044, 0x0045, 0x0046, 0x0047, 0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057, 0x0058, 0x0059, 0x005A, 0x005B,
    0x005C, 0x005D, 0x005E, 0x005F, 0x0060, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F, 0x0070, 0x0071, 0x0072, 0x0073,
    0x0074, 0x0075, 0x0076, 0x0077, 0x0078, 0x0079, 0x007A, 0x007B, 0x007C, 0x007D, 0x007E, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x00A0, 0x0104, 0x02D8, 0x0141,
    0x00A4, 0x013D, 0x015A, 0x00A7, 0x00A8, 0x0160, 0x015E, 0x0164, 0x0179, 0x00AD, 0x017D, 0x017B,
    0x00B0, 0x0105, 0x02DB, 0x0142, 0x00B4, 0x013E, 0x015B, 0x02C7, 0x00B8, 0x0161, 0x015F, 0x0165,
    0x017A, 0x02DD, 0x017E, 0x017C, 0x0154, 0x00C1, 0x00C2, 0x0102, 0x00C4, 0x0139, 0x0106, 0x00C7,
    0x010C, 0x00C9, 0x0118, 0x00CB, 0x011A, 0x00CD, 0x00CE, 0x010E, 0x0110, 0x0143, 0x0147, 0x00D3,
    0x00D4, 0x0150, 0x00D6, 0x00D7, 0x0158, 0x016E, 0x00DA, 0x0170, 0x00DC, 0x00DD, 0x0162, 0x00DF,
    0x0155, 0x00E1, 0x00E2, 0x0103, 0x00E4, 0x013A, 0x0107, 0x00E7, 0x010D, 0x00E9, 0x0119, 0x00EB,
    0x011B, 0x00ED, 0x00EE, 0x010F, 0x0111, 0x0144, 0x0148, 0x00F3, 0x00F4, 0x0151, 0x00F6, 0x00F7,
    0x0159, 0x016F, 0x00FA, 0x0171, 0x00FC, 0x00FD, 0x0163, 0x02D9,
};
/* page 0x83: ISO 8859-3 */
static u16 g_002F62A0[224] = {   /* 0x002F62A0 */
    0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027, 0x0028, 0x0029, 0x002A, 0x002B,
    0x002C, 0x002D, 0x002E, 0x002F, 0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F, 0x0040, 0x0041, 0x0042, 0x0043,
    0x0044, 0x0045, 0x0046, 0x0047, 0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057, 0x0058, 0x0059, 0x005A, 0x005B,
    0x005C, 0x005D, 0x005E, 0x005F, 0x0060, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F, 0x0070, 0x0071, 0x0072, 0x0073,
    0x0074, 0x0075, 0x0076, 0x0077, 0x0078, 0x0079, 0x007A, 0x007B, 0x007C, 0x007D, 0x007E, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x00A0, 0x0126, 0x02D8, 0x00A3,
    0x00A4, 0xFFFF, 0x0124, 0x00A7, 0x00A8, 0x0130, 0x015E, 0x011E, 0x0134, 0x00AD, 0xFFFF, 0x017B,
    0x00B0, 0x0127, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x0125, 0x00B7, 0x00B8, 0x0131, 0x015F, 0x011F,
    0x0135, 0x00BD, 0xFFFF, 0x017C, 0x00C0, 0x00C1, 0x00C2, 0xFFFF, 0x00C4, 0x010A, 0x0108, 0x00C7,
    0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF, 0xFFFF, 0x00D1, 0x00D2, 0x00D3,
    0x00D4, 0x0120, 0x00D6, 0x00D7, 0x011C, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x016C, 0x015C, 0x00DF,
    0x00E0, 0x00E1, 0x00E2, 0xFFFF, 0x00E4, 0x010B, 0x0109, 0x00E7, 0x00E8, 0x00E9, 0x00EA, 0x00EB,
    0x00EC, 0x00ED, 0x00EE, 0x00EF, 0xFFFF, 0x00F1, 0x00F2, 0x00F3, 0x00F4, 0x0121, 0x00F6, 0x00F7,
    0x011D, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x016D, 0x015D, 0x02D9,
};
/* page 0x84: ISO 8859-4 */
static u16 g_002F6460[224] = {   /* 0x002F6460 */
    0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027, 0x0028, 0x0029, 0x002A, 0x002B,
    0x002C, 0x002D, 0x002E, 0x002F, 0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F, 0x0040, 0x0041, 0x0042, 0x0043,
    0x0044, 0x0045, 0x0046, 0x0047, 0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057, 0x0058, 0x0059, 0x005A, 0x005B,
    0x005C, 0x005D, 0x005E, 0x005F, 0x0060, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F, 0x0070, 0x0071, 0x0072, 0x0073,
    0x0074, 0x0075, 0x0076, 0x0077, 0x0078, 0x0079, 0x007A, 0x007B, 0x007C, 0x007D, 0x007E, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x00A0, 0x0104, 0x0138, 0x0156,
    0x00A4, 0x0128, 0x013B, 0x00A7, 0x00A8, 0x0160, 0x0112, 0x0122, 0x0166, 0x00AD, 0x017D, 0x00AF,
    0x00B0, 0x0105, 0x02DB, 0x0157, 0x00B4, 0x0129, 0x013C, 0x02C7, 0x00B8, 0x0161, 0x0113, 0x0123,
    0x0167, 0x014A, 0x017E, 0x014B, 0x0100, 0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x012E,
    0x010C, 0x00C9, 0x0118, 0x00CB, 0x0116, 0x00CD, 0x00CE, 0x012A, 0x0110, 0x0145, 0x014C, 0x0136,
    0x00D4, 0x00D5, 0x00D6, 0x00D7, 0x00D8, 0x0172, 0x00DA, 0x00DB, 0x00DC, 0x0168, 0x016A, 0x00DF,
    0x0101, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x012F, 0x010D, 0x00E9, 0x0119, 0x00EB,
    0x0117, 0x00ED, 0x00EE, 0x012B, 0x0111, 0x0146, 0x014D, 0x0137, 0x00F4, 0x00F5, 0x00F6, 0x00F7,
    0x00F8, 0x0173, 0x00FA, 0x00FB, 0x00FC, 0x0169, 0x016B, 0x02D9,
};
/* page 0x85: ISO 8859-5 (Cyrillic) */
static u16 g_002F6620[224] = {   /* 0x002F6620 */
    0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027, 0x0028, 0x0029, 0x002A, 0x002B,
    0x002C, 0x002D, 0x002E, 0x002F, 0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F, 0x0040, 0x0041, 0x0042, 0x0043,
    0x0044, 0x0045, 0x0046, 0x0047, 0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057, 0x0058, 0x0059, 0x005A, 0x005B,
    0x005C, 0x005D, 0x005E, 0x005F, 0x0060, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F, 0x0070, 0x0071, 0x0072, 0x0073,
    0x0074, 0x0075, 0x0076, 0x0077, 0x0078, 0x0079, 0x007A, 0x007B, 0x007C, 0x007D, 0x007E, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x00A0, 0x0401, 0x0402, 0x0403,
    0x0404, 0x0405, 0x0406, 0x0407, 0x0408, 0x0409, 0x040A, 0x040B, 0x040C, 0x00AD, 0x040E, 0x040F,
    0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417, 0x0418, 0x0419, 0x041A, 0x041B,
    0x041C, 0x041D, 0x041E, 0x041F, 0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427,
    0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F, 0x0430, 0x0431, 0x0432, 0x0433,
    0x0434, 0x0435, 0x0436, 0x0437, 0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F,
    0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447, 0x0448, 0x0449, 0x044A, 0x044B,
    0x044C, 0x044D, 0x044E, 0x044F, 0x2116, 0x0451, 0x0452, 0x0453, 0x0454, 0x0455, 0x0456, 0x0457,
    0x0458, 0x0459, 0x045A, 0x045B, 0x045C, 0x00A7, 0x045E, 0x045F,
};
/* page 0x86: ISO 8859-6 (Arabic), 0x20..0xF2 */
static u16 g_002F67E0[211] = {   /* 0x002F67E0 */
    0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027, 0x0028, 0x0029, 0x002A, 0x002B,
    0x002C, 0x002D, 0x002E, 0x002F, 0x0660, 0x0661, 0x0662, 0x0663, 0x0664, 0x0665, 0x0666, 0x0667,
    0x0668, 0x0669, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F, 0x0040, 0x0041, 0x0042, 0x0043,
    0x0044, 0x0045, 0x0046, 0x0047, 0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057, 0x0058, 0x0059, 0x005A, 0x005B,
    0x005C, 0x005D, 0x005E, 0x005F, 0x0060, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F, 0x0070, 0x0071, 0x0072, 0x0073,
    0x0074, 0x0075, 0x0076, 0x0077, 0x0078, 0x0079, 0x007A, 0x007B, 0x007C, 0x007D, 0x007E, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x00A0, 0xFFFF, 0xFFFF, 0xFFFF,
    0x00A4, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x060C, 0x00AD, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x061B,
    0xFFFF, 0xFFFF, 0xFFFF, 0x061F, 0xFFFF, 0x0621, 0x0622, 0x0623, 0x0624, 0x0625, 0x0626, 0x0627,
    0x0628, 0x0629, 0x062A, 0x062B, 0x062C, 0x062D, 0x062E, 0x062F, 0x0630, 0x0631, 0x0632, 0x0633,
    0x0634, 0x0635, 0x0636, 0x0637, 0x0638, 0x0639, 0x063A, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0x0640, 0x0641, 0x0642, 0x0643, 0x0644, 0x0645, 0x0646, 0x0647, 0x0648, 0x0649, 0x064A, 0x064B,
    0x064C, 0x064D, 0x064E, 0x064F, 0x0650, 0x0651, 0x0652,
};
/* page 0x87: ISO 8859-7 (Greek), 0x20..0xFE */
static u16 g_002F6988[223] = {   /* 0x002F6988 */
    0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027, 0x0028, 0x0029, 0x002A, 0x002B,
    0x002C, 0x002D, 0x002E, 0x002F, 0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F, 0x0040, 0x0041, 0x0042, 0x0043,
    0x0044, 0x0045, 0x0046, 0x0047, 0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057, 0x0058, 0x0059, 0x005A, 0x005B,
    0x005C, 0x005D, 0x005E, 0x005F, 0x0060, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F, 0x0070, 0x0071, 0x0072, 0x0073,
    0x0074, 0x0075, 0x0076, 0x0077, 0x0078, 0x0079, 0x007A, 0x007B, 0x007C, 0x007D, 0x007E, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x00A0, 0x02BD, 0x02BC, 0x00A3,
    0xFFFF, 0xFFFF, 0x00A6, 0x00A7, 0x00A8, 0x00A9, 0xFFFF, 0x00AB, 0x00AC, 0x00AD, 0xFFFF, 0x2015,
    0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x0384, 0x0385, 0x0386, 0x00B7, 0x0388, 0x0389, 0x038A, 0x00BB,
    0x038C, 0x00BD, 0x038E, 0x038F, 0x0390, 0x0391, 0x0392, 0x0393, 0x0394, 0x0395, 0x0396, 0x0397,
    0x0398, 0x0399, 0x039A, 0x039B, 0x039C, 0x039D, 0x039E, 0x039F, 0x03A0, 0x03A1, 0xFFFF, 0x03A3,
    0x03A4, 0x03A5, 0x03A6, 0x03A7, 0x03A8, 0x03A9, 0x03AA, 0x03AB, 0x03AC, 0x03AD, 0x03AE, 0x03AF,
    0x03B0, 0x03B1, 0x03B2, 0x03B3, 0x03B4, 0x03B5, 0x03B6, 0x03B7, 0x03B8, 0x03B9, 0x03BA, 0x03BB,
    0x03BC, 0x03BD, 0x03BE, 0x03BF, 0x03C0, 0x03C1, 0x03C2, 0x03C3, 0x03C4, 0x03C5, 0x03C6, 0x03C7,
    0x03C8, 0x03C9, 0x03CA, 0x03CB, 0x03CC, 0x03CD, 0x03CE,
};
/* page 0x88: ISO 8859-8 (Hebrew), 0x20..0xFA */
static u16 g_002F6B48[219] = {   /* 0x002F6B48 */
    0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027, 0x0028, 0x0029, 0x002A, 0x002B,
    0x002C, 0x002D, 0x002E, 0x002F, 0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F, 0x0040, 0x0041, 0x0042, 0x0043,
    0x0044, 0x0045, 0x0046, 0x0047, 0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057, 0x0058, 0x0059, 0x005A, 0x005B,
    0x005C, 0x005D, 0x005E, 0x005F, 0x0060, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F, 0x0070, 0x0071, 0x0072, 0x0073,
    0x0074, 0x0075, 0x0076, 0x0077, 0x0078, 0x0079, 0x007A, 0x007B, 0x007C, 0x007D, 0x007E, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x00A0, 0xFFFF, 0x00A2, 0x00A3,
    0x00A4, 0x00A5, 0x00A6, 0x00A7, 0x00A8, 0x00A9, 0x00D7, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x203E,
    0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7, 0x00B8, 0x00B9, 0x00F7, 0x00BB,
    0x00BC, 0x00BD, 0x00BE, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x2017,
    0x05D0, 0x05D1, 0x05D2, 0x05D3, 0x05D4, 0x05D5, 0x05D6, 0x05D7, 0x05D8, 0x05D9, 0x05DA, 0x05DB,
    0x05DC, 0x05DD, 0x05DE, 0x05DF, 0x05E0, 0x05E1, 0x05E2, 0x05E3, 0x05E4, 0x05E5, 0x05E6, 0x05E7,
    0x05E8, 0x05E9, 0x05EA,
};
/* page 0x89: ISO 8859-9 */
static u16 g_002F6D00[224] = {   /* 0x002F6D00 */
    0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027, 0x0028, 0x0029, 0x002A, 0x002B,
    0x002C, 0x002D, 0x002E, 0x002F, 0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F, 0x0040, 0x0041, 0x0042, 0x0043,
    0x0044, 0x0045, 0x0046, 0x0047, 0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057, 0x0058, 0x0059, 0x005A, 0x005B,
    0x005C, 0x005D, 0x005E, 0x005F, 0x0060, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F, 0x0070, 0x0071, 0x0072, 0x0073,
    0x0074, 0x0075, 0x0076, 0x0077, 0x0078, 0x0079, 0x007A, 0x007B, 0x007C, 0x007D, 0x007E, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x00A0, 0x00A1, 0x00A2, 0x00A3,
    0x00A4, 0x00A5, 0x00A6, 0x00A7, 0x00A8, 0x00A9, 0x00AA, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF,
    0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7, 0x00B8, 0x00B9, 0x00BA, 0x00BB,
    0x00BC, 0x00BD, 0x00BE, 0x00BF, 0x00C0, 0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x00C7,
    0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF, 0x011E, 0x00D1, 0x00D2, 0x00D3,
    0x00D4, 0x00D5, 0x00D6, 0x00D7, 0x00D8, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x0130, 0x015E, 0x00DF,
    0x00E0, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x00E7, 0x00E8, 0x00E9, 0x00EA, 0x00EB,
    0x00EC, 0x00ED, 0x00EE, 0x00EF, 0x011F, 0x00F1, 0x00F2, 0x00F3, 0x00F4, 0x00F5, 0x00F6, 0x00F7,
    0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x0131, 0x015F, 0x00FF,
};
/* page 0x8E: ISO 8859-14 */
static u16 g_002F6EC0[224] = {   /* 0x002F6EC0 */
    0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027, 0x0028, 0x0029, 0x002A, 0x002B,
    0x002C, 0x002D, 0x002E, 0x002F, 0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F, 0x0040, 0x0041, 0x0042, 0x0043,
    0x0044, 0x0045, 0x0046, 0x0047, 0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057, 0x0058, 0x0059, 0x005A, 0x005B,
    0x005C, 0x005D, 0x005E, 0x005F, 0x0060, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F, 0x0070, 0x0071, 0x0072, 0x0073,
    0x0074, 0x0075, 0x0076, 0x0077, 0x0078, 0x0079, 0x007A, 0x007B, 0x007C, 0x007D, 0x007E, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x00A0, 0x1E02, 0x1E03, 0x00A3,
    0x010A, 0x010B, 0x1E0A, 0x00A7, 0x1E80, 0x00A9, 0x1E82, 0x1E0B, 0x1EF2, 0x00AD, 0x00AE, 0x0178,
    0x1E1E, 0x1E1F, 0x0120, 0x0121, 0x1E40, 0x1E41, 0x00B6, 0x1E56, 0x1E81, 0x1E57, 0x1E83, 0x1E60,
    0x1EF3, 0x1E84, 0x1E85, 0x1E61, 0x00C0, 0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x00C7,
    0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF, 0x0174, 0x00D1, 0x00D2, 0x00D3,
    0x00D4, 0x00D5, 0x00D6, 0x1E6A, 0x00D8, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x00DD, 0x0176, 0x00DF,
    0x00E0, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x00E7, 0x00E8, 0x00E9, 0x00EA, 0x00EB,
    0x00EC, 0x00ED, 0x00EE, 0x00EF, 0x0175, 0x00F1, 0x00F2, 0x00F3, 0x00F4, 0x00F5, 0x00F6, 0x1E6B,
    0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x00FD, 0x0177, 0x00FF,
};
/* page 0x8F: ISO 8859-15 */
static u16 g_002F7080[224] = {   /* 0x002F7080 */
    0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027, 0x0028, 0x0029, 0x002A, 0x002B,
    0x002C, 0x002D, 0x002E, 0x002F, 0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F, 0x0040, 0x0041, 0x0042, 0x0043,
    0x0044, 0x0045, 0x0046, 0x0047, 0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057, 0x0058, 0x0059, 0x005A, 0x005B,
    0x005C, 0x005D, 0x005E, 0x005F, 0x0060, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F, 0x0070, 0x0071, 0x0072, 0x0073,
    0x0074, 0x0075, 0x0076, 0x0077, 0x0078, 0x0079, 0x007A, 0x007B, 0x007C, 0x007D, 0x007E, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x00A0, 0x00A1, 0x00A2, 0x00A3,
    0x0080, 0x00A5, 0x0160, 0x00A7, 0x0161, 0x00A9, 0x00AA, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF,
    0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x017D, 0x00B5, 0x00B6, 0x00B7, 0x017E, 0x00B9, 0x00BA, 0x00BB,
    0x0152, 0x0153, 0x0178, 0x00BF, 0x00C0, 0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x00C7,
    0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF, 0x00D0, 0x00D1, 0x00D2, 0x00D3,
    0x00D4, 0x00D5, 0x00D6, 0x00D7, 0x00D8, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x00DD, 0x00DE, 0x00DF,
    0x00E0, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x00E7, 0x00E8, 0x00E9, 0x00EA, 0x00EB,
    0x00EC, 0x00ED, 0x00EE, 0x00EF, 0x00F0, 0x00F1, 0x00F2, 0x00F3, 0x00F4, 0x00F5, 0x00F6, 0x00F7,
    0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x00FD, 0x00FE, 0x00FF,
};

KanaPage g_kanaPages[16] = {     /* 0x002F7240 */
    { 0x20, 0xFF, 0, g_002F5D60 },   /* 0x80 */
    { 0x20, 0xFF, 0, g_002F5F20 },   /* 0x81 */
    { 0x20, 0xFF, 0, g_002F60E0 },   /* 0x82 */
    { 0x20, 0xFF, 0, g_002F62A0 },   /* 0x83 */
    { 0x20, 0xFF, 0, g_002F6460 },   /* 0x84 */
    { 0x20, 0xFF, 0, g_002F6620 },   /* 0x85 */
    { 0x20, 0xF2, 0, g_002F67E0 },   /* 0x86 */
    { 0x20, 0xFE, 0, g_002F6988 },   /* 0x87 */
    { 0x20, 0xFA, 0, g_002F6B48 },   /* 0x88 */
    { 0x20, 0xFF, 0, g_002F6D00 },   /* 0x89 */
    { 0x20, 0xFF, 0, g_002F5F20 },   /* 0x8A */
    { 0x20, 0xFF, 0, g_002F5F20 },   /* 0x8B */
    { 0x20, 0xFF, 0, g_002F5F20 },   /* 0x8C */
    { 0x20, 0xFF, 0, g_002F5F20 },   /* 0x8D */
    { 0x20, 0xFF, 0, g_002F6EC0 },   /* 0x8E */
    { 0x20, 0xFF, 0, g_002F7080 },   /* 0x8F */
};

/* The library state block (GsLibState, G001_flare.h): the texture / PRIM
 * override masks, the material colour scales, the text renderer's state. */
GsLibState g_002F72C0 = {          /* 0x002F72C0 */
    0xE000001800000000ull,          /* tex0Mask: CLD, TFX */
    0x1E0,                          /* tex1Mask: MMAG, MMIN */
    0,                              /* clampMask */
    0x60,                           /* primMask */
    1,                              /* unk_1A */
    0, { 0 },                       /* enable */
    { { 1.0f, 1.0f, 1.0f, 1.0f },   /* matScale */
      { 1.0f, 1.0f, 1.0f, 1.0f },
      { 1.0f, 1.0f, 1.0f, 1.0f },
      { 1.0f, 1.0f, 1.0f, 1.0f } },
    0, { 0 },                       /* matScaleOn */
    0, 0,                           /* fontTex0, fontTex1 */
    NULL, 0,                        /* font, align */
    -1,                             /* lastChar: none */
    0, 0x80808080,                  /* lastPrim, lastColor */
    NULL, NULL, NULL,               /* pkt, run, pktEnd */
    0.0f, 0.0f, 0.0f,               /* maxX, spacingX, spacingY */
    1.0f, 1.0f,                     /* scaleX, scaleY */
    0.0f, 0.0f, 0.0f, 0.0f,         /* startX, startY, startZ, startW */
    0.0f, 0.0f,                     /* curX, curY */
    { 0 }, 0, { 0 },                /* unk_C4, open */
    NULL, NULL, -1,                 /* kfont, kclut, kvram: none */
    0, { 0 },                       /* kinit */
    NULL                            /* out */
};

/* ======================================================================== */
/* font.c                                                                   */
/* ======================================================================== */

#define RELOC_P(p, d)       do { if ((p) != NULL) (p) = (__typeof__(p))((u8 *)(p) + (d)); } while (0)
#define RELOC_ALWAYS(p, d)  do { (p) = (__typeof__(p))((u8 *)(p) + (d)); } while (0)

// FUNC 0x00249B40 fontset_relocate
/* Relocates a font-set file in place: the textures (tex1_relocate), the
 * faces, their glyph tables and kerning profiles; each face gets its
 * texture. */
FontSet *fontset_relocate(FontSet *fs)
{
    s32 delta = (s32)((u8 *)fs - (u8 *)fs->self);
    int i;

    if (delta == 0)
        return fs;
    fs->self = fs;
    RELOC_P(fs->unk_08, delta);
    RELOC_P(fs->tex, delta);
    for (i = 0; i < fs->count; i++) {
        RELOC_ALWAYS(fs->tex[i], delta);
        tex1_relocate(fs->tex[i]);
    }
    RELOC_P(fs->fonts, delta);
    for (i = 0; i < fs->count; i++) {
        if (fs->fonts[i] != NULL) {
            PglFont *font;

            RELOC_P(fs->fonts[i], delta);
            font = fs->fonts[i];
            RELOC_P(font->name, delta);
            if (font->glyphs != NULL) {
                FontGlyph *g;
                int code, j = 0;

                RELOC_P(font->glyphs, delta);
                g = font->glyphs;
                for (code = font->first; code <= font->last; code++, j++) {
                    RELOC_P(g[j].left, delta);
                    RELOC_P(g[j].right, delta);
                }
            }
            fs->fonts[i]->tex = fs->tex[i];
        }
    }
    return fs;
}

// FUNC 0x00249CE8 kanjifile_relocate
KanjiFontFile *kanjifile_relocate(KanjiFontFile *f)
{
    s32 delta = (s32)((u8 *)f - (u8 *)f->self);
    int i, n;

    if (delta == 0)
        return f;
    f->self = f;
    RELOC_P(f->unk_08, delta);
    if (f->fonts != NULL) {
        RELOC_P(f->fonts, delta);
        n = f->count;
        for (i = 0; i < n; i++) {
            if (f->fonts[i] != NULL) {
                RELOC_P(f->fonts[i], delta);
                RELOC_P(f->fonts[i]->data, delta);
                RELOC_P(f->fonts[i]->map, delta);
                RELOC_P(f->fonts[i]->unk_14, delta);
            }
        }
    }
    RELOC_P(f->cluts, delta);
    return f;
}

/* PC (ROADMAP 36, vector fonts: engine/pgl/native/pgl_vfont.h): the face's
 * vector side (font_set; NULL: none), the TEX0 the open text packet's
 * context 1 holds (font_begin, font_use_tex0: a vector glyph's page, or the
 * face's own texture for the glyphs its font lacks), the last vector glyph
 * drawn or measured - its kerning, while lastChar is FONT_PC_LAST_VECTOR (a
 * pen moved or an original glyph drawn resets it, as for the original's
 * kerning). */
#define FONT_PC_LAST_VECTOR (-2)
static void *s_pcVf;
static u64 s_pcTex0;
static u32 s_pcLastId;

// FUNC 0x00249DB8 font_set
void font_set(const PglFont *font, const GsTexRegs *tex)
{
    GsLibState *st = &g_002F72C0;

    st->font = font;
    if (tex != NULL) {
        st->fontTex0 = tex->tex0;
        st->fontTex1 = tex->tex1;
    }
    st->lastChar = -1;
    s_pcVf = font != NULL ? pgl_vfont_face(font) : NULL;   /* PC */
}

/* PC (no original; G001_flare.h): Font::begin's palette ("#n") for the
 * vector glyphs. */
void font_set_palette(int palette)
{
    if (s_pcVf != NULL)
        pgl_vfont_palette(s_pcVf, palette);
}

// FUNC 0x00249DE8 font_set_kanji
void font_set_kanji(const KanjiFont *kf, const void *clut, s32 vram)
{
    GsLibState *st = &g_002F72C0;

    st->kfont = kf;
    st->kclut = clut;
    st->kvram = vram;
}

// FUNC 0x00249E00 font_set_param
void font_set_param(u32 param, f32 v)
{
    GsLibState *st = &g_002F72C0;

    switch (param) {
    case FONT_PARAM_SCALE_X:   st->scaleX = v;   break;
    case FONT_PARAM_SCALE_Y:   st->scaleY = v;   break;
    case FONT_PARAM_SPACING_X: st->spacingX = v; break;
    case FONT_PARAM_SPACING_Y: st->spacingY = v; break;
    }
}

// FUNC 0x00249E68 font_get_param
f32 font_get_param(u32 param)
{
    GsLibState *st = &g_002F72C0;

    switch (param) {
    case FONT_PARAM_SCALE_X:   return st->scaleX;
    case FONT_PARAM_SCALE_Y:   return st->scaleY;
    case FONT_PARAM_SPACING_X: return st->spacingX;
    case FONT_PARAM_SPACING_Y: return st->spacingY;
    }
    return 0.0f;
}

// FUNC 0x00249EE0 font_set_align
void font_set_align(s32 align)
{
    g_002F72C0.align = align;
}

/* PC (no original; G001_flare.h): the texts' squeeze, applied by the
 * outermost of font_vprintf / font_puts / font_putc / font_text_width
 * while it runs (the scale and spacing parameters times it, restored
 * after: what the game set and reads stays its own). */
static f32 s_pcSqueezeX = 1.0f, s_pcSqueezeY = 1.0f;
static int s_pcSqueezeDepth;

void font_set_squeeze(f32 kx, f32 ky)
{
    s_pcSqueezeX = kx > 0.0f ? kx : 1.0f;
    s_pcSqueezeY = ky > 0.0f ? ky : 1.0f;
}

namespace {
struct PcFontSqueeze {
    bool on;
    f32 scaleX, scaleY, spacingX, spacingY;
    PcFontSqueeze() : on(s_pcSqueezeDepth++ == 0 && (s_pcSqueezeX != 1.0f || s_pcSqueezeY != 1.0f))
    {
        if (!on)
            return;
        GsLibState *st = &g_002F72C0;
        scaleX = st->scaleX;
        scaleY = st->scaleY;
        spacingX = st->spacingX;
        spacingY = st->spacingY;
        st->scaleX *= s_pcSqueezeX;
        st->spacingX *= s_pcSqueezeX;
        st->scaleY *= s_pcSqueezeY;
        st->spacingY *= s_pcSqueezeY;
    }
    ~PcFontSqueeze()
    {
        s_pcSqueezeDepth--;
        if (!on)
            return;
        GsLibState *st = &g_002F72C0;
        st->scaleX = scaleX;
        st->scaleY = scaleY;
        st->spacingX = spacingX;
        st->spacingY = spacingY;
    }
};
} // namespace

// FUNC 0x00249EF0 font_set_start
/* Line start = draw-buffer offset + window position + (x, y) pixels (GS
 * pixel units, y halved for field rendering; the sums in double precision,
 * as the soft-float calls of the original). */
void font_set_start(s32 x, s32 y)
{
    GsLibState *st = &g_002F72C0;
    GsContext *ctx = GS_SPR_CTX;
    f32 yScale = 1.0f;
    f32 winX, winY;

    if (ctx->halfHeight)
        yScale = 0.5f;
    winX = (f32)ctx->winX;
    winY = (f32)ctx->winY;
    st->startX = (f32)((f64)(f32)ctx->tgtA.xoff * 0.0625 + (f64)winX + (f64)(f32)x);
    st->startY = (f32)((f64)(f32)ctx->tgtA.yoff * 0.0625 + (f64)winY + (f64)((f32)y * yScale));
    st->lastChar = -1;
}

// FUNC 0x0024A060 font_set_pen
void font_set_pen(s32 x, s32 y)
{
    GsLibState *st = &g_002F72C0;
    GsContext *ctx = GS_SPR_CTX;
    f32 yScale = 1.0f;
    f32 winX, winY;

    if (ctx->halfHeight)
        yScale = 0.5f;
    winX = (f32)ctx->winX;
    winY = (f32)ctx->winY;
    st->curX = (f32)((f64)(f32)ctx->tgtA.xoff * 0.0625 + (f64)winX + (f64)(f32)x);
    st->curY = (f32)((f64)(f32)ctx->tgtA.yoff * 0.0625 + (f64)winY + (f64)((f32)y * yScale));
    st->lastChar = -1;
}

// FUNC 0x0024A1D0 font_locate
void font_locate(s32 x, s32 y)
{
    GsLibState *st;

    font_set_start(x, y);
    st = &g_002F72C0;
    st->curX = st->startX;
    st->curY = st->startY;
}

// FUNC 0x0024A208 font_locate_f
void font_locate_f(f32 x, f32 y)
{
    GsLibState *st = &g_002F72C0;
    GsContext *ctx = GS_SPR_CTX;
    f32 yScale = 1.0f;
    f32 winX, winY;

    if (ctx->halfHeight)
        yScale = 0.5f;
    winX = (f32)ctx->winX;
    winY = (f32)ctx->winY;
    st->startX = (f32)((f64)(f32)ctx->tgtA.xoff * 0.0625 + (f64)winX + (f64)x);
    st->startY = (f32)((f64)(f32)ctx->tgtA.yoff * 0.0625 + (f64)winY + (f64)(y * yScale));
    st->curX = st->startX;
    st->curY = st->startY;
    st->lastChar = -1;
}

// FUNC 0x0024A378 font_locate_3d
/* The text position at the screen projection of pos (through vf24..vf27
 * and the viewport); the sprites' Z / F = the 12.4 values >> 4.  Returns 1
 * when the point is inside the clip volume, else 0. */
int font_locate_3d(const f32 *pos)
{
    GsLibState *st = &g_002F72C0;
    GsContext *ctx = GS_SPR_CTX;
    pgl::Vec4 v, c, q;
    u32 clip;
    s32 z, fog;
    float rq;

    vu0_load_screen_regs();
    v = pgl::vu::xformW1(&VU0.vf[24], pgl::Vec4{pos[0], pos[1], pos[2], 1.0f});
    c = pgl::vu::mul(v, VU0.vf[20]);
    clip = pgl::vu::clipFlags(c, c.w);
    if (c.w < 0.0f)
        clip = ((clip >> 1) | 0x2A) ^ ((clip << 1) | 0x15);
    clip &= 0x3F;
    rq = pgl::vu::div(1.0f, v.w);
    q = pgl::vu::scale(pgl::vu::mini(v, VU0.vf[21]), rq);
    v = pgl::vu::madd(VU0.vf[22], q, VU0.vf[23]);
    st->startX = v.x;
    st->startY = v.y;
    st->startZ = v.z;
    st->startW = v.w;
    z = pgl::vu::ftoi4(v.z) >> 4;
    fog = pgl::vu::ftoi4(v.w) >> 4;
    VU0.vf[16] = v;
    st->curX = st->startX;
    st->curY = st->startY;
    st->lastChar = -1;
    ctx->spriteZF = ((u32)fog << 24) | (u32)z;
    if (clip != 0)
        return 0;
    if (q.z < -1.0f)
        return 0;
    if (1.0f < q.z)
        return 0;
    return 1;
}

/* An A+D pair (data, GS register) of a text packet at q. */
#define FONT_PUT_AD(q, d, r) do { u64 *q_ = (q); q_[0] = (d); q_[1] = (r); } while (0)
#define FONT_PUT_AD_W(q, lo, hi, r) do {            \
        u64 *q_ = (q);                              \
        ((u32 *)q_)[0] = (lo);                      \
        ((u32 *)q_)[1] = (hi);                      \
        q_[1] = (r);                                \
    } while (0)

// FUNC 0x0024A4E8 font_begin
/* Opens a text packet: the context-2 copies of FRAME..SCISSOR flushed (the
 * kanji draw through context 2), then the A+D block TEST_1 (without ATE),
 * ALPHA_1 = (Cs - Cd) As + Cd, TEX1_1 / TEX0_1 of the font, CLAMP_1 = 0. */
void font_begin(void)
{
    GsContext *ctx = GS_SPR_CTX;
    GsLibState *st = &g_002F72C0;
    u64 enable = ctx->enable;
    u64 *p, *q;

    ctx->enable = enable | GS_DIRTY_CTX2_BITS(GS_REG_BITS(GS_REG_FRAME, GS_REG_MIPTBP2));
    ctx->dirty = (ctx->dirty & ~GS_DIRTY_BOTH_BITS(GS_REG_BITS(GS_REG_ALPHA, GS_REG_TEX0))) |
                 GS_DIRTY_CTX2_BITS(GS_REG_BITS(GS_REG_FRAME, GS_REG_SCISSOR));
    gs_flush_dirty_regs();
    p = (u64 *)ctx->cur;
    ctx->enable = enable;
    st->lastPrim = 0;
    st->lastColor = 0;
    st->pkt = p + 2;
    FONT_PUT_AD(p + 2, GIF_AD_TAG(5, 1), GIF_REG_AD);
    FONT_PUT_AD(p + 4, ctx->reg[GS_REG_TEST] & ~1ull, SCE_GS_TEST_1);
    FONT_PUT_AD_W(p + 6, GS_ALPHA_BLEND, 0, SCE_GS_ALPHA_1);
    FONT_PUT_AD(p + 8, st->fontTex1, SCE_GS_TEX1_1);
    FONT_PUT_AD(p + 10, st->fontTex0, SCE_GS_TEX0_1);
    q = p + 14;
    FONT_PUT_AD(p + 12, 0, SCE_GS_CLAMP_1);
    st->pktEnd = q;
    st->open = 1;
    st->run = NULL;
    st->kinit = 0;
    s_pcTex0 = st->fontTex0;                /* PC */
}

/* PC (ROADMAP 36, no original): the open text packet's TEX0_1 made tex0 -
 * the sprite run closed (as a change of PRIM or colour closes it), an A+D
 * write.  The vector glyphs' pages and the face's own texture (the glyphs
 * its font lacks) in one packet. */
static void font_use_tex0(GsLibState *st, u64 tex0)
{
    u64 *run = st->run;
    u64 *p = st->pktEnd;

    if (tex0 == s_pcTex0)
        return;
    if (run != NULL) {
        u32 n = ((u32)((u8 *)p - (u8 *)run) >> 5) | GIF_EOP;

        run[1] = FONT_SPRITE_REGS;
        run[0] = (GIF_NREG(4) | GIF_FLG_REGLIST) | n;
        st->run = NULL;
    }
    FONT_PUT_AD(p, GIF_AD_TAG(1, 0), GIF_REG_AD);
    FONT_PUT_AD(p + 2, tex0, SCE_GS_TEX0_1);
    st->pktEnd = p + 4;
    s_pcTex0 = tex0;
}

/* PC (ROADMAP 36, no original): a vector glyph (pgl_vfont_glyph /
 * pgl_vfont_kanji) at the pen, as font_putc draws the original's: kerned
 * against the vector glyph before it, the pen moved by its advance (a
 * digit's: whole output pixels, pgl_vfont_advance) and the spacing, its cell
 * a sprite of its page (texel corners + 1/2 as the original's, the
 * renderer's texel centres then fall where they should) in the run. */
static void font_put_vglyph(GsLibState *st, GsContext *ctx, const PglVGlyph *g, f32 xScale, f32 yScale)
{
    f32 kern = 0.0f, shift, advance, x0, x1, y0, y1;
    union { u32 w; struct { s16 x, y; } s; } xy0, xy1;
    s64 uv0, uv1;
    u32 prim;
    u64 *p, *run;

    advance = pgl_vfont_advance(g, xScale, st->spacingX, &shift);
    if (st->lastChar == FONT_PC_LAST_VECTOR && s_pcLastId != 0)
        kern = pgl_vfont_kern(s_pcVf, s_pcLastId, g->id);
    x0 = st->curX + (g->x0 + shift + kern) * xScale;
    st->curX = st->curX + (advance + kern) * xScale + st->spacingX;
    st->lastChar = FONT_PC_LAST_VECTOR;
    s_pcLastId = g->id;
    if (g->w == 0 || g->tex0 == 0)
        return;
    y0 = st->curY + g->y0 * yScale;
    x1 = x0 + (f32)g->w * xScale;
    y1 = y0 + (f32)g->h * yScale;
    x0 *= 16.0f;
    x1 *= 16.0f;
    y0 *= 16.0f;
    y1 *= 16.0f;
    {
        f32 xmax = (f32)ctx->tgtA.xend, xmin = (f32)ctx->tgtA.xoff;
        f32 ymin = (f32)ctx->tgtA.yoff, ymax = (f32)ctx->tgtA.yend;

        if (((xmax < x0) | (x1 < xmin) | (ymax < y0) | (y1 < ymin)) != 0)
            return;
    }
    uv0 = (s64)(s32)((((u32)g->v * 16 + 8) << 16) | ((u32)g->u * 16 + 8));
    uv1 = uv0 + (s32)(((u32)g->w << 4) | ((u32)g->h << 20));
    prim = ctx->primAttr | (GS_PRIM_FST | GS_PRIM_ABE | GS_PRIM_TME | GS_PRIM_SPRITE | GS_PRIM_FULL_COLOR);
    xy0.s.x = (s16)(s32)x0;
    xy0.s.y = (s16)(s32)y0;
    xy1.s.x = (s16)(s32)x1;
    xy1.s.y = (s16)(s32)y1;
    font_use_tex0(st, g->tex0);
    if (st->lastPrim != prim || pgl::prim::fullColor(*ctx) != st->lastColor) {
        run = st->run;
        p = st->pktEnd;
        if (run != NULL) {
            u32 n = ((u32)((u8 *)p - (u8 *)run) >> 5) | GIF_EOP;

            run[1] = FONT_SPRITE_REGS;
            run[0] = (GIF_NREG(4) | GIF_FLG_REGLIST) | n;
            st->run = NULL;
        }
        st->lastPrim = prim;
        st->lastColor = pgl::prim::fullColor(*ctx);
        {
            u64 *q = p;

            q[1] = GIF_REG_RGBAQ | (GIF_REG_PRIM << 4);
            p += 2;
            q[0] = GIF_NREG(2) | GIF_FLG_REGLIST | GIF_TAG_LO(1, 0);
            ((u32 *)p)[0] = st->lastColor;
            ((u32 *)p)[1] = GS_Q_ONE;
            p++;
            *p++ = (u64)st->lastPrim;
            st->pktEnd = p;
        }
    }
    run = st->run;
    p = st->pktEnd;
    if (run == NULL) {
        st->run = p;
        p += 2;
    }
    *p++ = (u64)uv0;
    ((u32 *)p)[0] = xy0.w;
    ((u32 *)p)[1] = ctx->spriteZF;
    p++;
    *p++ = (u64)uv1;
    ((u32 *)p)[0] = xy1.w;
    ((u32 *)p)[1] = ctx->spriteZF;
    p++;
    st->pktEnd = p;
}

/* PC (ROADMAP 36): the pen moved by a vector glyph, as font_put_vglyph moves
 * it (the measuring of font_measure_char / font_measure_kanji). */
static void font_measure_vglyph(GsLibState *st, const PglVGlyph *g, f32 xScale)
{
    f32 kern = 0.0f, shift, advance;

    advance = pgl_vfont_advance(g, xScale, st->spacingX, &shift);
    if (st->lastChar == FONT_PC_LAST_VECTOR && s_pcLastId != 0)
        kern = pgl_vfont_kern(s_pcVf, s_pcLastId, g->id);
    st->curX = st->curX + (advance + kern) * xScale + st->spacingX;
    st->lastChar = FONT_PC_LAST_VECTOR;
    s_pcLastId = g->id;
}

// FUNC 0x0024A5F8 font_end
/* Closes the text packet; ALPHA..TEX0 of both contexts dirty (the packet
 * overwrote them). */
void font_end(void)
{
    GsLibState *st = &g_002F72C0;
    GsContext *ctx = GS_SPR_CTX;

    if (st->pkt != NULL)
        font_flush();
    st->open = 0;
    ctx->dirty |= GS_DIRTY_BOTH_BITS(GS_REG_BITS(GS_REG_ALPHA, GS_REG_TEX0));
}

// FUNC 0x0024A658 font_puts
void font_puts(const u8 *s)
{
    PcFontSqueeze pcSqueeze;                /* PC: the menus' stretch */
    int c;

    while ((c = *s++) != 0) {
        if (c & 0x80)
            c = (c << 8) + *s++;
        font_putc(c);
    }
}

// FUNC 0x0024A6B0 font_put_kanji
/* One kanji of the kanji font in the bitmap font's kanji cell metrics: the
 * glyph (4 bit) uploaded into the VRAM cell (the CLUT once per packet), a
 * new packet with TEXFLUSH and, once, the context-2 registers (TEST_2,
 * ALPHA_2, TEX1_2, TEX0_2: the cell as a 128 x 128 PSMT4 texture, its CLUT
 * 0x1F blocks above, CLAMP_2: region clamp to the glyph); the sprite with
 * PRIM | 0x356 (sprite, TME, ABE, FST, context 2).  -1 without a kanji
 * font. */
int font_put_kanji(int code)
{
    GsLibState *st = &g_002F72C0;
    const KanjiFont *kf = st->kfont;
    const PglFont *font = st->font;
    GsContext *ctx;
    f32 yScale, x0, x1, y0, y1;
    u32 glyphIndex, prim;
    union { u32 w; struct { s16 x, y; } s; } xy0, xy1;
    s64 uv0, uv1;
    const u8 *glyph;
    u64 *p, *q;

    /* PC (ROADMAP 36): the kanji font's (or the face's vector font's) glyph,
     * drawn as the face's other vector glyphs (context 1, its pages) */
    if (s_pcVf != NULL) {
        PglVGlyph vg;

        if (pgl_vfont_kanji(s_pcVf, code, &vg)) {
            ctx = GS_SPR_CTX;
            yScale = st->scaleY;
            if (ctx->halfHeight)
                yScale *= 0.5f;
            font_put_vglyph(st, ctx, &vg, st->scaleX, yScale);
            return 0;
        }
    }
    st->lastChar = -1;
    if (kf == NULL)
        return -1;
    ctx = GS_SPR_CTX;
    yScale = st->scaleY;
    x0 = st->curX + (f32)font->kanjiX * st->scaleX;
    x1 = x0 + (f32)font->kanjiW * st->scaleX;
    st->curX = x1 + st->spacingX;
    if (ctx->halfHeight)
        yScale *= 0.5f;
    y0 = st->curY + (f32)font->kanjiY * yScale;
    y1 = y0 + (f32)font->kanjiH * yScale;
    x0 *= 16.0f;
    y0 *= 16.0f;
    x1 *= 16.0f;
    y1 *= 16.0f;
    {
        f32 xmax = (f32)ctx->tgtA.xend, xmin = (f32)ctx->tgtA.xoff;
        f32 ymin = (f32)ctx->tgtA.yoff, ymax = (f32)ctx->tgtA.yend;

        if (((xmax < x0) | (x1 < xmin) | (ymax < y0) | (y1 < ymin)) != 0)
            return 0;
    }
    if (code >= kf->lo && code <= kf->hi)
        glyphIndex = kf->map[code - kf->lo];
    else
        glyphIndex = 0;
    glyph = kf->data + (s32)glyphIndex * kf->glyphBytes;
    xy0.s.x = (s16)(s32)x0;
    xy0.s.y = (s16)(s32)y0;
    xy1.s.x = (s16)(s32)x1;
    xy1.s.y = (s16)(s32)y1;
    uv0 = 0x80008;
    uv1 = uv0 + (s32)(((u32)kf->h << 20) | ((u32)kf->w << 4));
    if (st->pkt != NULL)
        font_flush();
    if (!st->kinit)
        gs_upload_image(st->kvram + 0x7C0, GS_PSMCT32, 0x40, 0, 0, 8, 2, st->kclut);
    gs_upload_image(st->kvram, GS_PSMT4, 0x80, 0, 0, kf->w, kf->h, glyph);
    p = (u64 *)ctx->cur;
    st->pkt = p + 2;
    FONT_PUT_AD(p + 2, GIF_AD_TAG(1, 1), GIF_REG_AD);
    FONT_PUT_AD(p + 4, 0, SCE_GS_TEXFLUSH);
    q = p + 6;
    if (!st->kinit) {
        FONT_PUT_AD(q, GIF_AD_TAG(5, 1), GIF_REG_AD);
        FONT_PUT_AD(p + 8, ctx->reg[GS_REG_TEST] & ~1ull, SCE_GS_TEST_2);
        FONT_PUT_AD_W(p + 10, GS_ALPHA_BLEND, 0, SCE_GS_ALPHA_2);
        FONT_PUT_AD(p + 12, 0x61, SCE_GS_TEX1_2);
        FONT_PUT_AD(p + 14, GS_SET_TEX0(st->kvram >> 6, 2, GS_PSMT4, 7, 7, 1, 0,
                                        (u64)(st->kvram >> 6) + 0x1F, 0, 0, 1, 1), SCE_GS_TEX0_2);
        q = p + 18;
        FONT_PUT_AD(p + 16, GS_SET_CLAMP(GS_REGION_CLAMP, GS_REGION_CLAMP, 0, (u64)st->kfont->w - 1,
                                         0, (u64)st->kfont->h - 1), SCE_GS_CLAMP_2);
        st->kinit = 1;
    }
    prim = ctx->primAttr | (GS_PRIM_CTXT2 | GS_PRIM_FST | GS_PRIM_ABE | GS_PRIM_TME | GS_PRIM_SPRITE | GS_PRIM_FULL_COLOR);
    if (st->lastPrim != prim || pgl::prim::fullColor(*ctx) != st->lastColor) {
        u32 color;
        u64 *t = q;

        st->lastPrim = prim;
        color = pgl::prim::fullColor(*ctx);
        st->lastColor = color;
        t[1] = GIF_REG_RGBAQ | (GIF_REG_PRIM << 4);
        q += 2;
        t[0] = GIF_NREG(2) | GIF_FLG_REGLIST | GIF_TAG_LO(1, 1);
        ((u32 *)q)[0] = color;
        ((u32 *)q)[1] = GS_Q_ONE;
        q++;
        *q++ = (u64)st->lastPrim;
    }
    {
        u64 *t = q;

        q += 2;
        t[0] = GIF_NREG(4) | GIF_FLG_REGLIST | GIF_TAG_LO(1, 1);
        t[1] = FONT_SPRITE_REGS;
        *q++ = (u64)uv0;
        ((u32 *)q)[0] = xy0.w;
        ((u32 *)q)[1] = ctx->spriteZF;
        q++;
        *q++ = (u64)uv1;
        ((u32 *)q)[0] = xy1.w;
        ((u32 *)q)[1] = ctx->spriteZF;
        q++;
        st->pktEnd = q;
    }
    return 0;
}

// FUNC 0x0024AB50 font_putc
/* One character at the pen, then the pen moves: '\n' a new line, '\r' the
 * line start (both keep maxX).  Codes outside the font draw nothing.  The
 * glyph is kerned against the previous one and drawn as a sprite of PRIM |
 * 0x156 in the current run; a change of PRIM or colour closes the run and
 * emits RGBAQ + PRIM.  -1 without a font. */
int font_putc(int c)
{
    PcFontSqueeze pcSqueeze;                /* PC: the menus' stretch (once: a string's own nest) */
    GsLibState *st = &g_002F72C0;
    GsContext *ctx = GS_SPR_CTX;
    const PglFont *font = st->font;
    const FontGlyph *g;
    f32 xScale, yScale, x0, x1, y0, y1;
    int kern;
    u32 prim;
    union { u32 w; struct { s16 x, y; } s; } xy0, xy1;
    s64 uv0, uv1;
    u64 *p, *run;

    if (font == NULL)
        return -1;
    if (c >= 0x80) {
        int hi = c >> 8;
        int lo = c & 0xFF;

        if ((hi & 0xFF) >= FONT_KANJI_FIRST)
            return font_put_kanji(c & 0x7F7F);
        c = kana_page_lookup(hi & 0x7F, lo);
    }
    xScale = st->scaleX;
    yScale = st->scaleY;
    if (ctx->halfHeight)
        yScale *= 0.5f;
    switch (c) {
    case '\n':
        st->maxX = u114_maxf(st->maxX, st->curX);
        st->curX = st->startX;
        st->curY = st->curY + ((f32)font->lineHeight * yScale + st->spacingY);
        st->lastChar = -1;
        break;
    case '\r':
        st->maxX = u114_maxf(st->maxX, st->curX);
        st->curX = st->startX;
        st->lastChar = -1;
        break;
    default:
        /* PC (ROADMAP 36): the face's vector font's glyph, when it has one */
        if (s_pcVf != NULL && c >= 0x20) {
            PglVGlyph vg;

            if (pgl_vfont_glyph(s_pcVf, c, &vg)) {
                font_put_vglyph(st, ctx, &vg, xScale, yScale);
                break;
            }
        }
        if (c < font->first || font->last < c) {
            st->lastChar = -1;
            return 0;
        }
        g = &font->glyphs[c - font->first];
        kern = 0;
        if (st->lastChar >= 0) {
            const FontGlyph *prev = &font->glyphs[st->lastChar - font->first];

            kern = glyph_kerning(g->left, prev->right, font->kernRows);
        }
        x0 = st->curX + (f32)(g->bearingX - kern) * xScale;
        st->curX = x0 + (f32)g->advance * xScale + st->spacingX;
        st->lastChar = c;
        y0 = st->curY + (f32)g->bearingY * yScale;
        x1 = x0 + (f32)g->w * xScale;
        y1 = y0 + (f32)g->h * yScale;
        x0 *= 16.0f;
        x1 *= 16.0f;
        y0 *= 16.0f;
        y1 *= 16.0f;
        if (g->w == 0)
            break;
        {
            f32 xmax = (f32)ctx->tgtA.xend, xmin = (f32)ctx->tgtA.xoff;
            f32 ymin = (f32)ctx->tgtA.yoff, ymax = (f32)ctx->tgtA.yend;

            if (((xmax < x0) | (x1 < xmin) | (ymax < y0) | (y1 < ymin)) != 0)
                return 0;
        }
        uv0 = (s64)(s32)((((u32)g->v * 16 + 8) << 16) | ((u32)g->u * 16 + 8));
        uv1 = uv0 + (s32)(((u32)g->w << 4) | ((u32)g->h << 20));
        prim = ctx->primAttr | (GS_PRIM_FST | GS_PRIM_ABE | GS_PRIM_TME | GS_PRIM_SPRITE | GS_PRIM_FULL_COLOR);
        xy0.s.x = (s16)(s32)x0;
        xy0.s.y = (s16)(s32)y0;
        xy1.s.x = (s16)(s32)x1;
        xy1.s.y = (s16)(s32)y1;
        if (s_pcVf != NULL)
            font_use_tex0(st, st->fontTex0);    /* PC: the face's own texture (after vector glyphs) */
        if (st->lastPrim != prim || pgl::prim::fullColor(*ctx) != st->lastColor) {
            run = st->run;
            p = st->pktEnd;
            if (run != NULL) {
                u32 n = ((u32)((u8 *)p - (u8 *)run) >> 5) | GIF_EOP;

                run[1] = FONT_SPRITE_REGS;
                run[0] = (GIF_NREG(4) | GIF_FLG_REGLIST) | n;
                st->run = NULL;
                run = NULL;
            }
            st->lastPrim = prim;
            st->lastColor = pgl::prim::fullColor(*ctx);
            {
                u64 *q = p;

                q[1] = GIF_REG_RGBAQ | (GIF_REG_PRIM << 4);
                p += 2;
                q[0] = GIF_NREG(2) | GIF_FLG_REGLIST | GIF_TAG_LO(1, 0);
                ((u32 *)p)[0] = st->lastColor;
                ((u32 *)p)[1] = GS_Q_ONE;
                p++;
                *p++ = (u64)st->lastPrim;
                st->pktEnd = p;
            }
        } else {
            run = st->run;
        }
        p = st->pktEnd;
        if (run == NULL) {
            st->run = p;
            p += 2;
        }
        *p++ = (u64)uv0;
        ((u32 *)p)[0] = xy0.w;
        ((u32 *)p)[1] = ctx->spriteZF;
        p++;
        *p++ = (u64)uv1;
        ((u32 *)p)[0] = xy1.w;
        ((u32 *)p)[1] = ctx->spriteZF;
        p++;
        st->pktEnd = p;
        break;
    }
    return 0;
}

// FUNC 0x0024B080 kana_page_lookup
int kana_page_lookup(u32 page, int c)
{
    if (page < 16) {
        const KanaPage *pg = &g_kanaPages[page];

        if (c >= pg->first && !(pg->last < c))
            return pg->map[c - pg->first];
    }
    return 0xFFFF;
}

/* ======================================================================== */
/* fontprint.c                                                              */
/* ======================================================================== */

// FUNC 0x0024B0D8 font_vprintf
/* Prints through the library's printf engine with font_putc, in a text
 * packet of its own unless one is open; with an alignment the pen moves
 * first (font_apply_align, which formats the text once more to measure it:
 * the same va_list value is passed to both). */
void font_vprintf(const char *fmt, va_list ap)
{
    PcFontSqueeze pcSqueeze;                /* PC: the menus' stretch (the alignment measures it too) */
    GsLibState *st = &g_002F72C0;
    u8 wasOpen = st->open;

    if (!wasOpen)
        font_begin();
    if (st->align != 0)
        font_apply_align(st, fmt, ap);
    fmt_vprintf(font_putc, (const u8 *)fmt, (const u8 *)ap);
    if (!wasOpen)
        font_end();
}

// FUNC 0x0024B178 font_apply_align
/* Moves the pen for the alignment of the text fmt / ap (measured with
 * font_measure_char, or only its '\n's counted without a horizontal
 * alignment). */
void font_apply_align(GsLibState *st, const char *fmt, va_list ap)
{
    const PglFont *font;
    f32 x = st->curX;
    f32 y = st->curY;
    f32 xEnd, yEnd;
    u32 vAlign;

    if (st->align & FONT_ALIGN_H_MASK) {
        st->startX = x;
        st->startY = y;
        st->maxX = x;
        st->lastChar = -1;
        fmt_vprintf(font_measure_char, (const u8 *)fmt, (const u8 *)ap);
        xEnd = st->curX;
        yEnd = st->curY;
        if (xEnd < st->maxX)
            xEnd = st->maxX;
    } else {
        const s8 *s = (const s8 *)fmt;
        f32 yScale, lines;
        int c;

        font = st->font;
        yScale = st->scaleY;
        if (GS_SPR_CTX->halfHeight)
            yScale *= 0.5f;
        xEnd = x;
        lines = 0.0f;
        for (c = *s++; c != 0; c = *s++)
            if (c == '\n')
                lines += 1.0f;
        yEnd = y + ((f32)font->lineHeight * yScale + st->spacingY) * lines;
    }
    switch ((u32)st->align & FONT_ALIGN_H_MASK) {
    case FONT_ALIGN_LEFT:
        break;
    case FONT_ALIGN_RIGHT:
        x -= xEnd - x;
        break;
    case FONT_ALIGN_HCENTER:
        x -= (xEnd - x) * 0.5f;
        break;
    }
    vAlign = st->align & FONT_ALIGN_V_MASK;
    if (vAlign != 0) {
        f32 yScale, height, ascent;

        font = st->font;
        yScale = st->scaleY;
        if (GS_SPR_CTX->halfHeight)
            yScale *= 0.5f;
        height = (yEnd - y) + (f32)font->lineHeight * yScale;
        ascent = (f32)font->ascent * yScale;
        switch (vAlign) {
        case FONT_ALIGN_TOP:
            y += ascent;
            break;
        case FONT_ALIGN_BOTTOM:
            y += ascent - height;
            break;
        case FONT_ALIGN_VCENTER:
            y += ascent - height * 0.5f;
            break;
        }
    }
    st->curX = x;
    st->curY = y;
    st->startX = x;
    st->startY = y;
    st->maxX = x;
    st->lastChar = -1;
}

// FUNC 0x0024B3A0 sprintf_putc
int sprintf_putc(int c)
{
    GsLibState *st = &g_002F72C0;

    *st->out = (char)c;
    st->out++;
    return 0;
}

// FUNC 0x0024B3C8 fmt_sprintf
int fmt_sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = fmt_vsprintf(buf, fmt, ap);
    va_end(ap);
    return n;
}

// FUNC 0x0024B400 fmt_vsprintf
int fmt_vsprintf(char *buf, const char *fmt, va_list ap)
{
    GsLibState *st = &g_002F72C0;
    int n;

    st->out = buf;
    n = fmt_vprintf(sprintf_putc, (const u8 *)fmt, (const u8 *)ap);
    *st->out = 0;
    st->out = NULL;
    return n;
}

// FUNC 0x0024B440 font_text_width
/* Width of a string as drawn (kerning included, max over its lines); the
 * pen state restored. */
f32 font_text_width(const u8 *s)
{
    PcFontSqueeze pcSqueeze;                /* PC: the menus' stretch (a layout by the width keeps it) */
    GsLibState *st = &g_002F72C0;
    f32 maxX, startX, startY, curX, curY;
    s32 lastChar;
    f32 width;
    int c;
    const u32 pcLastId = s_pcLastId;        /* PC: (restored with lastChar) */

    startX = st->startX;
    startY = st->startY;
    curX = st->curX;
    curY = st->curY;
    maxX = st->maxX;
    lastChar = st->lastChar;
    st->lastChar = -1;
    st->startX = 0;
    st->startY = 0;
    st->curX = 0.0f;
    st->curY = 0;
    st->maxX = 0.0f;
    while ((c = *s++) != 0) {
        if (c & 0x80)
            c = (c << 8) + *s++;
        font_measure_char(c);
    }
    width = st->curX;
    if (width < st->maxX)
        width = st->maxX;
    st->startX = startX;
    st->startY = startY;
    st->curX = curX;
    st->curY = curY;
    st->maxX = maxX;
    st->lastChar = lastChar;
    s_pcLastId = pcLastId;
    return width;
}

// FUNC 0x0024B538 font_text_height
f32 font_text_height(const s8 *s)
{
    GsLibState *st = &g_002F72C0;
    f32 lines = 0.0f;
    f32 lineHeight = (f32)st->font->lineHeight * st->scaleY;
    int c;

    for (c = *s++; c != 0; c = *s++)
        if (c == '\n')
            lines += 1.0f;
    return lineHeight + (lineHeight + st->spacingY) * lines;
}

/* ======================================================================== */
/* fontfmt.c                                                                */
/* ======================================================================== */

// FUNC 0x0024D128 font_flush
/* Closes the open sprite run (its REGLIST tag: NLOOP = sprites) and the text
 * packet (DMA cnt + DIRECT of its qwords at the write pointer, which moves
 * past it).  PC: the closed packet goes to the renderer. */
void font_flush(void)
{
    GsLibState *st = &g_002F72C0;
    GsContext *ctx = GS_SPR_CTX;
    u32 qwc;
    u32 *cur;

    if (st->run != NULL) {
        u32 n = ((u32)((u8 *)st->pktEnd - (u8 *)st->run) >> 5) | GIF_EOP;

        st->run[1] = FONT_SPRITE_REGS;
        st->run[0] = (GIF_NREG(4) | GIF_FLG_REGLIST) | (u64)(s64)(s32)n;
        st->run = NULL;
    }
    qwc = (u32)((u8 *)st->pktEnd - (u8 *)st->pkt) >> 4;
    cur = ctx->cur;
    if (qwc != 0) {
        ctx->cur = cur + 4 + qwc * 4;
        cur[0] = DMATAG_CNT(qwc);
        cur[2] = VIF_FLUSH;
        cur[3] = VIF_DIRECT(qwc);
        cur[1] = 0;
        pglr_gif(st->pkt, qwc, ctx->halfHeight ? PGLR_HALF_HEIGHT : 0);
    }
    st->pkt = NULL;
    st->pktEnd = NULL;
}

// FUNC 0x0024D1C8 font_measure_kanji
int font_measure_kanji(int code)
{
    GsLibState *st = &g_002F72C0;
    const PglFont *font = st->font;

    /* PC (ROADMAP 36): a vector glyph's advance (font_put_kanji's) */
    if (s_pcVf != NULL) {
        PglVGlyph vg;

        if (pgl_vfont_kanji(s_pcVf, code, &vg)) {
            font_measure_vglyph(st, &vg, st->scaleX);
            return 0;
        }
    }
    (void)code;
    st->lastChar = -1;
    if (st->kfont == NULL)
        return -1;
    st->curX = st->curX + (f32)font->kanjiX * st->scaleX + (f32)font->kanjiW * st->scaleX + st->spacingX;
    return 0;
}

// FUNC 0x0024D230 font_measure_char
int font_measure_char(int c)
{
    GsLibState *st = &g_002F72C0;
    GsContext *ctx = GS_SPR_CTX;
    const PglFont *font = st->font;
    const FontGlyph *g;
    f32 xScale, yScale;
    int kern;

    if (font == NULL)
        return -1;
    if (c >= 0x80) {
        int hi = c >> 8;
        int lo = c & 0xFF;

        if ((hi & 0xFF) >= FONT_KANJI_FIRST)
            return font_measure_kanji(c & 0x7F7F);
        c = kana_page_lookup(hi & 0x7F, lo);
    }
    xScale = st->scaleX;
    yScale = st->scaleY;
    if (ctx->halfHeight)
        yScale *= 0.5f;
    switch (c) {
    case '\n':
        st->maxX = u114_maxf(st->maxX, st->curX);
        st->curX = st->startX;
        st->curY = st->curY + ((f32)font->lineHeight * yScale + st->spacingY);
        st->lastChar = -1;
        break;
    case '\r':
        st->maxX = u114_maxf(st->maxX, st->curX);
        st->curX = st->startX;
        st->lastChar = -1;
        break;
    default:
        /* PC (ROADMAP 36): the face's vector font's glyph, when it has one */
        if (s_pcVf != NULL && c >= 0x20) {
            PglVGlyph vg;

            if (pgl_vfont_glyph(s_pcVf, c, &vg)) {
                font_measure_vglyph(st, &vg, xScale);
                break;
            }
        }
        if (c < font->first || font->last < c) {
            st->lastChar = -1;
            return 0;
        }
        g = &font->glyphs[c - font->first];
        kern = 0;
        if (st->lastChar >= 0) {
            const FontGlyph *prev = &font->glyphs[st->lastChar - font->first];

            kern = glyph_kerning(g->left, prev->right, font->kernRows);
        }
        st->curX = st->curX + (f32)(g->bearingX - kern) * xScale + (f32)g->advance * xScale + st->spacingX;
        st->lastChar = c;
        break;
    }
    return 0;
}

/* ======================================================================== */
/* format_part1.c / format_part2.c: the printf engine                       */
/* ======================================================================== */

enum { FMT_LEFT = 0x01, FMT_PLUS = 0x02, FMT_ZERO = 0x04, FMT_SPACE = 0x08, FMT_ALT = 0x10 };
enum { FMT_SIZE_INT = 0, FMT_SIZE_SHORT = 1, FMT_SIZE_LONG = 2, FMT_SIZE_LONGLONG = 3 };
#define FMT_BASE_OCT            8
#define FMT_BASE_DEC            10
#define FMT_BASE_HEX            0x10
#define FMT_BASE_HEX_UPPER      0x11

// FUNC 0x0024D4F8 fmt_put_string
/* %s: at most prec bytes padded to width; a byte with bit 7 and the next
 * one put as one two-byte code (both count). */
int fmt_put_string(PglPutcFunc put, u32 flags, s32 width, s32 prec, const u8 *s)
{
    s32 padRight = 0;
    s32 padLeft = 0;
    s32 count = 0;
    s32 len, i;
    int c;

    len = (s32)strlen((const char *)s);
    if (prec >= 0)
        len = (prec < len) ? prec : len;
    width -= len;
    if (width < 0)
        width = 0;
    if (!(flags & FMT_LEFT))
        padLeft = width;
    else
        padRight = width;
    c = (flags & FMT_ZERO) ? '0' : ' ';
    for (i = 0; i < padLeft; i++) {
        if (put(c) < 0)
            return -1;
        count++;
    }
    for (i = 0; i < len; ) {
        c = *s++;
        if (c & 0x80) {
            c = (c << 8) + *s++;
            count++;
            i++;
        }
        if (put(c) < 0)
            return -1;
        i++;
        count++;
    }
    for (i = 0; i < padRight; i++) {
        if (put(' ') < 0)
            return -1;
        count++;
    }
    return count;
}

// FUNC 0x0024D670 fmt_put_number
int fmt_put_number(PglPutcFunc put, u32 flags, s32 width, s32 prec, s8 sign, const char *digits,
                   s32 ndigits, s32 base)
{
    char prefix[4];
    s32 minDigits;
    s32 padRight = 0;
    s32 nprefix = 0;
    s32 padLeft = 0;
    s32 zeros, count, i;

    minDigits = 1;
    if (prec >= 0)
        minDigits = prec;
    if (width < 0)
        width = 0;
    if (base < 11) {
        if (sign != '-' && (flags & FMT_PLUS))
            sign = '+';
        if (sign != 0) {
            prefix[0] = sign;
            nprefix = 1;
        }
    }
    if (flags & FMT_ALT) {
        if (base >= 8)
            prefix[nprefix++] = '0';
        if (base >= 16)
            prefix[nprefix++] = (base & 1) ? 'X' : 'x';
    }
    zeros = minDigits - ndigits;
    if (zeros < 0)
        zeros = 0;
    if ((ndigits | zeros) == 0) {
        padLeft = width;
        nprefix = 0;
    } else {
        i = width - (nprefix + ndigits) - zeros;
        if (i < 0) {
            zeros += i;
            if (!(zeros > 0))
                zeros = (ndigits == 0);
        } else if (!(flags & FMT_LEFT)) {
            if (flags & FMT_ZERO)
                zeros += i;
            else
                padLeft = i;
        } else {
            padRight = i;
        }
    }
    count = 0;
    for (i = 0; i < padLeft; i++, count++)
        if (put(' ') < 0)
            return -1;
    for (i = 0; i < nprefix; i++, count++)
        if (put(prefix[i]) < 0)
            return -1;
    for (i = 0; i < zeros; i++, count++)
        if (put('0') < 0)
            return -1;
    for (i = 0; i < ndigits; i++, count++)
        if (put(digits[i]) < 0)
            return -1;
    for (i = 0; i < padRight; i++, count++)
        if (put(' ') < 0)
            return -1;
    return count;
}

/* The powers of ten fmt_put_decimal counts with (0x002F73A8). */
u64 g_fmtPow10[19] = {
    1000000000000000000ull, 100000000000000000ull, 10000000000000000ull,
    1000000000000000ull, 100000000000000ull, 10000000000000ull,
    1000000000000ull, 100000000000ull, 10000000000ull,
    1000000000ull, 100000000ull, 10000000ull,
    1000000ull, 100000ull, 10000ull,
    1000ull, 100ull, 10ull,
    1ull
};

// FUNC 0x0024D8C0 fmt_put_decimal
/* Digits of v from 10^18 down; fixed (%t) with prec >= 0: a '.' after the
 * digit of 10^prec (printed even as a leading zero).  Zero prints no
 * digit. */
int fmt_put_decimal(PglPutcFunc put, int flags, int width, int prec, u64 v, int sign, int fixed)
{
    char digits[0x20];
    int pointAt;
    int started;
    int len;
    int i;
    int d;
    int c;
    u64 p;

    if (fixed == 0 || prec < 0) {
        pointAt = 19;
    } else {
        pointAt = 18 - prec;
        if (!(-1 < pointAt))
            pointAt = 19;
    }
    started = 0;
    len = 0;
    for (i = 0; i < 19; i++) {
        p = g_fmtPow10[i];
        d = 0;
        while (v >= p) {
            v -= p;
            d++;
        }
        c = d + '0';
        if (started != 0 || d != 0 || i == pointAt) {
            if (i == pointAt) {
                digits[len++] = (char)c;
                c = '.';
            }
            started = 1;
            digits[len++] = (char)c;
        }
    }
    return fmt_put_number(put, (u32)flags, width, prec, (s8)sign, digits, len, FMT_BASE_DEC);
}

// FUNC 0x0024D9C0 fmt_put_octal
int fmt_put_octal(PglPutcFunc put, int flags, int width, int prec, u64 v, int sign)
{
    char buf[0x20];
    int n;
    int started;
    int shift;
    int d;

    n = 1;
    buf[0] = (char)sign;
    started = 0;
    for (shift = 63; shift >= 0; shift -= 3) {
        d = (int)((v >> shift) & 7);
        if (started != 0 || d != 0) {
            buf[n++] = (char)(d + '0');
            started = 1;
        }
    }
    return fmt_put_number(put, (u32)flags, width, prec, (s8)buf[0], buf + 1, n - 1, FMT_BASE_OCT);
}

// FUNC 0x0024DA38 fmt_put_hex
int fmt_put_hex(PglPutcFunc put, int flags, int width, int prec, u64 v, int conv)
{
    char digits[0x20];
    const char *set;
    int n;
    int started;
    int shift;
    int d;

    n = 0;
    if (conv == 'x')
        set = "0123456789abcdef";
    else
        set = "0123456789ABCDEF";
    started = 0;
    for (shift = 60; shift >= 0; shift -= 4) {
        d = (int)((v >> shift) & 0xF);
        if (started != 0 || d != 0) {
            started = 1;
            digits[n++] = set[d];
        }
    }
    return fmt_put_number(put, (u32)flags, width, prec, 0, digits, n,
                          (conv == 'X') ? FMT_BASE_HEX_UPPER : FMT_BASE_HEX);
}

// FUNC 0x0024DAE0 fmt_parse_uint
int fmt_parse_uint(const u8 **pp)
{
    const u8 *p = *pp;
    int c = (s8)*p;
    int v = 0;

    while (c >= '0' && c <= '9') {
        p++;
        v = v * 10 + (c - '0');
        c = (s8)*p;
    }
    *pp = p;
    return v;
}

// FUNC 0x0024DB30 fmt_vprintf
/* The format engine: %d %u %t %o %i (octal, as written) %x %X %c %s; %e %f
 * %g %E %G consume their argument and print nothing, as %n and %p.
 * Returns the characters counted, -1 when the callback fails.  Quirks
 * kept: %c and %s are not counted; a '*' precision goes into the width; an
 * unknown conversion prints the text after the '%'.  PC: `args' is the
 * caller's va_list (MSVC x86: a char pointer), read with va_arg. */
int fmt_vprintf(PglPutcFunc put, const u8 *fmt, const u8 *args)
{
    va_list ap = (va_list)args;
    const u8 *p;
    const u8 *spec;
    u32 c;
    int count;
    int flags;
    int width;
    int prec;
    int size;
    int r;

    count = 0;
    c = fmt[0];
    p = fmt + 1;
    while (c != 0) {
        if (c == '%') {
            spec = p;
            if (*spec != c)
                goto conversion;
            p = spec + 1;
        } else if (c & 0x80) {
            count++;
            c = (c << 8) + *p++;
        }
        if (put((int)c) != 0)
            return -1;
        count++;
        goto next;

conversion:
        for (flags = 0; ; p++) {
            c = *p;
            switch (c) {
            case '-': flags |= FMT_LEFT;  continue;
            case '+': flags |= FMT_PLUS;  continue;
            case '0': flags |= FMT_ZERO;  continue;
            case ' ': flags |= FMT_SPACE; continue;
            case '#': flags |= FMT_ALT;   continue;
            }
            break;
        }
        width = -1;
        if (c >= '0' && c <= '9') {
            width = fmt_parse_uint(&p);
            c = *p;
        } else if (c == '*') {
            p++;
            width = va_arg(ap, int);
            c = *p;
        }
        prec = -1;
        if (c == '.') {
            p++;
            c = *p;
            if (c >= '0' && c <= '9') {
                prec = fmt_parse_uint(&p);
            } else if (c == '*') {
                width = va_arg(ap, int);         /* (sic) into the width */
                p++;
            }
        }
        switch (*p++) {
        case 'h': size = FMT_SIZE_SHORT;    break;
        case 'l': size = FMT_SIZE_LONG;     break;
        case 'L': size = FMT_SIZE_LONGLONG; break;
        default:  size = FMT_SIZE_INT; p--; break;
        }
        c = *p++;
        switch (c) {
        case 'X': case 'd': case 'i': case 'o': case 't': case 'u': case 'x': {
            s64 sv;
            u64 v;
            int sign;

            switch (size) {
            default: {
                s32 a = va_arg(ap, s32);
                sv = a;
                v = (u32)a;
                break;
            }
            case FMT_SIZE_SHORT: {
                s32 a = va_arg(ap, s32);
                sv = (s16)a;
                v = (u64)sv & 0xFFFF;
                break;
            }
            case FMT_SIZE_LONGLONG:
                v = va_arg(ap, u64);
                sv = (s64)v;
                break;
            }
            sign = 0;
            if (sv < 0 && (c == 'd' || c == 'i' || c == 't')) {
                v = (u64)(0 - sv);
                sign = '-';
            }
            switch (c) {
            case 'd':
            case 'u':
                r = fmt_put_decimal(put, flags, width, prec, v, sign, 0);
                break;
            case 't':
                r = fmt_put_decimal(put, flags, width, prec, v, sign, 1);
                break;
            case 'i':
            case 'o':
                r = fmt_put_octal(put, flags, width, prec, v, sign);
                break;
            case 'X':
            case 'x':
                r = fmt_put_hex(put, flags, width, prec, v, (int)c);
                break;
            default:
                r = 0;
                break;
            }
            if (r < 0)
                return -1;
            count += r;
            break;
        }
        case 'E': case 'G': case 'e': case 'f': case 'g':
            /* no float output: the argument (a double after promotion) skipped */
            (void)va_arg(ap, double);
            break;
        case 'c':
            r = put((s8)va_arg(ap, int));
            if (r < 0)
                return -1;
            break;
        case 's':
            r = fmt_put_string(put, (u32)flags, width, prec, va_arg(ap, const u8 *));
            if (r < 0)
                return -1;
            break;
        case 'n': case 'p':
            (void)va_arg(ap, void *);
            break;
        default:
            p = spec;
            break;
        }
next:
        c = *p;
        p++;
    }
    return count;
}
