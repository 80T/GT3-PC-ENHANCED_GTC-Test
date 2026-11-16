/* pc_aspect.cpp - the dynamic aspect of the races (pc_aspect.h).  No
 * original: new PC code. */
#include "game/app/pc_aspect.h"
#include "engine/pgl/G001_flare.h"              /* gfx_set_picture_aspect / _multi, gfx_set_design_scale */
#include "engine/text/T496_font.h"              /* g_002F1968 */
#include "platform/frame_pacing.h"              /* output_size, the screens' canvas */
#include "platform/settings.h"                  /* hudMaxAspect */

/* The frame's mapping: taken at its first race view, the 4:3 one else. */
struct PcAspectFrame {
    int race;
    f32 aspect;                                 /* A */
    f32 kx, ky;                                 /* the squeezes */
    f32 hud;                                    /* C */
    f32 shift;                                  /* 240 C - 320 */
    int multi;                                  /* drawn over the screens' canvas (platform/gpu.h "Screens") */
    f32 safe;                                   /* the HUD safe zone (pc_hud_safe) */
    f32 shown;                                  /* the picture's aspect as shown (a stretched menu's: the window's) */
    f32 menuKx, menuKy;                         /* a stretched menu's squeezes (pc_menu_kx / ky) */
};

static const PcAspectFrame kBase = {0, PC_ASPECT_BASE, 1.0f, 1.0f, PC_ASPECT_BASE, 0.0f, 0, 1.0f,
                                    PC_ASPECT_BASE, 1.0f, 1.0f};
static PcAspectFrame s_frame = kBase;
static f32 s_window = PC_ASPECT_BASE;           /* the window's last aspect (a minimised one keeps it) */
static f32 s_prevAspect = PC_ASPECT_BASE;       /* the frame before's A */
static int s_prevMulti;                         /* the frame before was drawn over the canvas */
static int s_menuView;
static int s_hud;                               /* a HUD draw sent the renderer its squeeze */
static int s_hudScreen;                         /* it is the 448-high 4:3 screen's (pc_hud_screen_fill) */
static int s_hudMenu;                           /* it is a menu HUD's: the font's menu squeeze off */
static struct {
    int on;
    f32 anchorX;
} s_menuHud;                                    /* pc_hud_menu */
static int s_splitSide;                         /* the split screen's kind (pc_split_side_take) */
static int s_dualFrame;                         /* this frame's split screen is on two monitors */

/* A HUD region (pc_hud_region): the GS rectangle x0 .. x1, y0 .. y1 of the
 * frame drawn as a screen of its own, the region's aspect a = A (x1 - x0) /
 * 640 x 448 / (y1 - y0) with its own squeezes, canvas and shift; sx, sy its
 * GS pixels per HUD unit. */
static struct {
    int on;
    f32 x0, x1, y0, y1;
    f32 kx, ky, hud, shift, sx, sy;
} s_region;

static f32 clampf(f32 v, f32 lo, f32 hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* The window's aspect (the game window's; a minimised one keeps the last). */
static f32 window_aspect(void)
{
    int w = 0, h = 0;
    pacing::output_size(&w, &h);
    if (w > 0 && h > 0)
        s_window = clampf((f32)w / (f32)h, PC_ASPECT_MIN, PC_ASPECT_MAX);
    return s_window;
}

