/* memcard.h - the PS2 memory cards of GT3 PC.
 *
 * Each memory card is a folder of the game folder: memcard1\ for the card
 * in slot 1 (port 0), memcard2\ for slot 2 (port 1).  A card's directories
 * and files are real folders and files, so saves can be copied, backed up
 * or edited.  The functions follow Sony's libmc (as the engine's blocking
 * wrappers used it): the same arguments, results and error codes
 * (sceMcRes*), synchronous.  Thread-safe.
 *
 * An 8 MB card: free space is 8000 KB minus what the folder holds, counted
 * in 1 KB clusters as the PS2 did (so the game's "not enough free space"
 * checks still mean something). */
#pragma once

#include "libmc.h"

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

} // namespace memcard
