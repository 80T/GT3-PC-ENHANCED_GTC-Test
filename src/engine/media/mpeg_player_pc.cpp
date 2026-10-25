/* mpeg_player_pc.cpp - the G002 MPEG player's interface on the native
 * decoder (GT3 PC; replaces mpeg_player.c, ipu.c, ipu_image.c and
 * dma_util.c, 0x0025F390..0x00265530).
 *
 * mpegif (T529) drives the player through the functions of
 * G002_mpeg_frameq.h: mpeg_player_create starts the decoder thread,
 * mpeg_player_put_pes hands it the 4 KB video PES packets, the game takes
 * the decoded frames from the 3-slot frame queue (peek / release), stops
 * it through g_dec_state and the queue's abort.  All of that is kept as
 * the original does it, on the game scheduler: the decoder thread is a
 * game thread at mpegif's prioDecode (11), with the same semaphores,
 * wake-ups and states.  What the IPU and the EE's MMI code did (bitstream,
 * IDCT, motion compensation, CSC) is the native decoder of
 * engine/media/native (media_services.h: gt3m_video_*).
 *
 * The input: on the PS2 a ring of 2^n elements of 16 KB (32 up to 480
 * lines) chained to the IPU's DMA, two elements kept free; put_pes answered
 * PUT_PES_NOT_READY when the packet did not fit, and the IPU's "input
 * exhausted" callback woke the sleeping feeder (g_mpeg_wake_thread).
 * Natively the packets go straight to the decoder's buffer, with the same
 * budget (ring size - 2 elements) and the same answers; the decoder thread
 * wakes the feeder after each picture and whenever it runs out of data,
 * and sleeps on a semaphore until put_pes brings more.
 *
 * Stopping: control() aborts the frame queue and sets DEC_STOP_REQUEST;
 * the decoder stops after the picture it is on, as mpeg_video_decode_main
 * did.  The end of a one-shot movie (mpegif::feed sets DEC_STOP_REQUEST at
 * the stream's empty unit, the queue not aborted): the PS2 decoder stopped
 * after its current picture too, the ring's last ~0.4 s never shown; here
 * the decoder first finishes what it has been given (up to the sequence
 * end), so the movie plays to its end.
 *
 * Frames: the queue's slots hold what the IPU's CSC wrote: RGB32, alpha
 * 0x80, 16 x 16 blocks of 0x400 bytes in columns of blocks (the frame
 * stores were column-major), which MenuBgMovie::uploadFrame / uploadField
 * (T253) upload block by block.  The queue memory comes from the game heap
 * as before (mpeg_dma_alloc); MenuBgMovie keeps pointing at the last frame
 * after the player is gone, as on the PS2.
 *
 * MP4 movies (docs/movies.md "MP4 movies"): the same path, the units frame
 * tokens, the frames made by a worker thread of the native decoder;
 * gt3m_video_next answers -1 while one is not ready and the decoder thread
 * waits a moment (DelayThread) for it.
 *
 * No size limits of the PS2's: the slots are sized by the movie's first
 * picture when it is bigger than mpegif's width x height (a later, bigger
 * sequence is cropped, logged), and the input budget doubles while a
 * picture needs more than it (up to kBudgetMax; the PS2's ring was 480 /
 * 224 KB and a bigger picture stopped the movie as damaged). */
#include "engine/media/G002_mpeg_frameq.h"
#include "engine/media/T529_mpegif.h"
#include "engine/media/native/media_services.h"
#include "eekernel.h"
#include "libc.h"

/* ======================================================================== */
/* Data                                                                     */
/* ======================================================================== */

/* .sdata words shared with mpegif (T529_mpegif.h), defined by the player. */
int g_dec_state = 0;                    /* 0x00353140 */
int g_dec_state_sema = 0;               /* 0x00353144 */

/* The input budget: mpegif's ring geometry (mpeg_vibuf_set_geometry). */
static int g_vibuf_n;                   /* elements (0x003532A4) */
static int g_vibuf_elm;                 /* element bytes (0x003532B0) */
static int g_vibuf_size;                /* total bytes (0x003532B8) */
static int g_mpeg_thread_running;       /* 0x003532C0 */
/* PC: the bytes the decoder may hold: the ring less two elements, doubled
 * while a picture needs more (up to kBudgetMax, the end of a damaged
 * stream's search for a picture); reset per player. */
