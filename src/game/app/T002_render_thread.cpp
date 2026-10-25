/* T002 - the render thread and GameObject.  0x001012E0..0x00101A80.
 *
 * The render thread (priority 7, its own 0x8000-byte stack) draws one
 * GameObject per frame, g_00350B10, which it copies from g_00350B14 (set by
 * render_set_object) at the top of every frame.  It alternates the two
 * packet buffers of g_0028F550 and the two frame buffers of
 * g_002F4230.frame.  The main thread asks it to stop with
 * render_thread_exit (g_00350B1C = its own thread id, then it sleeps until
 * the render thread wakes it and exits). */
#include "game/app/app_internal.h"
#include "eekernel.h"                       /* struct ThreadParam */
#include "cxxrt.h"                          /* type_info, __rtti_user (GCC 14's typeinfo function) */
/* _gp with a size, declared before G000's unsized declaration: egcs then
 * counts it as small data (-G8) and loads &_gp with one `la' (GAS: lui /
 * addiu) that the scheduler keeps in source order, as retail's
 * render_thread_start has it; unsized, it is split into %hi / %lo insns
 * scheduled with the other addresses. */
extern "C" char _gp[4];                     /* 0x003579F0 */
#include "runtime/startup/G000_collect2_ctors.h"       /* _gp (the render thread's gpReg) */
#include "engine/pgl/G001_flare.h"                /* gfx_* / gs_* / pgl_* (the GS driver), PGL_* enums */
#include "libgraph.h"                       /* PC: SCE_GS_NOINTERLACE */
#include "engine/media/G002_mpeg_frameq.h"          /* create_mutex_sema, signal_sema_helper */
#include "runtime/newlib/G007_stdio.h"                /* the thread / semaphore syscalls */
#include "game/app/T001_packet_buffers.h"       /* g_0028F550 */
#include "game/app/T002_render_thread.h"
#include "engine/input/T475_pad_manager.h"          /* PadManager g_002EA7F8 */
#include "engine/io/T501_memorycard_manager.h"   /* mc_manager_tick */
#include "engine/gfx/T512_render_context.h"       /* RenderContext g_002F4230, GSBufferPair */
#include "engine/gfx/T514_gs_buffer.h"            /* GSBuffer */
#include "game/app/pc_frame.h"                  /* PC: the dynamic frame rate */
#include "platform/pad_pc.h"                    /* PC: hold_reads (the ticks caught up) */

enum { RENDER_THREAD_STACK = 0x8000, RENDER_THREAD_PRIO = 7, RENDER_THREAD_PRIO_VSYNC = 1,
       MAIN_THREAD_PRIO = 9 };
/* PC: the render thread's priority for the frames between ticks
 * (game/app/pc_frame.h): below every game thread. */
enum { PC_FRAME_PRIO = 120 };

/* PC: the priority of a frame of `ticks' ticks: PC_FRAME_PRIO only between
 * the ticks of a race drawn interpolated.  Any other frame keeps 7, as on
 * the PS2, where no game thread below it runs while it draws: the menus
 * share their objects with the main thread without locks.  (A frame without
 * a tick in the menus' one frame per vblank - after ticks caught up,
 * pc_frame_ticks - went at 120, and the name entry's letters flickered,
 * most of them gone while a letter was confirmed: user report.) */
static int pc_frame_prio(int ticks)
{
    return (ticks > 0 || !pc_frame_interpolating()) ? RENDER_THREAD_PRIO : PC_FRAME_PRIO;
}
/* The image the bootstrap leaves on screen ("hot" start): its VRAM word
 * address, and the frames of boot_screen_fade (15 in, 120 full, 15 out). */
enum { BOOT_IMAGE_VRAM = 0x50000, BOOT_FADE_FRAMES = 150, BOOT_FADE_RAMP = 15 };