void pc_aspect_frame_begin(void)
{
    s_prevAspect = s_frame.shown;
    s_prevMulti = s_frame.multi;
    s_frame = kBase;
    if (s_hud) {                                /* a HUD draw left open: the engine's offsets back to GS pixels */
        gfx_set_design_scale(1.0f, 1.0f);
        s_hud = 0;
    }
    s_hudMenu = 0;
    s_dualFrame = 0;
    s_region.on = 0;
    s_menuHud.on = 0;
    /* the menus' stretch ([Display] MenuAspect): the 4:3 picture shown over
     * the window, its car models (pc_menu_squeeze_projection) and texts (the
     * font's squeeze) squeezed back to their shape.  PC (2026-11-15, user
     * request: "Menu aspect: 16:9 option"): 16:9 the same over a 16:9
     * picture, which the renderer fits into the window with bars (as a race
     * frame held to its limits: pgl_targets picture_size) */
    const settings::MenuAspect menuAspect = settings::menu_aspect();
    if (menuAspect != settings::MenuAspect::original) {
        const f32 a = menuAspect == settings::MenuAspect::wide ? 16.0f / 9.0f : window_aspect();
        s_frame.shown = a;
        s_frame.menuKx = a > PC_ASPECT_BASE ? PC_ASPECT_BASE / a : 1.0f;
        s_frame.menuKy = a < PC_ASPECT_BASE ? a / PC_ASPECT_BASE : 1.0f;
        gfx_set_menu_aspect(a);
    } else {
        gfx_set_menu_aspect(0.0f);
    }
    font_set_squeeze(s_frame.menuKx, s_frame.menuKy);
}

f32 pc_menu_kx(void)
{
    return s_frame.menuKx;
}

f32 pc_menu_ky(void)
{
    return s_frame.menuKy;
}

void pc_aspect_movie_frame(void)
{
    const f32 a = window_aspect();
    s_frame.shown = a;
    gfx_set_menu_aspect(a);
}

int pc_movie_fit(f32 aspect, f32 *w, f32 *h)
{
    if (settings::movie_stretch())
        return 0;
    if (aspect <= 0.0f) {
        if (*w < 600.0f || *h < 400.0f)
            return 0;                           /* (a PSS in a box of a page: the box is its shape) */
        aspect = PC_ASPECT_BASE;
    }
    /* the rectangle's shape as shown: its GS pixels are the picture's */
    const f32 shown = (*w / *h) * s_frame.shown * (448.0f / 640.0f);
    if (aspect > shown * 1.001f) {
        *h *= shown / aspect;
        return 1;
    }
    if (aspect < shown * 0.999f) {
        *w *= aspect / shown;
        return 1;
    }
    return 0;
}

void pc_menu_squeeze_projection(void)
{
    if (s_menuView)
        return;                                 /* (a race view in the menu: pc_aspect_kx / ky carry it) */
    if (s_frame.menuKx != 1.0f || s_frame.menuKy != 1.0f)
        vu0_scale(s_frame.menuKx, s_frame.menuKy, 1.0f);
}

void pc_aspect_menu_view(int on)
{
    s_menuView = on;
}

/* A menu HUD's squeeze applies: on, in a race view of a stretched menu. */
static int menu_hud_squeezed(void)
{
    return s_menuHud.on && s_menuView && !s_frame.race &&
           (s_frame.menuKx != 1.0f || s_frame.menuKy != 1.0f);
}

void pc_hud_menu(f32 anchorX)
{
    s_menuHud.on = 1;
    s_menuHud.anchorX = anchorX;
}

void pc_hud_menu_end(void)
{
    s_menuHud.on = 0;
}

/* GS = anchor + (x - anchor) k, as the projection's GS = (x - left) k: left
 * = anchor (1 - 1 / k), 640 / k wide; in y the top the anchor. */
void pc_hud_menu_window(f32 height, f32 *left, f32 *right, f32 *top, f32 *bottom)
{
    if (!menu_hud_squeezed())
        return;
    *left = s_menuHud.anchorX * (1.0f - 1.0f / s_frame.menuKx);
    *right = *left + 640.0f / s_frame.menuKx;
    *top = 0.0f;
    *bottom = height / s_frame.menuKy;
}

