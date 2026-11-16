/* song_manager.cpp - the song manager (song_manager.h). */
#include "song_manager.h"
#include "disc_files.h"                     /* PC (2026-11-15) */

#include "beats.h"
#include "music.h"
#include "util.h"

#include "engine/sound/native/bgm_files.h"

#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <thread>

namespace song_manager {
namespace {

using namespace spu2;

constexpr const wchar_t *kClass = L"GT3LauncherSongManager";

/* Layout, in 96-DPI units. */
constexpr int kW = 720, kH = 520, kM = 12;
constexpr int kBtnW = 120, kBtnH = 28, kEditH = 24, kLabelW = 56, kGap = 8, kBpmW = 64;

enum : int {
    IDC_VERSION = 2001,
    IDC_HINT,
    IDC_TABS,
    IDC_SONGS,
    IDC_TITLE_LABEL,
    IDC_TITLE,
    IDC_ARTIST_LABEL,
    IDC_ARTIST,
    IDC_ADD_SONGS,
    IDC_REMOVE,
    IDC_SLOTS,
    IDC_FILE_LABEL,
    IDC_FILE,
    IDC_ADD_FILE,
    IDC_ORIGINAL,
    IDC_SLOT_HINT,
    IDC_STATUS,
    IDC_VERSION_LABEL,
    IDC_BPM_LABEL,
    IDC_BPM,
    IDC_ADD_FOLDER,
};
constexpr UINT_PTR kSaveTimer = 1;
constexpr UINT_PTR kBpmTimer = 2;           /* a tempo typed: its markers made a moment later */
constexpr UINT WM_APP_MARKERS = WM_APP + 40;    /* lParam: a MarkerJob* done (the receiver deletes it) */
constexpr UINT WM_APP_CHECKED = WM_APP + 41;    /* wParam: its look's serial; lParam: a MusicFile* (the receiver's) */
constexpr UINT WM_APP_ADD_STEP = WM_APP + 42;   /* wParam of lParam songs looked at */
constexpr UINT WM_APP_ADDED = WM_APP + 43;      /* lParam: an AddJob* done (the receiver deletes it) */

/* A song's beat markers made on a thread of their own (beats.h): the song,
 * the tempo; then what came of it. */
struct MarkerJob {
    unsigned serial = 0;
    std::wstring data;
    std::string file;
    double bpm = 0;
    bool ok = false;
    std::wstring why;
    beats::Result result;
    int rate = 0;                           /* the file's rate and samples (a full path's: kept in the list) */
    size_t frames = 0;
};
std::atomic<unsigned> g_jobSerial{0};       /* (not reset with the window: a late job is told by it) */

/* Songs being added, looked at on a thread of their own (a whole library, a
 * slow disk): the files chosen or a folder's; then what they are. */
struct AddJob {
    std::wstring data, folder;              /* folder: "" for the files chosen */
    std::vector<std::wstring> paths;
    std::vector<music::MusicFile> songs;
};
std::atomic<unsigned> g_checkSerial{0};     /* the latest look at the songs outside bgm\ (check_external) */

struct Manager {
    HWND wnd = nullptr, owner = nullptr;
    HWND versionLabel = nullptr, version = nullptr, hint = nullptr, tabs = nullptr;
    HWND songs = nullptr, titleLabel = nullptr, title = nullptr, artistLabel = nullptr, artist = nullptr;
    HWND bpmLabel = nullptr, bpm = nullptr;
    HWND addSongs = nullptr, addFolder = nullptr, remove = nullptr;
    HWND slots = nullptr, fileLabel = nullptr, file = nullptr, addFile = nullptr, original = nullptr, slotHint = nullptr;
    HWND status = nullptr, close = nullptr;
    HFONT font = nullptr;
    UINT dpi = 96;
    std::vector<Version> versions;
    int cur = -1;
    std::wstring data;
    AdsList list, disc;
    bool haveDisc = false;
    std::vector<music::MusicFile> files;
    std::map<std::string, music::MusicFile> external;   /* lower-case full path -> a song outside bgm\ */
    std::map<std::string, int> songRows;    /* lower-case track file -> its row of the race songs */
    bool adding = false;                    /* an AddJob running */
    FILETIME stamp = {};
    int page = 0;                       /* 0 race songs, 1 event music */
    bool filling = false;               /* (the controls being filled: no edits) */
    bool pending = false;               /* an edit not written yet (kSaveTimer) */
    std::vector<int> slotGroups;        /* the event list's rows: their groups */
    std::vector<std::string> fileChoices;   /* the file list's entries ("" silence) */
    std::map<std::string, unsigned> making; /* lower-case file -> the job making its markers */
    int bpmRow = -1;                    /* the song the tempo field shows */
    bool bpmEdited = false;             /* typed, not taken yet (kBpmTimer) */
};

Manager g;
std::set<std::wstring> g_new;           /* lower-case "data|file" */

int px(int v) { return MulDiv(v, (int)g.dpi, 96); }

std::wstring lower(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
    return s;
}

std::string lower8(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; });
    return s;
}

std::wstring bgm_folder() { return util::with_slash(g.data) + L"bgm\\"; }

bool is_new(const std::wstring &file) { return g_new.count(lower(g.data + L"|" + file)) != 0; }

/* The music file a list entry names (or its .wav / .ads twin). */
const music::MusicFile *find_file(const std::string &name)
{
    if (bgm_system_path(name)) {
        auto it = g.external.find(lower8(name));
        return it != g.external.end() ? &it->second : nullptr;
    }
    const std::string a = lower8(name), b = lower8(bgm_other_name(name));
    for (const music::MusicFile &f : g.files) {
        const std::string n = lower8(f.bytes);
        if (n == a || (!b.empty() && n == b))
            return &f;
    }
    return nullptr;
}

/* A track's file without its folder and extension. */
std::wstring shown_stem(const std::string &file)
{
    std::wstring s = music::shown_file(file);
    s = s.substr(s.find_last_of(L"\\/") + 1);
    return s.substr(0, s.find_last_of(L'.'));
}

/* What the game shows (its rules: ads_list_image). */
std::wstring shown_title(const AdsListTrack &k, const music::MusicFile *f)
{
    if (k.hasTitle)
        return music::from_ansi(k.title);
    if (f != nullptr && !f->info.title.empty())
        return music::from_ansi(f->info.title);
    return shown_stem(k.file);
}

