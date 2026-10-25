/* T529 - mpegif, the MPEG movie streamer.  0x0025E548..0x0025F390.
 *
 * The feeder thread (threadMain / feed) opens the movie on the IOP stream
 * (pdimpeg, T522) or uses the preloaded copy, reads it 32 KB at a time and
 * hands it to the G002 MPEG player (u121) in 4 KB PES packets; the player's
 * thread signals g_mpeg_start_sema once it runs and wakes the feeder
 * (g_mpeg_wake_thread) when its input ring has room again.  The game thread
 * drives it through control() (MPEGIF_CMD_*) and takes decoded frames with
 * nextFrame() / releaseFrame().  The semaphores and flags are globals, so
 * only one mpegif can exist at a time.
 *
 * Callers: MenuBgMovie (T253: the opening / ending movies through
 * play_movie, T051, and the menu backgrounds). */
#include "engine/media/mpegif_internal.h"
#include "engine/io/T502_file_resolve.h"        /* PC: file_locate */
#include "engine/io/T503_vol_thread.h"          /* PC: vol_device_object */
#include "engine/io/file_missing.h"             /* PC: IO_PATH_MAX (file_locate's longest name) */

/* PC: file_locate writes up to IO_PATH_MAX bytes into the host: names */
static_assert(MPEGIF_NAME_SIZE >= IO_PATH_MAX, "mpegif's host: names");

#define FEED_BLOCK_SIZE     0x8000      /* bytes read from the stream at a time */
#define PES_PACKET_SIZE     0x1000      /* one PSS pack / video PES packet */
#define PACKETS_PER_BLOCK   (FEED_BLOCK_SIZE / PES_PACKET_SIZE)

#define FEEDER_STACK        ((void *)0x70001700)    /* scratchpad RAM */
#define FEEDER_STACK_SIZE   0xC00

/* The sector count of a size in bytes, computed in 64 bits
 * (lwu; daddiu 0x7FF; dsll 21; dsra32 0). */
#define BYTES_TO_SECTORS(sz) ((int)(((u64)(u32)(sz) + 0x7FF) >> 11))

/* .rodata: the semaphore parameters (external linkage, so egcs writes them
 * here, in front of control()'s jump table and the vtable). */
const struct SemaParam g_mutex_sema_param = { 0, 1, 1, 0, 0, 0 };   /* 0x00345A20 max 1, init 1 */
const struct SemaParam g_signal_sema_param = { 0, 1, 0, 0, 0, 0 };  /* 0x00345A38 max 1, init 0 */

/* .sdata: zeros at the end of the file's output (after the type name), in
 * first-declaration order (T529_mpegif.h, then mpegif_internal.h). */
int g_mpeg_start_sema;          /* 0x00353110 */
int g_frameq_filled_once;       /* 0x00353114 */
int g_mpeg_wake_thread;         /* 0x00353118 */
int g_frames_stopped;           /* 0x0035311C */
int g_reading_stream;           /* 0x00353120 */

/* PC: file_resolve_retry (T006) waits for a missing file forever, which a
 * disc never needed; three of the arcade table's movies (T029:
 * stream/menu_bg.pss, arcade_top.pss, arcade_level.pss) are not on the US
 * disc.  The movie's file is looked up once instead; when it is missing
 * the stream ends at once (state MPEGIF_ENDED, no frames), so the callers'
 * waits (play_movie, MenuBgMovie::stopAndWait) end too. */
static int mpegif_locate(const char *file, int *lsn, u32 *size, char *host)
{
    if (file_locate(file, host, lsn, size, &vol_device_object) != -1)
        return 1;
    /* PC: a mod's .mp4 alone in the .pss's place (docs/movies.md "MP4
     * movies"; beside a .pss the service picks it itself) */
    char mp4[MPEGIF_NAME_SIZE];
    int n = (int)strlen(file);
    if (n > 4 && n < (int)sizeof(mp4) && file[n - 4] == '.' && (file[n - 3] | 0x20) == 'p' &&
        (file[n - 2] | 0x20) == 's' && (file[n - 1] | 0x20) == 's') {
        memcpy(mp4, file, (size_t)n - 3);
        strcpy(mp4 + n - 3, "mp4");
        if (file_locate(mp4, host, lsn, size, &vol_device_object) != -1)
            return 1;
    }
    printf("mpegif: %s is not in the game data; the movie is skipped\n", file);
    return 0;
}

