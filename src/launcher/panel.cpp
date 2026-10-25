/* panel.cpp - a scrolling panel of the launcher window (panel.h).
 *
 * Scrolling moves the children and draws the panel again rather than
 * scrolling its pixels (ScrollWindowEx): the art behind the controls stays
 * where it is, only the controls move over it. */
#include "panel.h"

#include <windowsx.h>

#include <algorithm>
#include <vector>

namespace panel {
namespace {

const wchar_t kClassName[] = L"GT3LauncherPanel";
ATOM g_class = 0;

/* A child's place in the content (px). */
struct Item {
    HWND child;
    int x, y, w, h;
};

/* A panel's state (its GWLP_USERDATA). */
struct Panel {
    int height = 0;         /* the content's (px) */
    int pos = 0;            /* scrolled by (px) */
    int line = 1;           /* a step (px) */
    UINT dpi = 96;          /* of height, pos and line */
    std::vector<Item> items;
};

Panel *state(HWND p)
{
    return is_panel(p) ? reinterpret_cast<Panel *>(GetWindowLongPtrW(p, GWLP_USERDATA)) : nullptr;
}

int view_height(HWND p)
{
    RECT r;
    GetClientRect(p, &r);
    return r.bottom - r.top;
}

/* The top-level window's background under panel p into dc (p's client
 * coordinates): the window draws it (WM_PRINTCLIENT) at p's place. */
void paint_background(HWND p, HDC dc)
{
    HWND root = GetAncestor(p, GA_ROOT);
    POINT o = {0, 0};
    MapWindowPoints(p, root, &o, 1);
    POINT old;
    OffsetViewportOrgEx(dc, -o.x, -o.y, &old);
    SendMessageW(root, WM_PRINTCLIENT, (WPARAM)dc, PRF_CLIENT);
    SetViewportOrgEx(dc, old.x, old.y, nullptr);
}

void scroll(HWND p, Panel &s, int pos)
{
    pos = std::clamp(pos, 0, std::max(0, s.height - view_height(p)));
    if (pos == s.pos)
        return;
    s.pos = pos;
    SCROLLINFO si = {};
    si.cbSize = sizeof(si);
    si.fMask = SIF_POS;
    si.nPos = pos;
    SetScrollInfo(p, SB_VERT, &si, TRUE);
    /* the children moved without drawing, then the panel drawn again whole */
    for (const Item &i : s.items)
        SetWindowPos(i.child, nullptr, i.x, i.y - pos, 0, 0,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOREDRAW | SWP_NOCOPYBITS);
    RedrawWindow(p, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

/* The mouse wheel turned by `delta' over p: scrolled by the system's lines
 * a notch (a page with "one screen at a time"); false when p does not
 * scroll. */
bool scroll_wheel(HWND p, int delta)
{
    Panel *s = state(p);
    if (s == nullptr || !IsWindowVisible(p) || s->height <= view_height(p))
        return false;
    UINT lines = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    const int step = lines == WHEEL_PAGESCROLL ? view_height(p) : (int)std::min(lines, 100u) * s->line;
    scroll(p, *s, s->pos - MulDiv(delta, step, WHEEL_DELTA));
    return true;
}

LRESULT CALLBACK panel_proc(HWND p, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(p, GWLP_USERDATA, (LONG_PTR) new Panel);
        break;
    case WM_NCDESTROY:
        delete reinterpret_cast<Panel *>(GetWindowLongPtrW(p, GWLP_USERDATA));
        SetWindowLongPtrW(p, GWLP_USERDATA, 0);
        break;

    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(p, &ps);
        paint_background(p, dc);
        EndPaint(p, &ps);
        return 0;
    }
    case WM_PRINTCLIENT:
        /* themed buttons draw their parent's background behind their corners */
        paint_background(p, (HDC)wp);
        return 0;

    case WM_VSCROLL: {
        Panel *s = state(p);
        if (lp != 0 || s == nullptr)
            break;
        SCROLLINFO si = {};
        si.cbSize = sizeof(si);
        si.fMask = SIF_TRACKPOS;
        GetScrollInfo(p, SB_VERT, &si);
        int pos = s->pos;
        switch (LOWORD(wp)) {
        case SB_LINEUP:
            pos -= s->line;
            break;
        case SB_LINEDOWN:
            pos += s->line;
            break;
        case SB_PAGEUP:
            pos -= std::max(s->line, view_height(p) - s->line);
            break;
        case SB_PAGEDOWN:
            pos += std::max(s->line, view_height(p) - s->line);
            break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION:
            pos = si.nTrackPos;
            break;
        case SB_TOP:
            pos = 0;
            break;
        case SB_BOTTOM:
            pos = s->height;
            break;
        }
        scroll(p, *s, pos);
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (scroll_wheel(p, GET_WHEEL_DELTA_WPARAM(wp)))
            return 0;
        break;   /* (DefWindowProc: to the parent) */
    case WM_HSCROLL:
        if (lp != 0)                        /* a slider's (the panel has no horizontal bar) */
            return SendMessageW(GetAncestor(p, GA_ROOT), msg, wp, lp);
        break;

    /* the controls' notifications: the top-level window's */
    case WM_COMMAND:
    case WM_NOTIFY:
    case WM_DRAWITEM:
    case WM_MEASUREITEM:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return SendMessageW(GetAncestor(p, GA_ROOT), msg, wp, lp);
    }
    return DefWindowProcW(p, msg, wp, lp);
}

} // namespace

HWND create(HWND parent, HINSTANCE inst, int id)
{
    if (g_class == 0) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = panel_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        g_class = RegisterClassExW(&wc);
    }
    HWND p = CreateWindowExW(WS_EX_CONTROLPARENT, kClassName, L"", WS_CHILD | WS_VSCROLL | WS_CLIPCHILDREN, 0, 0, 10,
                             10, parent, (HMENU)(INT_PTR)id, inst, nullptr);
    /* no bar until the content needs one (begin: the same empty range) */
    if (p != nullptr) {
        SCROLLINFO si = {};
        si.cbSize = sizeof(si);
        si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        si.nPage = 1;
        SetScrollInfo(p, SB_VERT, &si, FALSE);
    }
    return p;
}

bool is_panel(HWND w)
{
    return w != nullptr && g_class != 0 && (ATOM)GetClassLongPtrW(w, GCW_ATOM) == g_class &&
           GetWindowThreadProcessId(w, nullptr) == GetCurrentThreadId();
}

int begin(HWND p, int height, int line)
{
    Panel *s = state(p);
    RECT r;
    GetWindowRect(p, &r);
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (s == nullptr)
        return w;
    const UINT dpi = GetDpiForWindow(p);
    if (dpi != s->dpi) {
        s->pos = MulDiv(s->pos, (int)dpi, (int)s->dpi);
        s->dpi = dpi;
    }
    s->height = std::max(0, height);
    s->line = std::max(1, line);
    s->pos = std::clamp(s->pos, 0, std::max(0, s->height - h));
    s->items.clear();
    /* the bar shown or hidden now, so the client area is the content's
     * (a page as tall as the range hides it) */
    SCROLLINFO si = {};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = std::max(0, s->height - 1);
    si.nPage = (UINT)std::max(0, h);
    si.nPos = s->pos;
    SetScrollInfo(p, SB_VERT, &si, TRUE);
    return s->height > h ? w - GetSystemMetricsForDpi(SM_CXVSCROLL, dpi) : w;
}

void place(HWND p, HWND child, int x, int y, int w, int h)
{
    Panel *s = state(p);
    if (s == nullptr)
        return;
    s->items.push_back(Item{child, x, y, w, h});
    SetWindowPos(child, nullptr, x, y - s->pos, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

void scroll_to(HWND p, int pos)
{
    if (Panel *s = state(p))
        scroll(p, *s, pos);
}

void show(HWND c)
{
    /* every panel up from c (an inner panel is a child of the outer one) */
    for (HWND w = c; w != nullptr && (GetWindowLongW(w, GWL_STYLE) & WS_CHILD); w = GetParent(w)) {
        HWND p = GetParent(w);
        Panel *s = state(p);
        if (s == nullptr)
            continue;
        const int h = view_height(p);
        if (s->height <= h)
            continue;
        RECT r;
        GetWindowRect(w, &r);
        MapWindowPoints(nullptr, p, reinterpret_cast<POINT *>(&r), 2);
        const int margin = s->line / 6;
        if (r.top < 0)
            scroll(p, *s, s->pos + r.top - margin);
        else if (r.bottom > h)   /* (its top kept in view when it is taller than the panel) */
            scroll(p, *s, s->pos + std::min(r.bottom - h + margin, r.top - margin));
    }
}

bool wheel(const MSG &m)
{
    /* (not while a list is dropped down: it has the mouse) */
    if (m.message != WM_MOUSEWHEEL || GetCapture() != nullptr)
        return false;
    const POINT pt = {GET_X_LPARAM(m.lParam), GET_Y_LPARAM(m.lParam)};
    for (HWND w = WindowFromPoint(pt); w != nullptr && (GetWindowLongW(w, GWL_STYLE) & WS_CHILD); w = GetParent(w))
        if (scroll_wheel(w, GET_WHEEL_DELTA_WPARAM(m.wParam)))
            return true;
    return false;
}

} // namespace panel
