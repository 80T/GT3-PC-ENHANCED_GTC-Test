/* pc_race_songs.cpp - the race's song changer, the race's side
 * (pc_race_songs.h). */
#include "game/app/pc_race_songs.h"
#include "game/app/pc_bgm_list.h"
#include "game/save/T430_music_play_list.h"
#include "game/sound/T464_bgm.h"                /* bgm_get_tracks */
#include "platform/song_osd.h"

#include <chrono>
#include <vector>

namespace {

constexpr int kSongsGroup = 0x14;           /* BGM group 0x14: the play list's songs */

struct RaceSongs {
    const void *owner = nullptr;            /* the race's music */
    std::vector<int> heard;                 /* tracks, in their order */
    int at = -1;                            /* the one playing (heard[at]) */
    int shown = -2;                         /* the track on the panel (-1 none; -2 not yet) */
};
RaceSongs g;

/* The panel's place of the song: entry `e''s among the enabled songs of the
 * list, in its order; count: those. */
void place(MusicPlayList *list, const MusicEntry *e, int *number, int *count)
{
    *number = 0;
    *count = 0;
    for (u32 i = 0; i < list->count; i++) {
        const MusicEntry *x = list->entryAt(i);
        if (x->enabled == 0)
            continue;
        (*count)++;
        if (x == e)
            *number = *count;
    }
}

} // namespace

void pc_race_songs_begin(const void *owner)
{
    g = RaceSongs();
    g.owner = owner;
    song_osd::race_end();
}

void pc_race_songs_end(const void *owner)
{
    if (owner != g.owner)
        return;
    g = RaceSongs();
    song_osd::race_end();
}

int pc_race_songs_forward(void)
{
    if (g.at + 1 >= (int)g.heard.size())
        return -1;
    return g.heard[++g.at];
}

void pc_race_songs_heard(int track)
{
    g.heard.resize((size_t)(g.at + 1));
    g.heard.push_back(track);
    g.at = (int)g.heard.size() - 1;
}

int pc_race_songs_back(MusicPlayList *list, int current)
{
    if (current < 0)
        return -1;
    if (g.at > 0)
        return g.heard[--g.at];
    /* before the first one heard: the song before it in the play order
     * (each position's entry; the songs not enabled, or whose file is not
     * there, passed; at most a second of looking, as nextThere), first in
     * the songs heard; none: the song again */
    MusicEntry *e = list->entryOfTrack((u32)current);
    const u32 n = list->count;
    if (e == NULL || e->order < 0 || (u32)e->order >= n)
        return current;
    std::vector<MusicEntry *> byOrder(n, nullptr);
    for (u32 i = 0; i < n; i++) {
        MusicEntry *x = list->entryAt(i);
        if (x->order >= 0 && (u32)x->order < n)
            byOrder[(u32)x->order] = x;
    }
    const auto start = std::chrono::steady_clock::now();
    int found = current;
    for (u32 k = 1; k < n; k++) {
        MusicEntry *x = byOrder[((u32)e->order + n - k) % n];
        if (x == nullptr || x->enabled == 0)
            continue;
        u32 track = 0;
        const MusicInfo *info = findMusicInfo(&x->code, &track);
        if (pc_bgm_song_there(info->name)) {
            found = (int)track;
            break;
        }
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(1))
            break;
    }
    if (found != current) {
        g.heard.insert(g.heard.begin(), found);
        g.at = 0;
    }
    return found;
}

int pc_race_songs_frame(MusicPlayList *list, int current)
{
    if (current != g.shown) {
        g.shown = current;
        const BgmTrackList songs = bgm_get_tracks(kSongsGroup);
        if (current >= 0 && (u32)current < songs.count) {
            const BgmTrack &t = songs.tracks[current];
            int number = 0, count = 0;
            const MusicEntry *e = list->entryOfTrack((u32)current);
            if (e != NULL)
                place(list, e, &number, &count);
            song_osd::set_song(t.title, t.artist, number, number != 0 ? count : 0);
        } else {
            song_osd::set_song(nullptr, nullptr, 0, 0);
        }
    }
    return song_osd::race_frame();
}