static int g_vibuf_budget;
static const int kBudgetMax = 64 << 20;
static int g_size_logged;               /* PC: the picture size line, once per player */
static int g_budget_logged;             /* PC: the budget line, once per player */

/* MpegViBuf.state's values (mpeg_player_internal.h). */
#define VIBUF_STOPPED       0
#define VIBUF_RUNNING       1
#define VIBUF_CREATED       3

/* The frame queue (0x28 bytes, mpeg_frameq_create's malloc). */
struct MpegFrameQueue {
    u8 *buf;                /* 0x00 mpeg_dma_alloc(bufSize) */
    int bufSize;            /* 0x04 */
    int elemSize;           /* 0x08 */
    int count;              /* 0x0C */
    int writeIdx;           /* 0x10 next slot to fill */
    int filled;             /* 0x14 */
    int freeSema;           /* 0x18 counts free slots; -1 once deleted */
    int lockSema;           /* 0x1C mutex */
    int priority;           /* 0x20 priority mpeg_frameq_abort runs at (mpegif's prioHigh) */
    int threadPriority;     /* 0x24 the decoder thread priority, not read */
};

/* The player (the PS2 kept the one player in the scratchpad, 0x70002344). */
struct MpegPlayer {
    int thread;             /* decoder thread */
    MpegFrameQueue *frames;
    int maxWidth;
    int maxHeight;
    int width;              /* the decoded pictures' size (mpeg_player_on_size) */
    int height;
    int mbCount;
    /* PC: the input (the PS2's MpegViBuf) */
    int sema;               /* mutex around `state' (vb->sema) */
    int dataSema;           /* the decoder waits here for input or a stop */
    int state;              /* VIBUF_* */
    Gt3mVideo *video;       /* the native decoder */
};

static MpegPlayer g_mpeg_player;

static const struct SemaParam s_mutex_param = { 0, 1, 1, 0, 0, 0 };
static const struct SemaParam s_signal_param = { 0, 1, 0, 0, 0, 0 };

/* The frame queue (mpeg_frameq_*, 0x002602E8..0x00260748; as the
 * original). */
static void mpeg_frameq_reset(MpegFrameQueue *q);
static void mpeg_frameq_abort(MpegFrameQueue *q);
static int  mpeg_frameq_is_full(MpegFrameQueue *q);
static int  mpeg_frameq_is_empty(MpegFrameQueue *q);
static u8  *mpeg_frameq_begin_write(MpegFrameQueue *q);
static void mpeg_frameq_end_write(MpegFrameQueue *q);
static u8  *mpeg_frameq_peek(MpegFrameQueue *q);
static void mpeg_frameq_end_read(MpegFrameQueue *q);
static void mpeg_frameq_resize(MpegFrameQueue *q, int elemSize);
static int  mpeg_pes_parse_timestamps(const u8 *pkt, int off, s64 *pts, s64 *dts);

/* ======================================================================== */
/* The player                                                               */
/* ======================================================================== */

// FUNC 0x0025F390 mpeg_player_create
/* PC: the decoder thread is a game thread as before (stack size ignored:
 * game threads get native stacks); no IPU interrupt handshake, no DMA
 * ring, no callback table. */
MpegPlayer *mpeg_player_create(int width, int height, int qpriority, int priority)
{
    MpegPlayer *pl = &g_mpeg_player;
    struct ThreadParam param;
    int tid;

    memset(pl, 0, sizeof(MpegPlayer));
    pl->maxWidth = width;
    pl->maxHeight = height;
    pl->sema = CreateSema((struct SemaParam *)&s_mutex_param);
    pl->dataSema = CreateSema((struct SemaParam *)&s_signal_param);
    pl->state = VIBUF_CREATED;
    pl->video = gt3m_video_create();
    gt3m_video_set_size(pl->video, width, height);      /* PC: an MP4 movie's pictures scaled to it */
    pl->frames = mpeg_frameq_create(width * height * 4, 3, qpriority, priority);
    g_vibuf_budget = (g_vibuf_n - 2) * g_vibuf_elm;     /* PC: the ring's, to start with */
    g_size_logged = 0;
    g_budget_logged = 0;

    memset(&param, 0, sizeof(param));
    param.entry = mpeg_player_thread;
    param.stack = NULL;
    param.stackSize = 0xC00;
    param.initPriority = priority;
    param.option = 0;
    tid = CreateThread(&param);
    StartThread(tid, pl);
    pl->thread = tid;
    return pl;
}

