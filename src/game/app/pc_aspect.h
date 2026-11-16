/* pc_aspect.h - the dynamic aspect of the races (docs/ROADMAP.md item 8,
 * docs/renderer.md "Aspect").  No original: new PC code.
 *
 * The menus stay 4:3: the renderer shows them pillarboxed, or, with
 * [Display] MenuAspect=stretch (ROADMAP item 10; the settings menu, the
 * launcher), stretched over the window (gfx_set_menu_aspect: every frame
 * without a race view) - their 3D car models (pc_menu_squeeze_projection:
 * MenuCarCamera, the GT mode car list, the prizes) and their texts (the
 * font's squeeze, font_set_squeeze: glyphs and advances, the alignment
 * kept) squeezed by pc_menu_kx / ky so they keep their shape; the rest (the
 * pictures, the boxes) is stretched.  A frame with a
 * race view (RacePS2Base::drawView / drawSubView call pc_aspect_race_frame)
 * fills the window at its aspect A (width / height, held to PC_ASPECT_MIN ..
 * PC_ASPECT_MAX: beyond, black bars): the game still draws into its 640 x 448
 * GS frame, squeezed - by kx = (4/3) / A horizontally when the window is
 * wider than 4:3 (the view shows more to the sides), by ky = A / (4/3)
 * vertically when it is taller (more above and below) - and the renderer's
 * targets at the window's size undo the squeeze at full resolution
 * (pgl_targets.cpp: sx != sy).  The mapping is taken once per frame, at its
 * first race view.  The game's own anamorphic 16:9 option (GameOptions,
 * RenderContext::wide) is dormant: its code drew the same squeeze for 16:9
 * (x 0.75, a HUD 853.33 wide, wide-aligned elements 106.67 out); those
 * places now draw it for A.
 *
 * The HUD (and the race's other 2D) is designed in HUD units: the 4:3 screen
 * is 640 x 448 of them (a split screen's view: its own, a HUD region).  Its
 * canvas has the aspect C = A held to 4:3 .. HudMaxAspect (gt3pc.ini, 21:9 by default):
 * while A >= 4:3 the full height, elements aligned to a side anchored to the
 * canvas's edge (pc_hud_shift); beyond the cap the canvas is centred; taller
 * than 4:3 it is the 4:3 band across the full width, centred vertically.  The
 * window spans pc_hud_window's rectangle of HUD units, the projection of a
 * HUD draw.  A HUD draw runs between pc_hud_begin and pc_hud_end: the system
 * font maps its texts as that projection maps the drawing, and the renderer
 * keeps the textures' texel centres in HUD pixels (gfx_set_design_scale), so
 * that the squeezed glyphs sample as they do at 4:3.
 *
 * The HUD safe zone s ([Graphics] HudSafeZone, 75 .. 100 %): the HUD's part
 * of the window - the window spans 1 / s times as many HUD units (about the
 * centre: pc_hud_window), a HUD unit s times the GS pixels (pc_hud_kx / ky),
 * so the HUD, its texts, the race menus and the mirror are drawn s times as
 * large about the window's centre, the side-anchored elements nearer it;
 * full-screen fills still reach the window's edges (pc_hud_rect,
 * pc_hud_screen_fill), and pc_view_window is the window without it (the
 * replays' vignette).
 *
 * Outside a race frame every function answers the 4:3 values (kx = ky = 1,
 * no shift, the 640 x 448 rectangle, no safe zone) and pc_hud_begin does
 * nothing (but for a race HUD in a stretched menu: pc_hud_menu). */
#ifndef GAME_APP_PC_ASPECT_H
#define GAME_APP_PC_ASPECT_H

#include "game/game.h"

/* The 4:3 base, as the aspect of a 640 x 448 GS frame shown on a 4:3 TV. */
#define PC_ASPECT_BASE (4.0f / 3.0f)
/* The window aspects a race frame follows (1:4 .. 8:1, three 21:9 monitors
 * spanned; beyond, bars). */
#define PC_ASPECT_MIN  0.25f
#define PC_ASPECT_MAX  8.0f

/* By the render thread before each frame is drawn: back to the 4:3 picture. */
void pc_aspect_frame_begin(void);
/* By a race view, first thing: this frame is a race frame at the window's
 * aspect (the renderer is told: gfx_set_picture_aspect).  The first call of
 * a frame takes the window's size; the others keep it. */
void pc_aspect_race_frame(void);
/* A race view inside a 4:3 menu (the licence test's pre-race menu, T103;
 * the race menus, T112; the data logger's replay window, T055): on, race
 * views leave the frame at 4:3 (pc_aspect_race_frame does nothing); off
 * again after.  Meanwhile, in a stretched menu, pc_aspect_kx / ky answer
 * the menu's squeezes (pc_menu_kx / ky): the view's 3D - its projection,
 * the mirror, the lens flare - keeps its shape as the menu's cars do (and
 * pc_menu_squeeze_projection does nothing: no second squeeze). */
