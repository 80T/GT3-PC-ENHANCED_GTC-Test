/* texture_manager.cpp - the texture manager (texture_manager.h).
 *
 * PC (2026-11-13): the mod manager's window (mod_manager.cpp) over another
 * list - its layout, its keys, its card (mod_card.h) - with the texture
 * folders' own count, no template, and a section of gt3pc.ini it shares with
 * the game (save). */
#include "texture_manager.h"

#include "mod_card.h"
#include "util.h"

#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <climits>

namespace texture_manager {
namespace {

using mod_card::full_path;
using mod_card::Info;
using mod_card::kImageFile;
using mod_card::load_image;
using mod_card::read_info;

constexpr const wchar_t *kClass = L"GT3LauncherTextureManager";
constexpr const wchar_t *kSection = L"Textures";
constexpr const wchar_t *kKey = L"Pack";    /* (PackN, PackNOn) */
constexpr int kMaxPacks = 256;              /* (as the mod manager's folders) */
constexpr int kMaxDepth = 64;               /* (folders deeper not followed: a link loop's guard) */
constexpr int kHashDigits = 16;             /* (a texture file's name starts with its texture's hash) */
/* The game's texture dumps, in the launcher's folder: all of them, the ones
 * picked. */
constexpr const wchar_t *kDumps = L"tex_replacements\\dumps", *kDumpsSelected = L"tex_replacements\\dumps_selected";

/* Layout, in 96-DPI units (the mod manager's). */
constexpr int kW = 680, kH = 540, kM = 12;
constexpr int kBtnW = 150, kBtnH = 28, kGap = 8, kHintH = 52, kCountW = 100, kTitleW = 170;
constexpr int kImage = 160, kTitleH = 26, kAuthorsH = 22;
constexpr int kImageEdge = 2;               /* (DrawEdge's, in pixels at any DPI) */

enum : int {
    IDC_HINT = 3101,
    IDC_LIST,
    IDC_ADD,
    IDC_REMOVE,
    IDC_UP,
    IDC_DOWN,
    IDC_OPEN,
    IDC_DUMPS,
    IDC_IMAGE,
    IDC_TITLE,
    IDC_AUTHORS,
    IDC_DESCRIPTION,
    IDC_STATUS,
};

struct Manager {
    HWND wnd = nullptr, owner = nullptr;
    HWND hint = nullptr, list = nullptr, add = nullptr, remove = nullptr, up = nullptr, down = nullptr,
         openFolder = nullptr, dumps = nullptr, image = nullptr, title = nullptr, authors = nullptr,
         description = nullptr, status = nullptr, close = nullptr;
    HFONT font = nullptr, titleFont = nullptr;
    UINT dpi = 96;
    std::wstring ini;
    void (*changed)() = nullptr;
    std::vector<Pack> packs;
    bool filling = false;                   /* (the list being filled: its check boxes not taken) */
    std::vector<uint32_t> pixels;           /* the selected folder's picture, ready to draw (empty: none) */
    int imageW = 0, imageH = 0;
};

Manager g;

int px(int v) { return MulDiv(v, (int)g.dpi, 96); }

/* gt3pc.ini (the ANSI code page's) can keep the path. */
bool ini_can_keep(const std::wstring &s)
{
    BOOL lossy = FALSE;
    WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, s.c_str(), (int)s.size(), nullptr, 0, nullptr, &lossy);
    return !lossy;
}

bool is_folder(const std::wstring &p)
{
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* A texture's file, by its name: the texture's hash - 16 hexadecimal digits,
 * either case - then anything, and .png, .jpg, .jpeg or .dds
 * (0123456789ABCDEF.png, 0123456789abcdef_bonnet.dds). */
bool is_texture(const std::wstring &name)
{
    if (name.size() <= (size_t)kHashDigits)
        return false;
    for (int i = 0; i < kHashDigits; i++) {
        const wchar_t c = name[(size_t)i];
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F')))
            return false;
    }
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos)
        return false;
    const wchar_t *ext = name.c_str() + dot;
    return _wcsicmp(ext, L".png") == 0 || _wcsicmp(ext, L".jpg") == 0 || _wcsicmp(ext, L".jpeg") == 0 ||
           _wcsicmp(ext, L".dds") == 0;
}

/* The texture files under `folder', in the folders under it too. */
long count_textures(const std::wstring &folder, int depth)
{
    if (depth > kMaxDepth)
        return 0;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW((util::with_slash(folder) + L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    long n = 0;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..")
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            n += count_textures(util::with_slash(folder) + name, depth + 1);
        else if (is_texture(name))
            n++;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return n;
}

void set_status(const std::wstring &t) { SetWindowTextW(g.status, t.c_str()); }

std::wstring summary()
{
    const long on = (long)std::count_if(g.packs.begin(), g.packs.end(), [](const Pack &p) { return p.on; });
    if (g.packs.empty())
        return L"No texture folders installed. Select `Add folder` to add one";
    return std::to_wstring(g.packs.size()) + (g.packs.size() == 1 ? L" folder, " : L" folders, ") +
           std::to_wstring(on) + L" enabled.";
}

/* ---- the list ---- */

/* The number of a key of the list - "Pack<number>" or "Pack<number>On", any
 * case - or -1: another key (the game's). */
long list_key(const wchar_t *key)
{
    const size_t n = wcslen(kKey);
    if (_wcsnicmp(key, kKey, n) != 0 || key[n] < L'0' || key[n] > L'9')
        return -1;
    wchar_t *end = nullptr;
    const unsigned long number = wcstoul(key + n, &end, 10);
    if (*end != 0 && _wcsicmp(end, L"On") != 0)
        return -1;
    return (long)std::min(number, (unsigned long)LONG_MAX);
}

/* The list written: Count, PackN and PackNOn, nothing else.  The section is
 * the game's too (Replace, Dump, PreCache ...: its keys stay as they are), so
 * it is not written anew as the mod manager's: the PackN and PackNOn past the
 * list's end - a longer list's before this one - are taken out one by one. */
void save()
{
    const long count = (long)g.packs.size();
    WritePrivateProfileStringW(kSection, L"Count", std::to_wstring(count).c_str(), g.ini.c_str());
    for (long i = 0; i < count; i++) {
        const std::wstring key = kKey + std::to_wstring(i + 1);
        WritePrivateProfileStringW(kSection, key.c_str(), g.packs[(size_t)i].folder.c_str(), g.ini.c_str());
        WritePrivateProfileStringW(kSection, (key + L"On").c_str(), g.packs[(size_t)i].on ? L"on" : L"off",
                                   g.ini.c_str());
    }
    std::vector<wchar_t> keys(32767);       /* (the section's keys, each ended by a 0) */
    const DWORD got = GetPrivateProfileStringW(kSection, nullptr, L"", keys.data(), (DWORD)keys.size(), g.ini.c_str());
    for (const wchar_t *k = keys.data(); k < keys.data() + got && *k != 0; k += wcslen(k) + 1)
        if (list_key(k) > count)
            WritePrivateProfileStringW(kSection, k, nullptr, g.ini.c_str());
    if (g.changed != nullptr)
        g.changed();
}

int selected() { return (int)SendMessageW(g.list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED); }

void buttons()
{
    const int i = selected();
    const int n = (int)g.packs.size();
    EnableWindow(g.add, n < kMaxPacks);
    EnableWindow(g.remove, i >= 0);
    EnableWindow(g.up, i > 0);
    EnableWindow(g.down, i >= 0 && i < n - 1);
    EnableWindow(g.openFolder, i >= 0);
}

void set_cell(int row, int col, const std::wstring &text)
{
    LVITEMW it = {};
    it.iSubItem = col;
    it.pszText = const_cast<wchar_t *>(text.c_str());
    SendMessageW(g.list, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&it);
}

/* The panel under the list, for the selected row (empty with none).  Its files
 * are read anew each time: a mod.txt or mod.png being worked on shows when the
 * window is come back to (WM_ACTIVATE). */
void show_details()
{
    const int i = selected();
    Info in;
    g.pixels.clear();
    if (i >= 0 && i < (int)g.packs.size()) {
        const std::wstring &folder = g.packs[(size_t)i].folder;
        in = read_info(folder);
        set_cell(i, 0, in.title);
        const int box = px(kImage) - 2 * kImageEdge;
        if (!load_image(util::with_slash(folder) + kImageFile, box, &g.pixels, &g.imageW, &g.imageH))
            load_image(L"", box, &g.pixels, &g.imageW, &g.imageH);
    }
    SetWindowTextW(g.title, in.title.c_str());
    SetWindowTextW(g.authors, in.authors.empty() ? L"" : (L"By " + in.authors).c_str());
    SetWindowTextW(g.description, in.description.c_str());
    InvalidateRect(g.image, nullptr, FALSE);
}

void fill(int keep)
{
    g.filling = true;
    SendMessageW(g.list, LVM_DELETEALLITEMS, 0, 0);
    for (size_t i = 0; i < g.packs.size(); i++) {
        const Pack &p = g.packs[i];
        const std::wstring title = read_info(p.folder).title;
        LVITEMW it = {};
        it.mask = LVIF_TEXT;
        it.iItem = (int)i;
        it.pszText = const_cast<wchar_t *>(title.c_str());
        SendMessageW(g.list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        std::wstring textures = L"not there";
        if (is_folder(p.folder)) {
            const long n = count_textures(p.folder, 0);
            textures = n == 0 ? L"none" : std::to_wstring(n) + (n == 1 ? L" texture" : L" textures");
        }
        set_cell((int)i, 1, p.folder);
        set_cell((int)i, 2, textures);
        ListView_SetCheckState(g.list, (int)i, p.on ? TRUE : FALSE);
    }
    if (!g.packs.empty()) {
        keep = std::max(0, std::min(keep, (int)g.packs.size() - 1));
        LVITEMW it = {};
        it.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
        it.state = LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageW(g.list, LVM_SETITEMSTATE, (WPARAM)keep, (LPARAM)&it);
        SendMessageW(g.list, LVM_ENSUREVISIBLE, (WPARAM)keep, FALSE);
    }
    g.filling = false;
    buttons();
    show_details();
}

/* `folder' put first in the list (ticked): true.  False when it is there
 * already: not listed twice, ticked and selected where it is. */
bool put_first(const std::wstring &folder)
{
    for (size_t i = 0; i < g.packs.size(); i++)
        if (_wcsicmp(full_path(g.packs[i].folder).c_str(), full_path(folder).c_str()) == 0) {
            g.packs[i].on = true;
            save();
            fill((int)i);
            return false;
        }
    g.packs.insert(g.packs.begin(), Pack{folder, true});
    save();
    fill(0);
    return true;
}

/* A folder the list can take: false (told) when gt3pc.ini cannot keep its
 * path. */
bool usable(const std::wstring &folder)
{
    if (!ini_can_keep(folder)) {
        MessageBoxW(g.wnd,
                    (folder + L"\n\nThe folder's path has characters gt3pc.ini cannot keep. Rename it, or move it to "
                              L"a folder with a plainer path.").c_str(),
                    L"Textures", MB_OK | MB_ICONWARNING);
        return false;
    }
    return true;
}

void add_folder()
{
    const std::wstring folder = util::choose_folder(g.wnd, L"Choose a texture folder (replacement textures named by their hashes)",
                                                    L"Add");
    if (folder.empty() || !usable(folder))
        return;
    if (put_first(folder))
        set_status(L"Added at the top: its textures win over the folders below it. " + summary());
    else
        set_status(L"This folder is in the list already: ticked where it is. " + summary());
}

void remove_folder()
{
    const int i = selected();
    if (i < 0 || i >= (int)g.packs.size())
        return;
    g.packs.erase(g.packs.begin() + i);
    save();
    fill(i);
    /* (the last one gone: the summary alone, as long as the line is) */
    set_status(g.packs.empty() ? summary()
                               : L"Removed from the list (the folder and its files are kept). " + summary());
}

void move(int delta)
{
    const int i = selected(), j = i + delta;
    if (i < 0 || j < 0 || j >= (int)g.packs.size())
        return;
    std::swap(g.packs[(size_t)i], g.packs[(size_t)j]);
    save();
    fill(j);
    SetFocus(g.list);
    set_status(summary());
}

void open_folder()
{
    const int i = selected();
    if (i < 0 || i >= (int)g.packs.size())
        return;
    const std::wstring &f = g.packs[(size_t)i].folder;
    if (!is_folder(f)) {
        set_status(f + L" is not there.");
        return;
    }
    ShellExecuteW(g.wnd, L"open", f.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

/* The game's texture dumps in Explorer: tex_replacements\dumps in the
 * launcher's folder, made first when missing, as tex_replacements\
 * dumps_selected beside it (the game writes its dumps there). */
void open_dumps()
{
    const std::wstring dumps = util::exe_folder() + kDumps;
    util::make_folders(util::exe_folder() + kDumpsSelected);
    if (!util::make_folders(dumps)) {
        set_status(dumps + L" could not be made.");
        return;
    }
    ShellExecuteW(g.wnd, L"open", dumps.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

/* ---- the window ---- */

void columns()
{
    RECT rc;
    GetClientRect(g.list, &rc);
    const int title = px(kTitleW), count = px(kCountW);
    const int folder = std::max(px(100), (int)rc.right - title - count);
    const struct {
        const wchar_t *name;
        int width;
    } cols[] = {{L"Pack", title}, {L"Folder", folder}, {L"Textures", count}};
    for (int i = 0; i < 3; i++) {
        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = const_cast<wchar_t *>(cols[i].name);
        col.cx = cols[i].width;
        if (SendMessageW(g.list, LVM_SETCOLUMNWIDTH, (WPARAM)i, (LPARAM)col.cx) == FALSE)
            SendMessageW(g.list, LVM_INSERTCOLUMNW, (WPARAM)i, (LPARAM)&col);
    }
}

void make_font()
{
    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    HFONT f = nullptr, title = nullptr;
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, g.dpi)) {
        f = CreateFontIndirectW(&ncm.lfMessageFont);
        ncm.lfMessageFont.lfHeight = ncm.lfMessageFont.lfHeight * 4 / 3;
        ncm.lfMessageFont.lfWeight = FW_SEMIBOLD;
        title = CreateFontIndirectW(&ncm.lfMessageFont);
    }
    for (HWND c = GetWindow(g.wnd, GW_CHILD); c != nullptr; c = GetWindow(c, GW_HWNDNEXT))
        SendMessageW(c, WM_SETFONT, (WPARAM)(c == g.title ? title : f), TRUE);
    if (g.font != nullptr)
        DeleteObject(g.font);
    if (g.titleFont != nullptr)
        DeleteObject(g.titleFont);
    g.font = f;
    g.titleFont = title;
}

void layout()
{
    RECT rc;
    GetClientRect(g.wnd, &rc);
    const int W = rc.right, H = rc.bottom, m = px(kM);
    auto put = [](HWND c, int x, int y, int w, int h) { SetWindowPos(c, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE); };
    put(g.hint, m, m, W - 2 * m, px(kHintH));
    const int top = m + px(kHintH) + px(kGap), bottom = H - m - px(kBtnH);
    const int panel = bottom - px(kGap) - px(kImage);
    const int bx = W - m - px(kBtnW);
    put(g.list, m, top, bx - px(kGap) - m, panel - px(kGap) - top);
    int y = top;
    for (HWND b : {g.add, g.remove, g.up, g.down, g.openFolder}) {
        put(b, bx, y, px(kBtnW), px(kBtnH));
        y += px(kBtnH) + px(kGap);
    }
    put(g.dumps, bx, y + px(kGap) * 2, px(kBtnW), px(kBtnH));
    put(g.image, m, panel, px(kImage), px(kImage));
    const int tx = m + px(kImage) + px(kM), tw = W - m - tx, dy = panel + px(kTitleH) + px(kAuthorsH);
    put(g.title, tx, panel, tw, px(kTitleH));
    put(g.authors, tx, panel + px(kTitleH), tw, px(kAuthorsH));
    put(g.description, tx, dy, tw, panel + px(kImage) - dy);
    put(g.status, m, bottom + px(6), bx - px(kGap) - m, px(20));
    put(g.close, bx, bottom, px(kBtnW), px(kBtnH));
    columns();
}

HWND make(const wchar_t *cls, const wchar_t *text, DWORD style, int id, DWORD ex = 0)
{
    return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g.wnd, (HMENU)(INT_PTR)id,
                           (HINSTANCE)GetWindowLongPtrW(g.wnd, GWLP_HINSTANCE), nullptr);
}

void create_controls()
{
    g.hint = make(WC_STATICW,
                  L"Tip: A texture folder holds replacement textures (.png, .jpg, .dds) named by their hashes, as the "
                  L"game dumps them.\n"
                  L"Top = 1st Priority",
                  SS_LEFT | SS_NOPREFIX, IDC_HINT);
    g.list = make(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
                  IDC_LIST, WS_EX_CLIENTEDGE);
    SendMessageW(g.list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                 LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_CHECKBOXES);
    g.add = make(WC_BUTTONW, L"&Add folder...", WS_TABSTOP | BS_PUSHBUTTON, IDC_ADD);
    g.remove = make(WC_BUTTONW, L"&Remove", WS_TABSTOP | BS_PUSHBUTTON, IDC_REMOVE);
    g.up = make(WC_BUTTONW, L"Move &up", WS_TABSTOP | BS_PUSHBUTTON, IDC_UP);
    g.down = make(WC_BUTTONW, L"Move &down", WS_TABSTOP | BS_PUSHBUTTON, IDC_DOWN);
    g.openFolder = make(WC_BUTTONW, L"&Open folder", WS_TABSTOP | BS_PUSHBUTTON, IDC_OPEN);
    g.dumps = make(WC_BUTTONW, L"Open dum&ps folder", WS_TABSTOP | BS_PUSHBUTTON, IDC_DUMPS);
    g.image = make(WC_STATICW, L"", SS_OWNERDRAW, IDC_IMAGE);
    g.title = make(WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, IDC_TITLE);
    g.authors = make(WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, IDC_AUTHORS);
    g.description = make(WC_EDITW, L"", WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                         IDC_DESCRIPTION, WS_EX_CLIENTEDGE);
    g.status = make(WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, IDC_STATUS);
    g.close = make(WC_BUTTONW, L"Close", WS_TABSTOP | BS_PUSHBUTTON, IDCANCEL);
}

LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NOTIFY: {
        const NMHDR *n = reinterpret_cast<const NMHDR *>(lp);
        if (n->hwndFrom != g.list)
            return 0;
        if (n->code == LVN_ITEMCHANGED) {
            const NMLISTVIEW *l = reinterpret_cast<const NMLISTVIEW *>(lp);
            if (!(l->uChanged & LVIF_STATE) || g.filling)
                return 0;
            if (((l->uNewState ^ l->uOldState) & LVIS_STATEIMAGEMASK) && l->iItem >= 0 &&
                l->iItem < (int)g.packs.size()) {
                const bool on = ListView_GetCheckState(g.list, l->iItem) != FALSE;
                if (on != g.packs[(size_t)l->iItem].on) {
                    g.packs[(size_t)l->iItem].on = on;
                    save();
                    set_status(summary());
                }
            }
            if ((l->uNewState ^ l->uOldState) & LVIS_SELECTED) {
                buttons();
                show_details();
            }
        } else if (n->code == LVN_KEYDOWN) {
            if (reinterpret_cast<const NMLVKEYDOWN *>(lp)->wVKey == VK_DELETE)
                remove_folder();
        } else if (n->code == NM_DBLCLK) {
            open_folder();
        }
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_ADD:
            add_folder();
            return 0;
        case IDC_REMOVE:
            remove_folder();
            return 0;
        case IDC_UP:
            move(-1);
            return 0;
        case IDC_DOWN:
            move(1);
            return 0;
        case IDC_OPEN:
            open_folder();
            return 0;
        case IDC_DUMPS:
            open_dumps();
            return 0;
        case IDCANCEL:                      /* Close, Esc */
            DestroyWindow(h);
            return 0;
        }
        break;
    case WM_DPICHANGED: {
        g.dpi = HIWORD(wp);
        const RECT *r = reinterpret_cast<const RECT *>(lp);
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        make_font();
        layout();
        show_details();
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE)
            show_details();
        break;
    case WM_DRAWITEM:
        mod_card::draw_image(reinterpret_cast<const DRAWITEMSTRUCT *>(lp), g.pixels, g.imageW, g.imageH);
        return TRUE;
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == g.description) {    /* (read-only sends this: a box like the list) */
            SetBkColor((HDC)wp, GetSysColor(COLOR_WINDOW));
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        if (g.font != nullptr)
            DeleteObject(g.font);
        if (g.titleFont != nullptr)
            DeleteObject(g.titleFont);
        g = Manager();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

} // namespace

std::vector<Pack> load(const std::wstring &ini)
{
    std::vector<Pack> out;
    const int count = (int)GetPrivateProfileIntW(kSection, L"Count", 0, ini.c_str());
    for (int i = 1; i <= count && i <= kMaxPacks; i++) {
        wchar_t folder[1024] = L"", on[16] = L"";
        const std::wstring key = kKey + std::to_wstring(i);
        GetPrivateProfileStringW(kSection, key.c_str(), L"", folder, 1024, ini.c_str());
        GetPrivateProfileStringW(kSection, (key + L"On").c_str(), L"on", on, 16, ini.c_str());
        if (folder[0] != 0)
            out.push_back(Pack{folder, _wcsicmp(on, L"on") == 0});
    }
    return out;
}

void open(HWND owner, const std::wstring &ini, void (*changed)())
{
    if (g.wnd != nullptr) {
        ShowWindow(g.wnd, SW_RESTORE);
        SetForegroundWindow(g.wnd);
        return;
    }
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.hIcon = (HICON)SendMessageW(owner, WM_GETICON, ICON_BIG, 0);
        wc.hIconSm = (HICON)SendMessageW(owner, WM_GETICON, ICON_SMALL, 0);
        wc.lpszClassName = kClass;
        registered = RegisterClassExW(&wc) != 0;
    }
    g = Manager();
    g.owner = owner;
    g.ini = ini;
    g.changed = changed;
    g.dpi = GetDpiForWindow(owner);
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN, ex = WS_EX_CONTROLPARENT;
    RECT r = {0, 0, px(kW), px(kH)};
    AdjustWindowRectExForDpi(&r, style, FALSE, ex, g.dpi);
    RECT o = {};
    GetWindowRect(owner, &o);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    g.wnd = CreateWindowExW(ex, kClass, L"Textures", style, o.left + ((o.right - o.left) - ww) / 2,
                            o.top + ((o.bottom - o.top) - wh) / 2, ww, wh, owner, nullptr, inst, nullptr);
    if (g.wnd == nullptr)
        return;
    create_controls();
    make_font();
    layout();
    g.packs = load(ini);
    fill(0);
    set_status(summary());
    ShowWindow(g.wnd, SW_SHOW);
    SetFocus(g.packs.empty() ? g.add : g.list);
}

bool is_open()
{
    return g.wnd != nullptr;
}

bool filter(MSG &m)
{
    return g.wnd != nullptr && (m.hwnd == g.wnd || IsChild(g.wnd, m.hwnd)) && IsDialogMessageW(g.wnd, &m);
}

} // namespace texture_manager
