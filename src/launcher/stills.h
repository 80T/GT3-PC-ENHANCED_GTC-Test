/* stills.h - the arcade menus' course and class stills converted for the PC
 * (docs/movies.md "Stills").
 *
 * stream\course.ipic of a data folder, an archive of Sony IPU pictures, is
 * written out as a PNG a picture, stream\course\<name>.png, which the game
 * reads instead (game/app/pc_still.h), the pixels it decoded them to.  Once
 * every PNG is read back and matches, the archive is deleted.  A PNG already
 * there (edited, an interrupted conversion's) is never replaced.  An install
 * converts as one of its last steps; the launcher converts an installed
 * version that still needs it when it starts. */
#pragma once

#include <string>

namespace stills {

/* The data folder has a stream\course.ipic. */
bool needs_conversion(const std::wstring &data);

/* Converts it.  message: a one-line summary, or what could not be done; false
 * with the archive kept (the game reads it for the stills it lacks) when one
 * could not.  No archive: true, nothing to say. */
bool convert(const std::wstring &data, std::wstring *message);

} // namespace stills
