/* crash.h - the crash report of GT3 PC: on an unhandled exception in any
 * thread, the exception, the registers and the code addresses found on the
 * stack (with their functions and lines from gt3pc.pdb) go to the log, and
 * a minidump to gt3pc_crash.dmp next to the program. */
#pragma once

namespace crash {

/* Installs the handler; call once, early. */
void install();

/* The code addresses found on the stack of another thread of the program
 * (`thread': its HANDLE, with suspend and context access; held still while
 * its stack is copied), nearest first, described as the crash report does,
 * each a log line after `prefix'.  A scan: stale return addresses show up
 * too.  Not for the calling thread. */
void log_stack(void *thread, const char *prefix);

/* "function+0xNN (file:line)" for a code address of the program. */
void describe_address(const void *addr, char *out, unsigned outSize);

} // namespace crash
