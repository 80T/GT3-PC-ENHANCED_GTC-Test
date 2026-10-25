/* ui.cpp - the launcher window (ui.h).
 *
 * A small fixed-size window over the launcher's art (launcher_bg.png, a
 * resource decoded by WIC and scaled to the window: draw_background): seven
 * tabs - Play, a row per version (USA, Europe, Japan: install.h) with its
 * state and a Play button, and the data-only extractions; Display,
 * Graphics, Audio, Game, every setting of gt3pc.ini the game's own settings
 * menu (F2) has, and the launcher's display ones; Controls, the control
 * mappings (controls.h); Debug, the debug console - on a darkened panel,
 * each tab a page that scrolls when it holds more than fits (panel.h), a
 * status line, a progress bar shown while installing, and the buttons
 * Install game files... / Exit.
 * Plain Win32 with the system's controls and message font, per-monitor DPI
 * aware (v2, from the manifest): every size below is in 96-DPI units and is
 * scaled to the window's DPI.  The install runs on a worker thread and
 * reports through posted messages, so the window stays responsive. */
#include "ui.h"
#include "controls.h"
#include "display.h"
#include "install.h"
#include "panel.h"
#include "resource.h"
#include "util.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <objbase.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace ui {
namespace {

const wchar_t kClassName[] = L"GT3LauncherWindow";
const wchar_t kTitle[] = L"Gran Turismo 3 A-Spec - Launcher";
const wchar_t kAppName[] = L"Gran Turismo 3 A-Spec";   /* message box captions */
const wchar_t kGameExe[] = L"gt3pc.exe";

/* Layout, in 96-DPI units. */
constexpr int kClientW = 640, kClientH = 360;
constexpr int kMargin = 16;
constexpr int kButtonH = 28, kButtonW = 100, kInstallW = 170, kButtonGap = 8;
constexpr int kStatusH = 36;    /* two lines of the message font */
constexpr int kStatusW = 440;   /* (left of the art's logo, bottom right) */
constexpr int kProgressH = 14;
/* The tabs: a row of headers over the settings' panel. */
constexpr int kTabY = 12, kTabW = 86, kTabH = 30;
constexpr int kTabs = 7;
/* The settings: a label and a drop-down list (or a slider and its value) per
 * row, on the panel (each tab's page: more rows than kViewRows scroll). */
constexpr int kRowH = 30, kLabelW = 150, kComboW = 300, kComboListH = 240;
constexpr int kSliderH = 26, kValueW = 140, kValueGap = 6;
constexpr int kPanelPad = 12;
constexpr int kViewRows = 6;
constexpr int kPanelY = kTabY + kTabH;
constexpr int kPanelW = kPanelPad + kLabelW + kComboW + kPanelPad;
constexpr int kPanelH = kPanelPad + kViewRows * kRowH + kPanelPad / 2;
/* The panel darkens the art behind the tabs and the settings, for the text. */
constexpr int kPanelShade = 150;    /* of 256: the art kept */

constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;

const wchar_t *const kTabNames[kTabs] = {L"Play", L"Display", L"Graphics", L"Audio", L"Game", L"Controls", L"Debug"};
enum { TAB_PLAY, TAB_DISPLAY, TAB_GRAPHICS, TAB_AUDIO, TAB_GAME, TAB_CONTROLS, TAB_DEBUG };

/* The Play tab: a version a row (its name and state, a Play button at the
 * right), then the data-only extractions. */
constexpr int kVersionRowH = 48, kPlayW = 90;

enum : int {
    IDC_INSTALL = 1001,
    IDC_CANCEL,
    IDC_EXIT,
    IDC_STATUS,
    IDC_PROGRESS,
    IDC_MODE,
    IDC_RES,
    IDC_RATE,
    IDC_SPAN,
    IDC_SPLIT,
    IDC_TAB0 = 1100,        /* .. + kTabs - 1 */
    IDC_PAGE0 = 1110,       /* the tabs' pages */
    IDC_VERSION_PLAY0 = 1180,   /* the Play tab's buttons, names and states, by version */
    IDC_VERSION_NAME0 = 1184,
    IDC_VERSION_STATE0 = 1188,
    IDC_EXTRACTIONS = 1192,
    IDC_OPTION0 = 1200,     /* the options' lists (or sliders), by index */
    IDC_LABEL0 = 1300,      /* the rows' labels */
                            /* 1400..1699: the Controls tab's (controls.h) */
    IDC_VALUE0 = 1700,      /* the sliders' values, by option index */
};

enum : UINT {
    WM_APP_PHASE = WM_APP + 1,   /* lParam: std::wstring* (the receiver deletes it) */
    WM_APP_PROGRESS,             /* g.done / g.total changed */
    WM_APP_DONE,                 /* the worker finished: join it, then read g.result */
};

/* An entry of an option's list: what it shows, the ini value it writes. */
struct Choice {
    std::wstring text, value;
};

/* A setting of gt3pc.ini written as one key (the game reads it at start; its
 * settings menu writes the same keys): its tab, label, key, default (the
 * game's, for a missing key) and the values offered.  A value in the file
 * that none of them stands for gets an entry of its own, so it is kept.
 * A slider instead (step > 0, no choices): min .. max by step, written with
 * `decimals', shown as text(v) beside it; a value in the file between its
 * steps or out of its range is shown as it is (the slider at the nearest)
 * until the slider moves. */
struct Option {
    int tab;
    const wchar_t *label, *section, *key, *def;
    std::vector<Choice> choices;
    HWND combo = nullptr;                   /* the list, or the slider */
    double min = 0, max = 0, step = 0;
    int decimals = 0;
    std::wstring (*text)(double v) = nullptr;
    HWND value = nullptr;                   /* the slider's value */
    double cur = 0;                         /* the slider's value in the file */
};

/* A row of a tab: its label and its list (or slider and value). */
struct Row {
    int tab;
    HWND label, combo;
    HWND value = nullptr;
};

struct Window {
    HINSTANCE inst = nullptr;
    HWND wnd = nullptr;
    HWND install = nullptr, cancel = nullptr, exit = nullptr, status = nullptr, progress = nullptr;
    HWND tabs[kTabs] = {};
    HWND pages[kTabs] = {};                 /* each tab's controls (panels: panel.h) */
    int tab = TAB_PLAY;
    /* The Play tab, by version (install::Version) */
    HWND versionPlay[install::kVersions] = {}, versionName[install::kVersions] = {},
         versionState[install::kVersions] = {};
    HWND extractionsLabel = nullptr;
    int lastVersion = 0;                    /* gt3pc.ini [Game] Version: the last played */
    std::vector<Row> rows;                  /* in tab order (the creation order: the Tab key's) */
    std::vector<Option> options;
    /* The display settings (gt3pc.ini [Display], display.h): their lists,
     * what each list entry stands for. */
    HWND mode = nullptr, res = nullptr, rate = nullptr, span = nullptr, split = nullptr;
    display::Settings disp;
    std::vector<display::Size> resSizes;    /* by entry ({0, 0}: the desktop's) */
    std::vector<int> rateValues;            /* by entry (0: the default) */
    std::vector<std::wstring> spanValues;   /* by entry: SpanMonitors (empty: off) */
    std::vector<std::wstring> splitValues;  /* by entry: SplitMonitor (empty: auto) */
    HFONT font = nullptr, tabFont = nullptr;
    HICON icon_big = nullptr, icon_small = nullptr;
    /* The background: the art at the window's size with the panel darkened
     * (bg, selected into bgDC), and a brush of it for the labels. */
    HBITMAP bg = nullptr;
    HDC bgDC = nullptr;
    HGDIOBJ bgOld = nullptr;
    HBRUSH bgBrush = nullptr;
    UINT dpi = 96;
    int default_id = 0;      /* the button Enter presses (DM_GETDEFID) */
    HWND last_focus = nullptr;
    std::wstring folder;   /* the launcher's folder: gt3pc.exe and the data folder live here */

    /* The install. */
    bool installing = false;
    bool close_when_done = false;
    std::thread worker;
    std::atomic<bool> cancel_requested{false};
    std::atomic<uint64_t> done{0}, total{0};
    std::wstring phase;
    bool copying = false;
    install::Result result = install::Result::ok;
    std::wstring result_message;
} g;

int px(int dip) { return MulDiv(dip, (int)g.dpi, 96); }

/* ---- the background ------------------------------------------------------ */

/* The art (the resource IDR_BACKGROUND) decoded and scaled to w x h into
 * top-down 32-bit pixels; false when it could not be (the window stays
 * black). */
bool load_art(int w, int h, uint32_t *pixels)
{
    HRSRC r = FindResourceW(g.inst, MAKEINTRESOURCEW(IDR_BACKGROUND), RT_RCDATA);
    HGLOBAL res = r ? LoadResource(g.inst, r) : nullptr;
    const void *data = res ? LockResource(res) : nullptr;
    const DWORD size = r ? SizeofResource(g.inst, r) : 0;
    if (data == nullptr || size == 0)
        return false;
    IWICImagingFactory *wic = nullptr;
    IWICStream *stream = nullptr;
    IWICBitmapDecoder *decoder = nullptr;
    IWICBitmapFrameDecode *frame = nullptr;
    IWICFormatConverter *conv = nullptr;
    IWICBitmapScaler *scaler = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                         IID_PPV_ARGS(&wic))) &&
              SUCCEEDED(wic->CreateStream(&stream)) &&
              SUCCEEDED(stream->InitializeFromMemory((BYTE *)data, size)) &&
              SUCCEEDED(wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
              SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
              SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeCustom)) &&
              SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
              SUCCEEDED(scaler->Initialize(conv, (UINT)w, (UINT)h, WICBitmapInterpolationModeFant)) &&
              SUCCEEDED(scaler->CopyPixels(nullptr, (UINT)w * 4, (UINT)(w * h * 4), (BYTE *)pixels));
    for (IUnknown *u : {(IUnknown *)scaler, (IUnknown *)conv, (IUnknown *)frame, (IUnknown *)decoder,
                        (IUnknown *)stream, (IUnknown *)wic})
        if (u)
            u->Release();
    return ok;
}

