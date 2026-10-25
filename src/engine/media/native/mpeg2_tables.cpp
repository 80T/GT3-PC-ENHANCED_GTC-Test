/* mpeg2_tables.cpp - the MPEG-2 video tables (ISO/IEC 13818-2 Annex B)
 * of the native decoder.  See mpeg2_tables.h. */
#include "engine/media/native/mpeg2_tables.h"

#include <cstdio>
#include <cstring>

namespace gt3media {

namespace {

/* A code of the standard's tables: the bits as a string (spaces ignored)
 * and what it decodes to. */
struct Code {
    const char *bits;
    int a;          /* value, or run */
    int b;          /* level (DCT tables) */
};

/* B.1 macroblock_address_increment. */
const Code kMbaInc[] = {
    {"1", 1},              {"011", 2},            {"010", 3},
    {"0011", 4},           {"0010", 5},           {"0001 1", 6},
    {"0001 0", 7},         {"0000 111", 8},       {"0000 110", 9},
    {"0000 1011", 10},     {"0000 1010", 11},     {"0000 1001", 12},
    {"0000 1000", 13},     {"0000 0111", 14},     {"0000 0110", 15},
    {"0000 0101 11", 16},  {"0000 0101 10", 17},  {"0000 0101 01", 18},
    {"0000 0101 00", 19},  {"0000 0100 11", 20},  {"0000 0100 10", 21},
    {"0000 0100 011", 22}, {"0000 0100 010", 23}, {"0000 0100 001", 24},
    {"0000 0100 000", 25}, {"0000 0011 111", 26}, {"0000 0011 110", 27},
    {"0000 0011 101", 28}, {"0000 0011 100", 29}, {"0000 0011 011", 30},
    {"0000 0011 010", 31}, {"0000 0011 001", 32}, {"0000 0011 000", 33},
    {"0000 0001 000", MBA_ESCAPE},
    {"0000 0001 111", MBA_STUFFING},
};

/* B.2 / B.3 / B.4 macroblock_type. */
const Code kMbTypeI[] = {
    {"1", MBT_INTRA},
    {"01", MBT_INTRA | MBT_QUANT},
};
const Code kMbTypeP[] = {
    {"1", MBT_FWD | MBT_PATTERN},
    {"01", MBT_PATTERN},
    {"001", MBT_FWD},
    {"0001 1", MBT_INTRA},
    {"0001 0", MBT_FWD | MBT_PATTERN | MBT_QUANT},
    {"0000 1", MBT_PATTERN | MBT_QUANT},
    {"0000 01", MBT_INTRA | MBT_QUANT},
};
const Code kMbTypeB[] = {
    {"10", MBT_FWD | MBT_BWD},
    {"11", MBT_FWD | MBT_BWD | MBT_PATTERN},
    {"010", MBT_BWD},
    {"011", MBT_BWD | MBT_PATTERN},
    {"0010", MBT_FWD},
    {"0011", MBT_FWD | MBT_PATTERN},
    {"0001 1", MBT_INTRA},
    {"0001 0", MBT_FWD | MBT_BWD | MBT_PATTERN | MBT_QUANT},
    {"0000 11", MBT_FWD | MBT_PATTERN | MBT_QUANT},
    {"0000 10", MBT_BWD | MBT_PATTERN | MBT_QUANT},
    {"0000 01", MBT_INTRA | MBT_QUANT},
};

/* B.9 coded_block_pattern (blocks 0..5 = bits 5..0). */
const Code kCbp[] = {
    {"111", 60},        {"1101", 4},        {"1100", 8},        {"1011", 16},
    {"1010", 32},       {"1001 1", 12},     {"1001 0", 48},     {"1000 1", 20},
    {"1000 0", 40},     {"0111 1", 28},     {"0111 0", 44},     {"0110 1", 52},
    {"0110 0", 56},     {"0101 1", 1},      {"0101 0", 61},     {"0100 1", 2},
    {"0100 0", 62},     {"0011 11", 24},    {"0011 10", 36},    {"0011 01", 3},
    {"0011 00", 63},    {"0010 111", 5},    {"0010 110", 9},    {"0010 101", 17},
    {"0010 100", 33},   {"0010 011", 6},    {"0010 010", 10},   {"0010 001", 18},
    {"0010 000", 34},   {"0001 1111", 7},   {"0001 1110", 11},  {"0001 1101", 19},
    {"0001 1100", 35},  {"0001 1011", 13},  {"0001 1010", 49},  {"0001 1001", 21},
    {"0001 1000", 41},  {"0001 0111", 14},  {"0001 0110", 50},  {"0001 0101", 22},
    {"0001 0100", 42},  {"0001 0011", 15},  {"0001 0010", 51},  {"0001 0001", 23},
    {"0001 0000", 43},  {"0000 1111", 25},  {"0000 1110", 37},  {"0000 1101", 26},
    {"0000 1100", 38},  {"0000 1011", 29},  {"0000 1010", 45},  {"0000 1001", 53},
    {"0000 1000", 57},  {"0000 0111", 30},  {"0000 0110", 46},  {"0000 0101", 54},
    {"0000 0100", 58},  {"0000 0011 1", 31}, {"0000 0011 0", 47}, {"0000 0010 1", 55},
    {"0000 0010 0", 59}, {"0000 0001 1", 27}, {"0000 0001 0", 39}, {"0000 0000 1", 0},
};

/* B.10 motion_code: the magnitude (a sign bit follows when nonzero). */
const Code kMotionCode[] = {
    {"1", 0},             {"01", 1},            {"001", 2},
    {"0001", 3},          {"0000 11", 4},       {"0000 101", 5},
    {"0000 100", 6},      {"0000 011", 7},      {"0000 0101 1", 8},
    {"0000 0101 0", 9},   {"0000 0100 1", 10},  {"0000 0100 01", 11},
    {"0000 0100 00", 12}, {"0000 0011 11", 13}, {"0000 0011 10", 14},
    {"0000 0011 01", 15}, {"0000 0011 00", 16},
};

/* B.12 / B.13 dct_dc_size. */
const Code kDcLum[] = {
    {"100", 0},  {"00", 1},  {"01", 2},  {"101", 3},  {"110", 4},  {"1110", 5},
    {"1111 0", 6},  {"1111 10", 7},  {"1111 110", 8},  {"1111 1110", 9},
    {"1111 1111 0", 10},  {"1111 1111 1", 11},
};
const Code kDcChrom[] = {
    {"00", 0},  {"01", 1},  {"10", 2},  {"110", 3},  {"1110", 4},  {"1111 0", 5},
    {"1111 10", 6},  {"1111 110", 7},  {"1111 1110", 8},  {"1111 1111 0", 9},
    {"1111 1111 10", 10},  {"1111 1111 11", 11},
};

/* B.14 DCT coefficients table zero: run, level (a sign bit follows).  "11"
 * is (0, 1) for every coefficient but the first of a non-intra block,
 * where "1" is (the decoder handles that case itself). */
const Code kDct0[] = {
    {"10", DCT_EOB, 0},
    {"11", 0, 1},
    {"011", 1, 1},
    {"0100", 0, 2},          {"0101", 2, 1},
    {"0010 1", 0, 3},        {"0011 1", 3, 1},        {"0011 0", 4, 1},
    {"0001 10", 1, 2},       {"0001 11", 5, 1},       {"0001 01", 6, 1},
    {"0001 00", 7, 1},
    {"0000 110", 0, 4},      {"0000 100", 2, 2},      {"0000 111", 8, 1},
    {"0000 101", 9, 1},
    {"0000 01", DCT_ESCAPE, 0},
    {"0010 0110", 0, 5},     {"0010 0001", 0, 6},     {"0010 0101", 1, 3},
    {"0010 0100", 3, 2},     {"0010 0111", 10, 1},    {"0010 0011", 11, 1},
    {"0010 0010", 12, 1},    {"0010 0000", 13, 1},
    {"0000 0010 10", 0, 7},  {"0000 0011 00", 1, 4},  {"0000 0010 11", 2, 3},
    {"0000 0011 11", 4, 2},  {"0000 0010 01", 5, 2},  {"0000 0011 10", 14, 1},
    {"0000 0011 01", 15, 1}, {"0000 0010 00", 16, 1},
    /* from here on the codes are shared with table one (kDctLong) */
};

/* The long codes (seven or more leading zeros) of both tables. */
const Code kDctLong[] = {
    {"0000 0001 1101", 0, 8},     {"0000 0001 1000", 0, 9},
    {"0000 0001 0011", 0, 10},    {"0000 0001 0000", 0, 11},
    {"0000 0001 1011", 1, 5},     {"0000 0001 0100", 2, 4},
    {"0000 0001 1100", 3, 3},     {"0000 0001 0010", 4, 3},
    {"0000 0001 1110", 6, 2},     {"0000 0001 0101", 7, 2},
    {"0000 0001 0001", 8, 2},     {"0000 0001 1111", 17, 1},
    {"0000 0001 1010", 18, 1},    {"0000 0001 1001", 19, 1},
    {"0000 0001 0111", 20, 1},    {"0000 0001 0110", 21, 1},
    {"0000 0000 1101 0", 0, 12},  {"0000 0000 1100 1", 0, 13},
    {"0000 0000 1100 0", 0, 14},  {"0000 0000 1011 1", 0, 15},
    {"0000 0000 1011 0", 1, 6},   {"0000 0000 1010 1", 1, 7},
    {"0000 0000 1010 0", 2, 5},   {"0000 0000 1001 1", 3, 4},
    {"0000 0000 1001 0", 5, 3},   {"0000 0000 1000 1", 9, 2},
    {"0000 0000 1000 0", 10, 2},  {"0000 0000 1111 1", 22, 1},
    {"0000 0000 1111 0", 23, 1},  {"0000 0000 1110 1", 24, 1},
    {"0000 0000 1110 0", 25, 1},  {"0000 0000 1101 1", 26, 1},
    {"0000 0000 0111 11", 0, 16}, {"0000 0000 0111 10", 0, 17},
    {"0000 0000 0111 01", 0, 18}, {"0000 0000 0111 00", 0, 19},
    {"0000 0000 0110 11", 0, 20}, {"0000 0000 0110 10", 0, 21},
    {"0000 0000 0110 01", 0, 22}, {"0000 0000 0110 00", 0, 23},
    {"0000 0000 0101 11", 0, 24}, {"0000 0000 0101 10", 0, 25},
    {"0000 0000 0101 01", 0, 26}, {"0000 0000 0101 00", 0, 27},
    {"0000 0000 0100 11", 0, 28}, {"0000 0000 0100 10", 0, 29},
    {"0000 0000 0100 01", 0, 30}, {"0000 0000 0100 00", 0, 31},
    {"0000 0000 0011 000", 0, 32}, {"0000 0000 0010 111", 0, 33},
    {"0000 0000 0010 110", 0, 34}, {"0000 0000 0010 101", 0, 35},
    {"0000 0000 0010 100", 0, 36}, {"0000 0000 0010 011", 0, 37},
    {"0000 0000 0010 010", 0, 38}, {"0000 0000 0010 001", 0, 39},
    {"0000 0000 0010 000", 0, 40}, {"0000 0000 0011 111", 1, 8},
    {"0000 0000 0011 110", 1, 9},  {"0000 0000 0011 101", 1, 10},
    {"0000 0000 0011 100", 1, 11}, {"0000 0000 0011 011", 1, 12},
    {"0000 0000 0011 010", 1, 13}, {"0000 0000 0011 001", 1, 14},
    {"0000 0000 0001 0011", 1, 15}, {"0000 0000 0001 0010", 1, 16},
    {"0000 0000 0001 0001", 1, 17}, {"0000 0000 0001 0000", 1, 18},
    {"0000 0000 0001 0100", 6, 3},  {"0000 0000 0001 1010", 11, 2},
    {"0000 0000 0001 1001", 12, 2}, {"0000 0000 0001 1000", 13, 2},
    {"0000 0000 0001 0111", 14, 2}, {"0000 0000 0001 0110", 15, 2},
    {"0000 0000 0001 0101", 16, 2}, {"0000 0000 0001 1111", 27, 1},
    {"0000 0000 0001 1110", 28, 1}, {"0000 0000 0001 1101", 29, 1},
    {"0000 0000 0001 1100", 30, 1}, {"0000 0000 0001 1011", 31, 1},
};

/* B.15 DCT coefficients table one (intra_vlc_format 1): the codes with
 * fewer than seven leading zeros; the rest are kDctLong.  (B.15 itself
 * leaves some of those long codes unused; decoding them as table zero
 * does is harmless for valid streams and what libmpeg2-style decoders,
 * and so PCSX2's IPU, do.) */
const Code kDct1[] = {
    {"0110", DCT_EOB, 0},
    {"10", 0, 1},            {"010", 1, 1},           {"110", 0, 2},
    {"0010 1", 2, 1},        {"0111", 0, 3},          {"0011 1", 3, 1},
    {"0001 10", 4, 1},       {"0011 0", 1, 2},        {"0001 11", 5, 1},
    {"0000 110", 6, 1},      {"0000 100", 7, 1},      {"1110 0", 0, 4},
    {"0000 111", 2, 2},      {"0000 101", 8, 1},      {"1111 000", 9, 1},
    {"0000 01", DCT_ESCAPE, 0},
    {"1110 1", 0, 5},        {"0001 01", 0, 6},       {"1111 001", 1, 3},
    {"0010 0110", 3, 2},     {"1111 010", 10, 1},     {"0010 0001", 11, 1},
    {"0010 0101", 12, 1},    {"0010 0100", 13, 1},    {"0001 00", 0, 7},
    {"0010 0111", 1, 4},     {"1111 1100", 2, 3},     {"1111 1101", 4, 2},
    {"0000 0010 0", 5, 2},   {"0000 0010 1", 14, 1},  {"0000 0011 1", 15, 1},
    {"0000 0011 01", 16, 1}, {"1111 011", 0, 8},      {"1111 100", 0, 9},
    {"0010 0011", 0, 10},    {"0010 0010", 0, 11},    {"0010 0000", 1, 5},
    {"0000 0011 00", 2, 4},  {"1111 1010", 0, 12},    {"1111 1011", 0, 13},
    {"1111 1110", 0, 14},    {"1111 1111", 0, 15},
};

const uint8_t kZigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

const uint8_t kAlternate[64] = {
     0,  8, 16, 24,  1,  9,  2, 10, 17, 25, 32, 40, 48, 56, 57, 49,
    41, 33, 26, 18,  3, 11,  4, 12, 19, 27, 34, 42, 50, 58, 35, 43,
    51, 59, 20, 28,  5, 13,  6, 14, 21, 29, 36, 44, 52, 60, 37, 45,
    53, 61, 22, 30,  7, 15, 23, 31, 38, 46, 54, 62, 39, 47, 55, 63
};

/* The default intra matrix in zigzag order, as the PS2's ipu_init loads
 * it into the IPU (g_ipu_default_iq, ipu.c 0x002FF0D0). */
const uint8_t kDefaultIntraZigzag[64] = {
     8, 16, 16, 19, 16, 19, 22, 22, 22, 22, 22, 22, 26, 24, 26, 27,
    27, 27, 26, 26, 26, 26, 27, 27, 27, 29, 29, 29, 34, 34, 34, 29,
    29, 29, 27, 27, 29, 29, 32, 32, 34, 34, 37, 38, 37, 35, 35, 34,
    35, 38, 38, 40, 40, 40, 48, 48, 46, 46, 56, 56, 58, 69, 69, 83
};

const uint8_t kNonLinearQ[32] = {
     0,  1,  2,  3,  4,  5,  6,  7,  8, 10, 12, 14, 16, 18, 20, 22,
    24, 28, 32, 36, 40, 44, 48, 52, 56, 64, 72, 80, 88, 96, 104, 112
};

/* Parses a code string: returns its length, the bits in *bits. */
int parse_code(const char *s, uint32_t *bits)
{
    int len = 0;
    uint32_t v = 0;
    for (; *s; s++) {
        if (*s == '0' || *s == '1') {
            v = (v << 1) | (uint32_t)(*s - '0');
            len++;
        }
    }
    *bits = v;
    return len;
}

/* Builder state: the first error found (the tables are fixed, so an error
 * is a typo in the code lists above; mpeg2_tables_selftest reports it). */
char g_buildError[256];

void build_error(const char *what, const char *code)
{
    if (g_buildError[0] == 0)
        snprintf(g_buildError, sizeof(g_buildError), "%s: code %s", what, code);
}

/* Fills the entries of a table indexed by `bits' bits for every code that
 * fits (skipping `prefixLen' leading bits that every code of the table
 * shares when prefixLen > 0). */
template <typename Entry, typename Fill>
void fill_table(Entry *table, int bits, const Code *codes, int n, int prefixLen,
                bool (*accept)(uint32_t code, int len), Fill fill, const char *name)
{
    for (int i = 0; i < n; i++) {
        uint32_t code;
        int len = parse_code(codes[i].bits, &code);
        if (accept != nullptr && !accept(code, len))
            continue;
        int rem = len - prefixLen;
        if (rem <= 0 || rem > bits) {
            build_error(name, codes[i].bits);
            continue;
        }
        uint32_t idx = (code & ((1u << rem) - 1)) << (bits - rem);
        uint32_t count = 1u << (bits - rem);
        for (uint32_t k = 0; k < count; k++) {
            Entry &e = table[idx + k];
            if (e.len != 0)
                build_error(name, codes[i].bits);     /* not prefix-free */
            fill(e, codes[i], len);
        }
    }
}

void fill_vlc(Vlc &e, const Code &c, int len)
{
    e.value = (int16_t)c.a;
    e.len = (uint8_t)len;
}

void fill_dct(DctVlc &e, const Code &c, int len)
{
    e.run = (uint8_t)c.a;
    e.level = (uint8_t)c.b;
    e.len = (uint8_t)len;
}

/* DCT table split: A = codes whose first six bits are not all zero
 * (escape included), B = the others (indexed by bits 6..15). */
bool dct_is_a(uint32_t code, int len)
{
    return len < 7 ? true : (code >> (len - 6)) != 0;
}

bool dct_is_b(uint32_t code, int len)
{
    return !dct_is_a(code, len);
}

/* For the B tables the six zero bits are part of the code; codes are
 * filled relative to bit 6 (prefixLen 6). */

struct Builder {
    Mpeg2Tables t;

