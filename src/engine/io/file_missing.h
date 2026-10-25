/* file_missing.h - what the port does where the game waits for a file
 * that is not there.
 *
 * The PS2 file layer retries file_resolve forever on "not found" (the disc
 * could not lack a file; only a read error ended a lookup that way).  The
 * PC data may lack a file (files moved between builds, a bad install), so
 * each retry loop calls this: it names the file in the log once and lets
 * the other game threads run for a while before the next try, instead of
 * hanging silently with the CPU taken.
 *
 * Also the file layer's path and log helpers (PC: the PS2 built its host:
 * names in 0x40..0x100-byte buffers with unchecked strcpy / strcat; data
 * transplanted from other builds may have longer paths and bigger files). */
#pragma once

void file_missing_wait(const char *path);

/* The size of the file layer's path buffers. */
#define IO_PATH_MAX 0x200

/* a + b + c (any may be NULL) into dst, cap bytes, always terminated.  1 if
 * it fits; 0 if cut (logged once per path: "PC: path too long ..."). */
int io_path_join(char *dst, int cap, const char *a, const char *b, const char *c);

/* 1 the first time `what' is asked for `path', then 0: for log lines that
 * come once per file (a hash of both is kept; a collision only hides a
 * line). */
int io_once(const char *what, const char *path);
