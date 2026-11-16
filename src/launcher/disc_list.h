/* disc_list.h - the disc images played from the launcher (the Play tab's Disc
 * sub-tab, ui.cpp).
 *
 * PC (2026-11-14, user request: "Disc tab acts as a play list for iso paths
 * ... After 1st play the launcher remembers the disc path and adds it to the
 * disc tab's list").  gt3pc.ini [Discs]: Count, then DiscN (an image's path)
 * and DiscNName (its release, as the launcher found it at its last Play: the
 * list is shown without opening the images), N = 1 the first; an image is in
 * the list once, by its path (any case), the last played first.  PC
 * (2026-11-15, user request: "change the launcher to only add the disc
 * without automatically starting it"): an image added comes first too, and
 * DiscNFolder is its data folder's name in discs\ (install::prepare_disc;
 * the song manager's, before it was ever played; "" for one listed before). */
#pragma once

#include <string>
#include <vector>

namespace disc_list {

constexpr int kMaxDiscs = 64;

struct Disc {
    std::wstring path, name;
    std::wstring folder;        /* PC (2026-11-15): its data folder in discs\ ("" not known yet) */
};

std::vector<Disc> load(const std::wstring &ini);
void save(const std::wstring &ini, const std::vector<Disc> &discs);
/* `path' put first with its release `name' and its folder (added or played
 * now: taken from where the list had it). */
void remember(const std::wstring &ini, const std::wstring &path, const std::wstring &name, const std::wstring &folder);
/* The folder of the image at `index' set, its place kept. */
void set_folder(const std::wstring &ini, size_t index, const std::wstring &folder);
/* The image at `index' taken out of the list (the file stays). */
void remove(const std::wstring &ini, size_t index);

} // namespace disc_list
