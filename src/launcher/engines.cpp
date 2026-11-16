/* engines.cpp - the engine selector and each engine's own settings file
 * (engines.h).  A small window of its own, shown before the launcher's:
 * plain Win32, the system's controls and message font, per-monitor DPI
 * aware; a list drawn two lines an engine (its name, its folder under it),
 * Start, Add engine..., Remove and Exit. */
#include "engines.h"
#include "resource.h"
#include "util.h"

#include <commctrl.h>

#include <algorithm>

namespace engines {
namespace {

const wchar_t kSection[] = L"Engines";
const wchar_t kIni[] = L"gt3pc.ini";
const wchar_t kClassName[] = L"GT3EngineSelector";
const wchar_t kTitle[] = L"Gran Turismo 3 PC Enhanced - Engine Selector";
const wchar_t kAppName[] = L"Gran Turismo 3 A-Spec";
const wchar_t kOwnName[] = L"Gran Turismo 3 A-Spec";

/* Layout, in 96-DPI units. */
constexpr int kClientW = 560, kClientH = 300, kMargin = 14;
constexpr int kButtonW = 124, kButtonH = 28, kButtonGap = 8, kItemH = 42, kHeadH = 22;
/* PC (2026-11-14, user request: "The engine selector's list needs to have the
 * launcher's .ico icon in the UI item"): each item's launcher's icon at its
 * left - this launcher's own (its resource), an engine's from its
 * gt3launcher.exe (a modder's launcher with an icon of its own shows it). */
constexpr int kIconSize = 32, kIconPad = 6;
constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;

enum : int { IDC_LIST = 100, IDC_START, IDC_ADD, IDC_REMOVE, IDC_EXIT, IDC_HEAD };

struct Item {
    std::wstring folder;        /* without a trailing '\'; this launcher's: its own */
    std::wstring name;
    bool own = false;
    bool there = true;          /* its launcher and its game are in the folder */
    HICON icon = nullptr;       /* its launcher's, kIconSize at the window's DPI (none: its launcher is gone) */
};

struct Selector {
    HINSTANCE inst = nullptr;
    HWND wnd = nullptr, head = nullptr, list = nullptr, start = nullptr, add = nullptr, remove = nullptr,
         exit = nullptr;
    HFONT font = nullptr, bold = nullptr;
    UINT dpi = 96;
    std::wstring folder, ini;
    std::vector<Item> items;
    bool done = false;
    Choice choice = Choice::exit;
    int chosen = 0;
} g;

int px(int dip) { return MulDiv(dip, (int)g.dpi, 96); }

std::wstring no_slash(std::wstring f)
{
    while (f.size() > 3 && (f.back() == L'\\' || f.back() == L'/'))
        f.pop_back();
    return f;
}

bool is_file(const std::wstring &p)
{
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool engine_there(const std::wstring &folder)
{
    return is_file(folder + L"\\" + kLauncherExe) && is_file(folder + L"\\" + kGameExe);
}

/* The items' icons, at the window's DPI (again when it changes): this
 * launcher's from its resource (else, as an engine's, from the file), an
 * engine's the first icon of its gt3launcher.exe. */
void free_icons()
{
    for (Item &it : g.items) {
        if (it.icon != nullptr)
            DestroyIcon(it.icon);
        it.icon = nullptr;
    }
}

void load_icons()
{
    free_icons();
    const int size = px(kIconSize);
    for (Item &it : g.items) {
        if (it.own)
            LoadIconWithScaleDown(g.inst, MAKEINTRESOURCEW(IDI_GT3LAUNCHER), size, size, &it.icon);
        if (it.icon == nullptr)
            PrivateExtractIconsW((it.folder + L"\\" + kLauncherExe).c_str(), 0, size, size, &it.icon, nullptr, 1, 0);
    }
}

/* The list: this launcher, then the engines added. */
void fill(int select)
{
    free_icons();
    g.items.clear();
    Item own;
    own.folder = no_slash(g.folder);
    own.own = true;
    /* (its own engine.txt names it too: a plugin started by itself) */
    own.name = is_file(g.folder + kNameFile) ? name_of(own.folder) : std::wstring(kOwnName);
    own.name;
    g.items.push_back(own);
    for (const std::wstring &f : load(g.ini)) {
        Item it;
        it.folder = f;
        it.name = name_of(f);
        it.there = engine_there(f);
        g.items.push_back(it);
    }
    load_icons();
    SendMessageW(g.list, LB_RESETCONTENT, 0, 0);
    for (const Item &it : g.items)
        SendMessageW(g.list, LB_ADDSTRING, 0, (LPARAM)it.name.c_str());
    select = std::max(0, std::min(select, (int)g.items.size() - 1));
    SendMessageW(g.list, LB_SETCURSEL, select, 0);
}

int selected()
{
    const int i = (int)SendMessageW(g.list, LB_GETCURSEL, 0, 0);
    return i >= 0 && i < (int)g.items.size() ? i : -1;
}

void update_buttons()
{
    const int i = selected();
    EnableWindow(g.start, i >= 0 && g.items[i].there);
    EnableWindow(g.remove, i > 0);
}

void apply_dpi(UINT dpi, const RECT *suggested)
{
    g.dpi = dpi;
    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    HFONT font = nullptr, bold = nullptr;
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi)) {
        font = CreateFontIndirectW(&ncm.lfMessageFont);
        ncm.lfMessageFont.lfWeight = FW_SEMIBOLD;
        ncm.lfMessageFont.lfHeight = ncm.lfMessageFont.lfHeight * 6 / 5;
        bold = CreateFontIndirectW(&ncm.lfMessageFont);
    }
    for (HWND c : {g.head, g.list, g.start, g.add, g.remove, g.exit})
        SendMessageW(c, WM_SETFONT, (WPARAM)font, FALSE);
    if (g.font)
        DeleteObject(g.font);
    if (g.bold)
        DeleteObject(g.bold);
    g.font = font;
    g.bold = bold;