std::wstring shown_artist(const AdsListTrack &k, const music::MusicFile *f)
{
    if (k.hasArtist)
        return music::from_ansi(k.artist);
    if (f != nullptr && !f->info.artist.empty())
        return music::from_ansi(f->info.artist);
    return L"unknown";
}

std::wstring describe(const music::MusicFile *f)
{
    if (f != nullptr && f->pending)
        return L"checking...";
    if (f == nullptr || !f->found)
        return L"missing";
    if (!f->playable)
        return L"not a format the game plays";
    /* (PC 2026-11-15: a song the game plays from the disc image) */
    return util::widen(bgm_describe(f->info)) + (f->disc ? L", on the disc" : L"");
}

std::wstring text_of(HWND c)
{
    std::wstring s((size_t)GetWindowTextLengthW(c) + 1, L'\0');
    s.resize((size_t)GetWindowTextW(c, &s[0], (int)s.size()));
    return s;
}

/* One line of the game's bytes: line breaks and control characters out. */
std::string game_text(const std::wstring &w)
{
    std::string s;
    music::ansi(w, &s);
    for (char &c : s)
        if ((unsigned char)c < 0x20)
            c = ' ';
    return s;
}

FILETIME list_stamp()
{
    WIN32_FILE_ATTRIBUTE_DATA a = {};
    if (!GetFileAttributesExW((bgm_folder() + L"ads.txt").c_str(), GetFileExInfoStandard, &a))
        return FILETIME{};
    return a.ftLastWriteTime;
}

void set_status(const std::wstring &t) { SetWindowTextW(g.status, t.c_str()); }

/* ---- the list ---- */

void save()
{
    KillTimer(g.wnd, kSaveTimer);
    g.pending = false;
    std::wstring msg;
    if (music::write_list(g.data, ads_list_text(g.list, music::prober(g.data)), &msg)) {
        g.stamp = list_stamp();
        set_status(L"Saved to bgm\\ads.txt. The game reads it when it starts.");
    } else {
        set_status(msg);
        MessageBoxW(g.wnd, msg.c_str(), L"Songs", MB_OK | MB_ICONWARNING);
    }
}

void flush()
{
    if (g.pending)
        save();
}

int selected(HWND lv) { return (int)SendMessageW(lv, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED); }

void select_row(HWND lv, int i)
{
    const int n = (int)SendMessageW(lv, LVM_GETITEMCOUNT, 0, 0);
    if (n == 0)
        return;
    i = std::max(0, std::min(i, n - 1));
    LVITEMW it = {};
    it.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    it.state = LVIS_SELECTED | LVIS_FOCUSED;
    SendMessageW(lv, LVM_SETITEMSTATE, (WPARAM)i, (LPARAM)&it);
    SendMessageW(lv, LVM_ENSUREVISIBLE, (WPARAM)i, FALSE);
}

void set_cell(HWND lv, int row, int col, const std::wstring &text)
{
    LVITEMW it = {};
    it.iSubItem = col;
    it.pszText = const_cast<wchar_t *>(text.c_str());
    SendMessageW(lv, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&it);
}

void add_row(HWND lv, int row, const std::wstring &first)
{
    LVITEMW it = {};
    it.mask = LVIF_TEXT;
    it.iItem = row;
    it.pszText = const_cast<wchar_t *>(first.c_str());
    SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&it);
}

/* The track has the disc's own beat markers (ads.inf's for its file). */
bool disc_markers(const AdsListTrack &k)
{
    if (!g.haveDisc || bgm_system_path(k.file))
        return false;
    for (const AdsListTrack &d : g.disc.group[ADS_GROUP_RACE_SONGS]) {
        if (lower8(bgm_base_name(d.file)) != lower8(bgm_base_name(k.file)))
            continue;
        bool any = false;
        for (int ch = 0; ch < ADS_MARKER_CHANNELS; ch++) {
            if (d.markers[ch].size() != k.markers[ch].size())
                return false;
            for (size_t i = 0; i < d.markers[ch].size(); i++)
                if (std::fabs(d.markers[ch][i] - k.markers[ch][i]) > 2e-6)
                    return false;
            any = any || !d.markers[ch].empty();
        }
        return any;
    }
    return false;
}

bool has_markers(const AdsListTrack &k)
{
    for (const std::vector<double> &m : k.markers)
        if (!m.empty())
            return true;
    return false;
}

std::wstring bpm_text(double bpm)
{
    wchar_t b[32];
    swprintf(b, 32, L"%g", std::round(bpm * 100.0) / 100.0);
    return b;
}

/* The Markers column: the disc's, made from a tempo, being made, none. */
std::wstring markers_text(const AdsListTrack &k)
{
    if (g.making.count(lower8(k.file)))
        return L"making...";
    if (disc_markers(k))
        return L"the disc's";
    if (!has_markers(k))
        return L"none";
    return k.bpm > 0 ? bpm_text(k.bpm) + L" BPM" : L"set";
}

void song_row(int i)
{
    const AdsListTrack &k = g.list.group[ADS_GROUP_RACE_SONGS][(size_t)i];
    const music::MusicFile *f = find_file(k.file);
    set_cell(g.songs, i, 0, f != nullptr && is_new(f->name) ? L"new" : L"");
    set_cell(g.songs, i, 1, shown_title(k, f));
    set_cell(g.songs, i, 2, shown_artist(k, f));
    set_cell(g.songs, i, 3, markers_text(k));
    set_cell(g.songs, i, 4, music::shown_file(k.file));
    set_cell(g.songs, i, 5, describe(f));
}

void song_fields()
{
    const int i = selected(g.songs);
    const auto &v = g.list.group[ADS_GROUP_RACE_SONGS];
    const bool on = i >= 0 && i < (int)v.size();
    /* the tempo: not for a song with the disc's own markers */
    const bool tempo = on && !disc_markers(v[(size_t)i]);
    g.filling = true;
    SetWindowTextW(g.title, on ? shown_title(v[(size_t)i], find_file(v[(size_t)i].file)).c_str() : L"");
    SetWindowTextW(g.artist, on ? shown_artist(v[(size_t)i], find_file(v[(size_t)i].file)).c_str() : L"");
    SetWindowTextW(g.bpm, tempo && v[(size_t)i].bpm > 0 ? bpm_text(v[(size_t)i].bpm).c_str() : L"");
    g.bpmRow = on ? i : -1;
    g.bpmEdited = false;
    g.filling = false;
    for (HWND c : {g.title, g.artist, g.remove})
        EnableWindow(c, on);
    EnableWindow(g.bpm, tempo);
}