// FUNC 0x0025F510 mpeg_player_delete
/* mpegif::feed deletes the player once put_pes answered PUT_PES_STOPPED:
 * the decoder thread sleeps in its final loop then. */
void mpeg_player_delete(MpegPlayer *pl)
{
    int tid;

    mpeg_frameq_delete(pl->frames);
    tid = pl->thread;
    TerminateThread(tid);
    DeleteThread(tid);
    DeleteSema(pl->dataSema);
    DeleteSema(pl->sema);
    gt3m_video_destroy(pl->video);
    pl->video = NULL;
    g_mpeg_thread_running = 0;
}

// FUNC 0x0025F598 mpeg_player_get_width
int mpeg_player_get_width(MpegPlayer *pl)
{
    return pl->width;
}

// FUNC 0x0025F5A0 mpeg_player_get_height
int mpeg_player_get_height(MpegPlayer *pl)
{
    return pl->height;
}

/* PC: the size line, once per player: pictures bigger than mpegif's
 * width x height (slots sized for them, or cropped mid-stream). */
static void mpeg_player_log_size(MpegPlayer *pl, int w, int h, int cropped)
{
    if (g_size_logged)
        return;
    g_size_logged = 1;
    if (cropped)
        printf("PC: movie: %d x %d pictures in %d x %d frame slots: cropped\n", w, h, pl->maxWidth,
               pl->maxHeight);
    else
        printf("PC: movie: %d x %d pictures (the caller asked %d x %d): frame slots sized for them\n", w, h,
               pl->maxWidth, pl->maxHeight);
}

/* The decoder loop (mpeg_video_decode_main 0x00260998 with the picture
 * callback mpeg_player_on_picture 0x0025F5A8): pictures in display order
 * into the queue; DEC_RUNNING after the first; a stop request checked
 * after each picture; DEC_ENDED at the end. */
