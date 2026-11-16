/* song_manager.h - the song manager: a window of its own, opened from the
 * Audio tab's Songs sub-tab, over an installed version's music list
 * (bgm\ads.txt: music.h, engine/sound/native/bgm_files.h; docs/sound.md
 * 2.4).  Standard Windows controls, owned by the launcher's window.
 *
 * Two pages:
 *   - Race songs (ADS group 2, the play list's): each song's title, artist,
 *     file and format; its title and artist edited below the list; Add
 *     files... and Add a folder... (with the folders under it) add .wav /
 *     .mp3 files anywhere on the machine, by their full paths: they stay
 *     where they are, nothing is copied; Remove takes a song off the list.
 *     No file is ever deleted: a file of bgm\ is kept out of the list ("#
 *     track FILE": the launcher would add it again).
 *   - Event music: the jingle slots - one music file each, for an event of
 *     the game (race start, race finish, license bronze...: ads_group_event)
 *     - any music file of bgm\, a song added, silence, a file anywhere, or
 *     the disc's (ads.inf, "Original").
 * Every change is written to ads.txt at once (the list written anew:
 * ads_list_text); the game reads it when it starts.  The songs found new in
 * this run (music::find_new_songs) are marked "new".
 *
 * PC (2026-11-15, user request: "If the user only links a disc with no
 * installation, the song manager needs to still work"): a version may be a
 * disc image added on the Play tab's Disc sub-tab, its data folder the
 * image's small one (discs\<boot file>_<id>): the list, the disc's songs
 * (.ads, listed with ", on the disc" in their format) and ads.inf are read
 * from the image (music.h, disc_files.h), and ads.txt is written into that
 * folder's bgm\. */
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace song_manager {

struct Version {
    std::wstring name;          /* "Gran Turismo 3 A-Spec (USA)"; a disc image's "Disc image: <its release>" */
    std::wstring data;          /* its data folder */
};

/* Opens the window on versions[version] (or brings it forward there). */
void open(HWND owner, const std::vector<Version> &versions, int version);
bool is_open();
/* A song of `data' found new in this run (music::find_new_songs). */
void mark_new(const std::wstring &data, const std::wstring &file);
/* The window's keyboard navigation (Tab, Enter, Esc) from the launcher's
 * message loop: true when the message was its. */
bool filter(MSG &m);

} // namespace song_manager