    RECT r = {0, 0, px(kClientW), px(kClientH)};
    AdjustWindowRectExForDpi(&r, kStyle, FALSE, 0, dpi);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    if (suggested) {
        SetWindowPos(g.wnd, nullptr, suggested->left, suggested->top, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromWindow(g.wnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        SetWindowPos(g.wnd, nullptr, (mi.rcWork.left + mi.rcWork.right - ww) / 2,
                     (mi.rcWork.top + mi.rcWork.bottom - wh) / 2, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    /* the head, the list under it, the buttons in a column at its right */
    const int m = px(kMargin), bw = px(kButtonW), bh = px(kButtonH), gap = px(kButtonGap);
    const int listW = px(kClientW) - 3 * m - bw, top = m + px(kHeadH);
    SetWindowPos(g.head, nullptr, m, m, listW, px(kHeadH), SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(g.list, nullptr, m, top, listW, px(kClientH) - top - m, SWP_NOZORDER | SWP_NOACTIVATE);
    const int bx = px(kClientW) - m - bw;
    SetWindowPos(g.start, nullptr, bx, top, bw, bh, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(g.add, nullptr, bx, top + bh + gap, bw, bh, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(g.remove, nullptr, bx, top + 2 * (bh + gap), bw, bh, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(g.exit, nullptr, bx, px(kClientH) - m - bh, bw, bh, SWP_NOZORDER | SWP_NOACTIVATE);
    SendMessageW(g.list, LB_SETITEMHEIGHT, 0, px(kItemH));
    load_icons();                           /* (at this DPI) */
    InvalidateRect(g.wnd, nullptr, TRUE);
}

/* An engine's row: its name, its folder under it (its end kept), or that its
 * programs are not there. */
void draw_item(const DRAWITEMSTRUCT *d)
{
    if (d->itemID == (UINT)-1 || d->itemID >= g.items.size())
        return;
    const Item &it = g.items[d->itemID];
    const bool sel = (d->itemState & ODS_SELECTED) != 0;
    FillRect(d->hDC, &d->rcItem, GetSysColorBrush(sel ? COLOR_HIGHLIGHT : COLOR_WINDOW));
    SetBkMode(d->hDC, TRANSPARENT);
    RECT r = d->rcItem;
    /* its launcher's icon, centred in the row's height; the texts right of it */
    const int size = px(kIconSize);
    if (it.icon != nullptr)
        DrawIconEx(d->hDC, r.left + px(kIconPad), r.top + (r.bottom - r.top - size) / 2, it.icon, size, size, 0, nullptr,
                   DI_NORMAL);
    r.left += px(kIconPad) + size + px(8);
    r.right -= px(8);
    RECT name = r, path = r;
    name.top += px(3);
    name.bottom = name.top + px(20);
    path.top = name.bottom;
    path.bottom -= px(3);
    HGDIOBJ old = SelectObject(d->hDC, g.bold ? g.bold : g.font);
    SetTextColor(d->hDC, GetSysColor(sel ? COLOR_HIGHLIGHTTEXT : it.there ? COLOR_WINDOWTEXT : COLOR_GRAYTEXT));
    DrawTextW(d->hDC, it.name.c_str(), -1, &name, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(d->hDC, g.font);
    SetTextColor(d->hDC, GetSysColor(sel ? COLOR_HIGHLIGHTTEXT : COLOR_GRAYTEXT));
    const std::wstring line = it.there ? it.folder
                                       : L"Not found (" + std::wstring(kLauncherExe) + L", " + kGameExe + L"): " + it.folder;
    DrawTextW(d->hDC, line.c_str(), -1, &path, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);
    SelectObject(d->hDC, old);
    if ((d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT))
        DrawFocusRect(d->hDC, &d->rcItem);
}

/* Start: this launcher's window follows, or the engine's launcher is started. */
void start_selected()
{
    const int i = selected();
    if (i < 0 || !g.items[i].there)
        return;
    g.chosen = i;
    g.choice = g.items[i].own ? Choice::own : Choice::other;
    WritePrivateProfileStringW(kSection, L"Last", g.items[i].own ? L"" : g.items[i].folder.c_str(), g.ini.c_str());
    g.done = true;
}

/* Add engine...: a folder with a launcher and a game, after the warning. */
void add_engine()
{
    const std::wstring picked = no_slash(util::choose_folder(
        g.wnd, L"Choose an engine's folder (it holds gt3launcher.exe and gt3pc.exe)", L"Add engine"));
    if (picked.empty())
        return;
    if (!engine_there(picked)) {
        MessageBoxW(g.wnd,
                    (picked + L" is not an engine: it has no " + kLauncherExe + L" and " + kGameExe +
                     L".\n\nAn engine is the folder \"Build Mod Plugin.cmd\" makes.")
                        .c_str(),
                    kAppName, MB_OK | MB_ICONWARNING);
        return;
    }
    for (size_t i = 0; i < g.items.size(); i++)
        if (_wcsicmp(g.items[i].folder.c_str(), picked.c_str()) == 0) {
            MessageBoxW(g.wnd,
                        g.items[i].own ? L"That is this launcher's own folder: it is the first engine of the list."
                                       : L"That engine is in the list already.",
                        kAppName, MB_OK | MB_ICONINFORMATION);
            SendMessageW(g.list, LB_SETCURSEL, i, 0);
            update_buttons();
            return;
        }
    if ((int)g.items.size() > kMaxEngines) {
        MessageBoxW(g.wnd, L"The list is full: remove an engine first.", kAppName, MB_OK | MB_ICONINFORMATION);
        return;
    }
    const std::wstring warning =
        L"Make sure this engine is from a source you trust.\n\n" + name_of(picked) + L"\n" + picked +
        L"\n\nAn engine is a program: its launcher and its game run on this PC as any program you start, with "
        L"your files and your settings in reach. Nothing checks what it does.\n\nAdd it to the list?";
    if (MessageBoxW(g.wnd, warning.c_str(), kAppName, MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK)
        return;
    std::vector<std::wstring> folders = load(g.ini);
    folders.push_back(picked);
    save(g.ini, folders);
    fill((int)folders.size());
    update_buttons();
    SetFocus(g.list);
}

/* Remove: out of the list; its folder stays as it is. */
void remove_engine()
{
    const int i = selected();
    if (i <= 0)
        return;
    std::vector<std::wstring> folders = load(g.ini);
    for (size_t k = 0; k < folders.size(); k++)
        if (_wcsicmp(folders[k].c_str(), g.items[i].folder.c_str()) == 0) {
            folders.erase(folders.begin() + (std::ptrdiff_t)k);
            break;
        }
    save(g.ini, folders);
    fill(i);
    update_buttons();
    SetFocus(g.list);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        g.wnd = hwnd;
        const auto button = [hwnd](const wchar_t *text, int id, DWORD style) {
            return CreateWindowExW(0, WC_BUTTONW, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, 0, 0, 10, 10, hwnd,
                                   (HMENU)(INT_PTR)id, g.inst, nullptr);
        };
        g.head = CreateWindowExW(0, WC_STATICW, L"Choose the engine to start:", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                                 0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_HEAD, g.inst, nullptr);
        g.list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTBOXW, L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED |
                                     LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
                                 0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_LIST, g.inst, nullptr);
        g.start = button(L"&Start", IDC_START, BS_DEFPUSHBUTTON);
        g.add = button(L"&Add engine...", IDC_ADD, BS_PUSHBUTTON);
        g.remove = button(L"&Remove", IDC_REMOVE, BS_PUSHBUTTON);
        g.exit = button(L"E&xit", IDC_EXIT, BS_PUSHBUTTON);
        return 0;
    }
    case DM_GETDEFID:
        return MAKELRESULT(IDC_START, DC_HASDEFID);
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT *m = (MEASUREITEMSTRUCT *)lp;
        if (m->CtlID == IDC_LIST)
            m->itemHeight = (UINT)px(kItemH);
        return TRUE;
    }
    case WM_DRAWITEM:
        if (((const DRAWITEMSTRUCT *)lp)->CtlID == IDC_LIST) {
            draw_item((const DRAWITEMSTRUCT *)lp);
            return TRUE;
        }
        break;
    case WM_DPICHANGED:
        apply_dpi(HIWORD(wp), (const RECT *)lp);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_LIST:
            if (HIWORD(wp) == LBN_SELCHANGE)
                update_buttons();
            else if (HIWORD(wp) == LBN_DBLCLK)
                start_selected();
            return 0;
        case IDOK:
        case IDC_START:
            start_selected();
            return 0;
        case IDC_ADD:
            add_engine();
            return 0;
        case IDC_REMOVE:
            remove_engine();
            return 0;
        case IDCANCEL:
        case IDC_EXIT:
            g.choice = Choice::exit;
            g.done = true;
            return 0;
        }
        break;
    case WM_CLOSE:
        g.choice = Choice::exit;
        g.done = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* A whole section of an ini file copied to another, then removed from the
 * first; false when it had none. */
bool move_section(const wchar_t *section, const std::wstring &from, const std::wstring &to)
{
    std::vector<wchar_t> buf(32768);
    DWORD n;
    while ((n = GetPrivateProfileSectionW(section, buf.data(), (DWORD)buf.size(), from.c_str())) == buf.size() - 2 &&
           buf.size() < (4u << 20))
        buf.resize(buf.size() * 4);
    if (n == 0)
        return false;
    WritePrivateProfileSectionW(section, buf.data(), to.c_str());
    WritePrivateProfileStringW(section, nullptr, nullptr, from.c_str());
    return true;
}

void move_key(const wchar_t *section, const wchar_t *key, const std::wstring &from, const std::wstring &to)
{
    wchar_t v[512] = L"";
    if (GetPrivateProfileStringW(section, key, L"", v, 512, from.c_str()) != 0)
        WritePrivateProfileStringW(section, key, v, to.c_str());
    WritePrivateProfileStringW(section, key, nullptr, from.c_str());
}

} // namespace

std::wstring name_of(const std::wstring &folder_in)
{
    const std::wstring folder = no_slash(folder_in);
    HANDLE h = CreateFileW((folder + L"\\" + kNameFile).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        char buf[512];
        DWORD got = 0;
        const bool ok = ReadFile(h, buf, sizeof(buf), &got, nullptr) != 0;
        CloseHandle(h);
        if (ok && got != 0) {
            std::string line(buf, got);
            if (line.compare(0, 3, "\xEF\xBB\xBF") == 0)
                line.erase(0, 3);
            line = line.substr(0, line.find_first_of("\r\n"));
            while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
                line.pop_back();
            const std::wstring name = util::widen(line);
            if (!name.empty())
                return name.substr(0, 80);
        }
    }
    const size_t slash = folder.find_last_of(L"\\/");
    return slash == std::wstring::npos ? folder : folder.substr(slash + 1);
}

std::vector<std::wstring> load(const std::wstring &ini)
{
    std::vector<std::wstring> out;
    const int count = (int)GetPrivateProfileIntW(kSection, L"Count", 0, ini.c_str());
    for (int i = 1; i <= count && (int)out.size() < kMaxEngines; i++) {
        wchar_t folder[2048] = L"";
        GetPrivateProfileStringW(kSection, (L"Engine" + std::to_wstring(i)).c_str(), L"", folder, 2048, ini.c_str());
        const std::wstring f = no_slash(folder);
        if (!f.empty())
            out.push_back(f);
    }
    return out;
}

void save(const std::wstring &ini, const std::vector<std::wstring> &folders)
{
    wchar_t last[2048] = L"";
    GetPrivateProfileStringW(kSection, L"Last", L"", last, 2048, ini.c_str());
    WritePrivateProfileStringW(kSection, nullptr, nullptr, ini.c_str());    /* (the section written anew) */
    WritePrivateProfileStringW(kSection, L"Count", std::to_wstring(folders.size()).c_str(), ini.c_str());
    for (size_t i = 0; i < folders.size(); i++)
        WritePrivateProfileStringW(kSection, (L"Engine" + std::to_wstring(i + 1)).c_str(), folders[i].c_str(), ini.c_str());
    if (last[0] != 0)
        WritePrivateProfileStringW(kSection, L"Last", last, ini.c_str());
}

void ensure_engine_ini(const std::wstring &folder)
{
    const std::wstring engineIni = folder + kEngineIni;
    if (!is_file(engineIni))
        WritePrivateProfileStringW(L"Engine", L"Format", L"1", engineIni.c_str());
}

void migrate(const std::wstring &folder)
{
    const std::wstring ini = folder + kIni, engineIni = folder + kEngineIni;
    if (is_file(engineIni))
        return;
    if (is_file(ini)) {
        move_section(L"Mods", ini, engineIni);
        move_section(L"Discs", ini, engineIni);
        move_key(L"Game", L"Version", ini, engineIni);
        move_key(L"Game", L"Disc", ini, engineIni);
    }
    ensure_engine_ini(folder);
}

bool start(const std::wstring &launcher, const std::wstring &engine, const std::wstring &folder, std::wstring *error)
{
    std::wstring cmd = L"\"" + launcher + L"\" --engine --shared \"" + no_slash(folder) + L"\"";
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(launcher.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, engine.c_str(), &si, &pi)) {
        *error = util::system_error_text(GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

Choice select(HINSTANCE inst, const std::wstring &folder, int show, std::wstring *launcher, std::wstring *engine)
{
    g = Selector();
    g.inst = inst;
    g.folder = folder;
    g.ini = folder + kIni;

    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_GT3LAUNCHER));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, kClassName, kTitle, kStyle, CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr, nullptr,
                                inst, nullptr);
    if (!hwnd)
        return Choice::own;                 /* (no selector: the launcher itself) */
    apply_dpi(GetDpiForWindow(hwnd), nullptr);
    /* the engine started last is the one Enter starts */
    wchar_t last[2048] = L"";
    GetPrivateProfileStringW(kSection, L"Last", L"", last, 2048, g.ini.c_str());
    fill(0);
    for (size_t i = 1; i < g.items.size(); i++)
        if (last[0] != 0 && _wcsicmp(g.items[i].folder.c_str(), no_slash(last).c_str()) == 0)
            SendMessageW(g.list, LB_SETCURSEL, i, 0);
    update_buttons();
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
    SetFocus(g.list);

    MSG m;
    while (!g.done && GetMessageW(&m, nullptr, 0, 0) > 0) {
        /* (Enter in the list starts the chosen engine, Delete removes it) */
        if (m.message == WM_KEYDOWN && m.hwnd == g.list && m.wParam == VK_DELETE) {
            remove_engine();
            continue;
        }
        if (!IsDialogMessageW(hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    const Choice choice = g.done ? g.choice : Choice::exit;
    if (choice == Choice::other) {
        *engine = g.items[g.chosen].folder;
        *launcher = *engine + L"\\" + kLauncherExe;
    }
    DestroyWindow(hwnd);
    free_icons();
    if (g.font)
        DeleteObject(g.font);
    if (g.bold)
        DeleteObject(g.bold);
    g.font = g.bold = nullptr;
    return choice;
}

} // namespace engines
