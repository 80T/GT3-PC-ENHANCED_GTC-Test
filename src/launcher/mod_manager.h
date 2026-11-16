/* mod_manager.h - the mod manager: a window of its own, opened from the
 * Play tab, over the mod folders of gt3pc.ini [Mods] (Count, then ModN the
 * folder and ModNOn on / off, N = 1 the first).  Standard Windows controls,
 * owned by the launcher's window.
 *
 * A mod folder is laid out as a data folder (cars\, courses\, menu\ ...):
 * the game reads its files in place of the data folder's of the same path
 * and adds its new ones (src/platform/host_file.h); a folder higher in the
 * list wins over the ones below it.  The list: each folder ticked (used) or
 * not, its file count; Add folder..., Remove (from the list only), Move up
 * / down, Open (Explorer), and Create template..., which makes the data
 * folder's folders - none of its files - in a folder chosen, with a mod.txt
 * and mod.png to change, as a start for a mod, and adds it.  Every change is
 * written to gt3pc.ini at once; the game reads the list when it starts.
 *
 * A folder can say what it is with two files at its top (mod_card.h), which
 * the game leaves out: mod.txt (Title:, Author: or Authors:, Description:, the
 * last one to the file's end) and mod.png (a 1:1 picture).  The list shows the
 * title (else the folder's name), a panel under it the selected row's
 * picture (else the launcher's default one), title, authors and
 * description. */
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace mod_manager {

struct Mod {
    std::wstring folder;
    bool on = true;
};

/* The list of gt3pc.ini `ini' (a folder named empty left out). */
std::vector<Mod> load(const std::wstring &ini);

/* Opens the window (or brings it forward).  data: the installed versions'
 * data folders (the template's folders are theirs); changed: called after
 * each change written. */
void open(HWND owner, const std::wstring &ini, const std::vector<std::wstring> &data, void (*changed)());
bool is_open();
/* The window's keyboard navigation (Tab, Enter, Esc) from the launcher's
 * message loop: true when the message was its. */
bool filter(MSG &m);

} // namespace mod_manager