void fill_songs(int keep)
{
    /* (a whole library: filled in one go, not drawn row by row) */
    SendMessageW(g.songs, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g.songs, LVM_DELETEALLITEMS, 0, 0);
    g.songRows.clear();
    const auto &v = g.list.group[ADS_GROUP_RACE_SONGS];
    SendMessageW(g.songs, LVM_SETITEMCOUNT, (WPARAM)v.size(), 0);
    for (size_t i = 0; i < v.size(); i++) {
        add_row(g.songs, (int)i, L"");
        song_row((int)i);
        g.songRows[lower8(v[i].file)] = (int)i;
    }
    SendMessageW(g.songs, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g.songs, nullptr, TRUE);
    select_row(g.songs, keep);
    song_fields();
}

/* The slot's file, its source, its title. */
void slot_row(int row)
{
    const int grp = g.slotGroups[(size_t)row];
    const auto &v = g.list.group[grp];
    set_cell(g.slots, row, 0, util::widen(ads_group_event(grp)));
    std::wstring file = v.empty() ? L"(silence)" : music::shown_file(v[0].file);
    if (v.size() > 1)
        file += L" (+" + std::to_wstring(v.size() - 1) + L" more)";
    set_cell(g.slots, row, 1, file);
    set_cell(g.slots, row, 2, v.empty() ? L"" : shown_title(v[0], find_file(v[0].file)));
    std::wstring source = L"";
    if (g.haveDisc) {
        const auto &d = g.disc.group[grp];
        const bool same = d.size() == v.size() &&
                          (v.empty() || (!bgm_system_path(v[0].file) &&
                                         lower8(bgm_base_name(d[0].file)) == lower8(bgm_base_name(v[0].file))));
        source = same ? L"the disc's" : L"changed";
    }
    set_cell(g.slots, row, 3, source);
}

void slot_fields()
{
    const int row = selected(g.slots);
    const bool on = row >= 0 && row < (int)g.slotGroups.size();
    g.filling = true;
    SendMessageW(g.file, CB_RESETCONTENT, 0, 0);
    g.fileChoices.clear();
    if (on) {
        const auto &v = g.list.group[g.slotGroups[(size_t)row]];
        const music::MusicFile *cur = v.empty() ? nullptr : find_file(v[0].file);
        int sel = 0;
        SendMessageW(g.file, CB_ADDSTRING, 0, (LPARAM)L"(silence)");
        g.fileChoices.push_back(std::string());
        if (!v.empty() && (cur == nullptr || (!cur->found && !cur->pending))) {
            SendMessageW(g.file, CB_ADDSTRING, 0, (LPARAM)(music::shown_file(v[0].file) + L" (missing)").c_str());
            g.fileChoices.push_back(v[0].file);
            sel = 1;
        }
        auto choice = [&](const music::MusicFile &f) {
            if (!f.playable && &f != cur)
                return;
            if (!f.nameOk || (&f == cur && sel == 1))
                return;
            if (&f == cur)
                sel = (int)g.fileChoices.size();
            SendMessageW(g.file, CB_ADDSTRING, 0, (LPARAM)f.name.c_str());
            g.fileChoices.push_back(f.bytes);
        };
        for (const music::MusicFile &f : g.files)
            choice(f);
        for (const auto &e : g.external)    /* (the songs added from elsewhere) */
            choice(e.second);
        SendMessageW(g.file, CB_SETCURSEL, (WPARAM)sel, 0);
    }
    g.filling = false;
    for (HWND c : {g.file, g.addFile})
        EnableWindow(c, on);
    EnableWindow(g.original, on && g.haveDisc);
}

void fill_slots(int keep)
{
    SendMessageW(g.slots, LVM_DELETEALLITEMS, 0, 0);
    g.slotGroups.clear();
    for (int grp = 0; grp < ADS_GROUPS; grp++) {
        if (grp == ADS_GROUP_RACE_SONGS)
            continue;
        add_row(g.slots, (int)g.slotGroups.size(), L"");
        g.slotGroups.push_back(grp);
        slot_row((int)g.slotGroups.size() - 1);
    }
    select_row(g.slots, keep);
    slot_fields();
}

/* The songs outside bgm\ looked at on a thread of their own, which a drive
 * asleep or a network folder not answering can hold up: their rows say
 * "checking..." and fill in as the answers come (song_checked).  One looked
 * at stays known; one that was missing is looked at again. */
void check_external()
{
    std::vector<std::string> files;
    for (const std::vector<AdsListTrack> &grp : g.list.group)
        for (const AdsListTrack &k : grp) {
            if (!bgm_system_path(k.file))
                continue;
            music::MusicFile &f = g.external[lower8(k.file)];
            if (f.bytes.empty()) {
                f.bytes = k.file;
                f.name = music::shown_file(k.file);
                f.nameOk = true;
            }
            if (!f.found) {
                f.pending = true;
                files.push_back(k.file);
            }
        }
    const unsigned serial = ++g_checkSerial;
    if (files.empty())
        return;
    const HWND wnd = g.wnd;
    std::thread([files, serial, wnd] {
        for (const std::string &file : files) {
            if (g_checkSerial != serial)
                return;                 /* (a newer look, or the window closed) */
            music::MusicFile *m = new music::MusicFile(music::probe_song(music::shown_file(file)));
            m->bytes = file;
            if (!PostMessageW(wnd, WM_APP_CHECKED, serial, (LPARAM)m)) {
                delete m;
                return;
            }
        }
    }).detach();
}

/* An answer of check_external: the song's rows filled in. */
void song_checked(unsigned serial, music::MusicFile *m)
{
    std::unique_ptr<music::MusicFile> own(m);
    if (serial != g_checkSerial)
        return;
    const std::string key = lower8(m->bytes);
    auto it = g.external.find(key);
    if (it == g.external.end())
        return;
    it->second = *m;
    auto row = g.songRows.find(key);
    if (row != g.songRows.end())
        song_row(row->second);
    for (size_t i = 0; i < g.slotGroups.size(); i++) {
        const auto &v = g.list.group[g.slotGroups[i]];
        if (!v.empty() && lower8(v[0].file) == key)
            slot_row((int)i);
    }
}

