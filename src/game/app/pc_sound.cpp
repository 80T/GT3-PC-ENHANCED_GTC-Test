/* pc_sound.cpp - the sound flush per game tick (pc_sound.h).  No original:
 * new PC code. */
#include "game/app/pc_sound.h"
#include "game/sound/T463_music_seq.h"          /* music_pc_wake */
#include "platform/frame_step.h"
#include "platform/scheduler.h"

#include <atomic>
#include <stdint.h>

namespace {

/* The timer wakes the music thread when the game ticks are this many
 * vblanks behind (a load, a long frame). */
constexpr int64_t PC_SOUND_STALL = 4;

/* A game tick after a gap this long (s) is the end of a stall: the vblanks
 * missed meanwhile (the timer's flushes, the ticks game/app/pc_frame.cpp
 * dropped) are forgotten. */
constexpr double PC_SOUND_RESYNC = 0.25;

std::atomic<uint64_t> g_woken{0};       /* wake-ups so far */
std::atomic<int64_t> g_base{-1};        /* vblank count - wake-ups at the first call */
std::atomic<double> g_wakeTime{-1.0};   /* the latest wake-up's block time */
double g_flushTime = -1.0;              /* the music thread's flush running: its block time (-1: none) */
double g_lastTick = -1.0;               /* when a game tick last ran (the render thread's) */

/* vblanks counted minus wake-ups made: how far the flushes are behind */
int64_t behind()
{
    int64_t base = g_base.load();
    const int64_t v = (int64_t)sched::vblank_count();
    if (base < 0) {
        int64_t want = v - (int64_t)g_woken.load();
        g_base.compare_exchange_strong(base, want);
        base = g_base.load();
    }
    return v - base - (int64_t)g_woken.load();
}

/* A wake-up: its block's time recorded first (the music thread reads it
 * after waking).  Two wake-ups before the thread runs are one flush (the
 * semaphore counts to 1), with the later time. */
void wake(double t, bool fromHandler)
{
    g_wakeTime.store(t);
    g_woken.fetch_add(1);
    music_pc_wake(fromHandler ? 1 : 0);
}

} // namespace

extern "C" void pc_sound_tick(double vblankTime)
{
    const double now = sched::now_seconds();
    const bool resync = g_lastTick >= 0.0 && now - g_lastTick > PC_SOUND_RESYNC;
    g_lastTick = now;
    int64_t b = behind();
    if (resync && b > 1) {
        g_base.fetch_add(b - 1);            /* the stall's vblanks forgotten */
        b = 1;
    }
    /* not ahead of the vblanks: a catch-up's extra ticks share a flush */
    if (b < 0)
        return;
    wake(vblankTime, false);
}

extern "C" int pc_sound_vblank(void)
{
    /* (the tick step's pause: the music waits for the stepped ticks too,
     * platform/frame_step.h) */
    if (frame_step::paused() || behind() <= PC_SOUND_STALL)
        return 0;
    wake(sched::vblank_time(), true);
    return 1;
}

extern "C" void pc_sound_flush_begin(void)
{
    g_flushTime = g_wakeTime.load();
}

extern "C" void pc_sound_flush_end(void)
{
    g_flushTime = -1.0;
}

extern "C" double pc_sound_block_time(void)
{
    return g_flushTime;
}
