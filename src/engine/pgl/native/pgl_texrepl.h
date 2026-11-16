/* pgl_texrepl.h - PC (2026-11-13, no original): external texture replacement
 * (docs/renderer.md "Texture replacement"; [Textures] of gt3pc.ini,
 * platform/settings.h; the interface: platform/texture_menu.h; the folders:
 * the launcher's texture manager).
 *
 * What a texture is here: a Tex1 set's texture as texture_bind decodes it
 * (pgl_textures.cpp) - the rectangle of its 2^TW x 2^TH space a draw reads
 * (the clamp region, or all of it), w x h texels of R, G, B and the GS's
 * alpha (0x80 = 1.0), after the palette.  Render targets, the movies'
 * pictures, uploads that are no set's (the kanji glyphs, the icons read
 * back) and the host images have no file form and are left out.
 *
 * Its name: content_hash of those texels, 16 hexadecimal digits.  The hash
 * reads nothing but the texels' values, their count across and down: not
 * the set, its place in memory or in VRAM, the format they were stored in or
 * their palette's order - two textures that decode to the same picture have
 * the same name wherever and however they are stored.  Two things in a
 * texture's rectangle are not its picture and are left out of the hash (read
 * as zeros): blocks no upload of the set wrote (TexSpace gives zeros), and
 * the blocks of the set's own CLUTs, which the game keeps in unused corners
 * of a texture's space and rewrites as a lamp glows (the registry's
 * set_content_hash).
 *
 * Dumps: [Textures] Dump=all writes each texture once, as it is first
 * decoded, to tex_replacements\dumps\<hash>.png; select writes the one chosen
 * in the interface to tex_replacements\dumps_selected.  A PNG's alpha is the
 * usual one: min(2 a, 255) of the GS's.
 *
 * Replacements: the folders of [Textures] PackN (the first wins) are listed
 * at start (and again at a refresh, with the pre-cache off: the files that
 * changed, came or went are taken), with the folders under them: a .dds, .png, .jpg or .jpeg
 * file whose name starts with a texture's 16 digits (anything may follow
 * them) replaces it, at any size.  A file is loaded on a worker thread the
 * first time its texture is decoded - at the set's load, by the worker that
 * decodes ahead, so usually before the first draw - and the texture is drawn
 * as the game's own until then.  The loaded form:
 *   - a .dds file as it is (BC1 / BC2 / BC3 / BC7 or 32-bit pixels, with the
 *     mip levels it has);
 *   - a .png / .jpg decoded (WIC), its mip levels made (2 x 2 means), then
 *     block-compressed (BC1, or BC3 with alpha; [Textures] Compress) or kept
 *     as 32-bit pixels.  The colours under alpha 0 are kept as the file has
 *     them: the game draws some textures' colours whatever their alpha.
 * [Textures] PreCache keeps that form in cache\textures\ (a file per source
 * file, named by the source's path, size and time), written at the file's
 * first use and, ahead of any use, for every listed file in the background:
 * a load is then a read and an upload.
 * A replacement's alpha is the usual one too (255 = opaque): the pixel
 * shader scales it to the GS's (TexBinding::alphaMode 3) and samples it
 * through its mip levels.
 *
 * Threads: the registry's calls come from the executor's thread but for
 * decoded() and ask(), which the decode-ahead worker calls too.  The
 * interface's calls come from the window's thread and the presenter's. */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "platform/texture_menu.h"          /* Status: the interface's (it is given these functions) */

struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;

namespace pglr {
namespace texrepl {

/* ---- the names ---------------------------------------------------------- */

/* The hash of w x h texels (0xAABBGGRR words, the GS's alpha).  Frozen: the
 * files' names are made of it. */
uint64_t content_hash(const uint32_t *px, uint32_t w, uint32_t h);
/* The 16 digits (upper case) and the terminator. */
void hash_name(uint64_t hash, char out[17]);

/* ---- the registry's side ------------------------------------------------ */

/* Once (registry_init): the settings and folders taken, the workers
 * started, the folders listed in the background. */
void init();
/* Replacing or dumping: the registry hashes what it decodes (any thread). */
bool hooks_on();
bool replacing();
/* Moves when what a bind gets may have changed for every texture: a
 * setting, the folders' list ready, the selection.  (A load's end moves no
 * serial: take_loaded.) */
uint64_t serial();
/* The textures bound keep their replacements though replacing is off: it
 * was turned off less than five minutes ago.  The on / off key
 * (TextureToggle) is for comparing a replacement with the game's own, back
 * and forth: both must be there at once, nothing loaded again.  Later the
 * registry lets them go (the setting left off). */
bool keep_loaded();
/* A texture decoded (any thread): Dump=all writes it once.  family: what its
 * indices are whatever its palette (0: none) - a texture whose palette is
 * animated would be written at every step, so a family that brings new
 * pictures several times a second is left out until it rests. */
void decoded(uint64_t hash, const uint32_t *px, uint32_t w, uint32_t h, uint64_t family);
/* The replacement of `hash': kReady when it is loaded - out's texture and
 * view hold a reference each for the caller; kPending: not loaded yet, which
 * this call asks for (take_loaded tells when); kNone: there is none (no
 * file, or one that was refused). */
enum { kNone, kPending, kReady };
struct Found {
    ID3D11Texture2D *tex = nullptr;
    ID3D11ShaderResourceView *srv = nullptr;
    uint32_t w = 0, h = 0, mean = 0;
    uint8_t aMin = 0, aMax = 128;           /* the GS's units */
    size_t bytes = 0;
};
int lookup(uint64_t hash, Found *out);
/* The same without taking it (any thread): its load asked for ahead. */
void ask(uint64_t hash);
/* The hashes whose loads ended since the last call (the executor, each
 * frame): their textures are bound anew. */
void take_loaded(std::vector<uint64_t> &out);
/* Refresh: the hashes whose replacement is another since the folders were
 * listed again - a file changed, new or gone (the executor, each frame; it
 * is the one that lets the old textures go): their textures ask anew, and
 * keep what they show until the new file is loaded. */
void take_changed(std::vector<uint64_t> &out);
/* The video memory the replacements bound may hold before the least
 * recently drawn go (bytes). */
size_t budget_bytes();
/* Each housekeeping turn of the registry (seconds: its clock): the loaded
 * files nothing asked for lately let go. */
void housekeep(double seconds);

/* Select mode (the interface open, Dump=select): the texture highlighted,
 * 0 none; its pixels given by the registry when it decodes it for the
 * highlight (for its dump). */
uint64_t selected();
void selected_pixels(uint64_t hash, const uint32_t *px, uint32_t w, uint32_t h);
/* The textures bound lately, when the interface wants them (list_wanted):
 * sorted by hash. */
struct Seen {
    uint64_t hash;
    uint16_t w, h;
    bool replaced;
};
bool list_wanted();
void publish(std::vector<Seen> &list);

/* ---- the interface's side (any thread) ----------------------------------- */

/* The interface shown or hidden; a setting changed (platform/settings.h):
 * taken at once. */
void menu_open(bool open);
void settings_changed();
/* The folders listed again, the changed files loaded anew (quick trials of
 * a replacement being made: save it, refresh, look).  False: refused - the
 * pre-cache is on.  refresh_serial moves when one is done (status has its
 * counts). */
bool refresh();
uint32_t refresh_serial();
/* Select mode: the next (1) / previous (-1) texture of the list. */
void select_step(int dir);
/* The selected texture written to dumps_selected (false: none, or its
 * pixels not here yet). */
bool dump_selected();

using Status = texture_menu::Status;
void status(Status *out);

} /* namespace texrepl */
} /* namespace pglr */
