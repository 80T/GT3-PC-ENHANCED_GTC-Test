/* movie_game.cpp - pssgame's game side (compiled with the decompiled code's
 * options: pc_prelude.h, as src/CMakeLists.txt builds the game).
 *
 * It does with the port's mpegif (T529) what the game does
 * (MenuBgMovie T253, play_movie T051), field by field at 59.94 Hz:
 * open the stream with MenuBgMovie's priorities, start a movie, on every
 * odd field release the last frame, take the next one and send the
 * presentation window (F_0025CFC0, as MenuBgMovie::update), stop and wait
 * as MenuBgMovie::stopAndWait, preload / play from memory as the arcade
 * menu, decode IPU stills through ipu_decode_image.  The frames are what
 * MenuBgMovie::upload would upload; the harness checks their cadence, the
 * sound's position against them, and dumps some.  Only the drawing is
 * missing (the renderer).  Stubs stand in for the volume lookup. */
#include "engine/media/T529_mpegif.h"
#include "engine/media/G002_mpeg_frameq.h"
#include "engine/pdi/T521_pdibgm.h"
#include "engine/pdi/T522_pdimpeg.h"
#include "engine/io/T502_file_resolve.h"
#include "engine/io/T503_vol_thread.h"
#include "engine/io/T510_vol_device.h"
#include "engine/sound/native/sound_native.h"
#include "eekernel.h"
#include "harness.h"

/* ======================================================================== */
/* Stubs: the volume (file_locate on gt3.vol's table) and the BGM volume    */
/* ======================================================================== */

VolDevice::VolDevice() {}
VolDevice::~VolDevice() {}
VolDevice vol_device_object;

int file_locate(const char *path, char *hostName, int *lsn, u32 *packedSize, VolDevice *dev)
{
    unsigned size;
    (void)dev;
    hostName[0] = 0;
    if (!hn_lookup(path, lsn, &size))
        return -1;
    *packedSize = size;
    return 0;
}

void pdibgm_set_volume(f32 volume)
{
    if (volume > 1.0f)
        volume = 1.0f;
    if (volume < -1.0f)
        volume = -1.0f;
    snd_native_bgm_set_volume((int)(volume * 32768.0f));
}

/* ======================================================================== */
/* The display: a field per vblank                                          */
/* ======================================================================== */

static int s_tid;
static volatile int s_fields;

static int vblank_handler(int cause)
{
    (void)cause;
    s_fields++;
    iWakeupThread(s_tid);
    ExitHandler();
    return 1;
}

/* gfx_vsync(n): wait n fields. */
static void vsync(int n)
{
    int target = s_fields + n;
    while (s_fields < target)
        SleepThread();
}

/* gs_get_field_mode() == PGL_FIELD_HALF_ODD on every second field. */
static int odd_field(void)
{
    return (s_fields & 1) != 0;
}

/* ======================================================================== */
/* MenuBgMovie, as far as the stream goes                                   */
/* ======================================================================== */

static const s64 kTimeScale = 2000000000LL;  /* MOVIE_TIME_SCALE / MOVIE_FIELD_RATE: us of frame n */
static const s64 kFieldRate = 59933;
static const s64 kHalfFrame = 0x412D;
static const s64 kTimeMax = 0x7FFFFFFFFFFFFFFFLL;

struct Movie {
    mpegif *stream;
    void *frame;
    void *lastFrame;
    s64 frameNo;
    int width, height;
    /* the harness's measurements */
    int frames;             /* frames taken */
    int firstField;         /* field of the first frame */
    int missed;             /* odd fields without a new frame after the first */
    int dupes;              /* frames equal to the one before */
    unsigned lastCrc;
    double avMin, avMax, avSum;
    int avCount;
    int windows;
    const char *dumpName;
    int dumpEvery;
};

static Movie g_mv;

static void movie_open(int w, int h)
{
    memset(&g_mv, 0, sizeof(g_mv));
    MpegifParam prio;
    prio.prioHigh = 1;
    prio.prioRead = 4;
    prio.prioFeed = 10;
    prio.prioDecode = 11;
    g_mv.stream = new mpegif(&prio, w, h);
    g_mv.frameNo = -1;
    g_mv.firstField = -1;
    g_mv.avMin = 1e30;
    g_mv.avMax = -1e30;
}

static void movie_close(void)
{
    delete g_mv.stream;
    g_mv.stream = NULL;
}

/* MenuBgMovie::play */
static int movie_play(const char *file, int withSound, int oneShot)
{
    mpegif *s = g_mv.stream;
    int busy;
    if (s->preloadBuf != NULL && strcmp(file, s->preloadName) == 0) {
        s->usePreload = 1;
        busy = s->start(NULL, 0, oneShot, MPEGIF_CMD_PLAY);
    } else {
        s->usePreload = 0;
        busy = s->start(file, withSound, oneShot, MPEGIF_CMD_PLAY);
    }
    g_mv.lastFrame = NULL;
    g_mv.frame = NULL;
    return busy;
}