void pc_aspect_menu_view(int on);

/* The menus' stretch this frame (1 at 4:3 and in a race frame): kx = (4/3)
 * / A when the window is wider than 4:3, ky = A / (4/3) when it is taller.
 * pc_menu_squeeze_projection: a menu 3D view's projection just set (the
 * projection matrix current), squeezed by them about its centre, so that a
 * car keeps its shape on the stretched picture (its place still follows the
 * menu's layout). */
f32 pc_menu_kx(void);
f32 pc_menu_ky(void);
void pc_menu_squeeze_projection(void);

/* The movies' shape ([Display] MovieAspect, docs/movies.md "Shape").
 * pc_aspect_movie_frame: a frame that is a full-screen movie alone
 * (MovieRender): its 640 x 448 picture shown over the window, whatever
 * MenuAspect is, so that the movie has the whole window.  pc_movie_fit: the
 * movie's rectangle w x h (GS pixels of the picture, drawn about its centre)
 * brought to `aspect', the shape of what is shown (> 0: an MP4's picture; <=
 * 0: a PSS, 4:3 when full-screen, else its box's shape as it is), as the
 * picture is shown now.  1 when it changed it: what is left of the rectangle
 * is the caller's to fill.  MovieAspect=stretch: never. */
void pc_aspect_movie_frame(void);
int pc_movie_fit(f32 aspect, f32 *w, f32 *h);

/* A race HUD inside a stretched menu (the data logger's replay window:
 * RaceSolitaire::virtual_45 draws its two race displays at the frame's two
 * sides).  From pc_hud_menu to pc_hud_menu_end, a race display's HUD draw in
 * a pc_aspect_menu_view of a stretched menu has its HUD units squeezed by
 * pc_menu_kx / ky toward the HUD point (anchorX, 0) - its side's edge of the
 * frame - so that its meters keep their shape where the 4:3 layout put them:
 * pc_hud_menu_window gives its projection's rectangle (the one given is kept
 * when there is no squeeze), and pc_hud_begin / pc_hud_end set the system
 * font so (the font's menu squeeze off meanwhile: no second squeeze). */
void pc_hud_menu(f32 anchorX);
void pc_hud_menu_end(void);
void pc_hud_menu_window(f32 height, f32 *left, f32 *right, f32 *top, f32 *bottom);

/* This frame is a race frame. */
int pc_aspect_race(void);
/* The picture's aspect A (4:3 outside a race frame). */
f32 pc_aspect(void);
/* The aspect of the frame drawn before this one (the buffer shown now). */
f32 pc_aspect_prev(void);
/* Where a picture of `aspect' (drawn in an earlier frame: an afterimage of
 * the frame shown before a race's first) lies in this frame's GS frame, as
 * the window showed it: the largest rectangle of that aspect in the window,
 * centred, in whole GS pixels (0, 0 .. 640, 448 when it is this frame's). */
void pc_aspect_picture_rect(f32 aspect, int *x0, int *y0, int *x1, int *y1);
/* The squeezes: kx = (4/3) / A when wider than 4:3, else 1; ky = A / (4/3)
 * when taller, else 1 (a race view in a stretched menu: the menu's). */
f32 pc_aspect_kx(void);
f32 pc_aspect_ky(void);

/* The HUD canvas's aspect C. */
f32 pc_hud_aspect(void);
/* How far the canvas's side edges lie beyond the 4:3 screen's, in HUD units
 * (240 C - 320; 0 at 4:3): the shift of an element aligned to a side (in a
 * HUD region the region's). */
f32 pc_hud_shift(void);
/* The window in HUD units for a HUD screen 640 x height (448): x left ..
 * right, y top .. bottom (0, 640, 0, height at 4:3; in a HUD region the
 * region's), through the safe zone.  pc_view_window: the same without it
 * (the window's own extent: the picture's effects). */
void pc_hud_window(f32 height, f32 *left, f32 *right, f32 *top, f32 *bottom);
void pc_view_window(f32 height, f32 *left, f32 *right, f32 *top, f32 *bottom);
/* The HUD safe zone s of this frame (0.75 .. 1; 1 outside a race frame). */
f32 pc_hud_safe(void);
/* The 448-high window as a rectangle of whole HUD units, a unit beyond it on
 * every side (full-screen fills). */
void pc_hud_rect(int *x, int *y, int *w, int *h);

