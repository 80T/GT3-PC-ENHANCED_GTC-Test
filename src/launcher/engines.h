/* engines.h - the engine selector, the launcher's first screen, and what is
 * each engine's own.
 *
 * PC (2026-11-14, user request: "The executable plugin system ... 1st screen
 * is an Engine Selector.  The 1st item in the list is the same launcher
 * itself, other items in the list are other launchers / other engines we've
 * added to the list.  There are buttons to add / remove engines.  When adding
 * an engine plugin, a warning popup is called, saying to make sure it is from
 * a trusted source.  All launchers share the same .ini.  Only change needed
 * is to separate the configuration of data packs (Mod folders) ... We don't
 * want the engine selector to start a launcher at its engine selector").
 *
 * An engine is a folder with a launcher and a game of its own
 * (gt3launcher.exe, gt3pc.exe: this tree built by someone else, "Build Mod
 * Plugin.cmd"), named by the first line of its engine.txt (else by the
 * folder).  The launcher the user starts shows the selector: itself first,
 * then the engines of gt3pc.ini [Engines] (Count, EngineN a folder; Last, the
 * folder started last, "" itself).  Choosing another engine starts its
 * launcher as
 *
 *     <engine>\gt3launcher.exe --engine --shared <this launcher's folder>
 *
 * --engine: no selector there (a launcher started by a selector goes straight
 * to its window); --shared: the folder whose gt3pc.ini it reads and writes,
 * and passes on to its game (gt3pc.exe --shared: platform/paths.h) - the
 * settings are one file for every engine.
 *
 * What stays an engine's own is in its own folder: its memory cards
 * (memcard1, memcard2; PC 2026-11-15: now its save folders, saves\<its
 * name>\ beside the shared gt3pc.ini, platform/save_folders.h, with the slots'
 * folders in gt3pc_engine.ini [Saves]), its installed versions and disc image folders
 * (data_US ..., discs\), its caches, and gt3pc_engine.ini (kEngineIni) - the
 * mod folders ([Mods]), the disc images played ([Discs]) and what was played
 * last ([Game] Version, Disc), which were gt3pc.ini's until this: migrate()
 * moves them out of it, once, for the launcher the user starts.
 *
 * No security beyond the one warning when an engine is added: an engine is a
 * program and runs as any program the user starts. */
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace engines {

constexpr const wchar_t *kEngineIni = L"gt3pc_engine.ini";
constexpr const wchar_t *kNameFile = L"engine.txt";
constexpr const wchar_t *kLauncherExe = L"gt3launcher.exe";
constexpr const wchar_t *kGameExe = L"gt3pc.exe";
constexpr int kMaxEngines = 64;

/* An engine's name: the first line of <folder>\engine.txt, else the folder's
 * own name. */
std::wstring name_of(const std::wstring &folder);

/* The engines added (gt3pc.ini [Engines] of `ini`): their folders, without a
 * trailing '\'. */
std::vector<std::wstring> load(const std::wstring &ini);
void save(const std::wstring &ini, const std::vector<std::wstring> &folders);

/* The launcher the user starts, in `folder` (with its '\'): what is an
 * engine's own moved out of its gt3pc.ini into its gt3pc_engine.ini - [Mods],
 * [Discs], [Game] Version and Disc - when that file is not there yet (then
 * written, so the game stops reading the old place). */
void migrate(const std::wstring &folder);
/* An engine started by a selector: its gt3pc_engine.ini made when missing. */
void ensure_engine_ini(const std::wstring &folder);

enum class Choice {
    own,    /* this launcher: its window follows */
    other,  /* another engine: *launcher its gt3launcher.exe, *engine its folder */
    exit,   /* the window was closed */
};
/* The selector, until an engine is started or it is closed.  `folder`: this
 * launcher's (with its '\'). */
Choice select(HINSTANCE inst, const std::wstring &folder, int show, std::wstring *launcher, std::wstring *engine);

/* Starts the engine's launcher for the selector (--engine --shared <folder>);
 * false with the reason. */
bool start(const std::wstring &launcher, const std::wstring &engine, const std::wstring &folder, std::wstring *error);

} // namespace engines
