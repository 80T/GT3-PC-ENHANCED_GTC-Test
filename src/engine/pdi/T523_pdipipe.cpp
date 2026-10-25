/* T523 - pdipipe.cxx.  0x0025D348..0x0025DA00.
 *
 * Client of the pdipipe IOP module (streamed file reads): one object of the
 * anonymous-namespace class RPC (_GLOBAL_$N$.._.._pdipipe.cxxIVS7RR::RPC) at
 * 0x004A7520, server 'PDIP' (0x50444950).  Functions (all blocking):
 * 1 close, 2 / 5 open (answer word 0 = the handle), 3 / 6 read into an EE
 * buffer, 4 read into SPU2 memory (buf = the SPU address), 7 / 8 with a name
 * and a two-word answer.  Users: GZIP::InflateLoader::fetchInput (0x0023E1E0),
 * vol_thread_main (0x002411E8), file_read_sound_bank (0x002418D0).
 *
 * PC: the IOP module read the disc (or a host: file) through its own
 * buffers and sent the bytes in order; here a pipe is a read position in
 * gt3.vol (platform/disc.h) or in a host file of the data folder, and
 * reads are done at once.  Reads into SPU2 memory go to the sound mixer's
 * sample memory (sound/native). */
#include "engine/pdi/T523_pdipipe.h"
#include "engine/pdi/pdi_internal.h"
#include "platform/disc.h"
#include "platform/host_file.h"
#include "platform/scheduler.h"
#include "engine/sound/spu_memory.h"

namespace {

/* One open pipe. */
struct Pipe {
    bool     used;
    bool     host;      /* a host: file (else the disc) */
    int      hostFd;
    u64      pos;       /* byte position */
    u64      end;       /* byte end of the stream */
};

const int kPipes = 16;
Pipe g_pipes[kPipes];
int  g_lockSema = -1;

void lock()
{
    if (g_lockSema >= 0)
        WaitSema(g_lockSema);
}

void unlock()
{
    if (g_lockSema >= 0)
        signal_sema_helper(g_lockSema);
}

/* Reads up to size bytes of pipe h into dst; the rest of dst is cleared
 * when the stream ends (the IOP module sent whole buffers). */
void pipe_read(int h, void *dst, int size)
{
    if (h < 0 || h >= kPipes || !g_pipes[h].used || size <= 0)
        return;
    Pipe &p = g_pipes[h];
    u64 left = p.end > p.pos ? p.end - p.pos : 0;
    u32 n = (u64)size < left ? (u32)size : (u32)left;
    bool ok = true;
    if (n > 0) {
        if (p.host) {
            /* PC: a short read (the file ends before the stream's size: a
             * .gz is opened with its unpacked size, vol_thread_main) keeps
             * what was read; the rest is cleared below (it was all
             * cleared, losing the .gz's last piece) */
            int got = host_file_read_at(p.hostFd, p.pos, dst, n);
            ok = got >= 0;
            if (ok)
                n = (u32)got;
        } else {
            ok = disc::read_bytes(p.pos, dst, n);
        }
    }
    if (!ok)
        n = 0;
    if (n < (u32)size)
        memset((u8 *)dst + n, 0, size - n);
    p.pos += n;
}

} // namespace

/* Caller: RaceIOPLoadModules::cmdBindClients. */
// FUNC 0x0025D520 pdipipe_init
void pdipipe_init(void)
{
    if (g_lockSema < 0)
        g_lockSema = create_mutex_sema(1);
}

/* Opens a stream of `size' bytes at sector `lsn' of the disc, or at byte
 * offset `lsn' of the host file hostPath ("" or NULL: the disc).  bufSize
 * was the IOP's buffer.  Returns the handle (-1: none free).  Callers:
 * vol_thread_main (0x002411E8), file_read_sound_bank (0x002418D0). */
