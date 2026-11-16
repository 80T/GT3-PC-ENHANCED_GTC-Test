/* T466 - the streamed BGM (ADS): bgm/ads.inf and the stream calls.
 * 0x0022AA78..0x0022AE38. */
#include "game/sound/gtsound_internal.h"
#include "game/sound/T466_bgm_ads.h"
#include "engine/io/file_missing.h"             /* PC: IO_PATH_MAX (ads_play's paths) */
#include "game/app/pc_bgm_list.h"               /* PC: bgm/ads.txt, a file's other format */

/* .sdata: the pointer first (written at its definition), then the
 * functions' short literals ("bgm/", "i_"). */
AdsInf *g_adsInf = NULL;                        /* 0x00352B00 */

/* Relocates the beat markers of a track: base = this, times[] += delta. */
// FUNC 0x0022AA78 bgm_markers_relocate
void bgm_markers_relocate(BgmMarkers *m)
{
    BgmMarkers *built = m->base;
    int delta, ch;

    m->base = m;
    delta = (u8 *)m - (u8 *)built;
    for (ch = 0; ch < 32; ch++)
        u104_reloc(m->times[ch], delta);
}

/* Relocates bgm/ads.inf: the 17 groups, their tracks (with the marker
 * pointers) and each track's markers. */
// FUNC 0x0022AAB8 ads_inf_relocate
void ads_inf_relocate(AdsInf *inf)
{
    u32 group, track;
    long delta = (long)inf - inf->base;     /* 64-bit: lwu, dsubu, truncated once */

    inf->base = (u32)inf;
    for (group = 0; group < ADS_GROUP_COUNT; group++) {
        bgm_tracklist_relocate(&inf->group[group], delta, 1);
        for (track = 0; track < inf->group[group].count; track++)
            bgm_markers_relocate(inf->group[group].tracks[track].markers);
    }
}

// FUNC 0x0022ABB0 ads_init
void ads_init(void)
{
    if (g_adsInf == NULL) {
        /* PC: the plain-text list bgm/ads.txt when there is one, built in
         * ads.inf's layout (pc_bgm_list.h); else ads.inf */
        AdsInf *inf = (AdsInf *)pc_ads_list_load();
        if (inf == NULL)
            inf = (AdsInf *)file_read_blocking("bgm/ads.inf", NULL, 0, 1, &vol_device_object);
        ads_inf_relocate(inf);
        g_adsInf = inf;
    }
}

// FUNC 0x0022AC08 ads_term
void ads_term(void)
{
    AdsInf *inf = g_adsInf;

    if (inf != NULL) {
        g_adsInf = NULL;
        free(inf);
    }
}

/* The tracks of a group; -1: all the tracks (from group 0's, ads.inf
 * total). */
// FUNC 0x0022AC30 ads_get_tracks
BgmTrackList ads_get_tracks(int group)
{
    if (group == -1)
        return BgmTrackList(g_adsInf->group[0].tracks, g_adsInf->total);
    return g_adsInf->group[group];
}

// FUNC 0x0022AC78 ads_set_volume
void ads_set_volume(f32 volume)
{
    pdibgm_set_volume(volume);              /* 0x0025C9F8 */
}

/* PC: the play list's preview of a song without an intro file (one added to
 * the list): the song from 32 % of its length, faded in over 5 s (GT3's
 * intro files: a part of the song from 6 - 23 % of it, faded in over about
 * 5 s in a straight line); it plays on to its end (the user's request,
 * 2026-11-02). */
static const f32 kPreviewAt = 0.32f;
static const f32 kPreviewFadeIn = 5.0f;

/* Starts track `track' of ADS group `group' (BGM_TRACK_RANDOM: a random one;
 * bit BGM_TRACK_INTRO: the intro file i_<name>).  A missing file plays sound
 * effect 4 instead. */