void pc_aspect_race_frame(void)
{
    int w = 0, h = 0;
    if (s_menuView || s_frame.race)
        return;
    /* the span's race frames, the split screen's on two monitors: over the
     * screens' canvas */
    const int multi = (pacing::span_active() || s_dualFrame) && pacing::screen_count() > 1;
    if (multi)
        pacing::canvas_size(&w, &h);
    else
        pacing::output_size(&w, &h);
    if (w > 0 && h > 0)
        s_window = clampf((f32)w / (f32)h, PC_ASPECT_MIN, PC_ASPECT_MAX);

    PcAspectFrame f;
    const f32 a = s_window;
    f.race = 1;
    f.aspect = a;
    f.kx = a > PC_ASPECT_BASE ? PC_ASPECT_BASE / a : 1.0f;
    f.ky = a < PC_ASPECT_BASE ? a / PC_ASPECT_BASE : 1.0f;
    f.hud = clampf(a, PC_ASPECT_BASE, settings::display().hudMaxAspect);   /* (the setting is 4:3 or more) */
    f.shift = 240.0f * f.hud - 320.0f;
    f.multi = multi;
    f.safe = (f32)settings::hud_safe_zone() / 100.0f;
    f.shown = a;
    f.menuKx = f.menuKy = 1.0f;                 /* (a race frame: no menu squeeze) */
    s_frame = f;
    gfx_set_picture_aspect(a);
    gfx_set_picture_multi(multi);
    font_set_squeeze(1.0f, 1.0f);
}

int pc_aspect_race(void)
{
    return s_frame.race;
}

f32 pc_aspect(void)
{
    return s_frame.aspect;
}

f32 pc_aspect_prev(void)
{
    return s_prevAspect;
}

void pc_aspect_picture_rect(f32 aspect, int *x0, int *y0, int *x1, int *y1)
{
    f32 a = s_frame.aspect;
    f32 gx0 = 0.0f, gx1 = 640.0f;
    *x0 = 0;
    *y0 = 0;
    *x1 = 640;
    *y1 = 448;
    if (aspect <= 0.0f)
        return;
    if (s_frame.multi && !s_prevMulti) {
        /* drawn over the canvas now, the picture before on the game window
         * alone: within its part (pacing::canvas_rect 0) */
        int cw = 0, ch = 0, cx = 0, w0 = 0;
        pacing::canvas_size(&cw, &ch);
        pacing::canvas_rect(0, &cx, &w0);
        if (cw > 0) {
            gx0 = 640.0f * (f32)cx / (f32)cw;
            gx1 = 640.0f * (f32)(cx + w0) / (f32)cw;
            a *= (gx1 - gx0) / 640.0f;
        }
    }
    const f32 mid = 0.5f * (gx0 + gx1);
    *x0 = (int)(gx0 + 0.5f);
    *x1 = (int)(gx1 + 0.5f);
    if (aspect < a * 0.999f) {
        const int half = (int)(0.5f * (gx1 - gx0) * aspect / a + 0.5f);
        *x0 = (int)(mid + 0.5f) - half;
        *x1 = (int)(mid + 0.5f) + half;
    } else if (aspect > a * 1.001f) {
        const int half = (int)(224.0f * a / aspect + 0.5f);
        *y0 = 224 - half;
        *y1 = 224 + half;
    }
}

/* (a race view inside a stretched menu, pc_aspect_menu_view: the menu's
 * squeezes, so that its 3D - the view's projection, the mirror, the lens
 * flare - keeps its shape on the stretched picture as the menu's cars do) */
f32 pc_aspect_kx(void)
{
    return s_menuView && !s_frame.race ? s_frame.menuKx : s_frame.kx;
}

f32 pc_aspect_ky(void)
{
    return s_menuView && !s_frame.race ? s_frame.menuKy : s_frame.ky;
}

f32 pc_hud_aspect(void)
{
    return s_frame.hud;
}

f32 pc_hud_shift(void)
{
    if (s_region.on)
        return s_region.shift;
    return s_frame.shift;
}

int pc_split_side_take(void)
{
    s_splitSide = (int)settings::split_screen();
    return s_splitSide;
}

int pc_split_side(void)
{
    return s_splitSide;
}

