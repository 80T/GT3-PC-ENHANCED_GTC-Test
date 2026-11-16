/* pc_race_songs.h - PC (ROADMAP 55, no original): the race's song changer,
 * the race's side (platform/song_osd.h: its panel and hotkeys; docs/sound.md
 * 2.4 "The song changer").
 *
 * The race's music (T130 RaceBGMPS2) keeps the race songs heard in this race
 * in their order here.  A step back plays the one heard before the song
 * playing - before the first one, the song before it in the play order
 * (enabled, its file there; wrapping) - and a step on the one heard after
 * it, as does a song's end; past the last one heard, the play list's next
 * song (RaceBGMPS2::play, as ever).  Each live race frame
 * (RaceBGMPS2::update) shows the song playing on the panel and takes a step
 * the panel was asked for.  One race's music at a time (the game thread). */
#ifndef GAME_APP_PC_RACE_SONGS_H
#define GAME_APP_PC_RACE_SONGS_H

struct MusicPlayList;

/* A race's music made (RaceBGMPS2's constructor) / gone (its destructor):
 * no songs heard yet, the panel closed.  owner: the music object (a
 * destructor of another one changes nothing). */
void pc_race_songs_begin(const void *owner);
void pc_race_songs_end(const void *owner);

/* RaceBGMPS2::play's next race song after a step back: the track (of BGM
 * group 0x14) heard after the one playing, now playing again; -1 none (the
 * play list's next, then pc_race_songs_heard). */
int  pc_race_songs_forward(void);
/* Track `track' started from the play list. */
void pc_race_songs_heard(int track);
/* A step back from track `current' (the song playing): the track to play,
 * -1 none. */
int  pc_race_songs_back(MusicPlayList *list, int current);

/* Each frame of a live race: the panel shows track `current' (-1: no race
 * song playing) and its place in `list'; returns the step asked for (-1,
 * +1; 0 none).  (The update a step's song makes calls it again: a second
 * step asked for in the same frame is taken there.) */
int  pc_race_songs_frame(MusicPlayList *list, int current);

#endif /* GAME_APP_PC_RACE_SONGS_H */
