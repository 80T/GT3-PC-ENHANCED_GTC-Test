/* menu_layout.h - the menu layout files (.pmb) as source text and a texture
 * file, offline (tools/menulayout; docs/menu_layouts.md).
 *
 * Written 2026-11-08 for a user request to convert the layouts' scripting
 * into source code; the in-game side was cancelled the same day (user: "The
 * game will read the .pmb files as it did.  Later on we will make tools to
 * externally export and convert instead").  This code stays as the offline
 * tool's, nothing of the game or the launcher uses it.
 *
 * A layout file (game/menu/framework T229 MenuLayoutData: pages, their items
 * and layers, a layer's items and object items, each item's rectangle,
 * texture and UVs, vertex colours, text, hook, active item and flags; then
 * the textures) is written as
 *   <name>.menu  the source: UTF-8 text, the syntax of docs/menu_layouts.md;
 *   <name>.tex   the textures as the file had them (gzipped Tex1 images),
 *                a 16-byte header, then the texture table and the data.
 * Compiled from the decompiled source, a layout comes back byte for byte:
 * decompile checks that itself (every layout of the game's format on the
 * US, JP, EU discs does).  The files of an older format on the discs (36- or
 * 32-byte names; the game's code cannot read them either), and empty or
 * zeroed ones, are refused.
 *
 * No game headers; Windows for the EUC-JP texts (code page 20932). */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace menu_layout {

/* How the texts' bytes are written in the source: "gt3" (the European
 * layouts: ASCII, a Latin-1 letter after a 0x81 byte - "Andr\x81\xe9" is
 * "André" - and Windows-1252's punctuation after 0x80 - "\x80\x92" is ’),
 * "euc-jp" (the Japanese ones: EUC-JP, those two-byte letters too).  A byte
 * that does not come back the same is written as an escape (\x88). */
enum class Encoding { gt3, euc_jp };
const char *encoding_name(Encoding e);

/* The source of the layout file `pmb' (`size' bytes) and its texture file,
 * checked: the two compile back to the file's bytes.  name: the layout's
 * path for the source's first line ("menu/US/gt_mode1").  False with the
 * reason when it is not a layout of the game's format, or does not come
 * back the same. */
bool decompile(const uint8_t *pmb, size_t size, Encoding encoding, const std::string &name, std::string *source,
               std::vector<uint8_t> *tex, std::string *why);

/* The layout file from its source (UTF-8) and texture file.  False with the
 * reason, "line N: ...". */
bool compile(const char *source, size_t sourceSize, const uint8_t *tex, size_t texSize, std::vector<uint8_t> *pmb,
             std::string *error);

/* The texture file's magic ("PMBT") and header size. */
constexpr uint32_t kTexMagic = 0x54424D50u;
constexpr size_t kTexHeader = 16;

} // namespace menu_layout