int pc_split_monitors(void)
{
    return s_splitSide == PC_SPLIT_MONITORS && pacing::screen_count() >= 2 && !pacing::span_active();
}

int pc_split_monitors_frame(void)
{
    pacing::want_split_screen();
    s_dualFrame = pc_split_monitors();
    return s_dualFrame;
}

int pc_split_monitor_x(void)
{
    int cw = 0, ch = 0, x = 0, w = 0;
    pacing::canvas_size(&cw, &ch);
    pacing::canvas_rect(1, &x, &w);
    if (cw <= 0 || x <= 0)
        return 320;
    const int gx = (int)(640.0f * (f32)x / (f32)cw + 0.5f);
    return gx < 64 ? 64 : (gx > 576 ? 576 : gx);
}

int pc_hud_style(void)
{
    return (settings::split_hud_analog() ? PC_HUD_SPLIT_ANALOG : 0) |
           (settings::single_hud_digital() ? PC_HUD_SINGLE_DIGITAL : 0);
}

void pc_view_scales(f32 w, f32 h, f32 *kx, f32 *ky, f32 *sx, f32 *sy)
{
    const f32 a = h > 0.0f ? s_frame.aspect * (w / 640.0f) * (448.0f / h) : PC_ASPECT_BASE;
    *kx = a > PC_ASPECT_BASE ? PC_ASPECT_BASE / a : 1.0f;
    *ky = a < PC_ASPECT_BASE ? a / PC_ASPECT_BASE : 1.0f;
    *sx = w * *kx / 640.0f;
    *sy = h * *ky / 448.0f;
}

void pc_hud_region(f32 x0, f32 y0, f32 x1, f32 y1)
{
    s_region.on = 0;
    if (!s_frame.race || x1 <= x0 || y1 <= y0)
        return;
    const f32 a = s_frame.aspect * ((x1 - x0) / 640.0f) * (448.0f / (y1 - y0));
    s_region.on = 1;
    s_region.x0 = x0;
    s_region.x1 = x1;
    s_region.y0 = y0;
    s_region.y1 = y1;
    pc_view_scales(x1 - x0, y1 - y0, &s_region.kx, &s_region.ky, &s_region.sx, &s_region.sy);
    s_region.hud = clampf(a, PC_ASPECT_BASE, settings::display().hudMaxAspect);
    s_region.shift = 240.0f * s_region.hud - 320.0f;
}

void pc_hud_region_end(void)
{
    s_region.on = 0;
}

f32 pc_hud_safe(void)
{
    return s_frame.safe;
}

f32 pc_hud_kx(void)
{
    return (s_region.on ? s_region.sx : s_frame.kx) * s_frame.safe;
}

f32 pc_hud_ky(void)
{
    return (s_region.on ? s_region.sy : s_frame.ky) * s_frame.safe;
}

f32 pc_hud_center_x(void)
{
    return s_region.on ? 0.5f * (s_region.x0 + s_region.x1) : 320.0f;
}

f32 pc_hud_center_y(void)
{
    return s_region.on ? 0.5f * (s_region.y0 + s_region.y1) : 224.0f;
}

int pc_hud_draw_area(int *x, int *y, int *w, int *h)
{
    if (!s_region.on)
        return 0;
    *x = (int)(s_region.x0 + 0.5f);
    *y = (int)(s_region.y0 + 0.5f);
    *w = (int)(s_region.x1 + 0.5f) - *x;
    *h = (int)(s_region.y1 + 0.5f) - *y;
    return 1;
}

/* The window's width / height in HUD units are 640 / kx x height / ky,
 * centred on the HUD screen; through the safe zone s, 1 / s of that (the HUD
 * drawn s times as large about the centre). */
static void window_units(f32 height, f32 safe, f32 *left, f32 *right, f32 *top, f32 *bottom)
{
    const f32 kx = s_region.on ? s_region.kx : s_frame.kx, ky = s_region.on ? s_region.ky : s_frame.ky;
    const f32 halfW = 320.0f / (kx * safe), halfH = 0.5f * height / (ky * safe);
    *left = 320.0f - halfW;
    *right = 320.0f + halfW;
    *top = 0.5f * height - halfH;
    *bottom = 0.5f * height + halfH;
}

