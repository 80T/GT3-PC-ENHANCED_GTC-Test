/* T522 - pdimpeg.cxx.  0x0025CE00..0x0025D348.
 *
 * Client of the pdimpeg IOP module (movie streams): an array of two objects
 * of the anonymous-namespace class RPC (_GLOBAL_$N$.._.._pdimpeg.cxxIPKYuE::RPC)
 * at 0x004A73A0, servers 'MPG1' (0x4D504731) and 'MPG2' (0x4D504732).
 * MPG1: 1 open (mode, lsn, sectors, host name), 2 feed (reads the next bytes
 * of the stream into an EE buffer), 4 close, all blocking; MPG2: 3 with two
 * doublewords (NOWAIT).  Users: mpegif::preload / mpegif::feed (T529,
 * 0x0025E710, 0x0025EF00), MenuBgMovie::update / stopAndWait (T253, 0x001CD1E0,
 * 0x001CD558).
 *
 * PC: there is no IOP.  The module behind 'MPG1' / 'MPG2' is the native
 * movie service of engine/media/native (media_services.h: gt3m_mpg_*,
 * docs/movies.md): it reads the PSS from gt3.vol (or the host: file), hands
 * out the video as 4 KB PES units as the module did, plays the sound
 * through the native BGM player (sound_native.h's push functions, the
 * BGM voices the module used) and holds it to the presentation window.
 * Each function keeps its semantics and calls it directly; the RPC objects,
 * their constructors and the static initialiser / destructor are gone. */
#include "engine/pdi/T522_pdimpeg.h"
#include "engine/media/native/media_services.h"
#include "eekernel.h"                           /* DelayThread */

/* Caller: RaceIOPLoadModules::cmdBindClients.
 * PC: nothing to bind. */
// FUNC 0x0025CE70 pdimpeg_init
void pdimpeg_init(void)
{
}

/* Callers: mpegif::preload (mode 1), mpegif::feed (2 / 3). */
// FUNC 0x0025CEB8 pdimpeg_open
void pdimpeg_open(int mode, int lsn, int sectors, const char *hostName)
{
    gt3m_mpg_open(mode, (unsigned)lsn, (unsigned)sectors, hostName);   /* PC: 'MPG1' fno 1 */
}

/* Callers: mpegif::preload, mpegif::feed.
 * PC: the RPC blocked the calling thread while the IOP filled the buffer;
 * mpegif::feed relies on that: after a one-shot movie's end it keeps
 * reading empty blocks until the decoder (priority 11, below the feeder's
 * 10) has finished.  The native read is immediate, so when nothing is left
 * the call waits a few milliseconds through the kernel, as the RPC did. */
#define PDIMPEG_EMPTY_FEED_WAIT 4000            /* us */
// FUNC 0x0025CF50 pdimpeg_feed
void pdimpeg_feed(void *buf, int size)
{
    if (gt3m_mpg_feed(buf, size) == 0)          /* PC: 'MPG1' fno 2 */
        DelayThread(PDIMPEG_EMPTY_FEED_WAIT);
}

/* Callers: MenuBgMovie::update (0x001CD1E0), MenuBgMovie::stopAndWait
 * (0x001CD558): the presentation window {lo, hi} in microseconds, (0, MAX)
 * to open it. */
// FUNC 0x0025CFC0 F_0025CFC0
void F_0025CFC0(s64 lo, s64 hi)
{
    gt3m_mpg_window(lo, hi);                    /* PC: 'MPG2' fno 3 */
}

/* Callers: mpegif::preload, mpegif::feed. */
// FUNC 0x0025D028 pdimpeg_close
void pdimpeg_close(void)
{
    gt3m_mpg_close();                           /* PC: 'MPG1' fno 4 */
}