/* MenuBgMovie::start (with the enhanced first-frame wait). */
static void movie_start(const char *file, int w, int h, int withSound, int oneShot)
{
    g_mv.frameNo = -1;
    g_mv.width = w;
    g_mv.height = h;
    while (movie_play(file, withSound, oneShot) != 0)
        vsync(1);
    int i;
    for (i = 0; i < 120; i++) {
        if (g_mv.stream->getSize(NULL, NULL) == 0)
            break;
        vsync(1);
    }
    if (i == 120)
        printf("pssgame: %s: no frame after 120 vsyncs\n", file);
}

/* MenuBgMovie::update, plus the measurements. */
static void movie_update(void)
{
    Movie &m = g_mv;
    if (m.stream == NULL)
        return;
    if (odd_field()) {
        if (m.frame != NULL)
            m.stream->releaseFrame();
        m.frame = m.stream->nextFrame();
        if (m.frame != NULL) {
            s64 n = m.frameNo++;
            if (n >= 0) {
                s64 start = n * kTimeScale / kFieldRate;
                s64 end = (n + 1) * kTimeScale / kFieldRate;
                F_0025CFC0(start < kHalfFrame ? start : start - kHalfFrame, end + kHalfFrame);
                m.windows++;
            }
            /* measurements */
            if (m.firstField < 0)
                m.firstField = s_fields;
            m.frames++;
            unsigned crc = hn_frame_crc(m.frame, m.width * m.height * 4);
            if (m.frames > 1 && crc == m.lastCrc)
                m.dupes++;
            m.lastCrc = crc;
            long long played = snd_native_bgm_played_us();
            if (played >= 0 && n >= 30) {
                /* the sound against the frame being shown (frame n + 1) */
                double av = (double)played / 1000.0 - (double)((n + 1) * kTimeScale / kFieldRate) / 1000.0;
                if (av < m.avMin) m.avMin = av;
                if (av > m.avMax) m.avMax = av;
                m.avSum += av;
                m.avCount++;
            }
            if (m.dumpName != NULL && m.dumpEvery > 0 && (m.frames - 1) % m.dumpEvery == 0) {
                char name[128];
                sprintf(name, "%s_%05d", m.dumpName, m.frames - 1);
                hn_dump_frame(m.frame, m.width, m.height, 1, name);
            }
        } else if (m.frameNo > 0) {
            int running = m.stream->control(MPEGIF_CMD_GET_STATE, NULL) != MPEGIF_ENDED;
            if (running) {
                F_0025CFC0(0, kTimeMax);
                m.missed++;
            }
        }
    }
    if (m.frame != NULL)
        m.lastFrame = m.frame;
}

static int movie_state(void)
{
    return g_mv.stream->control(MPEGIF_CMD_GET_STATE, NULL);
}

/* MenuBgMovie::stopAndWait */
static void movie_stop_and_wait(void)
{
    F_0025CFC0(0, kTimeMax);
    while (g_mv.stream->control(MPEGIF_CMD_STOP, NULL))
        vsync(1);
    for (;;) {
        int st = movie_state();
        if (st == MPEGIF_SWITCHED || st == MPEGIF_ENDED)
            break;
        vsync(1);
    }
    while (movie_state() != MPEGIF_ENDED)
        vsync(1);
}

/* Fields until `seconds' pass or the stream ends (oneShot). */
static int run_fields(double seconds)
{
    int fields = (int)(seconds * 59.94);
    int start = s_fields;
    while (s_fields - start < fields) {
        movie_update();
        vsync(1);
        if (movie_state() == MPEGIF_ENDED)
            return 1;
    }
    return 0;
}

static void report(const char *what, double t0)
{
    Movie &m = g_mv;
    printf("%s: %d frames in %.2f s (first after %d fields), %d odd fields without a new frame, %d repeated frames",
           what, m.frames, hn_now() - t0, m.firstField, m.missed, m.dupes);
    if (m.avCount > 0)
        printf(", sound - picture %.1f .. %.1f ms (mean %.1f)", m.avMin, m.avMax, m.avSum / m.avCount);
    printf("\n");
}

/* ======================================================================== */
/* Scenarios                                                                */
/* ======================================================================== */

/* play FILE SECONDS SOUND ONESHOT W H: play_movie (one-shot) or a menu
 * background (looping); a one-shot movie that has not ended after SECONDS
 * is skipped as start does in play_movie (30-frame fade, stopAndWait). */
