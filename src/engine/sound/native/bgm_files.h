/* bgm_files.h - the streamed music's files, shared by the game and the
 * launcher (docs/sound.md 2.4):
 *   - .ads, the PS2's: a 0x28-byte head, then the channels interleaved,
 *     SPU2 ADPCM or 16-bit PCM; decoded here to 16-bit PCM;
 *   - .wav (PC): 16-bit PCM, what the launcher converts each .ads to (the
 *     game plays it through the PCM output, sound_native.h);
 *   - bgm/ads.inf ("MADS"), the game's list of the music, and bgm/ads.txt
 *     (PC): the same list as plain text, which the game reads instead when
 *     it is there (built into ads.inf's layout: T466 relocates it as ever).
 * Plain C++, no Windows or game dependency. */
#ifndef GT3PC_BGM_FILES_H
#define GT3PC_BGM_FILES_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "engine/sound/native/spu2_adpcm.h"

namespace spu2 {

/* ---- .ads ---------------------------------------------------------------- */

constexpr uint32_t ADS_HEAD_BYTES = 0x28;
constexpr uint32_t ADS_TYPE_PCM   = 0x01;
constexpr uint32_t ADS_TYPE_ADPCM = 0x10;

struct AdsHeader {
    uint32_t type = 0;
    uint32_t rate = 0;
    uint32_t channels = 0;
    uint32_t interleave = 0;
    int32_t  loopStart = -1;
    int32_t  loopEnd = -1;
    uint32_t bodySize = 0;
};

/* Parses the 0x28-byte head; false if it is not one. */
bool ads_parse_header(const uint8_t *head, uint32_t size, AdsHeader *out);

/* Reads up to `size' bytes at byte `offset' of the file into dst: the
 * number read (0 past the end, < 0 on error). */
typedef int (*AdsReadFn)(void *user, uint32_t offset, void *dst, uint32_t size);

/* The samples per channel in `bytes' of an .ads body (whole chunks of
 * interleave x channels bytes; a shorter last one split evenly). */
uint32_t ads_frames(const AdsHeader &h, uint32_t bytes);
/* Decodes `bytes' of an .ads body, from a chunk boundary, to interleaved
 * 16-bit frames appended to out; hist: each channel's ADPCM history, kept
 * from call to call (PCM: unused). */
void ads_decode(const AdsHeader &h, const uint8_t *body, uint32_t bytes, AdpcmHistory hist[2],
                std::vector<int16_t> &out);

/* ---- .wav ---------------------------------------------------------------- */

/* A .wav of PCM: its format, where its samples are, its tags. */
struct WavInfo {
    uint32_t rate = 0, channels = 0;
    uint32_t bits = 0;              /* 8, 16, 24, 32 (integer); 32, 64 (float) */
    bool isFloat = false;
    uint32_t blockAlign = 0;        /* bytes a frame */
    uint32_t dataOffset = 0, dataBytes = 0;
    std::string title, artist;      /* LIST INFO INAM / IART ("" none) */
};
/* Walks a .wav's chunks (the file's first `size' bytes; reads past its
 * end give 0): true for integer PCM of 8 (unsigned), 16, 24 or 32 bits or
 * IEEE float of 32 or 64 bits (WAVE_FORMAT_PCM / _IEEE_FLOAT, or
 * EXTENSIBLE with either subformat), 1 to 8 channels. */
bool wav_probe(AdsReadFn read, void *user, uint32_t size, WavInfo *out);
/* The head of a .wav of `frames' 16-bit frames: RIFF, fmt, LIST INFO with
 * the title / artist given, the data chunk's header; the samples follow. */
std::vector<uint8_t> wav_head(uint32_t rate, uint32_t channels, uint32_t frames, const std::string &title,
                              const std::string &artist);

/* ---- .mp3 ---------------------------------------------------------------- */

/* An MPEG audio file (layer III, II or I; any bitrate, constant or
 * variable): its first frame's rate and channels, its length (the Xing /
 * Info or VBRI frame count, else its frames counted), its ID3 tags (v2's
 * TIT2 / TPE1, else v1's; as Latin-1 bytes, '?' for what Latin-1 lacks).
 * The game decodes it through Media Foundation (T521 pdibgm). */
struct Mp3Info {
    uint32_t rate = 0, channels = 0;
    uint64_t frames = 0;            /* samples per channel */
    std::string title, artist;
};
/* quick: its head alone - the length only from a Xing / Info or VBRI count
 * (else 0), no ID3v1 - what the game's start needs, without reading the
 * whole file.  estimate: a length no head gives from the file's size and
 * its first frame (a constant bitrate's right) instead of the frames
 * counted, so the whole file is not read: the launcher's lists of songs. */
bool mp3_probe(AdsReadFn read, void *user, uint32_t size, Mp3Info *out, bool quick = false, bool estimate = false);

/* ---- either ---------------------------------------------------------------- */

enum class BgmKind { ads, wav, mp3 };

/* What a music file says of itself. */
struct BgmFileInfo {
    BgmKind kind = BgmKind::ads;
    uint32_t rate = 0, channels = 0;
    uint32_t frames = 0;            /* its length, samples per channel */
    uint32_t dataOffset = 0;        /* its samples: a .wav's data chunk, an .ads's body */
    uint32_t dataBytes = 0;
    uint32_t bits = 16;             /* a .wav's sample format */
    bool isFloat = false;
    uint32_t blockAlign = 0;
    std::string title, artist;      /* a .wav's or an .mp3's tags */
    AdsHeader ads;                  /* an .ads's head */
};
/* A .wav (the formats of wav_probe), an .mp3 or an .ads; false for
 * anything else.  quick, estimate: an .mp3's (mp3_probe). */
bool bgm_probe(AdsReadFn read, void *user, uint32_t size, BgmFileInfo *out, bool quick = false, bool estimate = false);
/* `frames' frames of a .wav's samples (src; its format: info) as float
 * (-1..1), appended to out: 1 channel for a mono file, else 2 (more: the
 * front pair with the centre mixed in, as Mp4AudioReader does). */
void wav_to_float(const BgmFileInfo &info, const uint8_t *src, uint32_t frames, std::vector<float> &out);
/* "3:42, 44.1 kHz stereo 24-bit WAV": for the launcher's lists. */
std::string bgm_describe(const BgmFileInfo &info);

/* ---- bgm/ads.inf and bgm/ads.txt ------------------------------------------- */

constexpr int ADS_GROUPS = 17;
constexpr int ADS_MARKER_CHANNELS = 32;
constexpr int ADS_GROUP_RACE_SONGS = 2;     /* BGM group 0x14: the race music, the play list's */

/* What ADS group g plays for ("Race finish"), from the game's callers of
 * BGM groups 0x12 + g (docs/sound.md 2.4); group 2: "Race songs". */
const char *ads_group_event(int g);

/* A track of the list.  From ads.txt a field left out takes its value from
 * the file, else a fallback (ads_list_image). */
struct AdsListTrack {
    std::string file;               /* in bgm/, or its full path (bgm_system_path) */
    std::string title, artist, id;
    bool hasTitle = false, hasArtist = false, hasId = false;
    uint32_t rate = 0;              /* the markers' time base (0: the file's) */
    int64_t length = -1;            /* samples at rate: the markers' loop (-1: the file's) */
    double bpm = 0;                 /* the tempo the markers were made from (0: none; ads.txt only) */
    std::vector<double> markers[ADS_MARKER_CHANNELS];   /* seconds, in the list's order */
    int line = 0;                   /* (ads.txt: its line, for warnings) */
};
struct AdsList {
    std::vector<AdsListTrack> group[ADS_GROUPS];
    /* ads.txt: each group's commented-out track lines ("# track FILE"),
     * written back after its tracks (the launcher does not add those files) */
    std::vector<std::string> keptOut[ADS_GROUPS];
};

/* ads.inf into a list (every field given); false with the reason if it is
 * not one.  A track's marker block holds the address it was built at (+0;
 * 0 on the discs) and its channels' times by addresses of that build: they
 * are read relative to the block, as bgm_markers_relocate (T466) moves
 * them.  legacyMarkers: as the launcher misread them before 2026-10-26 -
 * relative to the file's start, which put the track table into channel 1
 * and slices of the beats into 2 .. 4 - to know the ads.txt it wrote then
 * (music::repair_markers). */
bool ads_inf_read(const uint8_t *data, size_t size, AdsList *out, std::string *error, bool legacyMarkers = false);
/* What a track's file is (the caller's: where it looks, how it reads):
 * false when it is not there.  found: the file's name (it may be the
 * other format's of the same name). */
using AdsListProbe = std::function<bool(const std::string &file, BgmFileInfo *info, std::string *found)>;
/* The list as ads.txt: the fields a file gives (rate, length, id, title and
 * artist tags) written only where the list's differ from it. */
std::string ads_list_text(const AdsList &list, const AdsListProbe &probe);
/* ads.txt into a list; what it cannot read is skipped, each with a line in
 * warnings ("line N: ..."). */
void ads_list_parse(const std::string &text, AdsList *out, std::vector<std::string> *warnings);
/* The list in ads.inf's layout (built at address 0, as the file), what a
 * track leaves out from its file or a fallback: rate 44100, length 0, title
 * the file's name, artist "unknown", markers none, id the file's name.  A
 * file not found is kept (ads_play plays sound effect 4 for it) with a
 * warning. */
std::vector<uint8_t> ads_list_image(const AdsList &list, const AdsListProbe &probe,
                                    std::vector<std::string> *warnings);

/* The files an ads.txt names: its tracks' (listed), and those of its
 * commented-out track lines, "# track FILE" (kept out: the launcher does
 * not add them as new songs). */
void ads_text_files(const std::string &text, std::vector<std::string> *listed, std::vector<std::string> *keptOut);
/* ads.txt with tracks added at the end of group `group' (its first
 * [group N] section; a new one at the end when it has none), the rest of
 * the text as it was. */
std::string ads_text_add(const std::string &text, int group, const std::vector<AdsListTrack> &tracks);

/* A track's file given as a full Windows path ("D:\Music\x.mp3",
 * "\\server\music\x.mp3"), a song anywhere on the machine: UTF-8, as the
 * song manager writes it (the ANSI code page's bytes also do, typed by hand:
 * host_file.h), where a name in bgm/ is the code page's.  It is read where it
 * is: no twin of the other format, no intro file. */
bool bgm_system_path(const std::string &file);
/* "race_a01.wav" -> "race_a01" */
std::string bgm_base_name(const std::string &file);
/* The other format's name of a music file: x.ads <-> x.wav ("" for
 * neither, or for a full path; an .mp3 has none). */
std::string bgm_other_name(const std::string &file);
/* A music file's name: .wav, .mp3 or .ads. */
bool bgm_music_name(const std::string &file);

} /* namespace spu2 */

#endif /* GT3PC_BGM_FILES_H */