/* The background at the window's size: the art, the panel behind the tabs
 * and the settings darkened (kPanelShade).  Made again at each DPI. */
void build_background()
{
    const int w = px(kClientW), h = px(kClientH);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;             /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp == nullptr)
        return;
    uint32_t *p = (uint32_t *)bits;
    if (!load_art(w, h, p))
        memset(p, 0, (size_t)w * h * 4);
    const int x0 = px(kMargin), y0 = px(kTabY), x1 = x0 + px(kPanelW), y1 = px(kPanelY) + px(kPanelH);
    for (int y = std::max(0, y0); y < std::min(h, y1); y++) {
        for (int x = std::max(0, x0); x < std::min(w, x1); x++) {
            const uint32_t c = p[(size_t)y * w + x];
            const uint32_t r = ((c >> 16) & 0xFF) * kPanelShade >> 8, gr = ((c >> 8) & 0xFF) * kPanelShade >> 8,
                           b = (c & 0xFF) * kPanelShade >> 8;
            p[(size_t)y * w + x] = 0xFF000000u | (r << 16) | (gr << 8) | b;
        }
    }
    if (g.bgDC == nullptr)
        g.bgDC = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(g.bgDC, bmp);
    if (g.bg != nullptr)
        DeleteObject(g.bg);
    else
        g.bgOld = old;
    g.bg = bmp;
    if (g.bgBrush != nullptr)
        DeleteObject(g.bgBrush);
    g.bgBrush = CreatePatternBrush(bmp);
}

/* Draws the window's background into `dc` over `client`. */
void draw_background(HDC dc, const RECT &client)
{
    if (g.bgDC == nullptr) {
        FillRect(dc, &client, (HBRUSH)GetStockObject(BLACK_BRUSH));
        return;
    }
    BitBlt(dc, client.left, client.top, client.right - client.left, client.bottom - client.top, g.bgDC, client.left,
           client.top, SRCCOPY);
}

/* A child's place in the client area (the background under it). */
POINT child_origin(HWND c)
{
    POINT o = {0, 0};
    MapWindowPoints(c, g.wnd, &o, 1);
    return o;
}

/* A label or the status line: its text drawn over the background. */
bool is_text(HWND c)
{
    if (c == g.status || c == g.extractionsLabel || controls::is_text(c))
        return true;
    for (int v = 0; v < install::kVersions; v++)
        if (c == g.versionName[v] || c == g.versionState[v])
            return true;
    /* (a slider takes the same brush for its background) */
    return std::any_of(g.rows.begin(), g.rows.end(), [&](const Row &r) {
        return r.label == c || (r.value != nullptr && (r.value == c || r.combo == c));
    });
}

/* A tab's header: its name over the background, the chosen one bright and
 * underlined in red. */
