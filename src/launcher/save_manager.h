/* save_manager.h - the save manager: a window of its own, opened from the
 * launcher's Saves... button (under Textures...), over the engine's save
 * folders - its memory cards (platform/save_folders.h).  Standard Windows
 * controls, owned by the launcher's window, as the texture manager's
 * (texture_manager.h).
 *
 * PC (2026-11-15, user request: "Save manager, under the Textures manager
 * button in the launcher ... Each engine gets their own save manager config.
 * 2 Slots for the virtual card ports: Slots are assigned a folder.  Show
 * Statistics of the folder, game data saves, replays, etc.  Rename a save
 * folder (New 1 --> Challenge Run A).  Clear a folder's contents but keep the
 * folder.  Delete a folder completely.  Manage the contents of a folder: Game
 * Data and replays, etc. are listed with their .ico, essentially the same as
 * the in-game file operation menu.  Folder list is populated by the
 * per-engine folder, what's in there is what it sees").
 *
 * The window: the folder in each memory card slot (gt3pc_engine.ini [Saves],
 * written at once; the game reads it when it starts, its F2 menu's MEMORY
 * CARDS changes it in the game); the folders of saves\<engine>\, as they are
 * on the disk (read again when the window is come back to) - New ("New 1",
 * "New 2" ..., its name then edited), Rename (the name in place, F2 too; a
 * slot holding it follows), Clear... (the saves deleted, the folder kept),
 * Delete... (the folder and its saves; a slot holding it: no card); the
 * selected folder's statistics (its saves by kind, its space as the game
 * counts it on an 8 MB card, its game data's progress) and its saves, each
 * with its icon (drawn from its .ico as the game's file screens do,
 * ps2_icon.h), its title, kind, size and date, the selected one's details
 * under them (save_card.h) - Copy to... (into another folder, the space
 * checked as the game's copy does, a save there already replaced when said
 * so) and Delete saves... */
#pragma once

#include <windows.h>

#include <string>

namespace save_manager {

/* Opens the window (or brings it forward).  savesDir: saves\<engine>\ (with
 * its '\'); engineIni: the engine's gt3pc_engine.ini; engine: its name (the
 * title); changed: called after each change of the slots or the folders. */
void open(HWND owner, const std::wstring &savesDir, const std::wstring &engineIni, const std::wstring &engine,
          void (*changed)());
bool is_open();
/* The window's keyboard navigation (Tab, Enter, Esc) from the launcher's
 * message loop: true when the message was its. */
bool filter(MSG &m);

/* What the slots hold, for the launcher's text under the button:
 * "Slot 1: Challenge Run A\nSlot 2: no card". */
std::wstring summary(const std::wstring &savesDir, const std::wstring &engineIni);

} // namespace save_manager