/* PC: 1 the first time file is named here (the preload log comes once per
 * movie, not each time a menu preloads it). */
static int mpegif_first_time(const char *file)
{
    static char s_seen[16][MPEGIF_NAME_SIZE];
    static int s_count;

    for (int i = 0; i < s_count; i++)
        if (strncmp(s_seen[i], file, MPEGIF_NAME_SIZE - 1) == 0)
            return 0;
    if (s_count < 16)
        strncpy(s_seen[s_count++], file, MPEGIF_NAME_SIZE - 1);
    return 1;
}

// FUNC 0x0025E548 mpegif::mpegif
mpegif::mpegif(const MpegifParam *p, int width, int height)
{
    /* vptr (0x150) = 0x00345A68 */
    g_dec_state_sema = CreateSema((struct SemaParam *)&g_mutex_sema_param);
    g_mpeg_start_sema = CreateSema((struct SemaParam *)&g_signal_sema_param);
    g_mpeg_wake_thread = -1;
    g_frameq_filled_once = 0;
    g_frames_stopped = 0;
    g_dec_state = 0;
    this->width = width;
    this->height = height;
    /* the player's input ring: 32 (up to 480 lines) or 16 elements of 16 KB
     * (retail's slti 0x1E1 / movn comes from this form, not from < 481)
     * PC: only the player's starting budget: it grows while a picture
     * needs more (mpeg_player_pc.cpp) */
    mpeg_vibuf_set_geometry(height > 480 ? 4 : 5, 14);
    prioHigh = p->prioHigh;
    stack = FEEDER_STACK;
    prioRead = p->prioRead;
    prioFeed = p->prioFeed;
    prioDecode = p->prioDecode;
    tid = -1;
    sema = CreateSema((struct SemaParam *)&g_mutex_sema_param);
    sleeper = -1;
    state = MPEGIF_FEEDING;
    unk_148 = 0;
    memset(name, 0, sizeof(name));
    memset(hostName, 0, sizeof(hostName));
    memset(preloadName, 0, sizeof(preloadName));
    memset(preloadHost, 0, sizeof(preloadHost));
    usePreload = 0;
    preloadBuf = 0;
    preloadEnd = 0;                         /* PC */
    dec = 0;
}

/* Deletes the two global semaphores and its own; deleting form (flags & 1:
 * operator delete). */
// FUNC 0x0025E698 mpegif::~mpegif
mpegif::~mpegif()
{
    DeleteSema(g_mpeg_start_sema);
    g_mpeg_start_sema = -1;
    DeleteSema(sema);
    sema = -1;
    DeleteSema(g_dec_state_sema);
    g_dec_state_sema = -1;
}

/* Reads file into a new size-byte buffer (at priority prioRead, pdimpeg
 * mode 1), 32 KB at a time, until the buffer is full or a 4 KB packet of the
 * last block is not a video PES packet (the end of the movie); play() then
 * plays it from memory.  The last block is copied whole: a size that is not
 * a multiple of 32 KB overruns the buffer (T253 passes 0x500000).
 * PC: the buffer is sized by the movie, whatever size says: its bytes
 * rounded up to 32 KB plus the block that holds the end, grown by half
 * when the 4 KB units outnumber the file's (a movie with short packets),
 * so the copy always holds the whole movie and its end; preloadEnd marks
 * where it stops (feed reads no further).  A movie bigger than size is
 * logged once. */