/* .sdata (-G8), in this order. */
GameObject *g_00350B10 = NULL;              /* 0x00350B10 gp-0x6EE0 the object being drawn */
GameObject *g_00350B14 = NULL;              /* 0x00350B14 gp-0x6EDC the object render_set_object requested */
int g_00350B18 = -1;                        /* 0x00350B18 gp-0x6ED8 the render thread (-1: none) */
int g_00350B1C = -1;                        /* 0x00350B1C gp-0x6ED4 the thread waiting for its exit (-1) */
int g_00350B20 = -1;                        /* 0x00350B20 gp-0x6ED0 the render semaphore */
int g_00350B24 = -1;                        /* 0x00350B24 gp-0x6ECC the boot-fade semaphore (-1: no fade) */
/* .bss: the render thread's stack (file-local). */
static u8 g_00493700[RENDER_THREAD_STACK];  /* 0x00493700 */

// FUNC 0x001012E0 render_thread_main
void render_thread_main(void *arg)
{
    if (g_00350B24 >= 0)
        boot_screen_fade();
    WaitSema(g_00350B20);
    gfx_vsync(1);
    /* PC: progressive, (0, 2, 1); the PS2's (1, 2, 1) was interlaced NTSC
     * (docs/ROADMAP.md, "Full progressive scan rendering everywhere") */
    g_002F4230.setDisplayMode(SCE_GS_NOINTERLACE, 2, 1);
    GSBufferPair *shown = &g_002F4230.frame;
    g_002F4230.setScreenSize(SCREEN_W, SCREEN_H);
    shown->buf[shown->cur].setDisplay();

    /* PC: the dynamic frame rate (game/app/pc_frame.h): an iteration runs
     * the ticks due - the pads and the object's update, once per vblank as
     * on the PS2; none for a frame between two ticks (a race on a display
     * faster than 60 Hz or with GT3PC_FRAME_RATE: interpolated), more than
     * one when the frames are slower than the ticks (caught up: the game
     * keeps its speed) - then draws once, with the post-draw slot after
     * ticks.  The first iteration runs a tick. */
    int ticks = 1;
    while (1) {                             /* not for (;;): egcs lays that one out rotated */
        /* Per frame (loop.c hoists them; retail copies the frame pair's
         * address of the code above into a register of its own). */
        GSBufferPair *frame = &g_002F4230.frame;
        VramAllocator *vram = &g_002F4230.vram;
        GSBufferPair *other = &g_002F4230.unkAC4;
        int doUpdate = 0;
        if (ticks > 0) {
            mc_manager_tick();
            g_00350B10 = g_00350B14;
            /* PC: of the ticks caught up, only the last reads the pads (the
             * others repeat the last read: platform/pad_pc.h) */
            pad_pc::hold_reads(ticks > 1);
            doUpdate = g_002EA7F8.update();
            pad_pc::hold_reads(false);
        }
        g_0028F550.cur = 1 - g_0028F550.cur;
        gfx_set_packet_buffer(g_0028F550.buf[g_0028F550.cur], g_0028F550.size);
        if (g_00350B10 != NULL) {
            for (int i = 0; i < ticks; i++) {
                if (i > 0) {
                    /* PC: a tick caught up (the memory card manager and the
                     * pads as the first's); not into another object */
                    if (g_00350B14 != g_00350B10)
                        break;
                    mc_manager_tick();
                    pad_pc::hold_reads(i < ticks - 1);
                    doUpdate = g_002EA7F8.update();
                    pad_pc::hold_reads(false);
                }
                pc_frame_tick_begin();
                if (doUpdate != 0)
                    g_00350B10->update();
                pc_frame_tick_end();
            }
            pc_frame_render_begin(ticks > 0);
            gfx_begin_packet();
            vram->lock();
            g_00350B10->render(frame->buf[frame->cur]);
            gfx_dma_sync_frame();
            if (ticks > 0)
                g_00350B10->virtual_3();
            vram->flushReleased();
            vram->unlock();
            gfx_end_packet();
            pc_frame_render_end();
            frame->buf[frame->cur].visible = 1;
        } else {
            for (int i = 0; i < ticks; i++) {
                pc_frame_tick_begin();      /* no object: no frames between ticks */
                pc_frame_tick_end();
            }
        }
        /* Up for the vsync wait, back to 7 for the next frame. */
        ChangeThreadPriority(g_00350B18, RENDER_THREAD_PRIO_VSYNC);
        gfx_sync_pipeline();
        pc_frame_wait();                    /* PC: was gfx_vsync(-1) */
        /* PC: a frame between ticks is presented, executed and drawn at the
         * lowest priority: it takes only the time the game's threads leave
         * (at 7 it kept the main thread from setting up the race) */
        ChangeThreadPriority(g_00350B18, pc_frame_prio(ticks));
        frame->cur = 1 - frame->cur;
        frame->buf[frame->cur].setDisplay();
        other->cur = 1 - other->cur;
        if (g_00350B1C != -1)
            break;
        if (g_00350B10 != NULL)
            gfx_kick_frame_lists(NULL);
        ticks = pc_frame_ticks();           /* PC: the next frame's ticks, by its clock */
        ChangeThreadPriority(g_00350B18, pc_frame_prio(ticks));
    }

    WakeupThread(g_00350B1C);
    g_00350B1C = -1;
    g_00350B18 = -1;
    ExitDeleteThread();
}

