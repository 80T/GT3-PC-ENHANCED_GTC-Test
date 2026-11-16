/* image_packs.h - the image packs (.imgs) of a data folder written as
 * folders of PNG files (docs/graphics.md "Image packs as PNG folders").
 *
 * User request (2026-11-08): ".imgs conversion to folder, and texture
 * conversion to standard format.  I suggest .png".  Each <path>\<pack>.imgs
 * becomes the folder <path>\<pack>\ with a <name>.png for each image of the
 * pack: 32-bit RGBA, the texture's own pixels (the GS's alpha 0x80, opaque,
 * written as 255), which the game reads instead (game/app/pc_image_pack.h).
 * Each PNG is read back, and the pack the game builds from the folder is
 * checked picture by picture against the pack's own (engine/pgl/native/
 * gs_imgs.h); then the pack file is deleted.  A PNG already there (edited, an
 * interrupted conversion's) is never replaced.  A pack that cannot be
 * converted (an image a PNG cannot keep, a name that is no file's) stays as
 * it is, and the game reads it.  An install converts as one of its last
 * steps; the launcher converts an installed version that still needs it when
 * it starts. */
#pragma once

#include <string>

namespace image_packs {

/* The data folder has a .imgs file (anywhere under it). */
bool needs_conversion(const std::wstring &data);

/* Converts them.  message: a one-line summary, then a line for each pack
 * that could not be converted (kept); false when one could not.  None: true,
 * nothing to say.  Each folder also gets sizes.txt (gstex::kImgsSizesFile):
 * each picture's size in the pack, which the game draws it at whatever the
 * PNG's size (PC 2026-11-08, user: "bigger / smaller texture dimensions
 * [should not] change the size of HUD elements"); one there is kept. */
bool convert(const std::wstring &data, std::wstring *message);

/* PC (2026-11-08): a pack folder converted before sizes.txt existed (a
 * folder of a pack's name - race\US\display, menu\disp ... - with PNG files
 * and no sizes.txt). */
bool needs_sizes(const std::wstring &data);
/* Writes their sizes.txt from the original packs, read from the disc image
 * the version was installed from (gt3pc_install.txt's source: an ISO image,
 * or a gt3.vol); when that cannot be read, from the pictures' sizes as they
 * are (a picture already resized then needs its line set by hand: said in
 * the message, and false).  message: what was done; nothing: true, empty. */
bool write_sizes(const std::wstring &data, std::wstring *message);

} // namespace image_packs
