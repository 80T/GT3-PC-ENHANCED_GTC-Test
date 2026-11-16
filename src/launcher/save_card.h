/* save_card.h - what a save folder (one memory card, platform/save_folders.h)
 * holds, for the save manager (save_manager.h).  PC (2026-11-15).
 *
 * A save is a folder of the card (BASCUS-97102GAMEDATA\ ...) with an
 * icon.sys (its title, its icon's lights; the PS2's sceMcIconSys), its icon
 * (.ico) and its files.  A GT3 save also has a "thumbnail" file, 0x60 bytes
 * (gt2k_game::GameThumbnail, include/game/menu/memorycard/
 * T108_game_thumbnail.h): its kind - game data, car setting, replay, best lap
 * replay (a licence test's when its category is 9..14) - and what the game's
 * file screens show of it (T069): the money, cars, licence, days and
 * achievement of a game, a replay's time, laps and power, a best lap's time
 * (or top speed), power and weight, the comment and the mode.  The cars' and
 * courses' names are the game database's (not read here).
 *
 * Sizes are counted as the game's cards count them (platform/memcard.cpp):
 * 1 KB clusters, a file's rounded up, two directory entries a cluster
 * ("." and ".." included), on an 8000 KB card. */
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace save_card {

constexpr int kCardKB = 8000;

enum class Kind { game, setting, replay, bestLap, licence, other };

struct Save {
    std::wstring dir;                   /* the save's folder in the card (BASCUS-97102GAMEDATA) */
    std::wstring title1, title2;        /* icon.sys's title, its two lines (full-width letters made plain) */
    Kind kind = Kind::other;
    std::wstring region;                /* "USA", "Europe", "Japan" by the name's second letter; "" */
    int kb = 0;                         /* its clusters */
    FILETIME saved = {};                /* the latest write of it or its files */
    std::vector<uint8_t> thumb;         /* the thumbnail file (0x60 bytes), empty without one */
    std::wstring icon;                  /* the icon file (icon.sys's), "" none */
};

struct Card {
    bool there = false;                 /* the folder exists */
    std::vector<Save> saves;            /* sorted: game data first, then by kind, then by date (newest first) */
    int usedKB = 0;
    int freeKB() const { return usedKB >= kCardKB ? 0 : kCardKB - usedKB; }
};

Card read(const std::wstring &folder);
/* The clusters a card folder uses (memcard.cpp used_clusters). */
int used_kb(const std::wstring &folder);

/* Its title on one line ("GT3 BEST LAP AT040001"). */
std::wstring title(const Save &s);
std::wstring kind_name(Kind k, bool plural = false);
/* The save's details, one item a line (what the game's file screens show). */
std::wstring details(const Save &s);
/* "Day 308, Cr. 1,286,790, 43 cars, license S, 47.3 %" (game data), "" for
 * the other kinds. */
std::wstring game_summary(const Save &s);
/* A date and time in the user's format. */
std::wstring date_text(const FILETIME &ft);
/* 1,234 */
std::wstring number(long long v);

/* File operations; false with *error (the system's text) on a failure. */
bool remove_tree(const std::wstring &dir, bool self, std::wstring *error);
/* `from` (a save folder) copied as `to`, the files' and the folder's times
 * kept. */
bool copy_tree(const std::wstring &from, const std::wstring &to, std::wstring *error);

} // namespace save_card