/* Run first by the render thread when the bootstrap started us ("hot"): the
 * image the bootstrap left at VRAM 0x50000 is drawn over the screen for 150
 * frames, fading in over 15, full for 120, fading out over 15; then both
 * frame buffers are cleared and g_00350B24 is signalled. */
// FUNC 0x00101528 boot_screen_fade
void boot_screen_fade(void)
{
    GSBuffer screen;                        /* sp+0x00 */
    screen.setAddr(0);
    screen.width   = SCREEN_W;
    screen.height  = SCREEN_H;
    screen.psm     = 1;                     /* PSMCT24 */
    screen.visible = 1;
    GSBuffer image;                         /* sp+0x40 */
    GSBuffer *img = &image;                 /* retail sets it up through $s1 = &image */
    img->setAddr(BOOT_IMAGE_VRAM);
    img->width  = SCREEN_W;
    img->height = SCREEN_H;
    img->psm    = 1;

    gfx_vsync(1);
    /* PC: no wait for the even field (progressive: the PS2 waited one vsync
     * more on the odd one, gs_get_field_mode() == PGL_FIELD_HALF_ODD) */

    for (int frameNo = 0; frameNo < BOOT_FADE_FRAMES; frameNo++) {
        float level;
        if (frameNo < BOOT_FADE_RAMP)
            level = (float)frameNo / (float)BOOT_FADE_RAMP;
        else if (frameNo < BOOT_FADE_FRAMES - BOOT_FADE_RAMP)
            level = 1.0f;
        else
            level = (float)(BOOT_FADE_FRAMES - frameNo) / (float)BOOT_FADE_RAMP;
        gfx_set_packet_buffer(g_0028F550.buf[g_0028F550.cur], g_0028F550.size);
        gfx_begin_packet();
        screen.setDrawBuffer();
        image.setTexture();
        gfx_tex_param(PGL_TEXPARAM_MMAG, PGL_FILTER_NEAREST);
        gfx_tex_param(PGL_TEXPARAM_MMIN, PGL_FILTER_NEAREST);
        pgl_color3f(level, level, level);
        gfx_disable(PGL_CAP_DEPTH_TEST);
        /* The origin as a variable: with 0.0f literals loop.c keeps 0.0 in
         * a saved FPU register; retail loads it in the loop. */
        float x0 = 0.0f;
        float x1 = (float)screen.width;
        float y1 = (float)screen.displayHeight();
        gs_draw_textured_sprite_f(x0, x0, x1, y1, x0, x0, (float)image.width, (float)image.height);
        gfx_end_packet();
        gfx_vsync(2);
        screen.setDisplay();
        gfx_kick_frame_lists(NULL);
        gfx_sync_pipeline();
    }

    gfx_set_packet_buffer(g_0028F550.buf[g_0028F550.cur], g_0028F550.size);
    gfx_begin_packet();
    gs_set_clear_color(0);
    GSBufferPair *frame = &g_002F4230.frame;
    frame->buf[frame->cur].setDrawBuffer();
    gs_clear_buffers(PGL_CLEAR_COLOR);
    frame->buf[1 - frame->cur].setDrawBuffer();
    gs_clear_buffers(PGL_CLEAR_COLOR);
    gfx_end_packet();
    gfx_vsync(2);
    gfx_kick_frame_lists(NULL);
    frame->buf[frame->cur].setDisplay();
    signal_sema_helper(g_00350B24);
}