// FUNC 0x0022AC98 ads_play
void ads_play(int group, int track, f32 volume, int arg)
{
    ads_stop();
    BgmTrackList list = ads_get_tracks(group);   /* sp+0x00 (built in place) */
    if (list.count == 0)
        return;

    /* PC: IO_PATH_MAX bytes each (the PS2's 0x70: a longer track name of a
     * transplanted ads.inf, or "host:data/" + the path, ran past them); the
     * name's length checked before the copies below */
    char hostName[IO_PATH_MAX];             /* sp+0x10 the host: file ("" on the disc) */
    char path[IO_PATH_MAX - 0x40];          /* sp+0x80 */
    int  lsn;                               /* sp+0xF0 */
    u32  size;                              /* sp+0xF4 the packed size */
    int  intro = 0;
    BgmTrack *t;

    if (track == BGM_TRACK_RANDOM)
        track = (u32)rand() % list.count;
    if (track & BGM_TRACK_INTRO) {
        intro = 1;
        track &= ~BGM_TRACK_INTRO;
    }
    t = &list.tracks[track % list.count];
    /* PC: a song anywhere on the machine, its full path in the list
     * (pc_bgm_list.h): opened there, the list's path as the host file; it
     * has no intro file, so a preview plays the song (kPreviewAt) */
    if (pc_bgm_system_path(t->name)) {
        unsigned bytes;

        if (!pc_bgm_file_size(t->name, &bytes)) {
            printf("snd: %s is not there\n", t->name);
            se_play(4, SE_PAN_CENTRE, -1, 1.0f);
            return;
        }
        ads_set_volume(volume);
        if (intro)
            pdibgm_pc_start_from(0, (int)(((u64)bytes + 0x7FF) >> 11), arg, 0x40000, t->name, kPreviewAt,
                                 kPreviewFadeIn);
        else
            pdibgm_start(0, (int)(((u64)bytes + 0x7FF) >> 11), arg, 0x40000, t->name);
        return;
    }
    if (strlen(t->name) >= sizeof(path) - sizeof("bgm/i_")) {
        /* PC: a name longer than the path buffer: as a missing file */
        printf("snd: the ADS track name \"%.48s...\" is too long: not played\n", t->name);
        se_play(4, SE_PAN_CENTRE, -1, 1.0f);
        return;
    }
    strcpy(path, "bgm/");
    if (intro)
        strcat(path, "i_");
    strcat(path, t->name);

    /* PC: file_locate_n: hostName's size known (a longer name cut, logged);
     * a missing file's other format (x.ads <-> x.wav: the launcher converts
     * the .ads to .wav, ads.inf names the .ads; pc_bgm_list.h) */
    int found = file_locate_n(path, hostName, sizeof(hostName), &lsn, &size, &vol_device_object) != -1 ||
                (pc_bgm_other_format(path, sizeof(path)) &&
                 file_locate_n(path, hostName, sizeof(hostName), &lsn, &size, &vol_device_object) != -1);
    int fromSong = 0;                       /* PC: the song in its intro's place */
    if (!found && intro) {
        /* PC: a song without an intro file (one added to the list: the play
         * list's preview asks for it) plays itself, from kPreviewAt */
        strcpy(path, "bgm/");
        strcat(path, t->name);
        found = file_locate_n(path, hostName, sizeof(hostName), &lsn, &size, &vol_device_object) != -1 ||
                (pc_bgm_other_format(path, sizeof(path)) &&
                 file_locate_n(path, hostName, sizeof(hostName), &lsn, &size, &vol_device_object) != -1);
        fromSong = 1;
    }
    if (!found) {
        se_play(4, SE_PAN_CENTRE, -1, 1.0f);    /* 0x0022C658 */
        return;
    }
    ads_set_volume(volume);
    /* 64-bit add: (size + 0x7FF) >> 11 sectors; a 0x40000-byte stream buffer */
    if (fromSong)
        pdibgm_pc_start_from(lsn, (int)(((u64)size + 0x7FF) >> 11), arg, 0x40000, hostName, kPreviewAt,
                             kPreviewFadeIn);
    else
        pdibgm_start(lsn, (int)(((u64)size + 0x7FF) >> 11), arg, 0x40000, hostName);   /* 0x0025C8C8 */
}

// FUNC 0x0022AE18 ads_stop
void ads_stop(void)
{
    pdibgm_stop();                          /* 0x0025C9D0 */
}