    Builder()
    {
        std::memset(&t, 0, sizeof(t));
#define N(a) (int)(sizeof(a) / sizeof((a)[0]))
        fill_table(t.mbaInc, 11, kMbaInc, N(kMbaInc), 0, nullptr, fill_vlc, "mba");
        fill_table(t.mbType[1], 6, kMbTypeI, N(kMbTypeI), 0, nullptr, fill_vlc, "mbtype I");
        fill_table(t.mbType[2], 6, kMbTypeP, N(kMbTypeP), 0, nullptr, fill_vlc, "mbtype P");
        fill_table(t.mbType[3], 6, kMbTypeB, N(kMbTypeB), 0, nullptr, fill_vlc, "mbtype B");
        fill_table(t.cbp, 9, kCbp, N(kCbp), 0, nullptr, fill_vlc, "cbp");
        fill_table(t.motionCode, 10, kMotionCode, N(kMotionCode), 0, nullptr, fill_vlc, "motion_code");
        fill_table(t.dcLum, 9, kDcLum, N(kDcLum), 0, nullptr, fill_vlc, "dc lum");
        fill_table(t.dcChrom, 10, kDcChrom, N(kDcChrom), 0, nullptr, fill_vlc, "dc chrom");

        fill_table(t.dctA[0], 9, kDct0, N(kDct0), 0, dct_is_a, fill_dct, "dct0 A");
        fill_table(t.dctB[0], 10, kDct0, N(kDct0), 6, dct_is_b, fill_dct, "dct0 B");
        fill_table(t.dctB[0], 10, kDctLong, N(kDctLong), 6, dct_is_b, fill_dct, "dct0 long");
        fill_table(t.dctA[1], 9, kDct1, N(kDct1), 0, dct_is_a, fill_dct, "dct1 A");
        fill_table(t.dctB[1], 10, kDct1, N(kDct1), 6, dct_is_b, fill_dct, "dct1 B");
        fill_table(t.dctB[1], 10, kDctLong, N(kDctLong), 6, dct_is_b, fill_dct, "dct1 long");
#undef N
        std::memcpy(t.scan[0], kZigzag, 64);
        std::memcpy(t.scan[1], kAlternate, 64);
        for (int i = 0; i < 64; i++)
            t.defaultIntra[kZigzag[i]] = kDefaultIntraZigzag[i];
        std::memcpy(t.nonLinearQ, kNonLinearQ, 32);
    }
};

/* Sum of 2^-len over the valid entries' codes of a table indexed by
 * `bits' bits (each code covers 2^(bits - len) entries): the Kraft sum. */
template <typename Entry>
double kraft(const Entry *table, int bits, int prefixLen)
{
    double sum = 0;
    for (int i = 0; i < (1 << bits); i++) {
        if (table[i].len != 0)
            sum += 1.0 / (double)(1 << bits);
    }
    return sum / (double)(1 << prefixLen);
}

} /* namespace */

const Mpeg2Tables &mpeg2_tables()
{
    static const Builder builder;
    return builder.t;
}

int mpeg2_tables_selftest(char *msg, int msgSize)
{
    const Mpeg2Tables &t = mpeg2_tables();
    if (g_buildError[0] != 0) {
        snprintf(msg, msgSize, "table build: %s", g_buildError);
        return 0;
    }
    for (int s = 0; s < 2; s++) {
        uint8_t seen[64] = {0};
        for (int i = 0; i < 64; i++)
            seen[t.scan[s][i]]++;
        for (int i = 0; i < 64; i++) {
            if (seen[i] != 1) {
                snprintf(msg, msgSize, "scan %d is not a permutation", s);
                return 0;
            }
        }
    }
    /* Completeness: the tables the standard fills completely (the DCT
     * tables up to their one unused 12-zero prefix, the others up to their
     * invalid codes). */
    struct Check { const char *name; double got, want; } checks[] = {
        {"dc lum", kraft(t.dcLum, 9, 0), 1.0},
        {"dc chrom", kraft(t.dcChrom, 10, 0), 1.0},
        {"cbp", kraft(t.cbp, 9, 0), 1.0 - 1.0 / 512},
        /* unused: 0000 0000 xxx, 0000 0010 xxx and six of 0000 0001 xxx */
        {"mba", kraft(t.mbaInc, 11, 0), 1.0 - 2.0 / 256 - 6.0 / 2048},
        {"dct0", kraft(t.dctA[0], 9, 0) + kraft(t.dctB[0], 10, 6), 1.0 - 1.0 / 4096},
        {"dct1", kraft(t.dctA[1], 9, 0) + kraft(t.dctB[1], 10, 6), 1.0 - 1.0 / 4096},
    };
    for (const Check &c : checks) {
        if (c.got < c.want - 1e-9 || c.got > c.want + 1e-9) {
            snprintf(msg, msgSize, "%s: code space %.6f, expected %.6f", c.name, c.got, c.want);
            return 0;
        }
    }
    snprintf(msg, msgSize, "ok");
    return 1;
}

} /* namespace gt3media */