// FUNC 0x0025E710 mpegif::preload
void mpegif::preload(const char *file, int size)
{
    const u8 *payload;                      /* sp+0x00 */
    int payloadLen;                         /* sp+0x04 */
    s64 pts;                                /* sp+0x08 */
    s64 dts;                                /* sp+0x10 */
    int oldPrio;                            /* sp+0x18 */
    int self = GetThreadId();

    if (!mpegif_locate(file, &lsn, &this->size, preloadHost))  /* PC: was file_resolve_retry */
        return;                             /* no preloaded copy: play() reads the disc */
    /* PC: was memalign(0x40, size) */
    u32 cap = ((this->size + FEED_BLOCK_SIZE - 1) & ~(u32)(FEED_BLOCK_SIZE - 1)) + FEED_BLOCK_SIZE;
    preloadBuf = (u8 *)memalign(0x40, cap);
    if (preloadBuf == NULL) {
        printf("PC: mpegif: no memory to preload %s (%u bytes); it plays from the file\n", file, cap);
        return;                             /* PC: as a missing file: no preloaded copy */
    }
    strncpy(preloadName, file, sizeof(preloadName) - 1);       /* PC: - 1, kept terminated */
    u8 *end = preloadBuf + cap;
    oldPrio = ChangeThreadPriority(self, prioRead);
    preloadPos = preloadBuf;
    preloadEnd = preloadBuf;                /* PC */
    pdimpeg_open(1, lsn, BYTES_TO_SECTORS(this->size), preloadHost);
    u8 *block = (u8 *)mpeg_dma_alloc(FEED_BLOCK_SIZE);
    int full = 0;                           /* PC: no memory to grow the copy */
    do {
        pdimpeg_feed(block, FEED_BLOCK_SIZE);
        if (preloadPos + FEED_BLOCK_SIZE > end) {
            /* PC: more units than the file's bytes: the copy grows by half */
            u32 used = (u32)(preloadPos - preloadBuf);
            u32 grown = (cap + cap / 2 + FEED_BLOCK_SIZE - 1) & ~(u32)(FEED_BLOCK_SIZE - 1);
            u8 *bigger = (u8 *)memalign(0x40, grown);
            if (bigger == NULL) {
                printf("PC: mpegif: no memory to preload %s past %u bytes; it is cut there\n", file, used);
                full = 1;
                break;
            }
            memcpy(bigger, preloadBuf, used);
            free(preloadBuf);
            preloadBuf = bigger;
            preloadPos = bigger + used;
            cap = grown;
            end = bigger + cap;
        }
        /* inlined by egcs as a 32-byte ld/sd loop (ldl/ldr when either
         * pointer is not 8-byte aligned) */
        memcpy(preloadPos, block, FEED_BLOCK_SIZE);
        for (int n = 0; n < PACKETS_PER_BLOCK; n++) {
            if (mpeg_pes_parse(block + n * PES_PACKET_SIZE, &payload, &payloadLen, &pts, &dts) != 0)
                goto done;
        }
        preloadPos += FEED_BLOCK_SIZE;
    } while (1);                            /* PC: was while (preloadPos < end): the copy grows */
done:
    /* PC: the copy ends after the block holding the end unit (or where
     * memory ran out: feed() then reads an empty block there) */
    preloadEnd = full ? preloadPos : preloadPos + FEED_BLOCK_SIZE;
    if ((u32)(preloadEnd - preloadBuf) > (u32)size && mpegif_first_time(file))
        printf("PC: mpegif: %s preloaded in %u bytes (the caller's buffer was %d)\n", file,
               (u32)(preloadEnd - preloadBuf), size);
    ChangeThreadPriority(self, oldPrio);
    mpeg_dma_free(block);
    pdimpeg_close();
    preloadPos = preloadBuf;
}

// FUNC 0x0025E940 mpegif::freePreload
void mpegif::freePreload()
{
    free(preloadBuf);
    preloadBuf = 0;
    preloadEnd = 0;                         /* PC */
    usePreload = 0;
}

/* The command interface (at priority prioHigh, under g_dec_state_sema):
 *   SWITCH with file already playing: 1 (and, a quirk, the caller's priority
 *     is NOT restored on this path);
 *   STOP once the feeder has ended: 0;
 *   any cmd but GET_STATE while the player is not running: 1;
 *   PLAY, SWITCH: switch to file (MPEGIF_SWITCH_REQUEST); STOP_RUNNING, STOP:
 *     stop (MPEGIF_STOP_REQUEST); >= 5: neither - these abort the frame
 *     queue, ask the decoder to stop and return 0;
 *   GET_STATE: the state. */
