/* pc_frame.h - the dynamic frame rate of GT3 PC (no original; docs/ROADMAP.md
 * item 9, docs/renderer.md "Frame rate").
 *
 * The game keeps its fixed 59.94 Hz tick: the render thread (T002) runs the
 * game's update once per vblank, as on the PS2, so the physics, the timings,
 * the replays and the ghosts are the same at any frame rate.  On a display
 * that refreshes faster, while a race asks for it (it runs, unpaused, not
 * behind a menu), the render thread also draws frames between the ticks, at
 * the display's rate, interpolated: the shown time lies up to one tick
 * before the last tick (g_pcViewLag), the cars through GT3's own
 * interpolating accessors (T302: prev x t + cur x (1 - t), the slow-motion
 * replays' mechanism), the camera's view and projection blended between
 * the last two ticks (pc_view_interpolate), the HUD's values (needles,
 * bars, times, fades) blended too (pc_tween) and the models' animation
 * tracks and palettes posed at the shown time (pc_anim_*, T131).  Smoke and
 * sparks move at 60 Hz.  On a 60 Hz display with vsync on and no frame cap
 * nothing changes: one frame per tick, no lag (with neither, the frames run
 * uncapped there too).  The frames between ticks
 * are drawn at the lowest priority (T002), without vsync, as fast as they
 * go (Present waits only for the GPU, outside the game CPU: gpu::present;
 * GT3PC_FRAME_RATE caps them). */
#ifndef GAME_APP_PC_FRAME_H
#define GAME_APP_PC_FRAME_H

#include "game/game.h"

/* While drawing: 1 if this frame follows a tick (the frame the PS2 drew);
 * 0 between ticks.  Draw code that changes state per frame does it only
 * when 1. */
extern int g_pcTickFrame;
/* While drawing: how far (0 .. 1 tick) the shown time lies before the last
 * tick; 0 outside drawing and on a 60 Hz display. */
extern f32 g_pcViewLag;

/* By the race, once per tick: draw frames between the ticks. */
void pc_frame_request_interpolation(void);

/* A replay's speed (docs/renderer.md "Replay speed", ROADMAP 51): 1/8 .. 8,
 * stepped by L2 / R2.  The ticks themselves run slower or faster, each the
 * same tick as ever, so all of the race keeps together and a replay plays the
 * same at any speed; in slow motion frames are drawn between them, whatever
 * the display.  What is not ticks stays at real time: the music, and the
 * camera cuts timed to it.
 * By a replay on screen, once per tick it runs (not paused, not behind a
 * menu: anything else keeps the normal rate). */
void pc_replay_speed_request(void);
/* A race starts: the normal speed. */
void pc_replay_speed_reset(void);
/* In a replay, each pad read of player `port' (0, 1): the buttons that step
 * the speed, held; each press of one is a step. */
void pc_replay_speed_buttons(int port, int slower, int faster);

/* After the projection and the view of a race view are set (key: the
 * view - one per screen of a split screen; shot: what it shows from, the
 * camera and its mount): kept per tick, and between ticks replaced by their
 * blend.  A new shot between two ticks is a cut, not blended: the view is
 * the tick's, and so is the rest of it (g_pcViewLag 0 until the next view:
 * cars at the shown time under a camera at the tick moved against it). */
void pc_view_interpolate(const void *key, u32 shot);
/* The frame's lag (what pc_frame_render_begin set g_pcViewLag to): each race
 * view starts from it (a cut in the one before set g_pcViewLag to 0). */
f32 pc_frame_lag(void);

/* Animations stepped once a tick (a model's tracks, T452; a car's lamp
 * blinks): drawn between the ticks at the shown time, dt x g_pcViewLag
 * before the tick's state.  key: the stepped object.  The step tells
 * pc_anim_stepped its dt (several steps in a tick add up); while drawing,
 * pc_anim_offset gives -dt x the lag when the key stepped in the last tick,
 * else 0 (not stepped: paused, or no frame between ticks). */
void pc_anim_stepped(const void *key, f32 dt);
f32  pc_anim_offset(const void *key);

/* Work for the end of the frame being drawn: fn(arg) once the object has
 * drawn and run its post-draw slot (T002's virtual_3), before the frame's
 * lists execute, with g_pcViewLag still the frame's.  Registered while
 * drawing (again in the same frame: once); for the frame only.  What the
 * renderer reads when the lists execute (the palettes) is set there. */
void pc_frame_at_end(void (*fn)(void *arg), void *arg);

/* Values the game changes once a tick and draws (the HUD's needles, bars,
 * times and fades): between the ticks they are drawn at their blend.  Each
 * belongs to an owner (the object that holds it) and is only touched inside
 * the owner's draw (pc_tween_begin / end), so a freed owner's values are
 * never written.  A step the kind does not expect (a timer started again, a
 * new lap) is not blended: that tick's frames show its value. */
enum {
    PC_TWEEN_ANY,                       /* any change (a needle, a bar) */
    PC_TWEEN_FADE,                      /* an alpha 0..1, by at most 0.1 a tick */
    PC_TWEEN_TIMER_UP,                  /* seconds counting up by 1/60 a tick */
    PC_TWEEN_MS,                        /* a time in ms counting up by ~17 a tick (-1: none) */
    PC_TWEEN_LOOP                       /* an animation time going up, from hi on again from lo */
};
/* Once a tick, after the update: the value just set. */
void pc_tween(const void *owner, f32 *value, int kind);
void pc_tween(const void *owner, int *value, int kind);
void pc_tween_loop(const void *owner, f32 *value, f32 lo, f32 hi);
/* Around the owner's draw: begin shows the values at their blend, end puts
 * back the tick's (a value the draw itself changed is left; nested pairs
 * for one owner: the outermost counts). */
void pc_tween_begin(const void *owner);
void pc_tween_end(const void *owner);
struct PcTweenScope {
    const void *owner;
    explicit PcTweenScope(const void *o) : owner(o) { pc_tween_begin(o); }
    ~PcTweenScope() { pc_tween_end(owner); }
};

/* The render thread (T002) */
int  pc_frame_interpolating(void);      /* frames are drawn between the ticks */
void pc_frame_tick_begin(void);         /* before each of the game's updates */
void pc_frame_tick_end(void);           /* after it */
void pc_frame_render_begin(int tick);   /* before a frame is drawn (tick: after ticks) */
void pc_frame_render_end(void);
/* In place of the loop's gfx_vsync(-1): one frame per vblank, or the
 * frames' own pacing (GT3PC_FRAME_RATE; none: as fast as they go). */
void pc_frame_wait(void);
/* Once the frame is presented and executed: the ticks the next frame runs
 * first - the vblanks due by its clock since the last tick (0: a frame
 * between ticks; more than 1: the frames are slower than the ticks, caught
 * up; at most 8, a longer stall drops the rest) - and its clock. */
int  pc_frame_ticks(void);

#endif /* GAME_APP_PC_FRAME_H */
