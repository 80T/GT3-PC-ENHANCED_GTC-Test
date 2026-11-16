/* pc_bgm_list.h - PC: bgm/ads.txt, the plain-text list of the streamed
 * music (docs/sound.md 2.4; the format: engine/sound/native/bgm_files.h).
 *
 * The launcher writes it from bgm/ads.inf when it converts the music to
 * .wav (src/launcher/music.h); ads_init (T466) reads it instead of ads.inf
 * when it is there.  A track's file gives what the list leaves out (its
 * rate and length, a .wav's title / artist tags); the rest has fallbacks
 * (bgm_files.h ads_list_image).  ads_play takes a missing file's other
 * format (x.ads <-> x.wav).
 *
 * A track can name a file anywhere on the machine by its full path
 * (bgm_files.h bgm_system_path; the song manager adds them so): ads_play
 * opens it there, and it has no intro file.  A drive can be away: a race
 * goes past a song not there (MusicPlayList::nextThere). */
#ifndef GT3PC_PC_BGM_LIST_H
#define GT3PC_PC_BGM_LIST_H

#ifdef __cplusplus
extern "C" {
#endif

/* bgm/ads.txt as an ads.inf image (built at address 0, from the game heap:
 * ads_inf_relocate relocates it, ads_term frees it), NULL when there is no
 * ads.txt (ads.inf is read then).  Logs what it read and its warnings. */
void *pc_ads_list_load(void);

/* A bgm/ path's other format in place (".ads" <-> ".wav", the path's
 * buffer of `size' bytes): 1, or 0 when it is neither. */
int pc_bgm_other_format(char *path, int size);

/* The track's name is a full path: 1. */
int pc_bgm_system_path(const char *name);
/* The size of the file at a full path (at most what pdibgm_start's sectors
 * hold): 1, or 0 when it cannot be opened. */
int pc_bgm_file_size(const char *path, unsigned *size);
/* The song can be played: 1 for a name in bgm/ (left to ads_play), or a full
 * path whose file is there. */
int pc_bgm_song_there(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* GT3PC_PC_BGM_LIST_H */