// FUNC 0x0025E970 mpegif::control
int mpegif::control(int cmd, const char *file)
{
    int result;
    int self = GetThreadId();
    int oldPrio = ChangeThreadPriority(self, prioHigh);

    if (cmd == MPEGIF_CMD_SWITCH && strncmp(file, name, sizeof(name)) == 0)
        return 1;                           /* priority left at prioHigh */

    WaitSema(g_dec_state_sema);
    if (cmd == MPEGIF_CMD_STOP && state == MPEGIF_ENDED) {
        result = 0;
    } else if (cmd != MPEGIF_CMD_GET_STATE && g_dec_state != DEC_RUNNING) {
        result = 1;
    } else {
        switch ((u32)cmd) {                 /* jump table 0x00345A50 */
        case MPEGIF_CMD_PLAY:
        case MPEGIF_CMD_SWITCH:
            if (usePreload == 0)
                strncpy(name, file, sizeof(name) - 1);  /* PC: - 1, kept terminated */
            state = MPEGIF_SWITCH_REQUEST;
            goto abort;
        case MPEGIF_CMD_STOP_RUNNING:
        case MPEGIF_CMD_STOP:
            state = MPEGIF_STOP_REQUEST;
            goto abort;
        case MPEGIF_CMD_GET_STATE:
            result = state;
            break;
        default:
        abort:
            mpeg_player_abort_frameq(dec);
            result = 0;
            g_dec_state = DEC_STOP_REQUEST;
            break;
        }
    }
    SignalSema(g_dec_state_sema);
    ChangeThreadPriority(self, oldPrio);
    return result;
}

/* Starts the feeder thread (priority prioFeed, 0xC00-byte stack in the
 * scratchpad) on file, or on the preloaded copy. */
// FUNC 0x0025EAC0 mpegif::startThread
void mpegif::startThread(const char *file, int withSound, int oneShot)
{
    struct ThreadParam tp;                  /* sp+0x00 */

    this->withSound = withSound;
    this->oneShot = oneShot;
    if (usePreload == 0) {
        strncpy(name, file, sizeof(name) - 1);  /* PC: - 1, kept terminated */
        if (!mpegif_locate(name, &lsn, &size, hostName)) {     /* PC: was file_resolve_retry */
            state = MPEGIF_ENDED;           /* no thread: the stream has ended */
            return;
        }
    }
    tp.entry = threadEntry;
    tp.stack = stack;
    tp.stackSize = FEEDER_STACK_SIZE;
    tp.initPriority = prioFeed;
    tp.gpReg = _gp;
    tp.option = 0;
    tid = CreateThread(&tp);
    StartThread(tid, this);
}

/* 1 while no frame is decoded yet; else 0 and the size of the decoded
 * pictures in *w, *h (each when not NULL).  No callers. */
// FUNC 0x0025EB58 mpegif::getSize
int mpegif::getSize(int *w, int *h)
{
    if (g_frameq_filled_once == 0)
        return 1;
    if (w != 0)
        *w = mpeg_player_get_width(dec);
    if (h != 0)
        *h = mpeg_player_get_height(dec);
    return 0;
}

/* Starts the feeder thread if it is not running, else passes cmd on (and
 * returns control()'s result: 1 = not now, retry). */
// FUNC 0x0025EBD0 mpegif::start
int mpegif::start(const char *file, int withSound, int oneShot, int cmd)
{
    if (tid == -1) {
        startThread(file, withSound, oneShot);
        return 0;
    }
    return control(cmd, file);
}

/* The oldest decoded frame (RGBA32, in 16 x 16 blocks), NULL if none. */
// FUNC 0x0025EC10 mpegif::nextFrame
void *mpegif::nextFrame()
{
    if (g_frames_stopped == 0 && g_frameq_filled_once != 0)
        return mpeg_player_peek_frame(dec);
    return 0;
}

// FUNC 0x0025EC50 mpegif::releaseFrame
void mpegif::releaseFrame()
{
    if (g_frameq_filled_once != 0)
        mpeg_player_release_frame(dec);
}

/* nextFrame() returns NULL from now until the next feed(). */
// FUNC 0x0025EC78 mpegif::stopFrames
void mpegif::stopFrames()
{
    g_frames_stopped = 1;
}

/* The player's input ring fill as a fraction (unsigned -> float
 * conversions).  No callers. */
// FUNC 0x0025EC88 mpegif::bufferFill
f32 mpegif::bufferFill()
{
    if (g_frameq_filled_once == 0)
        return 0.0f;
    u32 buffered = (u32)mpeg_player_get_buffered(dec);
    u32 ringSize = (u32)mpeg_vibuf_get_size();
    return (f32)buffered / (f32)ringSize;
}