static void mpeg_player_decode(MpegPlayer *pl)
{
    int total = 0;
    int finishing = 0;      /* PC: the end of a one-shot movie: decode what is buffered */
    int endsAtRequest = 0;
    int flushed = 0;
    int forced = 0;

    for (;;) {
        int w, h;
        const int got = gt3m_video_next(pl->video, &w, &h);
        if (got < 0) {
            /* PC: an MP4 movie's frame, decoded on its worker thread, not
             * ready yet (docs/movies.md "MP4 movies"): a moment through the
             * kernel, the other game threads running; a stop with the queue
             * aborted (a switch, a menu leaving) ends it at once */
            WaitSema(g_dec_state_sema);
            if (g_dec_state == DEC_STOP_REQUEST && pl->frames->freeSema == -1) {
                g_dec_state = DEC_STOPPED;
                SignalSema(g_dec_state_sema);
                return;
            }
            SignalSema(g_dec_state_sema);
            DelayThread(2000);
            continue;
        }
        if (got > 0) {
            forced = 0;
            if (w != pl->width || h != pl->height) {        /* mpeg_player_on_size */
                pl->width = w;
                pl->height = h;
                pl->mbCount = (w >> 4) * (h >> 4);
                /* PC: slots of the picture's size when it is bigger than
                 * mpegif's: on the first picture (nothing in the queue yet,
                 * so no slot is held); a later, bigger sequence is cropped */
                if (w * h * 4 > pl->frames->elemSize) {
                    if (total == 0) {
                        mpeg_player_log_size(pl, w, h, 0);
                        mpeg_frameq_resize(pl->frames, w * h * 4);
                        pl->maxWidth = w;
                        pl->maxHeight = h;
                    } else {
                        mpeg_player_log_size(pl, w, h, 1);
                    }
                }
            }
            u8 *slot = mpeg_frameq_begin_write(pl->frames);
            gt3m_video_output_blocks(pl->video, slot, pl->frames->elemSize);
            mpeg_frameq_end_write(pl->frames);
            total++;
            if (total == 1) {
                WaitSema(g_dec_state_sema);
                g_dec_state = DEC_RUNNING;
                SignalSema(g_dec_state_sema);
            }
            if (g_mpeg_wake_thread != -1)       /* PC: input consumed */
                WakeupThread(g_mpeg_wake_thread);
            WaitSema(g_dec_state_sema);
            if (g_dec_state == DEC_STOP_REQUEST) {
                if (pl->frames->freeSema == -1 || (finishing && gt3m_video_sequence_ends(pl->video) > endsAtRequest)) {
                    g_dec_state = DEC_STOPPED;
                    SignalSema(g_dec_state_sema);
                    return;
                }
                if (!finishing) {
                    finishing = 1;
                    endsAtRequest = gt3m_video_sequence_ends(pl->video);
                }
            }
            SignalSema(g_dec_state_sema);
            continue;
        }

        /* no picture: the decoder needs data */
        WaitSema(g_dec_state_sema);
        if (g_dec_state == DEC_STOP_REQUEST) {
            if (pl->frames->freeSema == -1 || flushed) {
                g_dec_state = DEC_STOPPED;
                SignalSema(g_dec_state_sema);
                return;
            }
            /* the end of a one-shot movie: what is buffered is decoded;
             * the last reference picture comes out */
            finishing = 1;
            flushed = 1;
            SignalSema(g_dec_state_sema);
            gt3m_video_end_of_stream(pl->video);
            continue;
        }
        SignalSema(g_dec_state_sema);
        if (gt3m_video_buffered(pl->video) + 0x1000 > g_vibuf_budget) {
            /* PC: a full budget and still no picture: a picture bigger
             * than the budget (a bigger movie than the PS2's ring held)
             * doubles it, the feeder is woken to bring the rest */
            if (g_vibuf_budget < kBudgetMax) {
                if (!g_budget_logged) {
                    g_budget_logged = 1;
                    printf("PC: movie: a picture needs more than %d KB of input: the budget grows\n",
                           g_vibuf_budget >> 10);
                }
                g_vibuf_budget = g_vibuf_budget * 2 > kBudgetMax ? kBudgetMax : g_vibuf_budget * 2;
                if (g_mpeg_wake_thread != -1)
                    WakeupThread(g_mpeg_wake_thread);
                WaitSema(pl->dataSema);
                continue;
            }
            /* PC: a full budget and still no picture (a damaged stream):
             * decode what is there rather than wait for room that never
             * comes; if even that gives nothing, give up */
            if (!forced) {
                forced = 1;
                gt3m_video_end_of_stream(pl->video);
                continue;
            }
            WaitSema(g_dec_state_sema);
            g_dec_state = DEC_STOPPED;
            SignalSema(g_dec_state_sema);
            return;
        }
        if (g_mpeg_wake_thread != -1)
            WakeupThread(g_mpeg_wake_thread);
        WaitSema(pl->dataSema);
    }
}

// FUNC 0x0025F5F8 mpeg_player_thread
/* Decoder thread: run, decode the stream to its end or a stop, clear the
 * queue, mark the input stopped, wake the feeder, sleep until terminated. */
void mpeg_player_thread(void *arg)
{
    MpegPlayer *pl = (MpegPlayer *)arg;

    g_mpeg_thread_running = 1;
    mpeg_frameq_reset(pl->frames);
    WaitSema(pl->sema);
    pl->state = VIBUF_RUNNING;
    SignalSema(pl->sema);
    SignalSema(g_mpeg_start_sema);
    mpeg_player_decode(pl);
    WaitSema(g_dec_state_sema);
    g_dec_state = DEC_ENDED;
    SignalSema(g_dec_state_sema);
    mpeg_frameq_reset(pl->frames);
    WaitSema(pl->sema);
    pl->state = VIBUF_STOPPED;
    SignalSema(pl->sema);
    if (g_mpeg_wake_thread != -1)
        WakeupThread(g_mpeg_wake_thread);
    for (;;)
        SleepThread();
}

// FUNC 0x0025F698 mpeg_vibuf_set_geometry
/* 2^log2n elements of 2^log2elm bytes (mpegif: 2^5 or 2^4 of 16 KB): the
 * input budget. */