/* Version v's list, files and the disc's list read anew. */
void load(int v)
{
    flush();
    g.cur = v;
    g.data = g.versions[(size_t)v].data;
    std::vector<std::string> warnings;
    g.list = AdsList();
    ads_list_parse(music::list_text(g.data), &g.list, &warnings);
    g.disc = AdsList();
    g.haveDisc = music::original_list(g.data, &g.disc);
    g.making.clear();                   /* (a job still running: its result is not taken) */
    g.files = music::music_files(g.data);
    /* PC (2026-11-15): a disc image's folder: the songs the game plays from the image */
    for (music::MusicFile &m : music::disc_music(g.data))
        g.files.push_back(std::move(m));
    check_external();
    g.stamp = list_stamp();
    SetWindowTextW(g.wnd, (L"Songs - " + g.versions[(size_t)v].name).c_str());
    SendMessageW(g.version, CB_SETCURSEL, (WPARAM)v, 0);
    TCITEMW ti = {};
    ti.mask = TCIF_TEXT;
    std::wstring t0 = L"Race songs (" + std::to_wstring(g.list.group[ADS_GROUP_RACE_SONGS].size()) + L")";
    ti.pszText = &t0[0];
    SendMessageW(g.tabs, TCM_SETITEMW, 0, (LPARAM)&ti);
    fill_songs(selected(g.songs));
    fill_slots(selected(g.slots));
    set_status(warnings.empty() ? L"Changes are saved to bgm\\ads.txt as they are made."
                                : L"bgm\\ads.txt has lines the game skips (" + std::to_wstring(warnings.size()) +
                                      L"); saving writes the list anew without them.");
}

/* The files chosen in an Open dialog (one or several): full paths. */
std::vector<std::wstring> choose_files(bool many, const wchar_t *title)
{
    std::wstring buf(65536, L'\0');
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.wnd;
    ofn.lpstrFilter = L"Music (*.wav, *.mp3)\0*.wav;*.mp3\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = &buf[0];
    ofn.nMaxFile = (DWORD)buf.size();
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR |
                (many ? OFN_ALLOWMULTISELECT : 0);
    std::vector<std::wstring> out;
    if (!GetOpenFileNameW(&ofn))
        return out;
    /* one file: its path; several: the folder, then the names */
    const wchar_t *p = buf.c_str();
    std::wstring first = p;
    p += first.size() + 1;
    if (*p == 0) {
        out.push_back(first);
        return out;
    }
    while (*p != 0) {
        std::wstring name = p;
        out.push_back(util::with_slash(first) + name);
        p += name.size() + 1;
    }
    return out;
}

/* A new track of a music file: its tags' title and artist, else its name
 * and "unknown" (written out: the list is where they are edited). */
AdsListTrack track_of(const std::string &bytes)
{
    const music::MusicFile *f = find_file(bytes);
    AdsListTrack k;
    k.file = f != nullptr ? f->bytes : bytes;
    k.title = f != nullptr && !f->info.title.empty() ? f->info.title : game_text(shown_stem(bytes));
    k.artist = f != nullptr && !f->info.artist.empty() ? f->info.artist : std::string("unknown");
    k.hasTitle = k.hasArtist = true;
    return k;
}

void set_adding(bool on)
{
    g.adding = on;
    for (HWND c : {g.addSongs, g.addFolder, g.version})
        EnableWindow(c, !on);
}

/* The files `paths', or those of `folder', looked at on a thread of their
 * own, then added (add_done); the status line counts them meanwhile. */
void start_add(std::vector<std::wstring> paths, const std::wstring &folder)
{
    AddJob *job = new AddJob;
    job->data = g.data;
    job->folder = folder;
    job->paths = std::move(paths);
    set_adding(true);
    set_status(L"Looking at the songs...");
    const HWND wnd = g.wnd;
    std::thread([job, wnd] {
        if (!job->folder.empty())
            job->paths = music::folder_songs(job->folder);
        const size_t n = job->paths.size();
        for (size_t i = 0; i < n; i++) {
            music::MusicFile m = music::probe_song(job->paths[i]);
            m.bytes = music::track_file(job->data, job->paths[i]);
            job->songs.push_back(std::move(m));
            if ((i & 7) == 7 && !PostMessageW(wnd, WM_APP_ADD_STEP, (WPARAM)(i + 1), (LPARAM)n)) {
                delete job;             /* (the window closed meanwhile) */
                return;
            }
        }
        if (!PostMessageW(wnd, WM_APP_ADDED, 0, (LPARAM)job))
            delete job;
    }).detach();
}

/* A job done: its songs on the list, by where they are (nothing copied). */
void add_done(AddJob *job)
{
    std::unique_ptr<AddJob> own(job);
    set_adding(false);
    if (job->data != g.data)
        return;
    auto &v = g.list.group[ADS_GROUP_RACE_SONGS];
    std::set<std::string> have;
    for (const AdsListTrack &k : v)
        have.insert(lower8(k.file));
    std::vector<std::wstring> problems;
    int added = 0, there = 0;
    for (const music::MusicFile &m : job->songs) {
        const std::string key = lower8(m.bytes);
        const bool system = bgm_system_path(m.bytes);
        if (!job->folder.empty() && !system && key.compare(0, 2, "i_") == 0)
            continue;                   /* (the bgm folder itself: an intro is no song) */
        if (!m.playable) {
            problems.push_back(m.name + L": not a format the game plays (a .wav of PCM or float samples, an .mp3)");
            continue;
        }
        if (!have.insert(key).second) {
            there++;
            continue;
        }
        if (system) {
            g.external[key] = m;
        } else {
            auto &out = g.list.keptOut[ADS_GROUP_RACE_SONGS];
            out.erase(std::remove_if(out.begin(), out.end(), [&](const std::string &f) { return lower8(f) == key; }),
                      out.end());
        }
        v.push_back(track_of(m.bytes));
        g_new.insert(lower(g.data + L"|" + music::shown_file(m.bytes)));
        added++;
    }
    if (added > 0) {
        save();
        load(g.cur);
        select_row(g.songs, (int)v.size() - 1);
        song_fields();
    }
    std::wstring status = job->songs.empty() ? L"No .wav or .mp3 files there."
                          : added == 0       ? L"No songs added."
                                             : std::to_wstring(added) + (added == 1 ? L" song added." : L" songs added.") +
                                             L" They play from where they are: nothing was copied.";
    if (there != 0)
        status += L" " + std::to_wstring(there) + L" already on the list.";
    set_status(status);
    if (!problems.empty()) {
        std::wstring m = L"Not added:";
        for (size_t i = 0; i < problems.size() && i < 12; i++)
            m += L"\n  " + problems[i];
        if (problems.size() > 12)
            m += L"\n  (" + std::to_wstring(problems.size() - 12) + L" more)";
        MessageBoxW(g.wnd, m.c_str(), L"Songs", MB_OK | MB_ICONWARNING);
    }
}

