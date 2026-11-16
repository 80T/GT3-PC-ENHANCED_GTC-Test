/* texture_manager.h - the texture manager: a window of its own, opened from
 * the Play tab, over the texture replacement folders of gt3pc.ini [Textures]
 * (Count, then PackN the folder and PackNOn on / off, N = 1 the first).
 * Standard Windows controls, owned by the launcher's window, as the mod
 * manager's (mod_manager.h).  PC (2026-11-13).
 *
 * A texture folder holds replacement textures for the game's own: .png,
 * .jpg, .jpeg and .dds files anywhere under it, each named by its texture's
 * hash - 16 hexadecimal digits, then anything (0123456789ABCDEF.png,
 * 0123456789abcdef_bonnet.dds) - as the game's texture dumps are
 * (docs/renderer.md "Texture replacement"); a folder higher in the list wins
 * over the ones below it.  The list: each folder ticked (used) or not, its
 * texture count; Add folder..., Remove (from the list only), Move up / down,
 * Open (Explorer), and Open dumps folder, the game's dumps beside the
 * launcher (tex_replacements\dumps, made when missing, with
 * tex_replacements\dumps_selected).  Every change is written to gt3pc.ini at
 * once; the game reads the list when it starts.  The section's other keys
 * (Replace, Dump, PreCache ...) are the game's: never written here.
 *
 * A folder can say what it is as a mod folder does, with a mod.txt and a
 * mod.png at its top (mod_card.h): the list shows the title (else the
 * folder's name), a panel under it the selected row's picture (else the
 * launcher's default one), title, authors and description. */
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace texture_manager {

struct Pack {
    std::wstring folder;
    bool on = true;
};

/* The list of gt3pc.ini `ini' (a folder named empty left out). */
std::vector<Pack> load(const std::wstring &ini);

/* Opens the window (or brings it forward).  changed: called after each
 * change written. */
void open(HWND owner, const std::wstring &ini, void (*changed)());
bool is_open();
/* The window's keyboard navigation (Tab, Enter, Esc) from the launcher's
 * message loop: true when the message was its. */
bool filter(MSG &m);

} // namespace texture_manager
