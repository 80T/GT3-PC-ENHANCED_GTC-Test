/* hostfs_io.cpp - fileio wrappers serialised by the pdistd global
 * lock (0x00269D80..0x00269F88).  Callers: host_read_file / host_write_file
 * (0x00241E08 / 0x00241F10), file_resolve, host_file_size,
 * host_file_exists.
 *
 * PC: hostfs_io.c of the decomp in C++.  sceOpen & co. went through the IOP
 * to "host:" (the development PC) or other devices; here the paths are
 * files of the data folder (platform/host_file.h) with a read / write
 * position per descriptor, as the EE fileio kept. */
#include "engine/media/G002_mpeg_frameq.h"
#include "engine/pdi/G003_pdi1394_rpc.h"      /* global_lock_acquire / release */
#include "sifdev.h"                             /* SCE_RDONLY .., SCE_SEEK_*, SCE_STM_* */
#include "platform/host_file.h"

namespace {

struct HostFd {
    bool used;
    int  file;          /* host_file descriptor */
    s64  pos;
};

const int kFds = 32;
HostFd g_fds[kFds];

HostFd *fd_of(int fd)
{
    return fd >= 0 && fd < kFds && g_fds[fd].used ? &g_fds[fd] : NULL;
}

} // namespace

// FUNC 0x00269D80 host_open
// Locked sceOpen(path, flags).  Returns the fd (>= 0) or a negative error.
// Callers host_read_file, host_write_file, io_getstat.
int host_open(const char *path, int flags)
{
    global_lock_acquire();
    int fd = -1;
    for (int i = 0; i < kFds; i++)
        if (!g_fds[i].used) {
            fd = i;
            break;
        }
    if (fd >= 0) {
        int file = (flags & (SCE_WRONLY | SCE_CREAT | SCE_TRUNC)) ? host_file_create(path)
                                                                 : host_file_open(path);
        if (file < 0) {
            fd = -1;
        } else {
            g_fds[fd].used = true;
            g_fds[fd].file = file;
            g_fds[fd].pos = 0;
        }
    }
    global_lock_release();
    return fd;
}

// FUNC 0x00269DC8 host_lseek
// Locked sceLseek(fd, offset, whence); returns the new position or a
// negative error.  Used with (0, SCE_SEEK_END) to get file sizes.
int host_lseek(int fd, int offset, int whence)
{
    global_lock_acquire();
    int pos = -1;
    HostFd *f = fd_of(fd);
    if (f != NULL) {
        s64 base = whence == SCE_SEEK_END ? host_file_size(f->file)
                 : whence == SCE_SEEK_CUR ? f->pos : 0;
        f->pos = base + offset;
        pos = (int)f->pos;
    }
    global_lock_release();
    return pos;
}

// FUNC 0x00269E20 host_read
// Locked sceRead(fd, buf, len); returns bytes read or a negative error.
int host_read(int fd, void *buf, int len)
{
    global_lock_acquire();
    int n = -1;
    HostFd *f = fd_of(fd);
    if (f != NULL) {
        n = host_file_read_at(f->file, f->pos, buf, len);
        if (n > 0)
            f->pos += n;
    }
    global_lock_release();
    return n;
}

// FUNC 0x00269E78 host_write
// Locked sceWrite(fd, buf, len); returns bytes written or a negative error.
int host_write(int fd, const void *buf, int len)
{
    global_lock_acquire();
    int n = -1;
    HostFd *f = fd_of(fd);
    if (f != NULL) {
        n = host_file_write_at(f->file, f->pos, buf, len);
        if (n > 0)
            f->pos += n;
    }
    global_lock_release();
    return n;
}

// FUNC 0x00269ED0 host_close
// Locked sceClose(fd); returns its result.
int host_close(int fd)
{
    global_lock_acquire();
    int r = -1;
    HostFd *f = fd_of(fd);
    if (f != NULL) {
        host_file_close(f->file);
        f->used = false;
        r = 0;
    }
    global_lock_release();
    return r;
}

// FUNC 0x00269F08 io_getstat
// stat-like probe of a file: open it read-only; on failure return -1
// without touching *out.  Otherwise size = lseek(fd, 0, SEEK_END), close,
// out->mode = 0x11 (SCE_STM_R | SCE_STM_F), return 0.  Callers:
// file_resolve, host_file_size, host_file_exists.
int io_getstat(const char *path, IoStat *out)
{
    host_file_quiet(1);                     /* PC: a probe, not in the file log (platform/host_file.h) */
    int fd = host_open(path, SCE_RDONLY);
    host_file_quiet(0);
    if (fd < 0)
        return -1;
    int size = host_lseek(fd, 0, SCE_SEEK_END);
    /* PC: a file of 2 GB or more: its size kept positive (IoStat's is an
     * int), the most an int holds */
    HostFd *f = fd_of(fd);
    if (f != NULL && host_file_size(f->file) > 0x7FFFFFFF)
        size = 0x7FFFFFFF;
    host_close(fd);
    out->mode = SCE_STM_R | SCE_STM_F;
    out->size = size;
    return 0;
}