void add_songs()
{
    std::vector<std::wstring> chosen = choose_files(true, L"Add songs to the race music (.wav, .mp3)");
    if (!chosen.empty())
        start_add(std::move(chosen), std::wstring());
}

void add_folder()
{
    const std::wstring folder =
        util::choose_folder(g.wnd, L"Add a folder of songs to the race music (with the folders under it)", L"Add");
    if (!folder.empty())
        start_add(std::vector<std::wstring>(), folder);
}

/* The song off the list; no file is ever deleted.  A file of bgm\ is kept
 * out ("# track FILE": the launcher would add it again). */
void remove_song()
{
    auto &v = g.list.group[ADS_GROUP_RACE_SONGS];
    const int i = selected(g.songs);
    if (i < 0 || i >= (int)v.size())
        return;
    const AdsListTrack k = v[(size_t)i];
    const std::wstring title = shown_title(k, find_file(k.file));
    v.erase(v.begin() + i);
    if (!bgm_system_path(k.file))
        g.list.keptOut[ADS_GROUP_RACE_SONGS].push_back(k.file);
    save();
    load(g.cur);
    select_row(g.songs, i);
    song_fields();
    set_status(L"\"" + title + L"\" is off the race songs. Its file was not touched.");
}

/* The slot of the event list's row `row' given file `bytes' ("" silence). */
void assign_slot(int row, const std::string &bytes)
{
    const int grp = g.slotGroups[(size_t)row];
    auto &v = g.list.group[grp];
    if (bytes.empty()) {
        v.clear();
    } else {
        /* (x.ads and x.wav in bgm\ are one song; full paths must be the same) */
        if (v.size() == 1 &&
            (bgm_system_path(bytes) || bgm_system_path(v[0].file)
                 ? lower8(v[0].file) == lower8(bytes)
                 : lower8(bgm_base_name(v[0].file)) == lower8(bgm_base_name(bytes))))
            return;
        v.assign(1, track_of(bytes));
    }
    save();
    slot_row(row);
}

void original_slot()
{
    const int row = selected(g.slots);
    if (row < 0 || !g.haveDisc)
        return;
    const int grp = g.slotGroups[(size_t)row];
    g.list.group[grp] = g.disc.group[grp];
    for (AdsListTrack &k : g.list.group[grp])
        if (const music::MusicFile *f = find_file(k.file))
            k.file = f->bytes;          /* (the .wav the launcher made of its .ads) */
    save();
    slot_row(row);
    slot_fields();
}

void add_slot_file()
{
    const int row = selected(g.slots);
    if (row < 0)
        return;
    std::vector<std::wstring> chosen = choose_files(false, L"Choose the music for this event (.wav, .mp3)");
    if (chosen.empty())
        return;
    music::MusicFile m = music::probe_song(chosen[0]);
    m.bytes = music::track_file(g.data, chosen[0]);
    if (!m.playable) {
        const std::wstring why = m.name + L": not a format the game plays (a .wav of PCM or float samples, an .mp3)";
        MessageBoxW(g.wnd, why.c_str(), L"Songs", MB_OK | MB_ICONWARNING);
        return;
    }
    if (bgm_system_path(m.bytes))
        g.external[lower8(m.bytes)] = m;
    assign_slot(row, m.bytes);
    fill_songs(selected(g.songs));      /* (the new file's formats) */
    slot_fields();
}

/* ---- the window ---- */

void show_page(int p)
{
    g.page = p;
    for (HWND c : {g.songs, g.titleLabel, g.title, g.artistLabel, g.artist, g.bpmLabel, g.bpm, g.addSongs, g.addFolder, g.remove})
        ShowWindow(c, p == 0 ? SW_SHOW : SW_HIDE);
    for (HWND c : {g.slots, g.fileLabel, g.file, g.addFile, g.original, g.slotHint})
        ShowWindow(c, p == 1 ? SW_SHOW : SW_HIDE);
}

void columns()
{
    struct Col {
        HWND lv;
        const wchar_t *name;
        int width;
    };
    const Col cols[] = {{g.songs, L"", 40},      {g.songs, L"Title", 210}, {g.songs, L"Artist", 130},
                        {g.songs, L"Markers", 80}, {g.songs, L"File", 240}, {g.songs, L"Format", 230},
                        {g.slots, L"Event", 210}, {g.slots, L"Music file", 170}, {g.slots, L"Title", 200},
                        {g.slots, L"Source", 80}};
    int index[2] = {0, 0};
    for (const Col &c : cols) {
        const int k = c.lv == g.songs ? 0 : 1;
        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = const_cast<wchar_t *>(c.name);
        col.cx = px(c.width);
        if (SendMessageW(c.lv, LVM_SETCOLUMNWIDTH, (WPARAM)index[k], (LPARAM)col.cx) == FALSE)
            SendMessageW(c.lv, LVM_INSERTCOLUMNW, (WPARAM)index[k], (LPARAM)&col);
        index[k]++;
    }
}

void make_font()
{
    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    HFONT f = nullptr;
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, g.dpi))
        f = CreateFontIndirectW(&ncm.lfMessageFont);
    for (HWND c = GetWindow(g.wnd, GW_CHILD); c != nullptr; c = GetWindow(c, GW_HWNDNEXT))
        SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
    if (g.font != nullptr)
        DeleteObject(g.font);
    g.font = f;
}

