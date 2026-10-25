/* mcard_io.cpp - blocking memory-card wrappers over Sony libmc.
 *
 * On the PS2 every *_sync wrapper took the MC lock, issued the libmc
 * request (retrying with yield_low_priority while libmc was busy), spun on
 * sceMcSync until mcserv on the IOP answered, released the lock and
 * returned the result.  Callers: the game's memory-card layer (T501 mc_*,
 * 0x0023E430..0x00240280).
 *
 * PC: the cards are folders of the game folder (platform/memcard.h, with
 * libmc's results), answered at once; the wrappers keep the lock. */
#include "engine/media/G002_mpeg_frameq.h"
#include "eekernel.h"
#include "platform/memcard.h"

/* .sdata: the lock, -1 until mc_init_once creates it. */
int g_mc_lock_sema = -1;                    /* 0x003531A0 */

namespace {

struct McLock {
    McLock() { WaitSema(g_mc_lock_sema); }
    ~McLock() { signal_sema_helper(g_mc_lock_sema); }
};

} // namespace

// FUNC 0x002670F0 mc_close_sync
int mc_close_sync(int fd)
{
    McLock lock;
    return memcard::close(fd);
}

// FUNC 0x00267160 mc_delete_sync
int mc_delete_sync(int port, int slot, const char *name)
{
    McLock lock;
    return memcard::remove(port, slot, name);
}

// FUNC 0x002671F0 mc_format_sync
int mc_format_sync(int port, int slot)
{
    McLock lock;
    return memcard::format(port, slot);
}

// FUNC 0x00267270 mc_getdir_sync
/* Returns the number of entries written or a negative sceMcRes*. */
int mc_getdir_sync(int port, int slot, const char *name, unsigned int mode,
                   int maxent, sceMcTblGetDir *table)
{
    McLock lock;
    return memcard::get_dir(port, slot, name, mode, maxent, table);
}

// FUNC 0x00267330 mc_getentspace_sync
int mc_getentspace_sync(int port, int slot, const char *path)
{
    McLock lock;
    return memcard::get_ent_space(port, slot, path);
}

// FUNC 0x002673C0 mc_getinfo_sync
int mc_getinfo_sync(int port, int slot, int *type, int *freeSpace, int *format)
{
    McLock lock;
    return memcard::get_info(port, slot, type, freeSpace, format);
}

// FUNC 0x00267470 mc_getslotmax_sync
int mc_getslotmax_sync(int port)
{
    McLock lock;
    return memcard::get_slot_max(port);
}

// FUNC 0x002674E0 mc_init_once
/* If the MC lock does not exist yet: create it (count 1) and bring libmc
 * up (PC: the card folders).  Caller: mc_manager_init (T501, 0x0023E9F8). */
void mc_init_once(void)
{
    if (g_mc_lock_sema < 0) {
        g_mc_lock_sema = create_mutex_sema(1);
        memcard::init();
    }
}

// FUNC 0x00267548 mc_mkdir_sync
int mc_mkdir_sync(int port, int slot, const char *name)
{
    McLock lock;
    return memcard::mkdir(port, slot, name);
}

// FUNC 0x002675D8 mc_open_sync
/* Returns the fd or a negative sceMcRes*. */
int mc_open_sync(int port, int slot, const char *name, int mode)
{
    McLock lock;
    return memcard::open(port, slot, name, mode);
}

// FUNC 0x00267678 mc_read_sync
int mc_read_sync(int fd, void *buf, int size)
{
    McLock lock;
    return memcard::read(fd, buf, size);
}

// FUNC 0x00267708 mc_rename_sync
int mc_rename_sync(int port, int slot, const char *org, const char *newname)
{
    McLock lock;
    return memcard::rename(port, slot, org, newname);
}

// FUNC 0x002677A8 mc_seek_sync
int mc_seek_sync(int fd, int offset, int origin)
{
    McLock lock;
    return memcard::seek(fd, offset, origin);
}

// FUNC 0x00267838 mc_setfileinfo_sync
int mc_setfileinfo_sync(int port, int slot, const char *name,
                        const char *info, unsigned int valid)
{
    McLock lock;
    return memcard::set_file_info(port, slot, name, info, valid);
}

// FUNC 0x00267948 mc_unformat_sync
int mc_unformat_sync(int port, int slot)
{
    McLock lock;
    return memcard::unformat(port, slot);
}

// FUNC 0x002679C8 mc_write_sync
int mc_write_sync(int fd, const void *buf, int size)
{
    McLock lock;
    return memcard::write(fd, buf, size);
}