void mpeg_vibuf_set_geometry(int log2n, int log2elm)
{
    g_vibuf_n = 1 << log2n;
    g_vibuf_elm = 1 << log2elm;
    g_vibuf_size = 1 << (log2elm + log2n);
}

// FUNC 0x0025F6E0 mpeg_player_put_pes
/* The payload of one video PES packet to the decoder.  PUT_PES_OK,
 * PUT_PES_STOPPED (the decoder has finished), PUT_PES_EMPTY (no payload:
 * the end of the stream), PUT_PES_NOT_READY (not running yet, or over the
 * budget: the ring's size less two elements). */
int mpeg_player_put_pes(MpegPlayer *pl, const u8 *pkt)
{
    const u8 *payload;
    int len;
    s64 pts;
    s64 dts;
    int state;

    WaitSema(pl->sema);
    state = pl->state;
    SignalSema(pl->sema);
    switch (state) {
    case VIBUF_STOPPED:
        return PUT_PES_STOPPED;
    case VIBUF_CREATED:
        return PUT_PES_NOT_READY;
    }
    mpeg_pes_parse(pkt, &payload, &len, &pts, &dts);   /* result ignored */
    if (len <= 0) {
        /* PC: a decoder asked to stop may be waiting for input: let it
         * see the request */
        if (g_dec_state == DEC_STOP_REQUEST)
            SignalSema(pl->dataSema);
        return PUT_PES_EMPTY;
    }
    if (gt3m_video_buffered(pl->video) + len > g_vibuf_budget) {   /* PC: was (n - 2) * elm */
        SignalSema(pl->dataSema);       /* PC: the decoder has work (or a stop) to see */
        return PUT_PES_NOT_READY;
    }
    gt3m_video_push(pl->video, payload, len, pts);
    SignalSema(pl->dataSema);           /* (fails harmlessly when already signalled) */
    return PUT_PES_OK;
}

// FUNC 0x0025F820 mpeg_player_is_frameq_full
int mpeg_player_is_frameq_full(MpegPlayer *pl)
{
    return mpeg_frameq_is_full(pl->frames);
}

// FUNC 0x0025F840 mpeg_player_peek_frame
u8 *mpeg_player_peek_frame(MpegPlayer *pl)
{
    if (g_mpeg_thread_running == 0)
        return 0;
    return mpeg_frameq_peek(pl->frames);
}

// FUNC 0x0025F870 mpeg_player_release_frame
void mpeg_player_release_frame(MpegPlayer *pl)
{
    if (g_mpeg_thread_running != 0)
        mpeg_frameq_end_read(pl->frames);
}

// FUNC 0x0025F890 mpeg_player_abort_frameq
void mpeg_player_abort_frameq(MpegPlayer *pl)
{
    mpeg_frameq_abort(pl->frames);
    /* PC: a decoder waiting for input sees the stop that follows (control
     * sets DEC_STOP_REQUEST next, then the feeder puts on) */
}

// FUNC 0x0025F978 mpeg_vibuf_get_size
int mpeg_vibuf_get_size(void)
{
    return g_vibuf_size;
}

// FUNC 0x0025F980 mpeg_player_get_buffered
/* Input bytes not yet decoded. */
int mpeg_player_get_buffered(MpegPlayer *pl)
{
    return pl->video ? gt3m_video_buffered(pl->video) : 0;
}

/* ======================================================================== */
/* The frame queue (as the original)                                        */
/* ======================================================================== */

// FUNC 0x002602E8 mpeg_frameq_create
MpegFrameQueue *mpeg_frameq_create(int elemSize, int count, int priority, int threadPriority)
{
    MpegFrameQueue *q;
    struct SemaParam param;

    q = (MpegFrameQueue *)malloc(sizeof(MpegFrameQueue));
    memset(q, 0, sizeof(MpegFrameQueue));
    q->lockSema = CreateSema((struct SemaParam *)&s_mutex_param);
    q->bufSize = elemSize * count;
    q->buf = (u8 *)mpeg_dma_alloc(q->bufSize);
    memset(q->buf, 0, q->bufSize);
    q->elemSize = elemSize;
    q->priority = priority;
    q->threadPriority = threadPriority;
    memset(&param, 0, sizeof(param));
    param.maxCount = count;
    q->count = count;
    q->filled = 0;
    q->writeIdx = 0;
    param.initCount = count;
    q->freeSema = CreateSema(&param);
    return q;
}