// FUNC 0x0025D548 pdipipe_open
int pdipipe_open(int lsn, int size, int bufSize, const char *hostPath)
{
    (void)bufSize;
    lock();
    int h = -1;
    for (int i = 0; i < kPipes; i++)
        if (!g_pipes[i].used) {
            h = i;
            break;
        }
    if (h >= 0) {
        Pipe &p = g_pipes[h];
        p.used = true;
        p.host = hostPath != NULL && hostPath[0] != 0;
        if (p.host) {
            p.hostFd = host_file_open(hostPath);
            p.pos = (u32)lsn;
        } else {
            p.hostFd = -1;
            p.pos = (u64)(u32)lsn * disc::kSectorSize;
        }
        p.end = p.pos + (u32)size;
    }
    unlock();
    return h;
}

/* Callers: GZIP::InflateLoader::fetchInput (0x0023E1E0), vol_thread_main. */
// FUNC 0x0025D5B8 pdipipe_read
void pdipipe_read(int h, void *buf, int size)
{
    lock();
    pipe_read(h, buf, size);
    unlock();
    sched::preempt_point();
}

/* PC: how long an upload into sound memory waits for the voices still
 * playing there (pdipipe_read2), and its polling step. */
const int kSpuUploadWaitUs = 2000000;
const int kSpuUploadPollUs = 10000;

/* Caller: file_read_sound_bank: toSpu 1 for the samples, buf = their SPU2
 * address.  PC: into the mixer's sample memory, once the voices still
 * sounding there have ended (below). */
// FUNC 0x0025D5F0 pdipipe_read2
void pdipipe_read2(int h, void *buf, int size, int toSpu)
{
    if (toSpu == 0) {
        pdipipe_read(h, buf, size);
        return;
    }
    if (size <= 0)
        return;                     /* PC: nothing (a damaged bank's size) */
    u8 *tmp = (u8 *)malloc(size);
    if (tmp == NULL) {
        printf("PC: no memory for a %d-byte sound upload; not loaded\n", size);   /* PC */
        return;
    }
    lock();
    pipe_read(h, tmp, size);
    unlock();
    /* PC: on the PS2 the samples reached SPU RAM after the IOP had read them
     * from the disc.  A race reads its course and cars before its engine
     * banks, seconds of disc time after the menu stopped its music
     * (bgm_stop: the sequencer keys its notes off at its next tick), so the
     * notes still releasing in music.ins, where the engine banks go, had
     * ended.  Here everything is read at once (the menu's last notes were
     * 0.15 s old: GT3PC_SND_TRACE showed them playing the new engine data,
     * looping to stale addresses).  So the transfer waits, sleeping on the
     * game scheduler (the music thread keeps sending the state), until no
     * voice sounds in the range, 2 s at most (a voice nobody keys off: then
     * as the PS2, the upload goes under it).  Not on a non-game thread. */
    if (sched::current_thread() != 0) {
        int waited = 0;
        int busy = spu_memory_voices_reading((u32)buf, (u32)size);
        int first = busy;
        while (busy > 0 && waited < kSpuUploadWaitUs) {
            sched::delay_thread(kSpuUploadPollUs);
            waited += kSpuUploadPollUs;
            busy = spu_memory_voices_reading((u32)buf, (u32)size);
        }
        if (first > 0 && snd_trace_level() > 0)
            printf("snd: upload to SPU 0x%07X waited %d ms for %d voice(s) sounding there%s\n", (u32)buf,
                   waited / 1000, first, busy > 0 ? " (gave up: some still sound)" : "");
    }
    spu_memory_write((u32)buf, tmp, size);
    free(tmp);
    sched::preempt_point();
}

/* Callers: vol_thread_main, file_read_sound_bank. */
// FUNC 0x0025D6C8 pdipipe_close
void pdipipe_close(int h)
{
    lock();
    if (h >= 0 && h < kPipes && g_pipes[h].used) {
        if (g_pipes[h].host)
            host_file_close(g_pipes[h].hostFd);
        g_pipes[h].used = false;
    }
    unlock();
}
