/* T521 - pdibgm.cxx.  0x0025C830..0x0025CE00.
 *
 * Client of the pdibgm IOP module (streamed background music): one object
 * of the anonymous-namespace class RPC (_GLOBAL_$N$.._.._pdibgm.cxxpLWmzt::RPC)
 * at 0x004A72E0, server 'PBGM' (0x5042474D).  Functions: 1 start, 2 stop,
 * 3 volume, 4 pause, 5 resume, 6 status (copied to g_002FCE00 by
 * pdibgm_update, read by pdibgm_get_status).  Users: RaceBGMPS2 (T130),
 * ads_play / ads_set_volume / ads_stop / music_thread (T466, 0x0022AC98 ..
 * 0x0022AFF8), mpegif::feed (0x0025EF00).
 *
 * PC: there is no IOP.  pdistr.irx's 'PBGM' service (the .ads reader) and
 * pdispu2.irx's stream voices are the native BGM player of
 * engine/sound/native (sound_native.h, docs/sound.md); each function keeps
 * its semantics and calls it directly.  The stream's bytes are read from
 * gt3.vol (platform/disc.h) or from a host: file (platform/host_file.h) by
 * the player's feeder thread, a native thread: the read callback below
 * uses no game kernel call. */
#include "engine/pdi/T521_pdibgm.h"
#include "engine/pdi/pdi_internal.h"
#include "engine/sound/native/sound_native.h"
#include "platform/disc.h"
#include "platform/host_file.h"

/* x clamped to [lo, hi]: max.s then min.s, volatile asm statements as every
 * max.s / min.s of the image behaves (u018's T055, u032's T077); an inline
 * clamp, as the listing loads both bounds before the max.s.  GCC 14: the C
 * ternaries (max.s / min.s differ only for -0.0 against 0.0). */
static inline f32 bgm_maxf(f32 a, f32 b) { return a > b ? a : b; }
static inline f32 bgm_minf(f32 a, f32 b) { return a < b ? a : b; }
static inline f32 bgm_clampf(f32 x, f32 lo, f32 hi)
{
    return bgm_minf(bgm_maxf(x, lo), hi);
}

/* The stream status (pdibgm_update copies the server's answer here).  A
 * public C++ global without an initialiser: .data, at the end of the file's
 * variables; aligned (at least 16: the 8 bytes from 0x002FCDF8, after
 * pdispu2's frame info, are padding). */
alignas(64) BgmStatus g_002FCE00;    /* 0x002FCE00 */

/* PC: native, no counterpart in the original. */
namespace {

/* PC: what the IOP's 'PBGM' start request carried ({lsn, sectors, host
 * name}), turned into a byte source for the native player: gt3.vol from
 * sector lsn, or the host: file from byte lsn; `sectors' x 2048 bytes. */
struct BgmSource {
    bool     host;
    int      hostFd;
    u64      base;
    u32      size;
};

BgmSource g_bgmSource = { false, -1, 0, 0 };

/* Called on the BGM player's feeder thread (native). */
int bgm_source_read(void *user, uint32_t offset, void *dst, uint32_t size)
{
    BgmSource *s = (BgmSource *)user;
    if (offset >= s->size)
        return 0;
    if (size > s->size - offset)
        size = s->size - offset;
    if (s->host)
        return host_file_read_at(s->hostFd, s->base + offset, dst, size);
    return disc::read_bytes(s->base + offset, dst, size) ? (int)size : -1;
}

} // namespace

/* Caller: RaceIOPLoadModules::cmdBindClients.
 * PC: nothing to bind (the native player is started with the platform). */
// FUNC 0x0025C8A0 pdibgm_init
void pdibgm_init(void)
{
}

/* Caller: ads_play (0x0022AC98), with bufSize 0x40000.  Resets the status
 * time to -1 and starts the stream (NOWAIT); hostName is copied into the
 * request when given.  `arg' is the loop flag (pdistr: at the end the
 * reader seeks back to the body).
 * PC: the player of sound_native.h, reading the file through
 * bgm_source_read; bufSize was the IOP's FIFO (the player has its own). */
