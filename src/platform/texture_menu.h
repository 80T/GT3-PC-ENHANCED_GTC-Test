/* texture_menu.h - PC (2026-11-13, no original): the texture interface of
 * GT3 PC, a small panel in the picture's top left corner for the texture
 * replacements (engine/pgl/native/pgl_texrepl.h; gt3pc.ini [Textures],
 * platform/settings.h).
 *
 * F10 (the TextureMenu hotkey, platform/input_map.h) shows or hides it.
 * While it is shown it takes these keys only - the rest of the keyboard and
 * the controllers stay the game's, so a car can be driven and a camera moved
 * with it open:
 *   Up / Down            choose a line
 *   Left / Right         change it (a held key repeats)
 *   Enter / Space        the next value; on TEXTURE: dump the chosen one
 *   Page Up / Page Down  Select mode: 10 textures back / on
 *   F10 / Esc / Backspace  close
 * The lines: REPLACEMENTS on / off; DUMPING off / select / all; PRE-CACHE
 * on / off; COMPRESSION on / off; REFRESH - Enter reads the texture folders
 * again and loads the files that changed, came or went (with the pre-cache
 * off; Ctrl+F10 does the same with the panel hidden), for quick trials of a
 * replacement being made; and, in Select mode, TEXTURE - the list of
 * the textures drawn in the last frames, one of them chosen: it is drawn
 * green in the game while the panel is shown, and Enter writes it to
 * tex_replacements\dumps_selected.  Under them what the replacements are
 * doing: the folders and files found, the files loaded, waiting and refused,
 * the dumps written, the pre-cache made ahead.  A change applies at once
 * and is written to gt3pc.ini.
 *
 * The renderer gives the panel its side as a table of functions
 * (set_backend: platform code calls nothing of the renderer's by name).
 * Drawn with platform/overlay.h.  No Windows headers. */
#pragma once

#include <stdint.h>

struct ID3D11RenderTargetView;

namespace texture_menu {

/* What the replacements are doing (pgl_texrepl.h status). */
struct Status {
    bool listed = false;                    /* the folders' list is ready */
    uint32_t folders = 0, files = 0;        /* the folders on, the replacement files found in them */
    uint32_t loaded = 0, waiting = 0, failed = 0;   /* files in memory, loads waiting, files that could not be read */
    uint32_t dumped = 0, dumpsWaiting = 0;  /* files written this run, pictures waiting to be written */
    uint32_t cacheMade = 0, cacheTodo = 0;  /* the pre-cache made ahead: files done / still to do */
    uint32_t seen = 0;                      /* Select mode: the list's length */
    int selIndex = -1;                      /* the chosen one's place in it (-1: not in it now) */
    uint64_t selHash = 0;                   /* 0: none chosen */
    uint16_t selW = 0, selH = 0;
    bool selReplaced = false, selDumped = false, selPixels = false;
    /* refresh: one is running; how many were done, the last one's counts */
    bool refreshing = false;
    uint32_t refreshes = 0, refChanged = 0, refNew = 0, refGone = 0;
};

/* The renderer's side (pgl_texrepl.cpp gives it at its start). */
struct Backend {
    void (*status)(Status *out);
    void (*menu_open)(bool open);           /* the panel shown or hidden */
    void (*settings_changed)();             /* a [Textures] switch changed */
    void (*select_step)(int dir);           /* Select mode: the next (1) / previous (-1) texture */
    bool (*dump_selected)();                /* the chosen one written; false: not ready */
    bool (*refresh)();                      /* the folders listed again; false: refused (the pre-cache is on) */
    uint32_t (*refresh_serial)();           /* moves when a refresh is done */
};
void set_backend(const Backend *backend);

/* F10 (the window thread). */
void toggle();
/* Ctrl+F10 (the TextureRefresh hotkey; the window thread): the texture
 * folders read again and the files that changed loaded anew, the panel shown
 * or not - a message over the picture says what it found (platform/osd.h). */
void refresh_key();
/* F12 (the TextureToggle hotkey; the window thread): the replacements off
 * or on, the panel shown or not - for comparing a replacement with the
 * game's own texture; a message over the picture says which. */
void replace_key();
void hide();
bool shown();
/* A key went down while it is shown (the window thread; repeat: the key's
 * autorepeat): true when it was the panel's - it does not reach the game. */
bool key(int vk, bool repeat);
/* The output (pgl_targets' present), on the back buffer just before
 * Present. */
void present(ID3D11RenderTargetView *backbuffer);

} // namespace texture_menu
