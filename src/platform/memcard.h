/* memcard.h - the PS2 memory cards of GT3 PC.
 *
 * Each memory card is a folder: until 2026-11-15 memcard1\ and memcard2\ of
 * the game folder; now (PC 2026-11-15, the save manager) a save folder of
 * the engine's, saves\<engine>\<name>\ (platform/save_folders.h), the one
 * gt3pc_engine.ini [Saves] Slot1 names in slot 1 (port 0), Slot2's in slot 2
 * (port 1) - none, or a folder not there: no card in that slot.  A card's
 * directories and files are real folders and files, so saves can be copied,
 * backed up or edited.  The functions follow Sony's libmc (as the engine's
 * blocking wrappers used it): the same arguments, results and error codes
 * (sceMcRes*), synchronous.  Thread-safe.
 *
 * An 8 MB card: free space is 8000 KB minus what the folder holds, counted
 * in 1 KB clusters as the PS2 did (so the game's "not enough free space"
 * checks still mean something). */
#pragma once

#include "libmc.h"

#include <string>
#include <vector>

namespace memcard {

void init();
int  get_slot_max(int port);
int  get_info(int port, int slot, int *type, int *freeClusters, int *format);
int  open(int port, int slot, const char *name, int mode);
int  close(int fd);
int  seek(int fd, int offset, int origin);
int  read(int fd, void *buf, int size);
int  write(int fd, const void *buf, int size);
int  mkdir(int port, int slot, const char *name);
int  remove(int port, int slot, const char *name);
int  rename(int port, int slot, const char *org, const char *newName);
int  get_dir(int port, int slot, const char *pattern, unsigned int mode, int maxent, sceMcTblGetDir *table);
int  set_file_info(int port, int slot, const char *name, const char *info, unsigned int valid);
int  get_ent_space(int port, int slot, const char *path);
int  format(int port, int slot);
int  unformat(int port, int slot);

/* A card is being changed: a file open for writing, or a change less than
 * 2 s ago (a save is several files: the gaps between them too).  The Esc
 * menu's Exit / Restart wait for the end (platform/system_menu.h). */
bool busy();

/* PC (2026-11-15, user request: "F2 menu gets selectors for slot 1 and 2,
 * options are populated from the engine's save folder"): the save folders
 * (ANSI names; save_folders.h), for the F2 menu's MEMORY CARDS.
 *
 * Changing a slot's folder is a card taken out and another put in: the
 * game's next sceMcGetInfo of that slot answers "a new card" (-1; none: no
 * card), its other calls -1 until then (libmc's answer after a swap, which
 * T501 mc_slot_set_result handles: the slot re-read).  While the slot's card
 * has a file open the change waits for it to close. */
const std::string &saves_dir();                 /* saves\<engine>\, with its '\' */
std::vector<std::string> folders();             /* the folders there a slot can take, sorted (read at most
                                                   once a second) */
std::string slot_folder(int port);              /* the folder chosen for the slot, "" none */
bool slot_folder_there(int port);               /* it is in saves\<engine>\ */
bool slot_waiting(int port);                    /* chosen, waiting for the slot's open file */
/* Puts `name` in the slot ("" no card) and writes gt3pc_engine.ini [Saves];
 * false (nothing changed) when the other slot has it. */
bool set_slot_folder(int port, const std::string &name);

} // namespace memcard
