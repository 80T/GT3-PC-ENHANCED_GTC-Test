/* panel.h - a scrolling panel of the launcher window (ui.cpp, controls.cpp).
 *
 * A child window holding a tab's controls, or a part of a tab, over the
 * window's art.  Its content is laid out in content coordinates (begin,
 * place); when it is taller than the panel, a vertical scroll bar scrolls
 * it, and so do the mouse wheel over it (wheel) and the keyboard focus
 * moving to a control out of view (show).  The panel paints the top-level
 * window's background under itself (it asks the window for it:
 * WM_PRINTCLIENT) and passes its controls' notifications (WM_COMMAND,
 * WM_DRAWITEM, WM_CTLCOLOR*, a slider's WM_HSCROLL) on to the top-level
 * window, which handles them
 * as its own controls'.  Panels nest; Tab moves through them
 * (WS_EX_CONTROLPARENT). */
#pragma once

#include <windows.h>

namespace panel {

/* A new panel in `parent' (hidden until shown). */
HWND create(HWND parent, HINSTANCE inst, int id);
bool is_panel(HWND w);

/* Lays out panel p, already in its place: its content `height' px tall,
 * `line' px a step (the scroll bar's arrows; the mouse wheel's lines).  The
 * content's width: the panel's, less the scroll bar when the content is
 * taller than the panel.  Then each child goes in with place (a child not
 * placed again stays where it was: hide it). */
int begin(HWND p, int height, int line);
/* A child of p at (x, y) of the content, w x h (px). */
void place(HWND p, HWND child, int x, int y, int w, int h);

/* Scrolls p to `pos' px of its content (held to the content). */
void scroll_to(HWND p, int pos);
/* Scrolls the panels holding the control c so that it is in view. */
void show(HWND c);
/* A WM_MOUSEWHEEL over a panel that scrolls: scrolls it (true: taken). */
bool wheel(const MSG &m);

} // namespace panel