// FUNC 0x0025C8C8 pdibgm_start
void pdibgm_start(int lsn, int sectors, int arg, int bufSize, const char *hostName)
{
    (void)bufSize;
    g_002FCE00.time  = -1;
    g_002FCE00.ended = 0;

    /* the previous stream's feeder ends before its source is reused */
    snd_native_bgm_stop();
    if (g_bgmSource.host && g_bgmSource.hostFd >= 0)
        host_file_close(g_bgmSource.hostFd);
    g_bgmSource.host = hostName != NULL && hostName[0] != '\0';
    g_bgmSource.size = (u32)sectors * disc::kSectorSize;
    if (g_bgmSource.host) {
        g_bgmSource.hostFd = host_file_open(hostName);
        g_bgmSource.base = (u32)lsn;
        if (g_bgmSource.hostFd < 0) {
            printf("pdibgm: %s not found\n", hostName);
            return;
        }
        /* a loose file ends before the size rounded up to sectors */
        s64 bytes = host_file_size(g_bgmSource.hostFd);
        if (bytes >= (s64)g_bgmSource.base && (u64)bytes - g_bgmSource.base < g_bgmSource.size)
            g_bgmSource.size = (u32)((u64)bytes - g_bgmSource.base);
    } else {
        g_bgmSource.hostFd = -1;
        g_bgmSource.base = (u64)(u32)lsn * disc::kSectorSize;
    }
    if (snd_native_bgm_start(bgm_source_read, &g_bgmSource, arg) != 0)
        printf("pdibgm: the stream at %s%d is not an ADPCM .ads\n", g_bgmSource.host ? "host: " : "lsn ", lsn);
}

/* Caller: RaceBGMPS2::pause (0x0017E3C0, pause). */
// FUNC 0x0025C980 pdibgm_pause
void pdibgm_pause(void)
{
    snd_native_bgm_pause();                     /* PC: 'PBGM' fno 4 */
}

/* Caller: RaceBGMPS2::resume (0x0017E3E0, resume). */
// FUNC 0x0025C9A8 pdibgm_resume
void pdibgm_resume(void)
{
    snd_native_bgm_resume();                    /* PC: 'PBGM' fno 5 */
}

/* Caller: ads_stop (0x0022AE18). */
// FUNC 0x0025C9D0 pdibgm_stop
void pdibgm_stop(void)
{
    snd_native_bgm_stop();                      /* PC: 'PBGM' fno 2 */
}

/* Callers: RaceBGMPS2::update (0x0017DDF0), ads_set_volume (0x0022AC78),
 * mpegif::feed (0x0025EF00).  volume is clamped to [-1, 1] (max.s / min.s)
 * and sent as volume * 32768 (cvt.w.s truncates). */
// FUNC 0x0025C9F8 pdibgm_set_volume
void pdibgm_set_volume(f32 volume)
{
    volume = bgm_clampf(volume, -1.0f, 1.0f);
    snd_native_bgm_set_volume((int)(volume * 32768.0f));   /* PC: 'PBGM' fno 3 */
}

/* Caller: music_thread (0x0022AFF8), per frame.  RPC::updateStatus
 * (0x0025CB88): 'PBGM' fno 6, the answer copied with interrupts off. */
// FUNC 0x0025CA78 pdibgm_update
void pdibgm_update(void)
{
    SndNativeBgmStatus answer;                  /* PC: the native player's status */

    snd_native_bgm_get_status(&answer);
    int wasEnabled = intr_suspend();
    memcpy(&g_002FCE00, &answer, sizeof(BgmStatus));
    intr_resume(wasEnabled);
}

/* Caller: RaceBGMPS2::update.  Reads the status with interrupts off and
 * returns its time in seconds (__floatdisf, then / 1e6). */
// FUNC 0x0025CA98 pdibgm_get_status
f32 pdibgm_get_status(int *pos, int *id, int *ended)
{
    int wasEnabled = intr_suspend();
    f32 seconds;

    if (pos != NULL)
        *pos = g_002FCE00.pos;
    if (id != NULL)
        *id = g_002FCE00.id;
    if (ended != NULL)
        *ended = g_002FCE00.ended;
    seconds = (f32)g_002FCE00.time / 1000000.0f;   /* implicit: __floatdisf (s64 -> float) */
    intr_resume(wasEnabled);
    return seconds;
}

/* PC: the native status block is the IOP's answer, byte for byte. */
static_assert(sizeof(SndNativeBgmStatus) == sizeof(BgmStatus), "BgmStatus");
