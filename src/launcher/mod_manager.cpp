/* mod_manager.cpp - the mod manager (mod_manager.h). */
#include "mod_manager.h"

#include "mod_card.h"
#include "util.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>

namespace mod_manager {
namespace {

/* PC (2026-11-13): a mod folder's mod.txt and mod.png - their names, their
 * reading, the picture's drawing - are mod_card.h's, the texture manager's
 * too (texture_manager.h); as they were here. */
using mod_card::default_image;
using mod_card::full_path;
using mod_card::Info;
using mod_card::kImageFile;
using mod_card::kInfoFile;
using mod_card::load_image;
using mod_card::read_info;

constexpr const wchar_t *kClass = L"GT3LauncherModManager";
constexpr const wchar_t *kSection = L"Mods";
constexpr int kMaxMods = 256;               /* (the most the game reads: host_file.cpp) */
constexpr int kMaxDepth = 64;               /* (folders deeper not followed: a link loop's guard) */

/* A template's mod.txt. */
constexpr char kTemplateInfo[] =
    "Title: Mod Title\r\n"
    "Author: Author\r\n"
    "Description: This is the description\r\n"
    "of the mod.\r\n";

/* Layout, in 96-DPI units. */
constexpr int kW = 680, kH = 540, kM = 12;
constexpr int kBtnW = 150, kBtnH = 28, kGap = 8, kHintH = 52, kFilesW = 90, kTitleW = 170;
constexpr int kImage = 160, kTitleH = 26, kAuthorsH = 22;
constexpr int kImageEdge = 2;               /* (DrawEdge's, in pixels at any DPI) */

enum : int {
    IDC_HINT = 3001,
    IDC_LIST,
    IDC_ADD,
    IDC_REMOVE,
    IDC_UP,
    IDC_DOWN,
    IDC_OPEN,
    IDC_TEMPLATE,
    IDC_IMAGE,
    IDC_TITLE,
    IDC_AUTHORS,
    IDC_DESCRIPTION,
    IDC_STATUS,
};

struct Manager {
    HWND wnd = nullptr, owner = nullptr;
    HWND hint = nullptr, list = nullptr, add = nullptr, remove = nullptr, up = nullptr, down = nullptr,
         openFolder = nullptr, tmpl = nullptr, image = nullptr, title = nullptr, authors = nullptr,
         description = nullptr, status = nullptr, close = nullptr;
    HFONT font = nullptr, titleFont = nullptr;
    UINT dpi = 96;
    std::wstring ini;
    std::vector<std::wstring> data;         /* the installed versions' data folders */
    void (*changed)() = nullptr;
    std::vector<Mod> mods;
    bool filling = false;                   /* (the list being filled: its check boxes not taken) */
    std::vector<uint32_t> pixels;           /* the selected mod's picture, ready to draw (empty: none) */
    int imageW = 0, imageH = 0;
};

Manager g;

int px(int v) { return MulDiv(v, (int)g.dpi, 96); }

std::wstring lower(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
    return s;
}

/* `inner' is `outer' or a folder under it. */
bool within(const std::wstring &inner, const std::wstring &outer)
{
    const std::wstring i = lower(util::with_slash(full_path(inner))), o = lower(util::with_slash(full_path(outer)));
    return i.compare(0, o.size(), o) == 0;
}

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

/* A file at a mod folder's top that is this window's, not the game's (the
 * game leaves them out too: host_file.cpp mod_scan). */
bool is_info_file(const std::wstring &name)
{
    return _wcsicmp(name.c_str(), kInfoFile) == 0 || _wcsicmp(name.c_str(), kImageFile) == 0;
}

/* The files under `folder' the game takes (not hidden or system ones). */
long count_files(const std::wstring &folder, int depth)
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
        if (name == L"." || name == L".." || (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            n += count_files(util::with_slash(folder) + name, depth + 1);
        else if (depth != 0 || !is_info_file(name))
            n++;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return n;
}

/* `path' made with these bytes; one already there is kept (not an error).
 * False with the reason. */
bool write_new(const std::wstring &path, const void *data, DWORD size, std::wstring *why)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_EXISTS)
            return true;
        *why = path + L": " + util::system_error_text(GetLastError());
        return false;
    }
    DWORD put = 0;
    const bool ok = WriteFile(f, data, size, &put, nullptr) && put == size;
    const DWORD err = GetLastError();
    CloseHandle(f);
    if (!ok) {
        DeleteFileW(path.c_str());
        *why = path + L": " + util::system_error_text(err);
    }
    return ok;
}