void layout()
{
    RECT rc;
    GetClientRect(g.wnd, &rc);
    const int W = rc.right, H = rc.bottom, m = px(kM);
    auto put = [](HWND c, int x, int y, int w, int h) { SetWindowPos(c, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE); };
    put(g.versionLabel, m, m + px(4), px(kLabelW), px(20));
    put(g.version, m + px(kLabelW), m, px(280), px(200));
    put(g.hint, m + px(kLabelW) + px(280) + px(16), m, W - (m + px(kLabelW) + px(296)) - m, px(36));
    const int tabY = m + px(40), bottom = H - m - px(kBtnH);
    put(g.tabs, m, tabY, W - 2 * m, bottom - px(kGap) - tabY);
    RECT in = {m, tabY, W - m, bottom - px(kGap)};
    SendMessageW(g.tabs, TCM_ADJUSTRECT, FALSE, (LPARAM)&in);
    const int x = in.left + px(4), w = in.right - in.left - px(8);
    const int fieldsY = in.bottom - px(4) - px(kBtnH) - px(kGap) - px(kEditH);
    const int listH = fieldsY - px(kGap) - (in.top + px(4));
    /* race songs */
    put(g.songs, x, in.top + px(4), w, listH);
    const int half = (w - 3 * px(kLabelW) - px(kBpmW) - 2 * px(16)) / 2;
    put(g.titleLabel, x, fieldsY + px(3), px(kLabelW), px(20));
    put(g.title, x + px(kLabelW), fieldsY, half, px(kEditH));
    put(g.artistLabel, x + px(kLabelW) + half + px(16), fieldsY + px(3), px(kLabelW), px(20));
    put(g.artist, x + 2 * px(kLabelW) + half + px(16), fieldsY, half, px(kEditH));
    put(g.bpmLabel, x + 2 * px(kLabelW) + 2 * half + 2 * px(16), fieldsY + px(3), px(kLabelW), px(20));
    put(g.bpm, x + 3 * px(kLabelW) + 2 * half + 2 * px(16), fieldsY, px(kBpmW), px(kEditH));
    const int by = in.bottom - px(4) - px(kBtnH);
    put(g.addSongs, x, by, px(kBtnW), px(kBtnH));
    put(g.addFolder, x + px(kBtnW) + px(kGap), by, px(kBtnW), px(kBtnH));
    put(g.remove, x + 2 * (px(kBtnW) + px(kGap)), by, px(kBtnW), px(kBtnH));
    /* event music */
    put(g.slots, x, in.top + px(4), w, listH);
    put(g.fileLabel, x, fieldsY + px(3), px(80), px(20));
    put(g.file, x + px(80), fieldsY, px(300), px(240));
    put(g.addFile, x + px(80) + px(300) + px(kGap), fieldsY - px(2), px(kBtnW), px(kBtnH));
    put(g.original, x + px(80) + px(300) + px(kGap) + px(kBtnW) + px(kGap), fieldsY - px(2), px(kBtnW), px(kBtnH));
    put(g.slotHint, x, by + px(4), w, px(20));
    /* the bottom */
    put(g.status, m, bottom + px(6), W - 3 * m - px(kBtnW), px(20));
    put(g.close, W - m - px(kBtnW), bottom, px(kBtnW), px(kBtnH));
    columns();
}

HWND make(const wchar_t *cls, const wchar_t *text, DWORD style, int id, DWORD ex = 0)
{
    return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g.wnd, (HMENU)(INT_PTR)id,
                           (HINSTANCE)GetWindowLongPtrW(g.wnd, GWLP_HINSTANCE), nullptr);
}

