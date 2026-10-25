/* host_file.h - "host:" files of GT3 PC.
 *
 * On the PS2 "host:" was the development PC's file system, reached through
 * the debugger (and HostFS patches).  In the port a host path names a file
 * in the data folder (platform/paths.h): "host:data/foo" or "data/foo" ->
 * <data folder>\data\foo.  Handles are small integers; reads are
 * positional and thread-safe.  The game reads all its files this way: the
 * volume is mounted as the loose host:data/ files (engine/io/T503 vol_init,
 * type 3), which the launcher unpacks from the disc's GT3.VOL. */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int  host_file_open(const char *path);                  /* -1 if missing */
/* The Windows path (the ANSI code page's) of a host path, into out (size
 * bytes): 0, or -1 when it does not fit.  Whether the file exists is not
 * checked. */
int  host_file_full_path(const char *path, char *out, uint32_t size);
/* The file exists (a quiet probe: not logged as missing; not the game's
 * host_file_exists, T505). */
int  host_file_present(const char *path);
int  host_file_create(const char *path);                /* for writing; -1 on error */
/* With gt3pc.ini [Debug] FileLog=on the opens are logged ("file: ..."; the
 * debug console shows them): each file read with its size, each one
 * written, a file missing.  Probes are not (file_resolve's io_getstat: the
 * size, the packed copy): on, the calling thread's opens are quiet. */
void host_file_quiet(int on);
void host_file_close(int fd);
int64_t host_file_size(int fd);
int  host_file_read_at(int fd, uint64_t offset, void *buf, uint32_t bytes);   /* bytes read */
int  host_file_write_at(int fd, uint64_t offset, const void *buf, uint32_t bytes);

/* A "RoFS" table of contents (the layout of include/engine/io/T500_vol_toc.h)
 * listing the folder `path` ("host:data") and everything under it, built in
 * a buffer from alloc(size); *size gets its size.  Entries are sorted by
 * name (byte order) as the volume's are; a file's record has its size, no
 * sector.  Left out: hidden and system files (desktop.ini, Thumbs.db),
 * "*.gz" (the packed form of a file next to it) and the install marker.
 * NULL when the folder cannot be listed. */
uint8_t *host_file_toc(const char *path, void *(*alloc)(uint32_t size), uint32_t *size);

#ifdef __cplusplus
}
#endif
