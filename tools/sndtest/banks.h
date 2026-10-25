/* banks.h - sndtest: GT3's sound bank files read into plain structures.
 *
 * INST (.ins: sound effects, music instruments) and ENGN (.es: engine
 * sounds), as engine/sound's inst_relocate / engn_relocate see them (see
 * docs/sound.md for the layouts), plus music.seq (SEQG) and music.inf
 * (MSEQ).  Offsets instead of the game's relocated pointers. */
#ifndef SNDTEST_BANKS_H
#define SNDTEST_BANKS_H

#include <cstdint>
#include <string>
#include <vector>

/* InstZone / SoundProgram, 0x18 bytes */
struct Zone {
    uint32_t addr8;         /* SPU address / 8 (relative to the bank until placed) */
    int16_t  vol;
    uint16_t baseNote;      /* 8.8 semitones: the note that plays at 1.0 (44.1 kHz pitch 0x1000) */
    uint16_t unk0A;         /* non-zero for one-shot samples (-> voice flag 0x100) */
    uint8_t  prio, pan, flags, unk0F;
    uint8_t  lo, hi, bendDown, bendUp;
    uint32_t adsr;          /* adsr1 | adsr2 << 16 */
};

/* InstProgram */
struct Program {
    uint8_t pan;
    int16_t vol;
    std::vector<uint8_t> zones;
};

struct InstBank {
    std::string name;
    uint32_t headerSize = 0;
    uint32_t spuBase = 0;           /* where the samples were placed (set by the loader) */
    std::vector<Zone> zones;
    std::vector<Program> programs;
    std::vector<uint8_t> samples;   /* spuSize bytes */
};

/* EngnSeg, 0x10 bytes */
struct EngnSeg {
    int16_t  baseRpm, lo, hi, vol;
    uint32_t pitch;                 /* 44.1 kHz-based pitch at baseRpm (0x1000 = 44.1 kHz) */
    uint32_t addr;                  /* bytes (relative until placed) */
};

struct EngnBank {
    std::string name;
    uint32_t spuBase = 0;
    std::vector<EngnSeg> sets[2];
    std::vector<uint8_t> samples;
};

/* SEQG entry */
struct SeqEntry {
    uint16_t volume = 0;
    uint16_t unk02 = 0;
    int32_t  tempo = 0;
    int32_t  track[16];             /* offsets in the file, -1: none */
};

struct SeqBank {
    std::vector<uint8_t> data;
    std::vector<SeqEntry> entries;
};

/* music.inf: groups of tracks, each track's sequence index */
struct MusicTrack {
    std::string group, name, title, artist;
    int32_t seq;
};

bool load_file(const std::string &path, std::vector<uint8_t> &out);
bool parse_inst(const std::vector<uint8_t> &file, InstBank &bank, std::string &err);
bool parse_engn(const std::vector<uint8_t> &file, EngnBank &bank, std::string &err);
bool parse_seqg(const std::vector<uint8_t> &file, SeqBank &bank, std::string &err);
bool parse_music_inf(const std::vector<uint8_t> &file, std::vector<std::vector<MusicTrack>> &groups,
                     std::string &err);

#endif /* SNDTEST_BANKS_H */
