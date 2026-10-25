/* banks.cpp - sndtest: parsing of the sound bank files (see banks.h). */
#include "banks.h"

#include <cstdio>
#include <cstring>

static uint32_t u32at(const std::vector<uint8_t> &b, uint32_t o)
{
    if (o + 4 > b.size())
        return 0;
    return (uint32_t)b[o] | ((uint32_t)b[o + 1] << 8) | ((uint32_t)b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24);
}

static uint16_t u16at(const std::vector<uint8_t> &b, uint32_t o)
{
    if (o + 2 > b.size())
        return 0;
    return (uint16_t)(b[o] | (b[o + 1] << 8));
}

static std::string cstr(const std::vector<uint8_t> &b, uint32_t o)
{
    std::string s;
    while (o < b.size() && b[o])
        s += (char)b[o++];
    return s;
}

bool load_file(const std::string &path, std::vector<uint8_t> &out)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? (size_t)n : 0);
    size_t got = n > 0 ? std::fread(out.data(), 1, (size_t)n, f) : 0;
    std::fclose(f);
    return got == out.size();
}

/* The common head (SoundBankHeader): magic, self, size, spuBase, spuSize,
 * samples (= the file offset of the sample data). */
bool parse_inst(const std::vector<uint8_t> &b, InstBank &bank, std::string &err)
{
    if (b.size() < 0x28 || std::memcmp(b.data(), "INST", 4) != 0) {
        err = "not an INST bank";
        return false;
    }
    bank.headerSize = u32at(b, 0x08);
    uint32_t spuSize = u32at(b, 0x10);
    uint32_t samples = u32at(b, 0x14);
    uint32_t nz = u32at(b, 0x18), zp = u32at(b, 0x1C);
    uint32_t np = u32at(b, 0x20), pp = u32at(b, 0x24);
    if (samples + spuSize > b.size() || zp + nz * 0x18 > b.size() || pp + np * 4 > b.size()) {
        err = "INST bank truncated";
        return false;
    }
    bank.samples.assign(b.begin() + samples, b.begin() + samples + spuSize);
    bank.zones.resize(nz);
    for (uint32_t i = 0; i < nz; i++) {
        uint32_t o = zp + i * 0x18;
        Zone &z = bank.zones[i];
        z.addr8 = u32at(b, o);
        z.vol = (int16_t)u16at(b, o + 6);
        z.baseNote = u16at(b, o + 8);
        z.unk0A = u16at(b, o + 0x0A);
        z.prio = b[o + 0x0C];
        z.pan = b[o + 0x0D];
        z.flags = b[o + 0x0E];
        z.unk0F = b[o + 0x0F];
        z.lo = b[o + 0x10];
        z.hi = b[o + 0x11];
        z.bendDown = b[o + 0x12];
        z.bendUp = b[o + 0x13];
        z.adsr = u32at(b, o + 0x14);
    }
    bank.programs.resize(np);
    for (uint32_t i = 0; i < np; i++) {
        uint32_t po = u32at(b, pp + 4 * i);
        if (po + 4 > b.size()) {
            err = "INST program out of the file";
            return false;
        }
        Program &p = bank.programs[i];
        uint8_t n = b[po];
        p.pan = b[po + 1];
        p.vol = (int16_t)u16at(b, po + 2);
        p.zones.assign(b.begin() + po + 4, b.begin() + po + 4 + n);
    }
    return true;
}

bool parse_engn(const std::vector<uint8_t> &b, EngnBank &bank, std::string &err)
{
    if (b.size() < 0x28 || std::memcmp(b.data(), "ENGN", 4) != 0) {
        err = "not an ENGN bank";
        return false;
    }
    uint32_t spuSize = u32at(b, 0x10);
    uint32_t samples = u32at(b, 0x14);
    if (samples + spuSize > b.size()) {
        err = "ENGN bank truncated";
        return false;
    }
    bank.samples.assign(b.begin() + samples, b.begin() + samples + spuSize);
    for (int s = 0; s < 2; s++) {
        uint32_t cnt = u32at(b, 0x18 + 8 * s), so = u32at(b, 0x1C + 8 * s);
        bank.sets[s].clear();
        if (so == 0)
            continue;
        for (uint32_t i = 0; i < cnt; i++) {
            uint32_t o = so + 0x10 * i;
            EngnSeg g;
            g.baseRpm = (int16_t)u16at(b, o);
            g.lo = (int16_t)u16at(b, o + 2);
            g.hi = (int16_t)u16at(b, o + 4);
            g.vol = (int16_t)u16at(b, o + 6);
            g.pitch = u32at(b, o + 8);
            g.addr = u32at(b, o + 12);
            bank.sets[s].push_back(g);
        }
    }
    return true;
}

bool parse_seqg(const std::vector<uint8_t> &b, SeqBank &bank, std::string &err)
{
    if (b.size() < 0x0C || std::memcmp(b.data(), "SEQG", 4) != 0) {
        err = "not a SEQG file";
        return false;
    }
    bank.data = b;
    uint32_t n = u32at(b, 8);
    bank.entries.resize(n);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t o = 0x0C + i * 0x48;
        SeqEntry &e = bank.entries[i];
        e.volume = u16at(b, o);
        e.unk02 = u16at(b, o + 2);
        e.tempo = (int32_t)u32at(b, o + 4);
        for (int k = 0; k < 16; k++) {
            uint32_t t = u32at(b, o + 8 + 4 * k);
            e.track[k] = t ? (int32_t)t : -1;
        }
    }
    return true;
}

/* MSEQ: +0x0C groups, +0x10 total, +0x14 {tracks offset, count} per group;
 * a track: group, name, title, artist (string offsets), seq index. */
bool parse_music_inf(const std::vector<uint8_t> &b, std::vector<std::vector<MusicTrack>> &groups,
                     std::string &err)
{
    if (b.size() < 0x14 || std::memcmp(b.data(), "MSEQ", 4) != 0) {
        err = "not a music.inf";
        return false;
    }
    uint32_t ng = u32at(b, 0x0C);
    groups.assign(ng, {});
    for (uint32_t g = 0; g < ng; g++) {
        uint32_t to = u32at(b, 0x14 + 8 * g), cnt = u32at(b, 0x18 + 8 * g);
        for (uint32_t t = 0; t < cnt; t++) {
            uint32_t o = to + 0x14 * t;
            MusicTrack mt;
            mt.group = cstr(b, u32at(b, o));
            mt.name = cstr(b, u32at(b, o + 4));
            mt.title = cstr(b, u32at(b, o + 8));
            mt.artist = cstr(b, u32at(b, o + 12));
            mt.seq = (int32_t)u32at(b, o + 16);
            groups[g].push_back(mt);
        }
    }
    return true;
}