// FUNC 0x002603B8 mpeg_frameq_delete
void mpeg_frameq_delete(MpegFrameQueue *q)
{
    if (q->freeSema != -1) {
        DeleteSema(q->freeSema);
        q->freeSema = -1;
    }
    DeleteSema(q->lockSema);
    mpeg_dma_free(q->buf);
    free(q);
}

/* PC (no original): new slots of elemSize bytes (zeroed).  Only while the
 * queue is empty (the first picture): no reader holds a slot then. */
static void mpeg_frameq_resize(MpegFrameQueue *q, int elemSize)
{
    WaitSema(q->lockSema);
    mpeg_dma_free(q->buf);
    q->bufSize = elemSize * q->count;
    q->buf = (u8 *)mpeg_dma_alloc(q->bufSize);
    memset(q->buf, 0, q->bufSize);
    q->elemSize = elemSize;
    SignalSema(q->lockSema);
}

// FUNC 0x00260410 mpeg_frameq_reset
static void mpeg_frameq_reset(MpegFrameQueue *q)
{
    WaitSema(q->lockSema);
    q->filled = 0;
    q->writeIdx = 0;
    memset(q->buf, 0, q->bufSize);
    SignalSema(q->lockSema);
}

// FUNC 0x00260458 mpeg_frameq_abort
/* Deletes the free-slot semaphore (releasing a writer blocked on it) at
 * the queue's priority. */
static void mpeg_frameq_abort(MpegFrameQueue *q)
{
    int tid = GetThreadId();
    int oldPriority = ChangeThreadPriority(tid, q->priority);

    if (q->freeSema != -1) {
        DeleteSema(q->freeSema);
        q->freeSema = -1;
    }
    ChangeThreadPriority(tid, oldPriority);
}

// FUNC 0x002604C8 mpeg_frameq_is_full
static int mpeg_frameq_is_full(MpegFrameQueue *q)
{
    int full;

    WaitSema(q->lockSema);
    full = q->filled == q->count;
    SignalSema(q->lockSema);
    return full;
}

// FUNC 0x00260518 mpeg_frameq_is_empty
static int mpeg_frameq_is_empty(MpegFrameQueue *q)
{
    int empty;

    WaitSema(q->lockSema);
    empty = q->filled == 0;
    SignalSema(q->lockSema);
    return empty;
}

// FUNC 0x00260560 mpeg_frameq_begin_write
static u8 *mpeg_frameq_begin_write(MpegFrameQueue *q)
{
    u8 *slot;

    for (;;) {
        if (q->freeSema != -1)
            WaitSema(q->freeSema);
        if (q->freeSema == -1 || !mpeg_frameq_is_full(q))
            break;
    }
    WaitSema(q->lockSema);
    slot = q->buf + q->writeIdx * q->elemSize;
    SignalSema(q->lockSema);
    return slot;
}

// FUNC 0x002605E8 mpeg_frameq_end_write
static void mpeg_frameq_end_write(MpegFrameQueue *q)
{
    WaitSema(q->lockSema);
    q->filled++;
    if (g_frameq_filled_once == 0 && q->filled == q->count)
        g_frameq_filled_once = 1;
    q->writeIdx = (q->writeIdx + 1) % q->count;
    SignalSema(q->lockSema);
}

// FUNC 0x00260660 mpeg_frameq_peek
static u8 *mpeg_frameq_peek(MpegFrameQueue *q)
{
    u8 *slot;

    if (q->freeSema == -1 || mpeg_frameq_is_empty(q))
        return 0;
    WaitSema(q->lockSema);
    slot = q->buf + ((q->writeIdx - q->filled + q->count) % q->count) * q->elemSize;
    SignalSema(q->lockSema);
    return slot;
}

// FUNC 0x002606F0 mpeg_frameq_end_read
static void mpeg_frameq_end_read(MpegFrameQueue *q)
{
    WaitSema(q->lockSema);
    if (q->filled > 0)
        q->filled--;
    SignalSema(q->lockSema);
    if (q->freeSema != -1)
        SignalSema(q->freeSema);
}