/* Sleeps until wakeIfBuffered() sees the ring 90% full.  No callers. */
// FUNC 0x0025ED30 mpegif::waitBuffered
void mpegif::waitBuffered()
{
    WaitSema(sema);
    sleeper = GetThreadId();
    SignalSema(sema);
    SleepThread();
}

/* Wakes the thread in waitBuffered() once the ring is 90% full. */
// FUNC 0x0025ED78 mpegif::wakeIfBuffered
void mpegif::wakeIfBuffered()
{
    WaitSema(sema);
    if (sleeper == -1) {
        SignalSema(sema);
        return;
    }
    u32 buffered = (u32)mpeg_player_get_buffered(dec);
    if ((int)(buffered * 10 / (u32)mpeg_vibuf_get_size()) >= 9) {  /* divu, signed compare */
        WakeupThread(sleeper);
        sleeper = -1;
    }
    SignalSema(sema);
}

/* The feeder thread's entry (arg = the mpegif). */
// FUNC 0x0025EE18 mpegif::threadEntry
void mpegif::threadEntry(void *arg)
{
    ((mpegif *)arg)->threadMain();
}

/* oneShot 0: feeds file after file while control() asks for a switch
 * (MPEGIF_SWITCH_REQUEST); oneShot 1: feeds once; any other value: nothing.
 * Then ends the thread (MPEGIF_ENDED).  The switch gives retail's block
 * order (case 1 after the tests, the loop after it); a for (;;) loop gets
 * its exit test rotated to the bottom by egcs's expand_end_loop, the
 * do / while (1) keeps it in place as retail has it. */
// FUNC 0x0025EE38 mpegif::threadMain
void mpegif::threadMain()
{
    state = MPEGIF_FEEDING;
    switch (oneShot) {
    case 1:
        feed();
        break;
    case 0:
        do {
            feed();
            ChangeThreadPriority(tid, prioHigh);
            if (state != MPEGIF_SWITCH_REQUEST)
                break;
            if (usePreload == 0 && !mpegif_locate(name, &lsn, &size, hostName))
                break;                      /* PC: was file_resolve_retry; a missing file ends the stream */
            state = MPEGIF_SWITCHED;
            ChangeThreadPriority(tid, prioFeed);
        } while (1);
        break;
    }
    /* Retail stores tid first and state last (in the jal's delay slot): the
     * game thread polls state (isPlaying) and may delete the mpegif once it
     * reads MPEGIF_ENDED, so nothing may be written after it.  egcs's
     * scheduler gives that order from this source; the GCC 14 branch writes
     * it out. */
    tid = -1;
    state = MPEGIF_ENDED;
    ExitDeleteThread();
}

/* Plays one file: creates the player, waits until its thread runs, then
 * reads 32 KB blocks (from the IOP stream at priority prioRead, or from the
 * preloaded copy) and puts them as 4 KB PES packets until the player has
 * stopped (PUT_PES_STOPPED, after control() aborted it).  A packet without
 * payload (PUT_PES_EMPTY: the end of the movie) rewinds the preloaded copy
 * (the rest of the block is skipped); on the IOP stream it is dropped and a
 * one-shot movie asks the decoder to stop.  A full ring (PUT_PES_NOT_READY)
 * puts the feeder to sleep until the player wakes it; the packet is put
 * again. */