static int scenario_play(const char *file, double seconds, int withSound, int oneShot, int w, int h)
{
    double t0 = hn_now();
    movie_open(w, h);
    g_mv.dumpName = "play";
    g_mv.dumpEvery = 150;
    movie_start(file, w, h, withSound, oneShot);
    int ended = run_fields(seconds);
    report(ended ? "ended" : "running", t0);
    int fade = 30 * 2;                      /* play_movie: 30 frames of fade */
    while (fade-- > 0) {
        movie_update();
        vsync(1);
    }
    double t1 = hn_now();
    movie_stop_and_wait();
    printf("stopAndWait: %.3f s\n", hn_now() - t1);
    movie_close();
    return 0;
}

/* switch FILE1 FILE2 SECONDS: a menu background, then another one (the
 * course select's preview), then stop. */
static int scenario_switch(const char *a, const char *b, double seconds)
{
    double t0 = hn_now();
    movie_open(640, 448);
    g_mv.dumpName = "switch";
    g_mv.dumpEvery = 120;
    movie_start(a, 640, 448, 0, 0);
    run_fields(seconds);
    report(a, t0);
    int before = g_mv.frames;
    double t1 = hn_now();
    while (movie_play(b, 0, 0) != 0)
        vsync(1);
    g_mv.frameNo = -1;
    run_fields(seconds);
    printf("after the switch: %d frames in %.2f s\n", g_mv.frames - before, hn_now() - t1);
    movie_stop_and_wait();
    movie_close();
    return 0;
}

/* preload FILE SECONDS W H: the arcade menu's preloaded menu_bg2 played
 * from memory (looping). */
static int scenario_preload(const char *file, double seconds, int w, int h)
{
    double t0 = hn_now();
    movie_open(640, 448);
    g_mv.stream->preload(file, 0x500000);
    printf("preloaded %s in %.3f s (%s)\n", file, hn_now() - t0, g_mv.stream->preloadBuf ? "ok" : "none");
    g_mv.dumpName = "preload";
    g_mv.dumpEvery = 100;
    t0 = hn_now();
    movie_start(file, w, h, 0, 0);
    run_fields(seconds);
    report("preloaded", t0);
    movie_stop_and_wait();
    g_mv.stream->freePreload();
    movie_close();
    return 0;
}

/* missing FILE: a movie that is not on the disc must end at once. */
static int scenario_missing(const char *file)
{
    double t0 = hn_now();
    movie_open(640, 448);
    movie_start(file, 640, 448, 0, 1);
    int st = movie_state();
    printf("missing %s: state %d after %.2f s\n", file, st, hn_now() - t0);
    movie_stop_and_wait();
    movie_close();
    printf("missing: stopAndWait returned, %.2f s in all\n", hn_now() - t0);
    return st == MPEGIF_ENDED ? 0 : 1;
}

/* ipic: the course stills through ipu_decode_image (Ipic::decode). */
struct IpicEntryRec {
    u32 offset;
    char name[0x1C];
};

static int scenario_ipic(void)
{
    void *data;
    unsigned size;
    if (!hn_read("stream/course.ipic", &data, &size)) {
        printf("ipic: no stream/course.ipic\n");
        return 1;
    }
    const u8 *base = (const u8 *)data;
    int count = *(const int *)(base + 4);
    const IpicEntryRec *e = (const IpicEntryRec *)(base + 8);
    double t0 = hn_now();
    for (int i = 0; i < count; i++) {
        const IpuImageHeader *hdr = (const IpuImageHeader *)(base + e[i].offset);
        int w = hdr->width, h = hdr->height;
        void *px = memalign(0x40, w * h * 4);
        ipu_decode_image((u32)(size_t)px, hdr);
        if (i % 10 == 0) {
            char name[64];
            sprintf(name, "ipic_%s", e[i].name);
            hn_dump_frame(px, w, h, 0, name);
        }
        free(px);
    }
    printf("ipic: %d pictures through ipu_decode_image in %.1f ms\n", count, (hn_now() - t0) * 1000.0);
    return 0;
}

int hg_run(int argc, char **argv)
{
    s_tid = GetThreadId();
    ChangeThreadPriority(s_tid, 7);         /* the render thread's priority */
    AddIntcHandler(INTC_VBLANK_S, vblank_handler, -1);
    EnableIntc(INTC_VBLANK_S);
    if (argc < 1)
        return 1;
    const char *cmd = argv[0];
    if (strcmp(cmd, "play") == 0 && argc >= 7)
        return scenario_play(argv[1], atof(argv[2]), atoi(argv[3]), atoi(argv[4]), atoi(argv[5]), atoi(argv[6]));
    if (strcmp(cmd, "switch") == 0 && argc >= 4)
        return scenario_switch(argv[1], argv[2], atof(argv[3]));
    if (strcmp(cmd, "preload") == 0 && argc >= 5)
        return scenario_preload(argv[1], atof(argv[2]), atoi(argv[3]), atoi(argv[4]));
    if (strcmp(cmd, "missing") == 0 && argc >= 2)
        return scenario_missing(argv[1]);
    if (strcmp(cmd, "ipic") == 0)
        return scenario_ipic();
    printf("pssgame: unknown scenario\n");
    return 1;
}