/* ======================================================================== */
/* The PES header parser (as the original)                                  */
/* ======================================================================== */

// FUNC 0x00260748 mpeg_pes_parse
/* *data / *len: the payload; *pts / *dts: -1 when absent.  0 on success,
 * 1 for a packet that is not an MPEG-2 video PES or a bad stamp. */
int mpeg_pes_parse(const u8 *pkt, const u8 **data, int *len, s64 *pts, s64 *dts)
{
    u32 startCode, ptsDtsFlags;
    int packetLen, headerLen;

    *len = 0;
    startCode = (pkt[0] << 24) | pkt[3] | ((pkt[2] << 8) | (pkt[1] << 16));
    if ((startCode >> 8) != 0x000001)
        return 1;
    if ((startCode & 0xF0) != 0xE0)             /* video stream 0xE0..0xEF */
        return 1;
    packetLen = pkt[5] | (pkt[4] << 8);         /* PES_packet_length */
    if ((pkt[6] & 0xC0) != 0x80)                /* '10': an MPEG-2 PES header */
        return 1;
    headerLen = pkt[8];                         /* PES_header_data_length */
    *dts = -1;
    ptsDtsFlags = pkt[7] >> 6;
    *len = packetLen - headerLen - 3;
    *data = pkt + headerLen + 9;
    *pts = -1;
    switch (ptsDtsFlags) {
    case 0:
        return 0;
    case 2:
    case 3:
        return mpeg_pes_parse_timestamps(pkt, 9, pts, dts) != 0 ? 1 : 0;
    default:
        return 1;
    }
}

// FUNC 0x00260880 mpeg_pes_parse_timestamps
static int mpeg_pes_parse_timestamps(const u8 *pkt, int off, s64 *pts, s64 *dts)
{
    u32 first = pkt[off];
    u32 prefix = first >> 4;
    const u8 *mid, *low;
    u64 stamp;

    off++;
    if ((u32)(prefix - 2) >= 2)                 /* '0010' or '0011' */
        return 1;
    mid = pkt + off;
    off += 2;
    low = pkt + off;
    off += 2;
    stamp = (u64)((first >> 1) & 7) << 30;
    stamp |= ((u64)(mid[1] | (mid[0] << 8)) >> 1) << 15;
    stamp |= (u64)(low[1] | (low[0] << 8)) >> 1;
    *pts = stamp;
    if ((prefix ^ 3) != 0)
        return 0;

    first = pkt[off];
    off++;
    if ((first >> 4) != 1)                      /* '0001' */
        return 1;
    mid = pkt + off;
    off += 2;
    low = pkt + off;
    stamp = (u64)((first >> 1) & 7) << 30;
    stamp |= ((u64)(mid[1] | (mid[0] << 8)) >> 1) << 15;
    stamp |= (u64)(low[1] | (low[0] << 8)) >> 1;
    *dts = stamp;
    return 0;
}

/* ======================================================================== */
/* DMA buffers and the IPU stills                                           */
/* ======================================================================== */

// FUNC 0x00263368 mpeg_dma_alloc
/* memalign(0x80, size rounded up to 64 bytes) from the game heap. */
void *mpeg_dma_alloc(u32 size)
{
    return memalign(0x80, ((size + 0x3F) >> 6) << 6);
}

// FUNC 0x002633A8 mpeg_dma_free
void mpeg_dma_free(void *p)
{
    free(p);
}

// FUNC 0x00264E58 ipu_decode_image
/* An IPU still (gt2k_menu::Ipic::decode, T250) into width * height RGBA32
 * pixels at dst, 16 x 16 blocks in rows: the IPU's IDEC output, natively
 * (media_services.h). */
void ipu_decode_image(u32 dst, const IpuImageHeader *img)
{
    gt3m_ipu_decode_image((void *)(size_t)dst, img);
}

/* PC (no original): ipu_decode_image into a buffer of dstSize bytes:
 * nothing is written past it (a bigger picture is cut, logged). */
void ipu_decode_image_n(void *dst, u32 dstSize, const IpuImageHeader *img)
{
    gt3m_ipu_decode_image_n(dst, dstSize, img);
}