void draw_tab(const DRAWITEMSTRUCT *d)
{
    const int i = (int)d->CtlID - IDC_TAB0;
    const RECT &r = d->rcItem;
    const POINT o = child_origin(d->hwndItem);
    if (g.bgDC != nullptr)
        BitBlt(d->hDC, 0, 0, r.right - r.left, r.bottom - r.top, g.bgDC, o.x, o.y, SRCCOPY);
    const bool chosen = i == g.tab;
    if (chosen) {
        RECT bar = {r.left + px(10), r.bottom - px(4), r.right - px(10), r.bottom - px(1)};
        HBRUSH red = CreateSolidBrush(RGB(214, 22, 22));
        FillRect(d->hDC, &bar, red);
        DeleteObject(red);
    }
    SetBkMode(d->hDC, TRANSPARENT);
    SetTextColor(d->hDC, chosen ? RGB(255, 255, 255) : RGB(168, 168, 168));
    HGDIOBJ old = SelectObject(d->hDC, g.tabFont ? g.tabFont : g.font);
    RECT t = r;
    t.bottom -= px(3);
    DrawTextW(d->hDC, kTabNames[i], -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(d->hDC, old);
    if ((d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT)) {
        RECT f = r;
        InflateRect(&f, -px(3), -px(3));
        DrawFocusRect(d->hDC, &f);
    }
}

/* ---- state ------------------------------------------------------------------ */

install::InstallState version_state(int v)
{
    return install::install_state(g.folder, install::version_folder((install::Version)v));
}

/* The status line, two lines at most: what to do. */
std::wstring state_text()
{
    for (int v = 0; v < install::kVersions; v++)
        if (version_state(v) == install::InstallState::ok)
            return L"Choose a version to play in the Play tab.\n"
                   L"\"Install game files...\" installs another version from its disc image.";
    if (install::has_volume(g.folder))
        return L"The game files need unpacking from gt3.vol (installed by an earlier version).\n"
               L"Choose \"Install game files...\" to unpack them.";
    return L"No version is installed.\n"
           L"Choose \"Install game files...\" and pick a Gran Turismo 3 disc image (USA, Europe or Japan).";
}

/* A version's state in the Play tab. */
std::wstring version_text(int v)
{
    const std::wstring data = install::version_folder((install::Version)v);
    switch (install::install_state(g.folder, data)) {
    case install::InstallState::ok:
        return L"Installed from " + install::installed_release(g.folder, data) + L".";
    case install::InstallState::damaged:
        return L"Incomplete (" + data + L"): install it again from its disc image.";
    default:
        return L"Not installed: \"Install game files...\" with its disc image.";
    }
}

/* The data-only extractions, as the Play tab lists them ("" with none). */
std::wstring extractions_text(int *lines)
{
    std::wstring t;
    *lines = 0;
    for (const install::Extraction &e : install::extractions(g.folder)) {
        t += (t.empty() ? L"Data only, not playable:" : L"") + std::wstring(L"\n  ") + e.release + L" (" + e.data + L")";
        *lines += *lines == 0 ? 2 : 1;
    }
    return t;
}

/* The chosen tab's page shown, the others hidden (the focus, if it was in
 * one of them, to the tab's header); the Controls tab told (its devices are
 * read while it is shown). */
void show_tab(int t)
{
    g.tab = t;
    HWND f = GetFocus();
    bool lost = false;
    for (int i = 0; i < kTabs; i++) {
        if (i != t && f != nullptr && IsChild(g.pages[i], f))
            lost = true;
        ShowWindow(g.pages[i], i == t ? SW_SHOW : SW_HIDE);
    }
    controls::show(t == TAB_CONTROLS);
    for (HWND h : g.tabs)
        InvalidateRect(h, nullptr, FALSE);
    if (lost)
        SetFocus(g.tabs[t]);
}

void layout()
{
    int w = px(kClientW), h = px(kClientH), m = px(kMargin);
    (void)w;
    for (int i = 0; i < kTabs; i++)
        SetWindowPos(g.tabs[i], nullptr, m + i * px(kTabW), px(kTabY), px(kTabW), px(kTabH),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    /* the pages over the panel below the headers; each tab's rows from its
     * page's top, the lists narrower by the scroll bar when there is one */
    const int pageY = px(kPanelY) + px(kPanelPad) / 2;
    for (HWND p : g.pages)
        SetWindowPos(p, nullptr, m, pageY, px(kPanelW), px(kPanelY) + px(kPanelH) - pageY, SWP_NOZORDER | SWP_NOACTIVATE);
    int count[kTabs] = {};
    for (const Row &r : g.rows)
        count[r.tab]++;
    int cw[kTabs] = {}, index[kTabs] = {};
    for (int i = 0; i < kTabs; i++)
        if (i != TAB_CONTROLS && i != TAB_PLAY)
            cw[i] = panel::begin(g.pages[i], px(kPanelPad) / 2 + count[i] * px(kRowH), px(kRowH));
    {
        /* the Play tab: a version a row - its name, its state under it, the
         * Play button at the right - then the extractions */
        HWND p = g.pages[TAB_PLAY];
        int lines = 0;
        extractions_text(&lines);
        const int top = px(kPanelPad) / 2, lineH = px(18);
        const int extraY = top + install::kVersions * px(kVersionRowH);
        const int pw = panel::begin(p, extraY + lines * lineH + top, px(kRowH));
        const int x = px(kPanelPad), bw = px(kPlayW);
        const int tw = pw - x - bw - px(kPanelPad) - px(8);
        for (int v = 0; v < install::kVersions; v++) {
            const int y = top + v * px(kVersionRowH);
            panel::place(p, g.versionName[v], x, y + px(2), tw, px(22));
            panel::place(p, g.versionState[v], x, y + px(24), tw, px(20));
            panel::place(p, g.versionPlay[v], pw - px(kPanelPad) - bw, y + px(8), bw, px(kButtonH));
        }
        panel::place(p, g.extractionsLabel, x, extraY, pw - 2 * x, std::max(1, lines) * lineH);
    }
    for (const Row &r : g.rows) {
        int y = px(kPanelPad) / 2 + index[r.tab]++ * px(kRowH);
        int x = px(kPanelPad);
        const int w = std::min(px(kComboW), cw[r.tab] - x - px(kLabelW) - px(kPanelPad) / 2);
        panel::place(g.pages[r.tab], r.label, x, y + px(4), px(kLabelW), px(20));
        if (r.value != nullptr) {
            /* the slider, its value right of it */
            const int vw = std::min(px(kValueW), w / 2);
            panel::place(g.pages[r.tab], r.combo, x + px(kLabelW), y, w - vw - px(kValueGap), px(kSliderH));
            panel::place(g.pages[r.tab], r.value, x + px(kLabelW) + w - vw, y + px(4), vw, px(20));
        } else {
            panel::place(g.pages[r.tab], r.combo, x + px(kLabelW), y, w, px(kComboListH));
        }
    }
    controls::layout(g.dpi);
    int by = h - m - px(kButtonH);
    int bh = px(kButtonH);
    SetWindowPos(g.install, nullptr, m, by, px(kInstallW), bh, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(g.cancel, nullptr, m, by, px(kButtonW), bh, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(g.exit, nullptr, px(kClientW) - m - px(kButtonW), by, px(kButtonW), bh,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    /* While installing the progress bar sits above the buttons and the
     * status line above it; otherwise the status line comes down.  Both left
     * of the art's logo. */
    int py = by - px(12) - px(kProgressH);
    SetWindowPos(g.progress, nullptr, m, py, px(kStatusW), px(kProgressH), SWP_NOZORDER | SWP_NOACTIVATE);
    int sy = g.installing ? py - px(6) - px(kStatusH) : by - px(12) - px(kStatusH);
    SetWindowPos(g.status, nullptr, m, sy, px(kStatusW), px(kStatusH), SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(g.wnd, nullptr, TRUE);
}

void set_status(const std::wstring &text) { SetWindowTextW(g.status, text.c_str()); }

void show_progress_text()
{
    std::wstring s = g.phase;
    uint64_t done = g.done, total = g.total;
    if (g.copying && total) {
        wchar_t pct[16];
        swprintf(pct, 16, L"%u%%", (unsigned)(done * 100 / total));
        s += L"  " + util::format_size(done) + L" of " + util::format_size(total) + L" (" + pct + L")";
        SendMessageW(g.progress, PBM_SETPOS, (WPARAM)(done * 1000 / total), 0);
    }
    set_status(s);
}

/* The version Enter plays: the last played when it is installed, else the
 * first installed; -1 with none. */
int default_version()
{
    int def = -1;
    for (int v = 0; v < install::kVersions; v++)
        if (version_state(v) == install::InstallState::ok && (def < 0 || v == g.lastVersion))
            def = v;
    return def;
}

HWND default_button()
{
    const int dv = default_version();
    return g.installing ? g.cancel : dv >= 0 ? g.versionPlay[dv] : g.install;
}

/* Updates the controls for the installed state, or for a running install. */
void refresh(const std::wstring &note = std::wstring())
{
    for (int v = 0; v < install::kVersions; v++) {
        SetWindowTextW(g.versionState[v], version_text(v).c_str());
        EnableWindow(g.versionPlay[v], !g.installing && version_state(v) == install::InstallState::ok);
    }
    int lines = 0;
    SetWindowTextW(g.extractionsLabel, extractions_text(&lines).c_str());
    ShowWindow(g.install, g.installing ? SW_HIDE : SW_SHOW);
    EnableWindow(g.install, !g.installing);
    ShowWindow(g.cancel, g.installing ? SW_SHOW : SW_HIDE);
    EnableWindow(g.cancel, g.installing);
    ShowWindow(g.progress, g.installing ? SW_SHOW : SW_HIDE);
    /* Enter plays the default version when one is installed, else installs. */
    const int dv = default_version();
    HWND def = g.installing ? g.cancel : dv >= 0 ? g.versionPlay[dv] : g.install;
    g.default_id = GetDlgCtrlID(def);
    std::vector<HWND> buttons = {g.install, g.cancel, g.exit};
    buttons.insert(buttons.end(), std::begin(g.versionPlay), std::end(g.versionPlay));
    for (HWND b : buttons)
        SendMessageW(b, BM_SETSTYLE, b == def ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON, TRUE);
    if (!g.installing)
        set_status(note.empty() ? state_text() : note + L"\n" + state_text().substr(0, state_text().find(L'\n')));
    layout();
}

/* ---- DPI ------------------------------------------------------------------------ */

/* Widens a list's drop-down to its longest entry in its font (the monitor
 * lists' entries can be longer than the list is wide). */
void fit_list(HWND c)
{
    HDC dc = GetDC(c);
    HGDIOBJ old = SelectObject(dc, (HFONT)SendMessageW(c, WM_GETFONT, 0, 0));
    int w = 0;
    int n = (int)SendMessageW(c, CB_GETCOUNT, 0, 0);
    for (int i = 0; i < n; i++) {
        std::wstring s((size_t)SendMessageW(c, CB_GETLBTEXTLEN, i, 0) + 1, L'\0');
        int len = (int)SendMessageW(c, CB_GETLBTEXT, i, (LPARAM)&s[0]);
        SIZE size = {};
        if (len > 0 && GetTextExtentPoint32W(dc, s.c_str(), len, &size) && size.cx > w)
            w = size.cx;
    }
    if (old)
        SelectObject(dc, old);
    ReleaseDC(c, dc);
    /* (never narrower than the list itself) */
    SendMessageW(c, CB_SETDROPPEDWIDTH, w + px(12) + GetSystemMetricsForDpi(SM_CXVSCROLL, g.dpi), 0);
}

void apply_dpi(UINT dpi, const RECT *suggested)
{
    g.dpi = dpi;

    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    HFONT font = nullptr, tabFont = nullptr;
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi)) {
        font = CreateFontIndirectW(&ncm.lfMessageFont);
        LOGFONTW lf = ncm.lfMessageFont;
        lf.lfWeight = FW_SEMIBOLD;
        lf.lfHeight = lf.lfHeight * 6 / 5;
        tabFont = CreateFontIndirectW(&lf);
    }
    std::vector<HWND> all = {g.install, g.cancel, g.exit, g.status, g.progress, g.extractionsLabel};
    for (int v = 0; v < install::kVersions; v++) {
        all.push_back(g.versionPlay[v]);
        all.push_back(g.versionState[v]);
    }
    for (const Row &r : g.rows) {
        all.push_back(r.label);
        all.push_back(r.combo);
        if (r.value != nullptr)
            all.push_back(r.value);
    }
    for (HWND c : all)
        SendMessageW(c, WM_SETFONT, (WPARAM)font, FALSE);
    if (g.font)
        DeleteObject(g.font);
    if (g.tabFont)
        DeleteObject(g.tabFont);
    g.font = font;
    g.tabFont = tabFont;
    for (HWND c : g.versionName)
        SendMessageW(c, WM_SETFONT, (WPARAM)(tabFont ? tabFont : font), FALSE);
    controls::set_font(font, tabFont);
    for (const Row &r : g.rows)
        if (r.value == nullptr)
            fit_list(r.combo);

    HICON icon_big = nullptr, icon_small = nullptr;
    int cb = GetSystemMetricsForDpi(SM_CXICON, dpi), cs = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
    LoadIconWithScaleDown(g.inst, MAKEINTRESOURCEW(IDI_GT3LAUNCHER), cb, cb, &icon_big);
    LoadIconWithScaleDown(g.inst, MAKEINTRESOURCEW(IDI_GT3LAUNCHER), cs, cs, &icon_small);
    SendMessageW(g.wnd, WM_SETICON, ICON_BIG, (LPARAM)icon_big);
    SendMessageW(g.wnd, WM_SETICON, ICON_SMALL, (LPARAM)icon_small);
    if (g.icon_big)
        DestroyIcon(g.icon_big);
    if (g.icon_small)
        DestroyIcon(g.icon_small);
    g.icon_big = icon_big;
    g.icon_small = icon_small;

    build_background();

    /* The fixed client size at this DPI. */
    RECT r = {0, 0, px(kClientW), px(kClientH)};
    AdjustWindowRectExForDpi(&r, kStyle, FALSE, 0, dpi);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    if (suggested) {
        SetWindowPos(g.wnd, nullptr, suggested->left, suggested->top, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        /* First showing: centred in the work area of its monitor. */
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromWindow(g.wnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        int x = (mi.rcWork.left + mi.rcWork.right - ww) / 2, y = (mi.rcWork.top + mi.rcWork.bottom - wh) / 2;
        SetWindowPos(g.wnd, nullptr, x, y, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    layout();
    RedrawWindow(g.wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

/* ---- actions --------------------------------------------------------------- */

void message(const std::wstring &text, UINT icon) { MessageBoxW(g.wnd, text.c_str(), kAppName, MB_OK | icon); }

/* ---- the settings (gt3pc.ini) ---------------------------------------------- */

std::wstring ini_path() { return g.folder + L"gt3pc.ini"; }
std::wstring size_text(int w, int h) { return std::to_wstring(w) + L" x " + std::to_wstring(h); }
void combo_reset(HWND c) { SendMessageW(c, CB_RESETCONTENT, 0, 0); }
void combo_add(HWND c, const std::wstring &s) { SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)s.c_str()); }
int combo_sel(HWND c) { return (int)SendMessageW(c, CB_GETCURSEL, 0, 0); }

/* The refresh rates of the fullscreen resolution chosen (the default: the
 * nearest to 60 Hz, the game's rate); none in the other modes. */
void fill_rates()
{
    combo_reset(g.rate);
    g.rateValues.clear();
    if (g.disp.mode != display::Mode::fullscreen) {
        combo_add(g.rate, g.disp.mode == display::Mode::borderless ? L"The desktop's" : L"-");
        SendMessageW(g.rate, CB_SETCURSEL, 0, 0);
        EnableWindow(g.rate, FALSE);
        return;
    }
    display::Size s = g.disp.width > 0 ? display::Size{g.disp.width, g.disp.height} : display::desktop_size();
    combo_add(g.rate, L"Default (60 Hz)");
    g.rateValues.push_back(0);
    int sel = 0;
    for (int r : display::refresh_rates(s.w, s.h)) {
        if (r == g.disp.refresh)
            sel = (int)g.rateValues.size();
        combo_add(g.rate, std::to_wstring(r) + L" Hz");
        g.rateValues.push_back(r);
    }
    g.disp.refresh = g.rateValues[sel];
    SendMessageW(g.rate, CB_SETCURSEL, sel, 0);
    EnableWindow(g.rate, TRUE);
}

/* The resolutions of the mode chosen: fullscreen, the monitor's (the
 * desktop's first); windowed, 4:3 window sizes that fit the screen (and the
 * window's own size when it is none of them); borderless, the desktop's. */
void fill_resolutions()
{
    combo_reset(g.res);
    g.resSizes.clear();
    const display::Size desk = display::desktop_size();
    int sel = 0;
    if (g.disp.mode == display::Mode::fullscreen) {
        combo_add(g.res, L"Desktop (" + size_text(desk.w, desk.h) + L")");
        g.resSizes.push_back(display::Size{0, 0});
        for (const display::Size &s : display::fullscreen_sizes()) {
            if (s.w == g.disp.width && s.h == g.disp.height)
                sel = (int)g.resSizes.size();
            combo_add(g.res, size_text(s.w, s.h));
            g.resSizes.push_back(s);
        }
        if (sel == 0)
            g.disp.width = g.disp.height = 0;
    } else if (g.disp.mode == display::Mode::windowed) {
        bool found = false;
        for (const display::Size &s : display::window_sizes()) {
            if (s.w == g.disp.windowW && s.h == g.disp.windowH) {
                sel = (int)g.resSizes.size();
                found = true;
            }
            combo_add(g.res, size_text(s.w, s.h));
            g.resSizes.push_back(s);
        }
        if (!found) {
            sel = (int)g.resSizes.size();
            combo_add(g.res, size_text(g.disp.windowW, g.disp.windowH) + L" (the window's size)");
            g.resSizes.push_back(display::Size{g.disp.windowW, g.disp.windowH});
        }
    } else {
        combo_add(g.res, L"Desktop (" + size_text(desk.w, desk.h) + L")");
        g.resSizes.push_back(display::Size{0, 0});
    }
    SendMessageW(g.res, CB_SETCURSEL, sel, 0);
    EnableWindow(g.res, g.disp.mode != display::Mode::borderless);
    fill_rates();
}

/* "DISPLAY2" of the device name \\.\DISPLAY2. */
std::wstring device_text(const std::wstring &device)
{
    size_t i = device.rfind(L'\\');
    return i == std::wstring::npos ? device : device.substr(i + 1);
}

/* The monitor lists: the runs of monitors side by side that one picture can
 * span (borderless and fullscreen only; display::spans), and the split
 * screen's second monitor.  The runs name their monitors by their friendly
 * names when every monitor has one of its own (two monitors of a kind would
 * read the same), else DISPLAYn.  A saved choice none of the entries stands
 * for gets an entry of its own, marked, so it is not lost. */
void fill_monitors()
{
    const std::vector<display::Monitor> mons = display::monitors();
    bool named = true;
    for (size_t i = 0; i < mons.size(); i++) {
        named = named && !mons[i].name.empty();
        for (size_t j = 0; j < i; j++)
            named = named && _wcsicmp(mons[i].name.c_str(), mons[j].name.c_str()) != 0;
    }
    auto connected = [&](const std::wstring &device) {
        return std::any_of(mons.begin(), mons.end(),
                           [&](const display::Monitor &m) { return _wcsicmp(m.device.c_str(), device.c_str()) == 0; });
    };

    combo_reset(g.span);
    g.spanValues.assign(1, std::wstring());
    combo_add(g.span, L"Off");
    int sel = 0;
    for (const display::Span &s : display::spans(mons)) {
        std::wstring text;
        for (int i : s.monitors)
            text += (text.empty() ? L"" : L" + ") + (named ? mons[i].name : device_text(mons[i].device));
        if (display::same_devices(s.value, g.disp.spanMonitors))
            sel = (int)g.spanValues.size();
        combo_add(g.span, text + L" (" + size_text(s.w, s.h) + L")");
        g.spanValues.push_back(s.value);
    }
    const std::vector<std::wstring> saved = display::device_list(g.disp.spanMonitors);
    if (sel == 0 && !saved.empty()) {
        /* (connected but no longer side by side, or one monitor only) */
        std::wstring text;
        bool all = true;
        for (const std::wstring &d : saved) {
            text += (text.empty() ? L"" : L" + ") + device_text(d);
            all = all && connected(d);
        }
        sel = (int)g.spanValues.size();
        combo_add(g.span, text + (all ? L" (not side by side)" : L" (not connected)"));
        g.spanValues.push_back(g.disp.spanMonitors);
    }
    SendMessageW(g.span, CB_SETCURSEL, sel, 0);
    EnableWindow(g.span, g.disp.mode != display::Mode::windowed);

    combo_reset(g.split);
    g.splitValues.assign(1, std::wstring());
    combo_add(g.split, L"Automatic");
    sel = 0;
    for (const display::Monitor &m : mons) {
        if (_wcsicmp(m.device.c_str(), g.disp.splitMonitor.c_str()) == 0)
            sel = (int)g.splitValues.size();
        combo_add(g.split, device_text(m.device) + (m.name.empty() ? L"" : L": " + m.name) + L" (" +
                               size_text(m.w, m.h) + L")");
        g.splitValues.push_back(m.device);
    }
    if (sel == 0 && !g.disp.splitMonitor.empty()) {
        sel = (int)g.splitValues.size();
        combo_add(g.split, device_text(g.disp.splitMonitor) + L" (not connected)");
        g.splitValues.push_back(g.disp.splitMonitor);
    }
    SendMessageW(g.split, CB_SETCURSEL, sel, 0);
}

/* ---- the game's other settings (gt3pc.ini [Graphics], [Audio],
 * HudMaxAspect; the settings menu's, src/platform/settings.h) ---------------- */

std::wstring fmt(const wchar_t *f, double v)
{
    wchar_t s[64];
    swprintf(s, 64, f, v);
    return s;
}

/* The sliders' values shown. */
std::wstring percent_text(double v) { return fmt(L"%g%%", v); }
std::wstring scale_text(double v) { return percent_text(v) + (v > 100.0 ? L" (supersampling)" : L""); }
std::wstring safe_zone_text(double v) { return percent_text(v); }
std::wstring ms_text(double v) { return fmt(L"%.1f ms", v); }

/* The languages (src/platform/language.h): their country codes (the menus'
 * folders), names and ini values. */
struct Language {
    const char *code;
    const wchar_t *name, *value;
};
const Language kLanguages[] = {
    {"JP", L"Japanese", L"japanese"}, {"US", L"English (US)", L"english_us"}, {"BR", L"English (UK)", L"english_uk"},
    {"FR", L"French", L"french"},     {"GR", L"German", L"german"},           {"IT", L"Italian", L"italian"},
    {"SP", L"Spanish", L"spanish"},
};

/* The language's list: automatic (each version's own) and the languages
 * whose menus an installed version has (every language without an install;
 * a version without the chosen one plays its own: platform/language.h).
 * Built again after an install (install_done). */
std::vector<Choice> language_choices()
{
    bool any = false, has[sizeof(kLanguages) / sizeof(kLanguages[0])] = {};
    for (int v = 0; v < install::kVersions; v++) {
        const std::wstring data = install::version_folder((install::Version)v);
        if (install::install_state(g.folder, data) != install::InstallState::ok)
            continue;
        any = true;
        for (size_t i = 0; i < sizeof(kLanguages) / sizeof(kLanguages[0]); i++)
            has[i] = has[i] || install::language_installed(g.folder, data, kLanguages[i].code);
    }
    std::vector<Choice> c = {{L"Automatic (each version's own)", L"auto"}};
    for (size_t i = 0; i < sizeof(kLanguages) / sizeof(kLanguages[0]); i++)
        if (!any || has[i])
            c.push_back({kLanguages[i].name, kLanguages[i].value});
    return c;
}

/* The options, in their tabs' order (their rows are placed among the
 * display ones: wnd_proc's WM_CREATE). */
void build_options()
{
    auto add = [](int tab, const wchar_t *label, const wchar_t *section, const wchar_t *key, const wchar_t *def,
                  std::vector<Choice> choices) {
        Option o;
        o.tab = tab;
        o.label = label;
        o.section = section;
        o.key = key;
        o.def = def;
        o.choices = std::move(choices);
        g.options.push_back(std::move(o));
    };
    auto add_slider = [](int tab, const wchar_t *label, const wchar_t *section, const wchar_t *key, const wchar_t *def,
                         double min, double max, double step, int decimals, std::wstring (*text)(double)) {
        Option o;
        o.tab = tab;
        o.label = label;
        o.section = section;
        o.key = key;
        o.def = def;
        o.min = min;
        o.max = max;
        o.step = step;
        o.decimals = decimals;
        o.text = text;
        g.options.push_back(std::move(o));
    };
    /* Display */
    add(TAB_DISPLAY, L"Vertical sync", L"Graphics", L"VSync", L"off",
        {{L"Off", L"off"}, {L"On, 1 frame queued", L"1"}, {L"On, 2 frames queued", L"2"}, {L"On, 3 frames queued", L"3"}});
    {
        std::vector<Choice> c = {{L"Off", L"0"}};
        for (double r : {30, 40, 45, 48, 50, 60, 72, 75, 90, 100, 120, 144, 165, 170, 175, 180, 200, 240, 280, 300, 360,
                         480, 500})
            c.push_back({fmt(L"%g FPS", r), fmt(L"%.3f", 1000.0 / r)});
        add(TAB_DISPLAY, L"Frame rate cap", L"Graphics", L"FrameCap", L"0", std::move(c));
    }
    add(TAB_DISPLAY, L"Menu aspect", L"Display", L"MenuAspect", L"original",
        {{L"Original (4:3)", L"original"}, {L"Stretch", L"stretch"}});
    /* Graphics */
    add(TAB_GRAPHICS, L"Anti-aliasing", L"Graphics", L"AntiAliasing", L"off",
        {{L"Off", L"off"}, {L"FXAA", L"fxaa"}, {L"MSAA 2x", L"msaa2"}, {L"MSAA 4x", L"msaa4"}, {L"MSAA 8x", L"msaa8"}});
    /* (the sliders' ranges: settings.h's kRenderScaleMin .. kRenderScaleMax,
     * kRenderDistanceMin .., kFovMin .., kHudSafeZoneMin .., kAudioBufferMin ..) */
    add_slider(TAB_GRAPHICS, L"Resolution scale", L"Graphics", L"RenderScale", L"100", 10, 400, 5, 0, scale_text);
    add(TAB_GRAPHICS, L"Levels of detail", L"Graphics", L"Lod", L"on",
        {{L"Normal", L"on"}, {L"Always best", L"off"}});
    add_slider(TAB_GRAPHICS, L"Render distance", L"Graphics", L"RenderDistance", L"100", 100, 400, 5, 0,
               percent_text);
    add_slider(TAB_GRAPHICS, L"Field of view", L"Graphics", L"Fov", L"100", 50, 200, 1, 0, percent_text);
    add_slider(TAB_GRAPHICS, L"Replay field of view", L"Graphics", L"ReplayFov", L"100", 50, 200, 1, 0, percent_text);
    add(TAB_GRAPHICS, L"Rear view (bumper cam)", L"Graphics", L"RearViewBumper", L"on", {{L"Off", L"off"}, {L"On", L"on"}});
    add(TAB_GRAPHICS, L"Rear view (chase cam)", L"Graphics", L"RearViewChase", L"off", {{L"Off", L"off"}, {L"On", L"on"}});
    /* Audio */
    add(TAB_AUDIO, L"Audio output", L"Audio", L"Output", L"auto",
        {{L"Automatic", L"auto"}, {L"Kernel streaming (lowest latency)", L"ks"}, {L"Windows WASAPI", L"wasapi"}});
    add_slider(TAB_AUDIO, L"Audio buffer", L"Audio", L"Buffer", L"2", 1.0, 20.0, 0.1, 1, ms_text);
    /* Game */
    add(TAB_GAME, L"Language", L"Game", L"Language", L"auto", language_choices());
    add(TAB_GAME, L"Speed unit", L"Game", L"SpeedUnit", L"auto",
        {{L"Default", L"auto"},
         {L"km/h, km", L"kmh"},
         {L"mph, miles", L"mph"}});
    add(TAB_GAME, L"Power unit", L"Game", L"PowerUnit", L"auto",
        {{L"Default", L"auto"},
         {L"PS", L"ps"},
         {L"HP", L"hp"},
         {L"BHP", L"bhp"},
         {L"CV", L"cv"},
         {L"kW", L"kw"}});
    add(TAB_GAME, L"Torque unit", L"Game", L"TorqueUnit", L"auto",
        {{L"Default", L"auto"},
         {L"kgf.m", L"kgfm"},
         {L"ft.lb", L"ftlb"},
         {L"Nm", L"nm"}});
    add(TAB_GAME, L"Starting credits", L"Game", L"StartingCredits", L"auto",
        {{L"Default", L"auto"},
         {L"15,000", L"15000"},
         {L"18,000", L"18000"}});
    add(TAB_GAME, L"Split screen", L"Graphics", L"SplitScreen", L"horizontal",
        {{L"Horizontal", L"horizontal"},
         {L"Vertical", L"vertical"},
         {L"Two monitors", L"monitors"}});
    add(TAB_GAME, L"Split screen HUD", L"Graphics", L"SplitHud", L"digital",
        {{L"Digital", L"digital"}, {L"Analog", L"analog"}});
    add(TAB_GAME, L"Single player HUD", L"Graphics", L"SingleHud", L"analog",
        {{L"Analog", L"analog"}, {L"Digital", L"digital"}});
    add(TAB_GAME, L"Race HUD max width", L"Display", L"HudMaxAspect", L"21:9",
        {{L"4:3", L"4:3"}, {L"16:10", L"16:10"}, {L"16:9", L"16:9"}, {L"21:9", L"21:9"}, {L"32:9", L"32:9"}});
    add_slider(TAB_GAME, L"HUD safe zone", L"Graphics", L"HudSafeZone", L"100", 75, 100, 1, 0, safe_zone_text);
    /* Debug */
    add(TAB_DEBUG, L"Debug console", L"Debug", L"Console", L"off",
        {{L"Off", L"off"}, {L"On", L"on"}});
    add(TAB_DEBUG, L"Log file loads", L"Debug", L"FileLog", L"off",
        {{L"Off", L"off"}, {L"On", L"on"}});
    add(TAB_DEBUG, L"Log D3D syncs", L"Debug", L"SyncLog", L"off",
        {{L"Off", L"off"}, {L"On", L"on"}});
    add(TAB_DEBUG, L"Fill garage (New game)", L"Debug", L"FillGarage", L"off",
        {{L"Off", L"off"}, {L"On", L"on"}});
    add(TAB_DEBUG, L"Fill money (New game)", L"Debug", L"FillMoney", L"off",
        {{L"Off", L"off"}, {L"On", L"on"}});
    add(TAB_DEBUG, L"Unlock arcade tracks", L"Debug", L"UnlockArcade", L"off",
        {{L"Off", L"off"}, {L"On", L"on"}});
    add(TAB_DEBUG, L"Unlock all licenses", L"Debug", L"UnlockLicenses", L"off",
        {{L"Off", L"off"}, {L"On", L"on"}});
}

/* A number of an ini value: "12.5", or "W:H" as W / H. */
bool parse_number(const std::wstring &s, double *v)
{
    const wchar_t *p = s.c_str();
    wchar_t *end = nullptr;
    double a = wcstod(p, &end);
    if (end == p)
        return false;
    if (*end == L':') {
        const wchar_t *q = end + 1;
        double b = wcstod(q, &end);
        if (end == q || b == 0.0)
            return false;
        a /= b;
    }
    while (*end == L' ')
        end++;
    if (*end != 0)
        return false;
    *v = a;
    return true;
}

/* Two ini values meaning the same: numbers within 0.15 % (the frame cap's
 * ms are rounded to 0.001), else the same text in any case. */
bool same_value(const std::wstring &a, const std::wstring &b)
{
    double x, y;
    if (parse_number(a, &x) && parse_number(b, &y))
        return std::fabs(x - y) <= 0.0015 * std::max(1.0, std::fabs(y));
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

/* A slider's positions: 0 .. slider_count, value min + position * step. */
int slider_count(const Option &o) { return (int)std::lround((o.max - o.min) / o.step); }
double slider_value(const Option &o, int pos)
{
    const double v = o.min + std::clamp(pos, 0, slider_count(o)) * o.step;
    return o.decimals > 0 ? std::round(v * 10.0) / 10.0 : std::round(v);
}

void fill_option(Option &o)
{
    wchar_t buf[128] = L"";
    GetPrivateProfileStringW(o.section, o.key, o.def, buf, 128, ini_path().c_str());
    const std::wstring cur = buf;
    if (o.step > 0) {
        /* the file's value as it is, the slider at the nearest step */
        double v = 0.0;
        if (!parse_number(cur, &v) && !parse_number(o.def, &v))
            v = o.min;
        o.cur = v;
        SendMessageW(o.combo, TBM_SETRANGEMIN, FALSE, 0);
        SendMessageW(o.combo, TBM_SETRANGEMAX, FALSE, slider_count(o));
        SendMessageW(o.combo, TBM_SETLINESIZE, 0, 1);
        SendMessageW(o.combo, TBM_SETPAGESIZE, 0, 5);
        SendMessageW(o.combo, TBM_SETPOS, TRUE,
                     std::clamp((int)std::lround((v - o.min) / o.step), 0, slider_count(o)));
        SetWindowTextW(o.value, o.text(v).c_str());
        return;
    }
    combo_reset(o.combo);
    int sel = -1;
    for (size_t i = 0; i < o.choices.size(); i++) {
        combo_add(o.combo, o.choices[i].text);
        if (sel < 0 && same_value(cur, o.choices[i].value))
            sel = (int)i;
    }
    /* a value none of the entries stands for, kept */
    if (sel < 0) {
        o.choices.push_back({cur + L" (as set)", cur});
        combo_add(o.combo, o.choices.back().text);
        sel = (int)o.choices.size() - 1;
    }
    SendMessageW(o.combo, CB_SETCURSEL, sel < 0 ? 0 : sel, 0);
}

void load_settings()
{
    g.disp = display::load(ini_path());
    combo_reset(g.mode);
    combo_add(g.mode, L"Windowed");             /* in display::Mode's order */
    combo_add(g.mode, L"Borderless window");
    combo_add(g.mode, L"Fullscreen");
    SendMessageW(g.mode, CB_SETCURSEL, (WPARAM)g.disp.mode, 0);
    fill_resolutions();
    fill_monitors();
    for (Option &o : g.options)
        fill_option(o);
}

/* A slider moved (WM_HSCROLL's code): its value shown, and written to
 * gt3pc.ini once it is let go (not while its thumb is dragged). */
void slider_moved(int id, int code)
{
    if (id < IDC_OPTION0 || id >= IDC_OPTION0 + (int)g.options.size())
        return;
    Option &o = g.options[id - IDC_OPTION0];
    if (o.step <= 0)
        return;
    const double v = slider_value(o, (int)SendMessageW(o.combo, TBM_GETPOS, 0, 0));
    SetWindowTextW(o.value, o.text(v).c_str());
    if (code == TB_THUMBTRACK || std::fabs(v - o.cur) < 1e-6)
        return;
    o.cur = v;
    WritePrivateProfileStringW(o.section, o.key, fmt(o.decimals > 0 ? L"%.1f" : L"%.0f", v).c_str(),
                               ini_path().c_str());
}

/* A list's choice, written to gt3pc.ini at once. */
void setting_changed(int id)
{
    if (id >= IDC_OPTION0 && id < IDC_OPTION0 + (int)g.options.size()) {
        Option &o = g.options[id - IDC_OPTION0];
        int i = combo_sel(o.combo);
        if (i >= 0 && i < (int)o.choices.size())
            WritePrivateProfileStringW(o.section, o.key, o.choices[i].value.c_str(), ini_path().c_str());
        return;
    }
    if (id == IDC_MODE) {
        int i = combo_sel(g.mode);
        if (i < 0)
            return;
        g.disp.mode = (display::Mode)i;
        fill_resolutions();
        EnableWindow(g.span, g.disp.mode != display::Mode::windowed);   /* (its choice kept) */
    } else if (id == IDC_RES) {
        int i = combo_sel(g.res);
        if (i < 0 || i >= (int)g.resSizes.size())
            return;
        const display::Size &s = g.resSizes[i];
        if (g.disp.mode == display::Mode::fullscreen) {
            g.disp.width = s.w;
            g.disp.height = s.h;
            fill_rates();
        } else if (g.disp.mode == display::Mode::windowed) {
            g.disp.windowW = s.w;
            g.disp.windowH = s.h;
        }
    } else if (id == IDC_RATE) {
        int i = combo_sel(g.rate);
        if (i < 0 || i >= (int)g.rateValues.size())
            return;
        g.disp.refresh = g.rateValues[i];
    } else if (id == IDC_SPAN) {
        int i = combo_sel(g.span);
        if (i < 0 || i >= (int)g.spanValues.size())
            return;
        g.disp.spanMonitors = g.spanValues[i];
    } else if (id == IDC_SPLIT) {
        int i = combo_sel(g.split);
        if (i < 0 || i >= (int)g.splitValues.size())
            return;
        g.disp.splitMonitor = g.splitValues[i];
    }
    display::save(ini_path(), g.disp);
}

/* Starts version v: gt3pc.exe --data <its data folder>, remembered as the
 * last played (gt3pc.ini [Game] Version: the game started without --data
 * takes it too, platform/paths.h). */
void play(int v)
{
    if (v < 0 || v >= install::kVersions || version_state(v) != install::InstallState::ok) {
        refresh();
        return;
    }
    std::wstring exe = g.folder + kGameExe;
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        message(L"The game (" + std::wstring(kGameExe) + L") was not found in " + g.folder + L".", MB_ICONERROR);
        return;
    }
    WritePrivateProfileStringW(L"Game", L"Version", util::widen(install::version_code((install::Version)v)).c_str(),
                               ini_path().c_str());
    controls::show(false);                  /* (SDL lets go of the devices before the game opens them) */
    std::wstring cmd = L"\"" + exe + L"\" --data \"" + g.folder + install::version_folder((install::Version)v) + L"\"";
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, g.folder.c_str(), &si, &pi)) {
        message(L"The game could not be started: " + util::system_error_text(GetLastError()) + L".", MB_ICONERROR);
        controls::show(g.tab == TAB_CONTROLS);
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    DestroyWindow(g.wnd);
}

/* The worker's view of the window. */
class WindowListener : public install::Listener {
public:
    void phase(const std::wstring &text) override
    {
        auto *s = new std::wstring(text);
        if (!PostMessageW(g.wnd, WM_APP_PHASE, 0, (LPARAM)s))
            delete s;
    }
    void progress(uint64_t done, uint64_t total) override
    {
        g.done = done;
        g.total = total;
        int permille = total ? (int)(done * 1000 / total) : 0;
        if (permille != last_) {
            last_ = permille;
            PostMessageW(g.wnd, WM_APP_PROGRESS, 0, 0);
        }
    }
    bool cancel_requested() override { return g.cancel_requested; }

private:
    int last_ = -1;
};

/* Installs from the disc image `source`, or unpacks the earlier version's
 * volume file `source` when from_volume. */
void start_install(const std::wstring &source, bool from_volume)
{
    g.installing = true;
    g.close_when_done = false;
    g.cancel_requested = false;
    g.done = 0;
    g.total = 0;
    g.copying = false;
    g.phase.clear();
    SendMessageW(g.progress, PBM_SETRANGE32, 0, 1000);
    SendMessageW(g.progress, PBM_SETPOS, 0, 0);
    refresh();
    SetFocus(g.cancel);
    std::wstring folder = g.folder;
    g.worker = std::thread([source, from_volume, folder] {
        WindowListener listener;
        std::wstring msg;
        install::Result r = from_volume ? install::install_from_volume(source, folder, listener, &msg)
                                        : install::install_from_iso(source, folder, listener, &msg);
        g.result = r;
        g.result_message = msg;
        PostMessageW(g.wnd, WM_APP_DONE, 0, 0);
    });
}

/* After an install: the earlier version's gt3.vol is not read any more. */
void offer_volume_delete()
{
    if (!install::has_volume(g.folder))
        return;
    std::wstring vol = g.folder + install::kVolumeFile;
    WIN32_FILE_ATTRIBUTE_DATA a = {};
    uint64_t size = GetFileAttributesExW(vol.c_str(), GetFileExInfoStandard, &a)
                        ? ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow
                        : 0;
    std::wstring q = L"The game now reads the unpacked files in the data folder, and no longer uses gt3.vol (" +
                     util::format_size(size) + L"), which an earlier version installed.\n\nDelete gt3.vol?";
    if (MessageBoxW(g.wnd, q.c_str(), kAppName, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;
    if (!DeleteFileW(vol.c_str()))
        message(L"gt3.vol could not be deleted: " + util::system_error_text(GetLastError()) + L".", MB_ICONWARNING);
    refresh();
}

void install_done()
{
    if (g.worker.joinable())
        g.worker.join();   /* after the join g.result is the worker's */
    g.installing = false;
    if (g.close_when_done) {
        DestroyWindow(g.wnd);
        return;
    }
    /* the languages the files have now */
    for (Option &o : g.options)
        if (o.step <= 0 && wcscmp(o.key, L"Language") == 0) {
            o.choices = language_choices();
            fill_option(o);
        }
    switch (g.result) {
    case install::Result::ok:
        refresh();
        offer_volume_delete();
        SetFocus(default_button());
        break;
    case install::Result::cancelled:
        refresh(L"The installation was cancelled.");
        SetFocus(g.install);
        break;
    case install::Result::wrong_disc:
        refresh();
        message(g.result_message, MB_ICONWARNING);
        SetFocus(g.install);
        break;
    default:
        refresh(L"The installation failed.");
        message(g.result_message, MB_ICONERROR);
        SetFocus(g.install);
        break;
    }
}

void choose_and_install()
{
    if (g.installing)
        return;
    /* An earlier version's gt3.vol holds the US disc's files: no image
     * needed while that version is not installed. */
    if (version_state((int)install::Version::us) != install::InstallState::ok && install::has_volume(g.folder)) {
        int a = MessageBoxW(g.wnd,
                            L"This folder has gt3.vol, the game files as an earlier version installed them.\n\n"
                            L"Unpack the game files from gt3.vol? (No: install them from a disc image instead.)",
                            kAppName, MB_YESNOCANCEL | MB_ICONQUESTION);
        if (a == IDCANCEL)
            return;
        if (a == IDYES) {
            start_install(g.folder + install::kVolumeFile, true);
            return;
        }
    }

    std::wstring file(32768, L'\0');
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.wnd;
    ofn.lpstrFilter = L"PlayStation 2 disc image (*.iso)\0*.iso\0All files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = &file[0];
    ofn.nMaxFile = (DWORD)file.size();
    ofn.lpstrTitle = L"Choose a Gran Turismo 3 disc image (USA, Europe, Japan; other Gran Turismo 3 engine discs: data only)";
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
    if (!GetOpenFileNameW(&ofn))
        return;
    file.resize(wcslen(file.c_str()));

    /* which disc it is: its data folder, replaced or extracted (the worker
     * checks it again) */
    iso9660::Image image;
    std::wstring err, msg;
    install::DiscInfo disc;
    bool wrong = false;
    if (!image.open(file, &err)) {
        message(L"The disc image could not be used: " + err + L".", MB_ICONERROR);
        return;
    }
    if (!install::check_disc(image, &disc, &msg, &wrong)) {
        message(msg, wrong ? MB_ICONWARNING : MB_ICONERROR);
        return;
    }
    image.close();
    const bool dataOnly = disc.version == install::Version::data_only;
    std::wstring q = dataOnly ? disc.release + L" is not a version the game plays: its files are extracted as data "
                                               L"only, into " + disc.data + L"."
                              : disc.release + L" is installed as " + install::version_name(disc.version) + L", into " +
                                    disc.data + L".";
    if (install::install_state(g.folder, disc.data) == install::InstallState::ok)
        q += L"\n\nThe files of " + install::installed_release(g.folder, disc.data) + L" there are replaced.";
    q += dataOnly ? L"\n\nExtract them?" : L"\n\nInstall?";
    if (MessageBoxW(g.wnd, q.c_str(), kAppName, MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    start_install(file, false);
}

bool ask(const wchar_t *question)
{
    return MessageBoxW(g.wnd, question, kAppName, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
}

/* Stops a running install (the worker checks between files and deletes
 * its temporary folder). */
void request_cancel()
{
    g.cancel_requested = true;
    EnableWindow(g.cancel, FALSE);
    g.phase = L"Cancelling...";
    g.copying = false;
    show_progress_text();
}

void cancel_clicked()
{
    /* (the install may finish while the question is up) */
    if (g.installing && !g.cancel_requested && ask(L"Stop installing the game files?") && g.installing)
        request_cancel();
}


/* ---- the window procedure ------------------------------------------------------- */

HWND make_button(const wchar_t *text, int id)
{
    return CreateWindowExW(0, WC_BUTTONW, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10, g.wnd,
                           (HMENU)(INT_PTR)id, g.inst, nullptr);
}

HWND make_tab(int i)
{
    return CreateWindowExW(0, WC_BUTTONW, kTabNames[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10,
                           g.wnd, (HMENU)(INT_PTR)(IDC_TAB0 + i), g.inst, nullptr);
}

HWND make_label(HWND parent, const wchar_t *text, int id)
{
    return CreateWindowExW(0, WC_STATICW, text, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 10, 10, parent,
                           (HMENU)(INT_PTR)id, g.inst, nullptr);
}

HWND make_combo(HWND parent, int id)
{
    return CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 0,
                           0, 10, 10, parent, (HMENU)(INT_PTR)id, g.inst, nullptr);
}

/* A row of tab `tab': a label and a list, on its page (shown with it:
 * show_tab). */
HWND add_row(int tab, const wchar_t *label, int id)
{
    Row r;
    r.tab = tab;
    r.label = make_label(g.pages[tab], label, IDC_LABEL0 + (int)g.rows.size());
    r.combo = make_combo(g.pages[tab], id);
    g.rows.push_back(r);
    return r.combo;
}

HWND make_slider(HWND parent, int id)
{
    return CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS, 0, 0,
                           10, 10, parent, (HMENU)(INT_PTR)id, g.inst, nullptr);
}

/* The rows of the options of tab `tab' from the index `from' up to the
 * next option of another tab, or `count' of them. */
void add_options(int tab, size_t &next, int count = 1000)
{
    for (; next < g.options.size() && g.options[next].tab == tab && count > 0; next++, count--) {
        Option &o = g.options[next];
        if (o.step <= 0) {
            o.combo = add_row(tab, o.label, IDC_OPTION0 + (int)next);
            continue;
        }
        Row r;
        r.tab = tab;
        r.label = make_label(g.pages[tab], o.label, IDC_LABEL0 + (int)g.rows.size());
        r.combo = make_slider(g.pages[tab], IDC_OPTION0 + (int)next);
        r.value = make_label(g.pages[tab], L"", IDC_VALUE0 + (int)next);
        g.rows.push_back(r);
        o.combo = r.combo;
        o.value = r.value;
    }
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        g.wnd = hwnd;
        /* Creation order = tab order: the tabs' headers, the rows tab by
         * tab, then the buttons left to right. */
        for (int i = 0; i < kTabs; i++)
            g.tabs[i] = make_tab(i);
        for (int i = 0; i < kTabs; i++)
            g.pages[i] = panel::create(hwnd, g.inst, IDC_PAGE0 + i);
        build_options();
        size_t next = 0;
        for (int v = 0; v < install::kVersions; v++) {
            HWND p = g.pages[TAB_PLAY];
            g.versionName[v] = make_label(p, install::version_name((install::Version)v), IDC_VERSION_NAME0 + v);
            g.versionState[v] = CreateWindowExW(0, WC_STATICW, L"",
                                                WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS | SS_NOPREFIX,
                                                0, 0, 10, 10, p, (HMENU)(INT_PTR)(IDC_VERSION_STATE0 + v), g.inst, nullptr);
            g.versionPlay[v] = CreateWindowExW(0, WC_BUTTONW, L"&Play", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                               0, 0, 10, 10, p, (HMENU)(INT_PTR)(IDC_VERSION_PLAY0 + v), g.inst, nullptr);
        }
        g.extractionsLabel = make_label(g.pages[TAB_PLAY], L"", IDC_EXTRACTIONS);
        g.mode = add_row(TAB_DISPLAY, L"Display mode", IDC_MODE);
        g.res = add_row(TAB_DISPLAY, L"Resolution", IDC_RES);
        g.rate = add_row(TAB_DISPLAY, L"Refresh rate", IDC_RATE);
        add_options(TAB_DISPLAY, next);                 /* vertical sync, frame rate cap */
        g.span = add_row(TAB_DISPLAY, L"Multi-Monitor", IDC_SPAN);
        add_options(TAB_GRAPHICS, next);
        add_options(TAB_AUDIO, next);
        add_options(TAB_GAME, next, 6);                 /* language, the units, starting credits, split screen */
        g.split = add_row(TAB_GAME, L"Split screen 2nd monitor", IDC_SPLIT);
        add_options(TAB_GAME, next);                    /* the HUD's */
        controls::create(g.pages[TAB_CONTROLS], g.inst, ini_path());
        add_options(TAB_DEBUG, next);
        g.install = make_button(L"&Install game files...", IDC_INSTALL);
        g.cancel = make_button(L"&Cancel", IDC_CANCEL);
        g.exit = make_button(L"E&xit", IDC_EXIT);
        g.status = CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 10, 10, hwnd,
                                   (HMENU)(INT_PTR)IDC_STATUS, g.inst, nullptr);
        g.progress = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD, 0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_PROGRESS,
                                     g.inst, nullptr);
        load_settings();
        show_tab(TAB_PLAY);
        return 0;
    }

    case DM_GETDEFID:
        return MAKELRESULT(g.default_id, DC_HASDEFID);
    case WM_ACTIVATE:
        /* Keep the focused button across deactivation, as a dialog does. */
        if (LOWORD(wp) == WA_INACTIVE) {
            g.last_focus = GetFocus();
        } else {
            HWND f = g.last_focus;
            if (!f || !IsChild(hwnd, f) || !IsWindowVisible(f) || !IsWindowEnabled(f))
                f = default_button();
            SetFocus(f);
        }
        return 0;

    case WM_DPICHANGED:
        apply_dpi(HIWORD(wp), reinterpret_cast<const RECT *>(lp));
        return 0;
    case WM_DISPLAYCHANGE:
        /* A monitor plugged in, unplugged or moved: the monitor lists again
         * (a choice gone stays, marked). */
        fill_monitors();
        for (HWND c : {g.span, g.split})
            fit_list(c);
        return 0;

    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        draw_background(dc, rc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_PRINTCLIENT: {
        /* Themed buttons draw the parent's background behind their corners. */
        RECT rc;
        GetClientRect(hwnd, &rc);
        draw_background((HDC)wp, rc);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        /* the labels and the status line over the background: its brush,
         * from the child's place */
        HWND c = (HWND)lp;
        if (!is_text(c) || g.bgBrush == nullptr)
            break;
        HDC dc = (HDC)wp;
        const POINT o = child_origin(c);
        SetTextColor(dc, RGB(236, 236, 236));
        SetBkMode(dc, TRANSPARENT);
        SetBrushOrgEx(dc, -o.x, -o.y, nullptr);
        return (LRESULT)g.bgBrush;
    }
    case WM_CTLCOLORBTN:
        return (LRESULT)GetStockObject(BLACK_BRUSH);
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT *d = (const DRAWITEMSTRUCT *)lp;
        if (d->CtlID >= IDC_TAB0 && d->CtlID < (UINT)(IDC_TAB0 + kTabs)) {
            draw_tab(d);
            return TRUE;
        }
        if (controls::draw_item(d))
            return TRUE;
        break;
    }
    case WM_TIMER:
        if (controls::timer(wp))
            return 0;
        break;
    case WM_HSCROLL: {
        /* a slider moved (its page passes it on: panel.h) */
        const int id = lp != 0 ? GetDlgCtrlID((HWND)lp) : 0;
        if (id >= controls::kFirstId && id <= controls::kLastId)
            controls::slider(id, LOWORD(wp));
        else if (id >= IDC_OPTION0 && id < IDC_OPTION0 + (int)g.options.size())
            slider_moved(id, LOWORD(wp));
        return 0;
    }

    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id >= controls::kFirstId && id <= controls::kLastId) {
            controls::command(id, HIWORD(wp), (HWND)lp);
            return 0;
        }
        if (id >= IDC_TAB0 && id < IDC_TAB0 + kTabs) {
            if (HIWORD(wp) == BN_CLICKED)
                show_tab(id - IDC_TAB0);
            return 0;
        }
        if ((id >= IDC_MODE && id <= IDC_SPLIT) || (id >= IDC_OPTION0 && id < IDC_OPTION0 + (int)g.options.size())) {
            if (HIWORD(wp) == CBN_SELCHANGE)
                setting_changed(id);
            return 0;
        }
        if (id >= IDC_VERSION_PLAY0 && id < IDC_VERSION_PLAY0 + install::kVersions) {
            if (HIWORD(wp) == BN_CLICKED && !g.installing)
                play(id - IDC_VERSION_PLAY0);
            return 0;
        }
        switch (id) {
        case IDOK:   /* Enter (IsDialogMessage): the default version, else the install */
            if (g.installing)
                return 0;
            if (default_version() >= 0)
                play(default_version());
            else
                choose_and_install();
            return 0;
        case IDCANCEL:   /* Esc */
            if (g.installing)
                cancel_clicked();
            else
                DestroyWindow(hwnd);
            return 0;
        case IDC_INSTALL:
            choose_and_install();
            return 0;
        case IDC_CANCEL:
            cancel_clicked();
            return 0;
        case IDC_EXIT:
            SendMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        break;
    }

    case WM_APP_PHASE: {
        std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring *>(lp));
        if (!g.cancel_requested) {
            g.phase = *text;
            g.copying = false;
            show_progress_text();
        }
        return 0;
    }
    case WM_APP_PROGRESS:
        if (!g.cancel_requested) {
            g.copying = true;
            show_progress_text();
        }
        return 0;
    case WM_APP_DONE:
        install_done();
        return 0;

    case WM_CLOSE:
        if (g.installing) {
            if (!g.cancel_requested && !ask(L"The game files are being installed.\n\nStop the installation and exit?"))
                return 0;
            if (g.installing) {
                /* Exit once the worker has removed its temporary folder. */
                if (!g.cancel_requested)
                    request_cancel();
                g.close_when_done = true;
                return 0;
            }
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        controls::show(false);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int run(int show_command)
{
    g.inst = GetModuleHandleW(nullptr);
    g.folder = util::exe_folder();

    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);
    /* The Open dialog is a shell component and wants COM on this thread (WIC
     * too: the background). */
    HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = g.inst;
    wc.hIcon = LoadIconW(g.inst, MAKEINTRESOURCEW(IDI_GT3LAUNCHER));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, kClassName, kTitle, kStyle, CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr, nullptr,
                                g.inst, nullptr);
    if (!hwnd) {
        MessageBoxW(nullptr, L"The launcher window could not be created.", kAppName, MB_OK | MB_ICONERROR);
        return 1;
    }
    apply_dpi(GetDpiForWindow(hwnd), nullptr);
    /* an earlier launcher's data\ into its version's folder; the last
     * played version (Enter's) */
    const std::wstring moved = install::migrate_earlier_install(g.folder);
    wchar_t last[16] = L"";
    GetPrivateProfileStringW(L"Game", L"Version", L"US", last, 16, ini_path().c_str());
    for (int v = 0; v < install::kVersions; v++)
        if (_wcsicmp(last, util::widen(install::version_code((install::Version)v)).c_str()) == 0)
            g.lastVersion = v;
    if (!moved.empty())
        for (Option &o : g.options)
            if (o.step <= 0 && wcscmp(o.key, L"Language") == 0) {
                o.choices = language_choices();
                fill_option(o);
            }
    refresh(moved);
    ShowWindow(hwnd, show_command);
    UpdateWindow(hwnd);
    SetFocus(default_button());

    MSG m;
    HWND focus = nullptr;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        /* the Controls tab's capture first (a key or click taken as a
         * binding goes nowhere else) */
        if (controls::filter(m))
            continue;
        /* the mouse wheel over a page that scrolls: the page's */
        if (panel::wheel(m))
            continue;
        /* Ctrl+Tab / Ctrl+Shift+Tab, Ctrl+PageDown / PageUp: the next /
         * previous tab */
        if (m.message == WM_KEYDOWN && GetKeyState(VK_CONTROL) < 0 &&
            (m.wParam == VK_TAB || m.wParam == VK_NEXT || m.wParam == VK_PRIOR)) {
            const bool back = m.wParam == VK_PRIOR || (m.wParam == VK_TAB && GetKeyState(VK_SHIFT) < 0);
            show_tab((g.tab + (back ? kTabs - 1 : 1)) % kTabs);
            continue;
        }
        /* Tab between the controls, Enter and Esc. */
        if (!IsDialogMessageW(hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        /* the focus moved to a control out of its page's view: scrolled to */
        HWND f = GetFocus();
        if (f != focus) {
            focus = f;
            if (f != nullptr)
                panel::show(f);
        }
    }

    if (g.worker.joinable()) {
        g.cancel_requested = true;
        g.worker.join();
    }
    if (g.font)
        DeleteObject(g.font);
    if (g.tabFont)
        DeleteObject(g.tabFont);
    if (g.bgBrush)
        DeleteObject(g.bgBrush);
    if (g.bgDC) {
        SelectObject(g.bgDC, g.bgOld);
        DeleteDC(g.bgDC);
    }
    if (g.bg)
        DeleteObject(g.bg);
    if (g.icon_big)
        DestroyIcon(g.icon_big);
    if (g.icon_small)
        DestroyIcon(g.icon_small);
    if (SUCCEEDED(com))
        CoUninitialize();
    return (int)m.wParam;
}

} // namespace ui