void create_controls()
{
    g.versionLabel = make(WC_STATICW, L"Version", SS_LEFT, IDC_VERSION_LABEL);
    g.version = make(WC_COMBOBOXW, L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, IDC_VERSION);
    for (const Version &v : g.versions)
        SendMessageW(g.version, CB_ADDSTRING, 0, (LPARAM)v.name.c_str());
    g.hint = make(WC_STATICW,
                  L"Tip: Music files are in the bgm folder."
                  L"Optional: Add the beats per minute to get replay markers for a song",
                  SS_LEFT | SS_NOPREFIX, IDC_HINT);
    g.tabs = make(WC_TABCONTROLW, L"", WS_TABSTOP | WS_CLIPSIBLINGS, IDC_TABS);
    TCITEMW ti = {};
    ti.mask = TCIF_TEXT;
    wchar_t t0[] = L"Race songs", t1[] = L"Event music";
    ti.pszText = t0;
    SendMessageW(g.tabs, TCM_INSERTITEMW, 0, (LPARAM)&ti);
    ti.pszText = t1;
    SendMessageW(g.tabs, TCM_INSERTITEMW, 1, (LPARAM)&ti);
    const DWORD lvStyle = WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER;
    g.songs = make(WC_LISTVIEWW, L"", lvStyle, IDC_SONGS, WS_EX_CLIENTEDGE);
    g.titleLabel = make(WC_STATICW, L"Title", SS_LEFT, IDC_TITLE_LABEL);
    g.title = make(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, IDC_TITLE, WS_EX_CLIENTEDGE);
    g.artistLabel = make(WC_STATICW, L"Artist", SS_LEFT, IDC_ARTIST_LABEL);
    g.artist = make(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, IDC_ARTIST, WS_EX_CLIENTEDGE);
    g.bpmLabel = make(WC_STATICW, L"BPM", SS_RIGHT, IDC_BPM_LABEL);
    g.bpm = make(WC_EDITW, L"", WS_TABSTOP | ES_AUTOHSCROLL, IDC_BPM, WS_EX_CLIENTEDGE);
    g.addSongs = make(WC_BUTTONW, L"&Add files...", WS_TABSTOP | BS_PUSHBUTTON, IDC_ADD_SONGS);
    g.addFolder = make(WC_BUTTONW, L"Add a &folder...", WS_TABSTOP | BS_PUSHBUTTON, IDC_ADD_FOLDER);
    g.remove = make(WC_BUTTONW, L"&Remove", WS_TABSTOP | BS_PUSHBUTTON, IDC_REMOVE);
    g.slots = make(WC_LISTVIEWW, L"", lvStyle, IDC_SLOTS, WS_EX_CLIENTEDGE);
    g.fileLabel = make(WC_STATICW, L"Music file", SS_LEFT, IDC_FILE_LABEL);
    g.file = make(WC_COMBOBOXW, L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, IDC_FILE);
    g.addFile = make(WC_BUTTONW, L"A&dd a file...", WS_TABSTOP | BS_PUSHBUTTON, IDC_ADD_FILE);
    g.original = make(WC_BUTTONW, L"&Original", WS_TABSTOP | BS_PUSHBUTTON, IDC_ORIGINAL);
    g.slotHint = make(WC_STATICW,
                      L"Each event plays one music file. Original: the disc's. Silence: no music for the event.",
                      SS_LEFT | SS_NOPREFIX, IDC_SLOT_HINT);
    g.status = make(WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, IDC_STATUS);
    g.close = make(WC_BUTTONW, L"Close", WS_TABSTOP | BS_PUSHBUTTON, IDCANCEL);
    for (HWND lv : {g.songs, g.slots})
        SendMessageW(lv, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    /* (the tab control under the pages' controls it frames) */
    SetWindowPos(g.tabs, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SendMessageW(g.title, EM_SETLIMITTEXT, 120, 0);
    SendMessageW(g.artist, EM_SETLIMITTEXT, 120, 0);
    SendMessageW(g.bpm, EM_SETLIMITTEXT, 8, 0);
    SendMessageW(g.bpm, EM_SETCUEBANNER, FALSE, (LPARAM)L"tempo");
}

/* A title / artist edited: into the list, written a moment later. */
void field_changed(bool title)
{
    if (g.filling)
        return;
    const int i = selected(g.songs);
    auto &v = g.list.group[ADS_GROUP_RACE_SONGS];
    if (i < 0 || i >= (int)v.size())
        return;
    AdsListTrack &k = v[(size_t)i];
    const std::wstring t = text_of(title ? g.title : g.artist);
    if (title) {
        k.title = game_text(t);
        k.hasTitle = true;
    } else {
        k.artist = game_text(t);
        k.hasArtist = true;
    }
    song_row(i);
    g.pending = true;
    SetTimer(g.wnd, kSaveTimer, 700, nullptr);
}

/* ---- the beat markers (beats.h) ---- */

/* The tempo typed for the song the field shows, taken: kept with the list
 * and the song's markers made from it on a thread of their own (channel 1
 * the beats, 2 the bars, 3 and 4 the camera cuts, as the discs' songs
 * have); emptied, the markers made from it gone. */
void commit_bpm()
{
    KillTimer(g.wnd, kBpmTimer);
    if (!g.bpmEdited)
        return;
    g.bpmEdited = false;
    auto &v = g.list.group[ADS_GROUP_RACE_SONGS];
    const int i = g.bpmRow;
    if (i < 0 || i >= (int)v.size() || disc_markers(v[(size_t)i]))
        return;
    AdsListTrack &k = v[(size_t)i];
    const std::wstring title = shown_title(k, find_file(k.file));
    const std::wstring t = text_of(g.bpm);
    if (t.find_first_not_of(L" \t") == std::wstring::npos) {
        if (k.bpm <= 0 && !g.making.count(lower8(k.file)))
            return;
        k.bpm = 0;
        for (int ch = 1; ch <= 4; ch++)
            k.markers[ch].clear();
        if (bgm_system_path(k.file)) {
            k.rate = 0;
            k.length = -1;
        }
        g.making.erase(lower8(k.file));
        save();
        song_row(i);
        set_status(L"\"" + title + L"\": no tempo, no beat markers (the replay cameras cut by time).");
        return;
    }
    wchar_t *end = nullptr;
    const double bpm = wcstod(t.c_str(), &end);
    while (end != nullptr && (*end == L' ' || *end == L'\t'))
        end++;
    if (end == nullptr || *end != 0 || !(bpm >= beats::kMinBpm && bpm <= beats::kMaxBpm)) {
        set_status(L"BPM: the song's beats a minute, a number from " + bpm_text(beats::kMinBpm) + L" to " +
                   bpm_text(beats::kMaxBpm) + L".");
        return;
    }
    if (std::fabs(bpm - k.bpm) < 1e-9 && has_markers(k))
        return;
    k.bpm = bpm;
    save();
    MarkerJob *job = new MarkerJob;
    job->serial = ++g_jobSerial;
    job->data = g.data;
    job->file = k.file;
    job->bpm = bpm;
    g.making[lower8(k.file)] = job->serial;
    song_row(i);
    set_status(L"Making the beat markers of \"" + title + L"\" at " + bpm_text(bpm) + L" BPM...");
    const HWND wnd = g.wnd;
    std::thread([job, wnd] {
        std::vector<float> mono;
        int rate = 0;
        job->ok = music::song_samples(job->data, job->file, &mono, &rate, &job->why) &&
                  beats::analyze(mono.data(), mono.size(), rate, job->bpm, &job->result);
        job->rate = rate;
        job->frames = mono.size();
        if (!job->ok && job->why.empty())
            job->why = L"no beats found in its sound";
        if (!PostMessageW(wnd, WM_APP_MARKERS, 0, (LPARAM)job))
            delete job;                 /* (the window closed meanwhile) */
    }).detach();
}

/* A job done: its markers into the song (unless the song, its tempo or the
 * version changed meanwhile), written. */
void markers_done(MarkerJob *job)
{
    std::unique_ptr<MarkerJob> own(job);
    auto it = g.making.find(lower8(job->file));
    if (it == g.making.end() || it->second != job->serial || job->data != g.data)
        return;
    g.making.erase(it);
    auto &v = g.list.group[ADS_GROUP_RACE_SONGS];
    for (size_t i = 0; i < v.size(); i++) {
        AdsListTrack &k = v[i];
        if (lower8(k.file) != lower8(job->file))
            continue;
        const std::wstring title = shown_title(k, find_file(k.file));
        if (job->ok && std::fabs(k.bpm - job->bpm) < 1e-9) {
            const beats::Result &r = job->result;
            for (std::vector<double> &m : k.markers)
                m.clear();
            k.markers[1] = r.beats;
            k.markers[2] = r.bars;
            k.markers[3] = r.cuts;
            k.markers[4] = r.cuts;
            /* a full path's: the markers' time base in the list, so the game's
             * start need not open the file (ads_list_image) */
            if (bgm_system_path(k.file)) {
                k.rate = (uint32_t)job->rate;
                k.length = (int64_t)job->frames;
            }
            save();
            set_status(L"\"" + title + L"\": " + std::to_wstring(r.beats.size()) + L" beats, " +
                       std::to_wstring(r.bars.size()) + L" bars, " + std::to_wstring(r.cuts.size()) +
                       L" camera cuts. The replays' cameras follow them.");
        } else if (!job->ok) {
            set_status(L"The beat markers of \"" + title + L"\" were not made: " + job->why + L".");
        }
        song_row((int)i);
        break;
    }
}

LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER:
        if (wp == kSaveTimer)
            save();
        else if (wp == kBpmTimer)
            commit_bpm();
        return 0;
    case WM_APP_MARKERS:
        markers_done(reinterpret_cast<MarkerJob *>(lp));
        return 0;
    case WM_APP_CHECKED:
        song_checked((unsigned)wp, reinterpret_cast<music::MusicFile *>(lp));
        return 0;
    case WM_APP_ADD_STEP:
        set_status(L"Looking at the songs... " + std::to_wstring((size_t)wp) + L" of " + std::to_wstring((size_t)lp));
        return 0;
    case WM_APP_ADDED:
        add_done(reinterpret_cast<AddJob *>(lp));
        return 0;
    case WM_ACTIVATE:
        /* the list written meanwhile (the launcher's new songs): read again */
        if (LOWORD(wp) != WA_INACTIVE && g.cur >= 0 && !g.pending && !g.adding) {
            const FILETIME s = list_stamp();
            if (CompareFileTime(&s, &g.stamp) != 0)
                load(g.cur);
        }
        break;
    case WM_NOTIFY: {
        const NMHDR *n = reinterpret_cast<const NMHDR *>(lp);
        if (n->hwndFrom == g.tabs && n->code == TCN_SELCHANGE) {
            show_page((int)SendMessageW(g.tabs, TCM_GETCURSEL, 0, 0));
        } else if (n->code == LVN_ITEMCHANGED) {
            const NMLISTVIEW *l = reinterpret_cast<const NMLISTVIEW *>(lp);
            if ((l->uChanged & LVIF_STATE) && ((l->uNewState ^ l->uOldState) & LVIS_SELECTED)) {
                if (n->hwndFrom == g.songs) {
                    commit_bpm();       /* (the song the field showed) */
                    flush();
                    song_fields();
                } else if (n->hwndFrom == g.slots) {
                    slot_fields();
                }
            }
        }
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_VERSION:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                const int v = (int)SendMessageW(g.version, CB_GETCURSEL, 0, 0);
                if (v >= 0 && v != g.cur)
                    load(v);
            }
            return 0;
        case IDC_TITLE:
        case IDC_ARTIST:
            if (HIWORD(wp) == EN_CHANGE)
                field_changed(LOWORD(wp) == IDC_TITLE);
            else if (HIWORD(wp) == EN_KILLFOCUS)
                flush();
            return 0;
        case IDC_BPM:
            if (HIWORD(wp) == EN_CHANGE && !g.filling) {
                g.bpmEdited = true;
                SetTimer(g.wnd, kBpmTimer, 900, nullptr);
            } else if (HIWORD(wp) == EN_KILLFOCUS) {
                commit_bpm();
            }
            return 0;
        case IDC_ADD_SONGS:
            flush();
            add_songs();
            return 0;
        case IDC_ADD_FOLDER:
            flush();
            add_folder();
            return 0;
        case IDC_REMOVE:
            flush();
            remove_song();
            return 0;
        case IDC_FILE:
            if (HIWORD(wp) == CBN_SELCHANGE && !g.filling) {
                const int row = selected(g.slots), c = (int)SendMessageW(g.file, CB_GETCURSEL, 0, 0);
                if (row >= 0 && c >= 0 && c < (int)g.fileChoices.size())
                    assign_slot(row, g.fileChoices[(size_t)c]);
            }
            return 0;
        case IDC_ADD_FILE:
            add_slot_file();
            return 0;
        case IDC_ORIGINAL:
            original_slot();
            return 0;
        case IDOK:                          /* Enter in a field: written now */
            commit_bpm();
            flush();
            return 0;
        case IDCANCEL:                      /* Close, Esc */
            DestroyWindow(h);
            return 0;
        }
        break;
    case DM_GETDEFID:
        return MAKELRESULT(IDOK, DC_HASDEFID);
    case WM_DPICHANGED: {
        g.dpi = HIWORD(wp);
        const RECT *r = reinterpret_cast<const RECT *>(lp);
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        make_font();
        layout();
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        flush();
        ++g_checkSerial;                /* (its look ends) */
        disc_files::release_all();      /* PC (2026-11-15): the disc images let go of */
        if (g.font != nullptr)
            DeleteObject(g.font);
        g = Manager();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

} // namespace

void open(HWND owner, const std::vector<Version> &versions, int version)
{
    if (versions.empty())
        return;
    version = std::max(0, std::min(version, (int)versions.size() - 1));
    if (g.wnd != nullptr) {
        if (g.versions.size() == versions.size() && version != g.cur)
            load(version);
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
    g.versions = versions;
    g.dpi = GetDpiForWindow(owner);
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN, ex = WS_EX_CONTROLPARENT;
    RECT r = {0, 0, px(kW), px(kH)};
    AdjustWindowRectExForDpi(&r, style, FALSE, ex, g.dpi);
    RECT o = {};
    GetWindowRect(owner, &o);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    g.wnd = CreateWindowExW(ex, kClass, L"Songs", style, o.left + ((o.right - o.left) - ww) / 2,
                            o.top + ((o.bottom - o.top) - wh) / 2, ww, wh, owner, nullptr, inst, nullptr);
    if (g.wnd == nullptr)
        return;
    create_controls();
    make_font();
    layout();
    show_page(0);
    load(version);
    ShowWindow(g.wnd, SW_SHOW);
    SetFocus(g.songs);
}

bool is_open()
{
    return g.wnd != nullptr;
}

void mark_new(const std::wstring &data, const std::wstring &file)
{
    g_new.insert(lower(data + L"|" + file));
}

bool filter(MSG &m)
{
    return g.wnd != nullptr && (m.hwnd == g.wnd || IsChild(g.wnd, m.hwnd)) && IsDialogMessageW(g.wnd, &m);
}

} // namespace song_manager
