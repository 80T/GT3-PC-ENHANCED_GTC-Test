/* gs_imgs.h - PC (2026-11-08): image packs (.imgs, engine/io T506) and their
 * images as RGBA pictures (docs/graphics.md "Image packs as PNG folders").
 *
 * User request: ".imgs conversion to folder, and texture conversion to
 * standard format.  I suggest .png".  The launcher writes each pack's images
 * as <pack folder>\<name>.png (launcher/image_packs.h); the game builds the
 * pack from those files (game/app/pc_image_pack.h) and relocates it as the
 * original's (image_pack_relocate), so its users - the race HUD's
 * race/<country>/display.imgs - see a pack as ever.
 *
 * Every image of every pack on the discs is one texture, T4 or T8 through a
 * CT32 or CT16 CLUT, with the same settings: RGBA (TCC 1), modulate (TFX 0),
 * linear filters, no mip levels, REGION_CLAMP from 0 to the picture's size
 * (the HUD takes an image's size from CLAMP.MAXU / MAXV), no CLUT variations
 * or animations.  So an image is its pixels and size alone, and imgs_read
 * refuses a pack whose images are otherwise.  tex1_build makes the texture
 * back with those settings: T4 for 16 colours or fewer, T8 for 256 or fewer
 * (with a CT32 CLUT: the original's texture size in VRAM), else CT32 - a
 * picture painted with more colours.  The pixels are exact both ways.
 *
 * Alpha: the GS's, 0x80 = 1.0, in ImgsImage; a PNG's 255 = 1.0 (the
 * imgs_alpha_* conversions are exact for 0 .. 0x80, the range the images
 * use; imgs_read refuses alpha above 0x80, which a PNG cannot keep).
 *
 * Plain C++, no Windows or game headers: the game, the launcher and texdump
 * share it. */
#ifndef GT3PC_GS_IMGS_H
#define GT3PC_GS_IMGS_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gstex {

/* An image's name: at most this many characters (ImagePackEntry.name holds
 * 0x3C bytes with its NUL). */
constexpr uint32_t kImgsNameMax = 0x3B;
/* An image's side: at most this (TEX0.TW / TH up to 10, CLAMP.MAXU / MAXV
 * 10 bits). */
constexpr uint32_t kImgsSideMax = 1024;

struct ImgsImage {
    std::string name;
    uint32_t w = 0, h = 0;
    std::vector<uint32_t> rgba;     /* w * h, rows top down, 0xAABBGGRR, GS alpha (0x80 = 1.0) */
    /* PC (2026-11-08): the size the HUD draws it at - its original's - when
     * the picture is another size (0: w x h).  User: "bigger / smaller
     * texture dimensions [should not] change the size of HUD elements".
     * tex1_build keeps it in the set's header (kImgsDrawMagic), imgs_read
     * reads it back; the game's draws take it (game/app/pc_image_pack.h
     * pc_image_draw_size). */
    uint32_t drawW = 0, drawH = 0;
};

/* The draw size in a set's header: u16 width at +0x28, u16 height at +0x2A,
 * this word at +0x2C ("PCdz"); the original files have zeros there
 * (Tex1Image.unk_28), which nothing reads. */
constexpr uint32_t kImgsDrawMagic = 0x7A644350u;
/* A side of a draw size: at most this. */
constexpr uint32_t kImgsDrawMax = 4096;

/* The pack folder's list of draw sizes (launcher/image_packs.h writes it
 * from the pack; any editor may change it): "<name> <width> <height>" a
 * line, '#' comments. */
constexpr const char *kImgsSizesFile = "sizes.txt";
struct ImgsSize {
    std::string name;
    uint32_t w = 0, h = 0;
};
/* The file's text for these images (each its draw size, else its size). */
std::string imgs_sizes_text(const std::vector<ImgsImage> &images);
/* Its lines (others - comments, blanks, a line that is no size - left out). */
std::vector<ImgsSize> imgs_sizes_parse(const std::string &text);

/* GS alpha (0 .. 0x80) -> a PNG's (0 .. 255) and back; exact for 0 .. 0x80.
 * A PNG's any value maps to 0 .. 0x80. */
inline uint32_t imgs_alpha_to_png(uint32_t a) { return a >= 0x80 ? 255u : (a * 255u + 64u) / 128u; }
inline uint32_t imgs_alpha_from_png(uint32_t a) { return (a * 128u + 127u) / 255u; }

/* The order of the pack's entries: image_pack_bsearch's (signed chars). */
int imgs_name_compare(const std::string &a, const std::string &b);

/* The images of the pack file `file' in its entries' order: each texture's
 * clamp region decoded as the renderer reads it.  The entries may be the
 * game's (0x40 bytes) or an older layout's (0x20 bytes, a 0x1C-byte name: the
 * packs on the discs that the game never loads).  False with the reason
 * (*why) when it is no pack, or an image is not one this form keeps (the
 * settings above, alpha above 0x80).  imgs_build always writes the game's. */
bool imgs_read(const uint8_t *file, size_t size, std::vector<ImgsImage> &out, std::string *why);

/* The Tex1 set of one image (the file form: offsets from its start, VRAM
 * from block 0), appended to out at a 16-byte boundary.  False with the
 * reason when the image is empty, too big or its pixels do not match its
 * size. */
bool tex1_build(const ImgsImage &img, std::vector<uint8_t> &out, std::string *why);

/* The pack file of `images' (sorted here by name; the names must differ and
 * fit) into out.  False with the reason. */
bool imgs_build(std::vector<ImgsImage> images, std::vector<uint8_t> &out, std::string *why);

} /* namespace gstex */

#endif /* GT3PC_GS_IMGS_H */