// FUNC 0x0025EF00 mpegif::feed
void mpegif::feed()
{
    int streamMode = 0;
    int fromMemory = usePreload;            /* s7 */

    if (withSound == 0)
        streamMode = 1;                     /* 3: video only */
    else
        pdibgm_set_volume(1.0f);
    streamMode |= 2;
    dec = mpeg_player_create(width, height, prioHigh, prioDecode);
    g_frames_stopped = 0;
    g_frameq_filled_once = 0;
    if (fromMemory != 0)
        preloadPos = preloadBuf;
    else
        pdimpeg_open(streamMode, lsn, BYTES_TO_SECTORS(size), hostName);
    u8 *block = (u8 *)mpeg_dma_alloc(FEED_BLOCK_SIZE);
    WaitSema(g_mpeg_start_sema);

    for (;;) {
        wakeIfBuffered();
        if (fromMemory != 0) {
            /* inlined 32-byte ld/sd (or ldl/ldr) loop, as in preload() */
            if (preloadPos + FEED_BLOCK_SIZE <= preloadEnd)
                memcpy(block, preloadPos, FEED_BLOCK_SIZE);
            else
                memset(block, 0, FEED_BLOCK_SIZE);  /* PC: past the copy: an empty block (rewinds) */
            preloadPos += FEED_BLOCK_SIZE;
        } else {
            ChangeThreadPriority(tid, prioRead);
            g_reading_stream = 1;
            pdimpeg_feed(block, FEED_BLOCK_SIZE);
            g_reading_stream = 0;
            ChangeThreadPriority(tid, prioFeed);
        }
        for (int n = 0; n < PACKETS_PER_BLOCK; n++) {
            int put;
            do {
                put = mpeg_player_put_pes(dec, block + (n << 12));
                /* the case PUT_PES_OK node gives retail's decision tree
                 * (== 1, < 2, == 2, == 3) */
                switch (put) {
                case PUT_PES_OK:
                    break;
                case PUT_PES_STOPPED:
                    goto end;
                case PUT_PES_EMPTY:
                    if (fromMemory != 0) {
                        preloadPos = preloadBuf;
                        goto block_done;
                    }
                    put = PUT_PES_OK;
                    if (oneShot == 1) {
                        WaitSema(g_dec_state_sema);
                        g_dec_state = DEC_STOP_REQUEST;     /* CSE: oneShot's register */
                        SignalSema(g_dec_state_sema);
                    }
                    break;
                case PUT_PES_NOT_READY:
                    g_mpeg_wake_thread = tid;
                    SleepThread();
                    g_mpeg_wake_thread = -1;
                    break;
                }
            } while (put != PUT_PES_OK);    /* other values (none known): put again */
        }
    block_done:
        if (g_frames_stopped == 0 && g_frameq_filled_once == 0
            && mpeg_player_is_frameq_full(dec) != 0)
            g_frameq_filled_once = 1;
    }

end:
    g_frameq_filled_once = 0;
    mpeg_dma_free(block);
    if (fromMemory == 0)
        pdimpeg_close();
    mpeg_player_abort_frameq(dec);
    mpeg_player_delete(dec);
}

/* Nonzero until the feeder thread has ended (or while it has not started).
 * No callers (play_movie, T051, and T253 inline it). */
// FUNC 0x0025F1F8 mpegif::isPlaying
int mpegif::isPlaying()
{
    return control(MPEGIF_CMD_GET_STATE, 0) != MPEGIF_ENDED;
}

/* Nonzero while the state is neither MPEGIF_SWITCHED nor MPEGIF_ENDED.
 * No callers. */
// FUNC 0x0025F220 mpegif::isBusy
int mpegif::isBusy()
{
    int s = control(MPEGIF_CMD_GET_STATE, 0);
    return s != MPEGIF_SWITCHED && s != MPEGIF_ENDED;
}

// FUNC 0x0025F260 mpegif::stop
int mpegif::stop()
{
    return control(MPEGIF_CMD_STOP, 0);
}

/* Starts file from the disc, or switches to it (which does nothing when it
 * is already playing).  No callers (T253 inlines it). */
// FUNC 0x0025F280 mpegif::switchTo
int mpegif::switchTo(const char *file, int withSound, int oneShot)
{
    usePreload = 0;
    return start(file, withSound, oneShot, MPEGIF_CMD_SWITCH);
}

/* Starts file, from the preloaded copy (withSound 0) when it is that file.
 * No callers (T253 inlines it). */
// FUNC 0x0025F2A8 mpegif::play
int mpegif::play(const char *file, int withSound, int oneShot)
{
    if (preloadBuf != 0 && strcmp(file, preloadName) == 0) {
        usePreload = 1;
        return start(0, 0, oneShot, MPEGIF_CMD_PLAY);
    }
    usePreload = 0;
    return start(file, withSound, oneShot, MPEGIF_CMD_PLAY);
}

// FUNC 0x0025F340 mpegif::rewindPreload
void mpegif::rewindPreload()
{
    preloadPos = preloadBuf;
}