void pc_hud_window(f32 height, f32 *left, f32 *right, f32 *top, f32 *bottom)
{
    window_units(height, s_frame.safe, left, right, top, bottom);
}

void pc_view_window(f32 height, f32 *left, f32 *right, f32 *top, f32 *bottom)
{
    window_units(height, 1.0f, left, right, top, bottom);
}

void pc_hud_rect(int *x, int *y, int *w, int *h)
{
    f32 l, r, t, b;
    pc_hud_window(448.0f, &l, &r, &t, &b);
    *x = (int)l - 1;
    *y = (int)t - 1;
    *w = (int)(r - l) + 3;
    *h = (int)(b - t) + 3;
}

/* The system font's open glyph batch recorded now (under the squeeze it was
 * drawn for) and a new one opened. */
static void font_cut(void)
{
    if (g_002F1968.active != 0) {
        g_002F1968.flush();
        g_002F1968.begin();
    }
}

void pc_hud_begin(PcHudLayout layout, f32 height)
{
    f32 l, r, t, b;
    if (!s_frame.race) {
        if (!menu_hud_squeezed())
            return;
        /* a menu HUD (pc_hud_menu): its texts squeezed by the game font's
         * scale alone, the menu's squeeze back at pc_hud_end */
        l = 0.0f;
        r = 640.0f;
        t = 0.0f;
        b = height;
        pc_hud_menu_window(height, &l, &r, &t, &b);
        font_cut();
        font_set_squeeze(1.0f, 1.0f);
        g_002F1968.setScale(s_frame.menuKx, s_frame.menuKy);
        g_002F1968.setOffset(-l, -t);
        gfx_set_design_scale(s_frame.menuKx, s_frame.menuKy);
        s_hud = 1;
        s_hudMenu = 1;
        s_hudScreen = 0;
        return;
    }
    pc_hud_window(height, &l, &r, &t, &b);
    font_cut();
    /* setPos: GS = (pos + offset) x scale; the projection: GS = (pos - l) x
     * kx (a region's: its GS pixels per HUD unit) */
    g_002F1968.setScale(pc_hud_kx(), pc_hud_ky());
    g_002F1968.setOffset(-l - (layout != PC_HUD_SCREEN ? pc_hud_shift() : 0.0f), -t);
    gfx_set_design_scale(pc_hud_kx(), pc_hud_ky());
    s_hud = 1;
    s_hudScreen = layout == PC_HUD_SCREEN && height == 448.0f;
}

void pc_hud_end(void)
{
    if (s_hud) {
        font_cut();
        gfx_set_design_scale(1.0f, 1.0f);
        s_hud = 0;
    }
    if (s_hudMenu) {
        font_set_squeeze(s_frame.menuKx, s_frame.menuKy);   /* the menu's texts */
        s_hudMenu = 0;
    }
    s_hudScreen = 0;
    g_002F1968.setScale(1.0f, 1.0f);
    g_002F1968.setOffset(0.0f, 0.0f);
}

int pc_hud_screen_fill(f32 *left, f32 *top, f32 *right, f32 *bottom)
{
    f32 l, r, t, b;
    if (!s_hud || !s_hudScreen)
        return 0;
    if (*left > 0.0f || *right < 640.0f || *top > 0.0f || *bottom < 448.0f)
        return 0;
    pc_hud_window(448.0f, &l, &r, &t, &b);
    if (*left > l - 1.0f)
        *left = l - 1.0f;
    if (*right < r + 1.0f)
        *right = r + 1.0f;
    if (*top > t - 1.0f)
        *top = t - 1.0f;
    if (*bottom < b + 1.0f)
        *bottom = b + 1.0f;
    return 1;
}