/* The folders under `from' (not hidden, system or links) made under `to';
 * *made counts the new ones.  False with the reason. */
bool make_tree(const std::wstring &from, const std::wstring &to, int depth, int *made, std::wstring *why)
{
    if (depth > kMaxDepth)
        return true;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW((util::with_slash(from) + L"*").c_str(), FindExInfoBasic, &fd,
                                FindExSearchLimitToDirectories, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (f == INVALID_HANDLE_VALUE)
        return true;
    bool ok = true;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L".." || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_REPARSE_POINT)))
            continue;
        const std::wstring dst = util::with_slash(to) + name;
        if (CreateDirectoryW(dst.c_str(), nullptr)) {
            (*made)++;
        } else if (GetLastError() != ERROR_ALREADY_EXISTS) {
            *why = dst + L": " + util::system_error_text(GetLastError());
            ok = false;
            break;
        }
        if (!make_tree(util::with_slash(from) + name, dst, depth + 1, made, why)) {
            ok = false;
            break;
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return ok;
}

void set_status(const std::wstring &t) { SetWindowTextW(g.status, t.c_str()); }

std::wstring summary()
{
    const long on = (long)std::count_if(g.mods.begin(), g.mods.end(), [](const Mod &m) { return m.on; });
    if (g.mods.empty())
        return L"No mods installed. Select `Add folder` to add a mod";
    return std::to_wstring(g.mods.size()) + (g.mods.size() == 1 ? L" folder, " : L" folders, ") + std::to_wstring(on) +
           L" enabled.";
}

/* ---- the list ---- */

void save()
{
    WritePrivateProfileStringW(kSection, nullptr, nullptr, g.ini.c_str());     /* (the section written anew) */
    WritePrivateProfileStringW(kSection, L"Count", std::to_wstring(g.mods.size()).c_str(), g.ini.c_str());
    for (size_t i = 0; i < g.mods.size(); i++) {
        const std::wstring n = std::to_wstring(i + 1);
        WritePrivateProfileStringW(kSection, (L"Mod" + n).c_str(), g.mods[i].folder.c_str(), g.ini.c_str());
        WritePrivateProfileStringW(kSection, (L"Mod" + n + L"On").c_str(), g.mods[i].on ? L"on" : L"off",
                                   g.ini.c_str());
    }
    if (g.changed != nullptr)
        g.changed();
}

int selected() { return (int)SendMessageW(g.list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED); }