/* The 2-player split screen's kind (gt3pc.ini [Graphics] SplitScreen): one
 * view above the other (0: race mode 7), side by side (1: race mode 8,
 * which retail kept for its dormant 16:9 screen), or on two monitors
 * (PC_SPLIT_MONITORS: side by side over the screens' canvas, each player's
 * view a monitor; mode 8 too).  Whichever, each view is a screen of its own
 * as a single player's (user request): its 3D at its own aspect from the
 * 4:3 base (pc_view_scales), the single player's HUD layouts in its HUD
 * region.  pc_split_side_take reads the setting at a split screen race's
 * (or replay's) start, RaceDisplay::virtual_0; pc_split_side answers it
 * until the next. */
#define PC_SPLIT_MONITORS 2
int pc_split_side_take(void);
int pc_split_side(void);
/* The two monitors are on: the kind PC_SPLIT_MONITORS, the second monitor's
 * screen open (platform/gpu.h "Screens"; not with the span). */
int pc_split_monitors(void);
/* By RaceSplitBattle::render each frame of the kind, before its views: asks
 * for the second monitor (kept open while asked), and this frame is drawn
 * over the canvas when it is open.  1: it is. */
int pc_split_monitors_frame(void);
/* Where the second monitor's part starts in the GS frame (its share of the
 * canvas; 320 for two alike monitors). */
int pc_split_monitor_x(void);

/* The race HUD's meter style (gt3pc.ini [Graphics] SplitHud / SingleHud,
 * the settings menu): bits PC_HUD_*.  RaceDisplay::setLayout places the
 * meters by it, again when it changes. */
enum {
    PC_HUD_SPLIT_ANALOG   = 1,      /* the split screens: the single player's analog meters, else its digital set */
    PC_HUD_SINGLE_DIGITAL = 2       /* a single player: the digital set */
};
int pc_hud_style(void);

/* A view of the race frame as a screen of its own: a GS rectangle w x h,
 * its aspect a = A (w / 640) (448 / h) (the frame's picture is A wide).  kx,
 * ky: its squeezes from the 4:3 base, as a single player's screen of that
 * aspect has them (its 3D's projection: drawSubView); sx, sy: its GS
 * pixels per HUD unit (a 640 x 448 design over it, wider than 4:3 the full
 * height, taller the full width - the mirror's rectangle, a HUD region). */
void pc_view_scales(f32 w, f32 h, f32 *kx, f32 *ky, f32 *sx, f32 *sy);

/* A HUD region: the GS rectangle x0 .. x1, y0 .. y1 of the frame drawn as a
 * screen of its own (a split screen's view: its HUD; on two monitors also
 * the race menus), at its own aspect (pc_view_scales) - pc_hud_window,
 * pc_hud_shift and pc_hud_begin answer for it until pc_hud_region_end.
 * pc_hud_kx / ky: the GS pixels per HUD unit (the frame's kx / ky outside a
 * region; times the safe zone), pc_hud_center_x / _y the GS point of the HUD screen's middle
 * (320, 224), and pc_hud_draw_area the region's GS rectangle for a draw into
 * the whole frame (0 outside a region). */
void pc_hud_region(f32 x0, f32 y0, f32 x1, f32 y1);
void pc_hud_region_end(void);
f32 pc_hud_kx(void);
f32 pc_hud_ky(void);
f32 pc_hud_center_x(void);
f32 pc_hud_center_y(void);
int pc_hud_draw_area(int *x, int *y, int *w, int *h);

/* Where a HUD draw's texts placed in screen units (Font::setPos) go. */
enum PcHudLayout {
    PC_HUD_CANVAS,          /* RaceDisplay: relative to the canvas's left edge */
    PC_HUD_SCREEN           /* the 4:3 screen in the middle (race menus, the examination messages) */
};
/* Begins a HUD draw of a race frame, the projection set (or about to be) to
 * pc_hud_window(height): the system font (g_002F1968) squeezed by (kx, ky)
 * and offset so that its texts land where the projection puts that layout's
 * HUD units, the renderer told the squeeze.  The font's open glyph batch is
 * cut there (closed and opened again), so that the glyphs before it keep
 * their own mapping.  Outside a race frame nothing, but for a menu HUD
 * (pc_hud_menu): the font squeezed and offset as its projection maps. */
void pc_hud_begin(PcHudLayout layout, f32 height);
/* Ends it (cuts the font's batch, the renderer back to 1:1); the font's
 * scale back to 1 and offset to 0 in any frame. */
void pc_hud_end(void);
/* In a HUD draw of the 448-high 4:3 screen (PC_HUD_SCREEN: the race
 * menus), a rectangle (HUD units, y down) that covers the whole 640 x 448
 * screen - a full-screen fill: the pause menus' dimmer, 896 wide, which a
 * window wider than 1.87:1 exceeded - is widened to the window, a unit beyond
 * it on every side.  1 when it was. */
int pc_hud_screen_fill(f32 *left, f32 *top, f32 *right, f32 *bottom);

#endif /* GAME_APP_PC_ASPECT_H */