/* Creates the render semaphore (and, with bootFade, the boot-fade one),
 * creates and starts the render thread (priority 7) and drops the calling
 * thread to priority 9. */
// FUNC 0x00101820 render_thread_start
void render_thread_start(int bootFade)
{
    struct ThreadParam param;               /* sp+0x00 */

    g_00350B1C = -1;
    g_00350B20 = create_mutex_sema(0);
    if (bootFade != 0)
        g_00350B24 = create_mutex_sema(0);
    param.entry        = render_thread_main;
    param.stack        = g_00493700;
    param.stackSize    = RENDER_THREAD_STACK;
    param.initPriority = RENDER_THREAD_PRIO;
    param.gpReg        = _gp;
    param.option       = 0;
    g_00350B18 = CreateThread(&param);
    StartThread(g_00350B18, NULL);
    ChangeThreadPriority(GetThreadId(), MAIN_THREAD_PRIO);
}

/* Starts the render thread if needed, waits for the boot fade and deletes
 * its semaphore, then lets the render thread run. */
// FUNC 0x001018C8 render_thread_release
void render_thread_release(void)
{
    if (g_00350B18 == -1)
        render_thread_start(0);
    if (g_00350B24 >= 0) {
        WaitSema(g_00350B24);
        DeleteSema(g_00350B24);
        g_00350B24 = -1;
    }
    signal_sema_helper(g_00350B20);
}

// FUNC 0x00101920 render_thread_exit
void render_thread_exit(void)
{
    if (g_00350B18 != -1) {
        g_00350B1C = GetThreadId();
        SleepThread();
    }
}

// FUNC 0x00101950 render_thread_lock
void render_thread_lock(void)
{
    WaitSema(g_00350B20);
    gfx_vsync(2);
}

/* NULL releases the render semaphore, anything else polls (takes) it. */
// FUNC 0x00101978 render_set_object
void render_set_object(GameObject *obj)
{
    g_00350B14 = obj;
    if (obj == NULL)
        signal_sema_helper(g_00350B20);
    else
        PollSema(g_00350B20);
}

// FUNC 0x001019B0 render_get_object
GameObject *render_get_object(void)
{
    return g_00350B10;
}

// FUNC 0x001019B8 GameObject::GameObject
GameObject::GameObject()
{
}

// FUNC 0x001019D0 GameObject::~GameObject
GameObject::~GameObject()
{
    /* The vptr, then operator delete with flags & 1. */
    /* implicit: operator delete (deleting destructor) */
}

// FUNC 0x00101A00 GameObject::update
void GameObject::update()
{
}

/* The default render: clears the buffer. */
// FUNC 0x00101A08 GameObject::render
void GameObject::render(GSBuffer &buf)
{
    buf.setDrawBuffer();
    gs_set_clear_color(0);
    gs_clear_buffers(PGL_CLEAR_COLOR);
}

// FUNC 0x00101A38 GameObject::virtual_3
void GameObject::virtual_3()
{
}

