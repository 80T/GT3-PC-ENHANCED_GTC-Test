/* still_cache.h - the arcade menus' stills in memory (docs/movies.md "Stills";
 * game/app/pc_still.h).
 *
 * A menu shows a still for the moment before a preview movie starts, and a menu
 * must not wait for a disk or a decoder there: the first time the arcade menus
 * open is the game's first impression.  So all of it happens before the game
 * starts (still_cache_prepare, WinMain):
 *
 *  - the PNG files of stream\course\ (the launcher's, from the disc's
 *    course.ipic: the stills of the .pss movies), of the data folder and the
 *    mods, are read into memory as they are;
 *  - an .mp4 preview movie, stream\<name>_full.mp4, opens on a picture of its
 *    own: its first picture is saved as a PNG in cache\movie_frames\ of the
 *    game's folder, apart from the game's stills, every missing one before the
 *    game goes on, and that file read into memory in place of the still of
 *    that name.  <movie>.<size>-<time>.png: named by the movie's size and date,
 *    so another file of that name is another picture and the old one's goes.
 *    What is there is only ever a copy: the folder can be deleted at any time.
 *
 * Memory holds the PNG files, not their pixels: the memory of a 32-bit process
 * is for the game, and decoding one takes a few milliseconds without a disk.
 *
 * No Windows or game headers: game code can include it. */
#pragma once

#include <cstddef>
#include <cstdint>

namespace gt3media {

/* Does all of the above; returns when it is done.  Before the game's threads
 * start, after paths::init and settings::load. */
void still_cache_prepare();

/* A still's PNG file, in memory for the rest of the run. */
struct StillFile {
    const uint8_t *data = nullptr;
    size_t size = 0;
    bool movie = false;                     /* an MP4's first picture, not a still of stream\course\ */
};

/* The still called `name` (any case), false when memory has none.  Any thread:
 * nothing changes after still_cache_prepare. */
bool still_cache_find(const char *name, StillFile *out);

} // namespace gt3media