void buttons()
{
    const int i = selected();
    const int n = (int)g.mods.size();
    EnableWindow(g.add, n < kMaxMods);
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
    if (i >= 0 && i < (int)g.mods.size()) {
        const std::wstring &folder = g.mods[(size_t)i].folder;
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

/* The picture box: the selected row's picture. */
void draw_image(const DRAWITEMSTRUCT *d)
{
    mod_card::draw_image(d, g.pixels, g.imageW, g.imageH);
}

void fill(int keep)
{
    g.filling = true;
    SendMessageW(g.list, LVM_DELETEALLITEMS, 0, 0);
    for (size_t i = 0; i < g.mods.size(); i++) {
        const Mod &m = g.mods[i];
        const std::wstring title = read_info(m.folder).title;
        LVITEMW it = {};
        it.mask = LVIF_TEXT;
        it.iItem = (int)i;
        it.pszText = const_cast<wchar_t *>(title.c_str());
        SendMessageW(g.list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        std::wstring files = L"not there";
        if (is_folder(m.folder)) {
            const long n = count_files(m.folder, 0);
            files = n == 0 ? L"empty" : std::to_wstring(n) + (n == 1 ? L" file" : L" files");
        }
        set_cell((int)i, 1, m.folder);
        set_cell((int)i, 2, files);
        ListView_SetCheckState(g.list, (int)i, m.on ? TRUE : FALSE);
    }
    if (!g.mods.empty()) {
        keep = std::max(0, std::min(keep, (int)g.mods.size() - 1));
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

/* `folder' put first in the list (ticked), or ticked and selected where it
 * is when it is there already. */
void put_first(const std::wstring &folder)
{
    for (size_t i = 0; i < g.mods.size(); i++)
        if (_wcsicmp(full_path(g.mods[i].folder).c_str(), full_path(folder).c_str()) == 0) {
            g.mods[i].on = true;
            save();
            fill((int)i);
            return;
        }
    g.mods.insert(g.mods.begin(), Mod{folder, true});
    save();
    fill(0);
}

/* A folder the list can take: false (told) when gt3pc.ini cannot keep its
 * path or it is a data folder's. */
bool usable(const std::wstring &folder)
{
    if (!ini_can_keep(folder)) {
        MessageBoxW(g.wnd,
                    (folder + L"\n\nThe folder's path has characters gt3pc.ini cannot keep. Rename it, or move it to "
                              L"a folder with a plainer path.").c_str(),
                    L"Mods", MB_OK | MB_ICONWARNING);
        return false;
    }
    for (const std::wstring &d : g.data)
        if (within(folder, d) || within(d, folder)) {
            MessageBoxW(g.wnd,
                        (folder + L"\n\nThis is the game's data folder, or holds or is inside one. Choose a folder of "
                                  L"its own for the mod.").c_str(),
                        L"Mods", MB_OK | MB_ICONWARNING);
            return false;
        }
    return true;
}

void add_folder()
{
    const std::wstring folder = util::choose_folder(g.wnd, L"Choose a mod folder (laid out as the game's data folder)", L"Add");
    if (folder.empty() || !usable(folder))
        return;
    put_first(folder);
    set_status(L"Added at the top: its files win over the folders below it. " + summary());
}

void remove_folder()
{
    const int i = selected();
    if (i < 0 || i >= (int)g.mods.size())
        return;
    g.mods.erase(g.mods.begin() + i);
    save();
    fill(i);
    set_status(L"Removed from the list (the folder and its files are kept). " + summary());
}

void move(int delta)
{
    const int i = selected(), j = i + delta;
    if (i < 0 || j < 0 || j >= (int)g.mods.size())
        return;
    std::swap(g.mods[(size_t)i], g.mods[(size_t)j]);
    save();
    fill(j);
    SetFocus(g.list);
    set_status(summary());
}

void open_folder()
{
    const int i = selected();
    if (i < 0 || i >= (int)g.mods.size())
        return;
    const std::wstring &f = g.mods[(size_t)i].folder;
    if (!is_folder(f)) {
        set_status(f + L" is not there.");
        return;
    }
    ShellExecuteW(g.wnd, L"open", f.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

/* The data folders' folders (none of their files) made in a folder chosen,
 * with a mod.txt and mod.png to change (ones it has are kept), and the folder
 * put first in the list. */
void create_template()
{
    if (g.data.empty()) {
        MessageBoxW(g.wnd, L"No version is installed: install one first (Play tab).", L"Mods",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    const std::wstring folder = util::choose_folder(g.wnd, L"Choose (or make, with New folder) the folder for the template",
                                                    L"Create template", util::exe_folder());
    if (folder.empty() || !usable(folder))
        return;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW((util::with_slash(folder) + L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                nullptr, 0);
    bool empty = true;
    if (f != INVALID_HANDLE_VALUE) {
        do
            empty = empty && (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0);
        while (empty && FindNextFileW(f, &fd));
        FindClose(f);
    }
    if (!empty &&
        MessageBoxW(g.wnd,
                    (folder + L"\n\nThis folder is not empty. Make the data folder's folders in it anyway? Nothing in "
                              L"it is changed or deleted.").c_str(),
                    L"Mods", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    int made = 0;
    std::wstring why;
    bool ok = true;
    for (const std::wstring &d : g.data)
        if (!(ok = make_tree(d, folder, 0, &made, &why)))
            break;
    const void *image = nullptr;
    DWORD imageSize = 0;
    ok = ok && write_new(util::with_slash(folder) + kInfoFile, kTemplateInfo, sizeof(kTemplateInfo) - 1, &why) &&
         (!default_image(&image, &imageSize) ||
          write_new(util::with_slash(folder) + kImageFile, image, imageSize, &why));
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    if (!ok) {
        MessageBoxW(g.wnd, (L"The template could not be made: " + why + L".").c_str(), L"Mods",
                    MB_OK | MB_ICONWARNING);
        if (made == 0)
            return;
    }
    put_first(folder);
    set_status(L"Template made: " + std::to_wstring(made) + L" folders. Edit mod.txt and mod.png; add files where the "
               L"game has its own.");
    if (ok)
        MessageBoxW(g.wnd,
                    (L"Template made. Edit these two files to set the mod's title, author, description and picture:\n\n" +
                     util::with_slash(folder) + kInfoFile + L"\n" + util::with_slash(folder) + kImageFile).c_str(),
                    L"Mods", MB_OK | MB_ICONINFORMATION);
}

/* ---- the window ---- */

void columns()
{
    RECT rc;
    GetClientRect(g.list, &rc);
    const int title = px(kTitleW), files = px(kFilesW);
    const int folder = std::max(px(100), (int)rc.right - title - files);
    const struct {
        const wchar_t *name;
        int width;
    } cols[] = {{L"Mod", title}, {L"Folder", folder}, {L"Files", files}};
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
    put(g.tmpl, bx, y + px(kGap) * 2, px(kBtnW), px(kBtnH));
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
                  L"Tip: A mod folder is laid out the same as the game's data folder and overrides the original files.\n"
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
    g.tmpl = make(WC_BUTTONW, L"Create &template...", WS_TABSTOP | BS_PUSHBUTTON, IDC_TEMPLATE);
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
                l->iItem < (int)g.mods.size()) {
                const bool on = ListView_GetCheckState(g.list, l->iItem) != FALSE;
                if (on != g.mods[(size_t)l->iItem].on) {
                    g.mods[(size_t)l->iItem].on = on;
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
        case IDC_TEMPLATE:
            create_template();
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
        draw_image(reinterpret_cast<const DRAWITEMSTRUCT *>(lp));
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

std::vector<Mod> load(const std::wstring &ini)
{
    std::vector<Mod> out;
    const int count = (int)GetPrivateProfileIntW(kSection, L"Count", 0, ini.c_str());
    for (int i = 1; i <= count && i <= kMaxMods; i++) {
        wchar_t folder[1024] = L"", on[16] = L"";
        const std::wstring n = std::to_wstring(i);
        GetPrivateProfileStringW(kSection, (L"Mod" + n).c_str(), L"", folder, 1024, ini.c_str());
        GetPrivateProfileStringW(kSection, (L"Mod" + n + L"On").c_str(), L"on", on, 16, ini.c_str());
        if (folder[0] != 0)
            out.push_back(Mod{folder, _wcsicmp(on, L"on") == 0});
    }
    return out;
}

void open(HWND owner, const std::wstring &ini, const std::vector<std::wstring> &data, void (*changed)())
{
    if (g.wnd != nullptr) {
        g.data = data;
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
    g.data = data;
    g.changed = changed;
    g.dpi = GetDpiForWindow(owner);
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN, ex = WS_EX_CONTROLPARENT;
    RECT r = {0, 0, px(kW), px(kH)};
    AdjustWindowRectExForDpi(&r, style, FALSE, ex, g.dpi);
    RECT o = {};
    GetWindowRect(owner, &o);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    g.wnd = CreateWindowExW(ex, kClass, L"Mods", style, o.left + ((o.right - o.left) - ww) / 2,
                            o.top + ((o.bottom - o.top) - wh) / 2, ww, wh, owner, nullptr, inst, nullptr);
    if (g.wnd == nullptr)
        return;
    create_controls();
    make_font();
    layout();
    g.mods = load(ini);
    fill(0);
    set_status(summary());
    ShowWindow(g.wnd, SW_SHOW);
    SetFocus(g.mods.empty() ? g.add : g.list);
}

bool is_open()
{
    return g.wnd != nullptr;
}

bool filter(MSG &m)
{
    return g.wnd != nullptr && (m.hwnd == g.wnd || IsChild(g.wnd, m.hwnd)) && IsDialogMessageW(g.wnd, &m);
}

} // namespace mod_manager
