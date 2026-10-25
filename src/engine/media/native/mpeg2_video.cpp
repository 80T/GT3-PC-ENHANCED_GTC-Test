/* mpeg2_video.cpp - the native MPEG-2 video decoder.  See mpeg2_video.h.
 *
 * The structure follows ISO 13818-2 and the PS2 player it replaces
 * (mpeg_player.c): start codes and headers (mpeg_video_next_picture
 * 0x002614A0 and the header parsers after it), one picture at a time
 * (mpeg_video_decode_picture 0x00260E60), slices and macroblocks
 * (mpeg_video_decode_slice 0x00260EC0, mpeg_video_macroblock_modes
 * 0x00261260), motion vectors (0x00261A90..0x00261C98), three frame
 * stores rotated per reference picture (mpeg_video_rotate_frames
 * 0x002613C0) and display-order output (mpeg_video_output_picture
 * 0x00260BD8: a B picture at once, an I / P picture when the next
 * reference arrives, the last one at the sequence end).
 *
 * Differences from the PS2 player, all where it departs from the
 * standard: frame stores are planar instead of column-major macroblocks;
 * the chroma vector is vector / 2 truncated toward zero (7.6.3.7; the PS2
 * shifts, rounding toward minus infinity, mpeg_mc_form_prediction
 * 0x00261DE0); field prediction in frame pictures and field DCT are done
 * (the PS2 player only uses the first vector, frame prediction; GT3's
 * movies need nothing more); macroblocks lost to a stream error are
 * concealed from the reference instead of left as they were.
 */
#include "engine/media/native/mpeg2_video.h"

#include "engine/media/native/mpeg2_bits.h"
#include "engine/media/native/mpeg2_block.h"
#include "engine/media/native/mpeg2_idct.h"
#include "engine/media/native/mpeg2_tables.h"

#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>

#if !defined(GT3MEDIA_NO_SSE2) && (defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2) || defined(__SSE2__))
#define GT3MEDIA_SSE2 1
#include <emmintrin.h>
#endif

namespace gt3media {

namespace {

enum { PCT_I = 1, PCT_P = 2, PCT_B = 3, PCT_D = 4 };
enum { PS_TOP = 1, PS_BOTTOM = 2, PS_FRAME = 3 };
enum { MT_FIELD = 1, MT_FRAME = 2, MT_DUAL_PRIME = 3 };

const size_t NPOS = (size_t)-1;

struct Frame {
    std::vector<uint8_t> mem;
    uint8_t *y = nullptr;
    uint8_t *cb = nullptr;
    uint8_t *cr = nullptr;
    int type = 0;
    int temporalRef = 0;
    int64_t pts = -1;
    bool tff = false;
    bool rff = false;
    bool progressive = true;
    int concealed = 0;
    int64_t decodeIndex = 0;
    int64_t sequenceEnds = 0;
};

/* A plane (or one field of it) as a prediction source. */
struct Plane {
    const uint8_t *base;
    int stride;
    int w;
    int h;
};

/* ------------------------------------------------------------------------ */
/* Motion compensation kernels: put (first direction) and average (second:  */
/* (d + p + 1) >> 1), whole / half-pel horizontally / vertically / both.    */
/* ------------------------------------------------------------------------ */

template <int W, bool AVG>
void mc_block(uint8_t *d, int ds, const uint8_t *s, int ss, int h, int half)
{
    switch (half) {
    case 0:
        for (int y = 0; y < h; y++, d += ds, s += ss)
            for (int x = 0; x < W; x++) {
                int p = s[x];
                d[x] = (uint8_t)(AVG ? (d[x] + p + 1) >> 1 : p);
            }
        break;
    case 1:
        for (int y = 0; y < h; y++, d += ds, s += ss)
            for (int x = 0; x < W; x++) {
                int p = (s[x] + s[x + 1] + 1) >> 1;
                d[x] = (uint8_t)(AVG ? (d[x] + p + 1) >> 1 : p);
            }
        break;
    case 2:
        for (int y = 0; y < h; y++, d += ds, s += ss)
            for (int x = 0; x < W; x++) {
                int p = (s[x] + s[x + ss] + 1) >> 1;
                d[x] = (uint8_t)(AVG ? (d[x] + p + 1) >> 1 : p);
            }
        break;
    default:
        for (int y = 0; y < h; y++, d += ds, s += ss)
            for (int x = 0; x < W; x++) {
                int p = (s[x] + s[x + 1] + s[x + ss] + s[x + ss + 1] + 2) >> 2;
                d[x] = (uint8_t)(AVG ? (d[x] + p + 1) >> 1 : p);
            }
        break;
    }
}

#if defined(GT3MEDIA_SSE2)
/* SSE2: pavgb is (a + b + 1) >> 1, the half-pel and the averaging
 * rounding; the 2-D case sums in 16 bits. */
template <int W>
inline __m128i mc_load(const uint8_t *p)
{
    return W == 16 ? _mm_loadu_si128((const __m128i *)p) : _mm_loadl_epi64((const __m128i *)p);
}

template <int W>
inline void mc_store(uint8_t *p, __m128i v)
{
    if (W == 16)
        _mm_storeu_si128((__m128i *)p, v);
    else
        _mm_storel_epi64((__m128i *)p, v);
}

inline __m128i avg4(__m128i a, __m128i b, __m128i c, __m128i e)
{
    const __m128i z = _mm_setzero_si128(), two = _mm_set1_epi16(2);
    __m128i lo = _mm_add_epi16(_mm_add_epi16(_mm_unpacklo_epi8(a, z), _mm_unpacklo_epi8(b, z)),
                               _mm_add_epi16(_mm_unpacklo_epi8(c, z), _mm_unpacklo_epi8(e, z)));
    __m128i hi = _mm_add_epi16(_mm_add_epi16(_mm_unpackhi_epi8(a, z), _mm_unpackhi_epi8(b, z)),
                               _mm_add_epi16(_mm_unpackhi_epi8(c, z), _mm_unpackhi_epi8(e, z)));
    lo = _mm_srli_epi16(_mm_add_epi16(lo, two), 2);
    hi = _mm_srli_epi16(_mm_add_epi16(hi, two), 2);
    return _mm_packus_epi16(lo, hi);
}

template <int W, bool AVG>
void mc_block_sse2(uint8_t *d, int ds, const uint8_t *s, int ss, int h, int half)
{
    for (int y = 0; y < h; y++, d += ds, s += ss) {
        __m128i p;
        switch (half) {
        case 0:
            p = mc_load<W>(s);
            break;
        case 1:
            p = _mm_avg_epu8(mc_load<W>(s), mc_load<W>(s + 1));
            break;
        case 2:
            p = _mm_avg_epu8(mc_load<W>(s), mc_load<W>(s + ss));
            break;
        default:
            p = avg4(mc_load<W>(s), mc_load<W>(s + 1), mc_load<W>(s + ss), mc_load<W>(s + ss + 1));
            break;
        }
        if (AVG)
            p = _mm_avg_epu8(p, mc_load<W>(d));
        mc_store<W>(d, p);
    }
}
#endif

template <int W>
inline void mc(uint8_t *d, int ds, const uint8_t *s, int ss, int h, int half, bool avg)
{
#if defined(GT3MEDIA_SSE2)
    if (avg)
        mc_block_sse2<W, true>(d, ds, s, ss, h, half);
    else
        mc_block_sse2<W, false>(d, ds, s, ss, h, half);
#else
    if (avg)
        mc_block<W, true>(d, ds, s, ss, h, half);
    else
        mc_block<W, false>(d, ds, s, ss, h, half);
#endif
}

inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* frame_rate_code -> pictures per second. */
double frame_rate(int code)
{
    static const double rates[16] = {
        0, 24000.0 / 1001, 24, 25, 30000.0 / 1001, 30, 50, 60000.0 / 1001, 60,
        0, 0, 0, 0, 0, 0, 0
    };
    return rates[code & 15];
}

} /* namespace */

/* ======================================================================== */
/* The decoder                                                              */
/* ======================================================================== */

struct Mpeg2VideoDecoder::Impl {
    const Mpeg2Tables &t = mpeg2_tables();

    /* Elementary-stream buffer: [rd, end) not yet decoded, then
     * kBitReaderPadding zeros. */
    std::vector<uint8_t> buf;
    size_t rd = 0;
    size_t end = 0;
    size_t scanFrom = 0;    /* where the search for the current picture's end resumes */
    struct PtsMark {
        size_t start, stop;
        int64_t pts;
    };
    std::deque<PtsMark> ptsMarks;
    bool eos = false;
    bool eosFlushed = false;

    /* Sequence. */
    SequenceInfo seq;
    bool seqExtPending = false;     /* a sequence header without its extension yet */
    int mbW = 0, mbH = 0;
    int ls = 0, cs = 0;             /* luma / chroma strides */
    uint8_t intraW[64];
    uint8_t nonIntraW[64];

    /* Picture header and coding extension. */
    int pct = 0;
    int temporalRef = 0;
    int fcode[2][2] = {{15, 15}, {15, 15}};
    int dcPrec = 0;
    int picStruct = PS_FRAME;
    int tff = 0, fpfd = 1, cmv = 0, qst = 0, ivf = 0, altScan = 0, rff = 0, progFrame = 1;
    bool havePce = false;

    /* Frame stores. */
    Frame frames[3];
    int fwd = -1;                   /* older reference */
    int bwd = -1;                   /* newer reference */
    int cur = -1;
    bool bwdOutput = true;          /* the newer reference has been output */
    std::deque<int> outQueue;
    int64_t decodeCount = 0;

    /* Slice / macroblock state. */
    int qs = -1;
    alignas(16) uint16_t wqIntra[64];
    alignas(16) uint16_t wqNonIntra[64];
    CoefParams intraParams;
    CoefParams nonIntraParams;
    int dcPred[3];
    int pmv[2][2][2];
    int mvfs[2][2];
    int prevMbType = 0;
    int prevMotionType = MT_FRAME;
    std::vector<uint8_t> mbDone;
    alignas(16) int16_t blk[64];
    alignas(16) uint8_t fetchTmp[32 * 24];

    VideoStats stats;

    Impl()
    {
        std::memset(blk, 0, sizeof(blk));
        resetMatrices();
        buf.resize(kBitReaderPadding, 0);
    }

    void error(const char *msg)
    {
        stats.errors++;
        snprintf(stats.lastError, sizeof(stats.lastError), "%s", msg);
    }

    void resetMatrices()
    {
        std::memcpy(intraW, t.defaultIntra, 64);
        std::memset(nonIntraW, 16, 64);
        qs = -1;
    }

    /* -------------------------------------------------------------------- */
    /* Input                                                                */
    /* -------------------------------------------------------------------- */

    void compact()
    {
        if (rd == 0 || (rd < (1u << 20) && rd < end / 2))
            return;
        size_t n = end - rd;
        std::memmove(buf.data(), buf.data() + rd, n + kBitReaderPadding);
        for (PtsMark &m : ptsMarks) {
            m.start = m.start > rd ? m.start - rd : 0;
            m.stop = m.stop > rd ? m.stop - rd : 0;
        }
        scanFrom = scanFrom > rd ? scanFrom - rd : 0;
        end = n;
        rd = 0;
    }

    void push(const uint8_t *data, size_t size, int64_t pts)
    {
        compact();
        if (pts >= 0)
            ptsMarks.push_back({end, end + size, pts});
        if (buf.size() < end + size + kBitReaderPadding)
            buf.resize((end + size) * 2 + kBitReaderPadding);
        std::memcpy(buf.data() + end, data, size);
        end += size;
        std::memset(buf.data() + end, 0, kBitReaderPadding);
        eos = false;
        eosFlushed = false;
    }

    /* The next start code prefix at or after pos with its code byte
     * available. */
    size_t findStartCode(size_t pos) const
    {
        const uint8_t *b = buf.data();
        while (pos + 4 <= end) {
            if (b[pos + 2] > 1)
                pos += 3;
            else if (b[pos] == 0 && b[pos + 1] == 0 && b[pos + 2] == 1)
                return pos;
            else
                pos++;
        }
        return NPOS;
    }

    /* The end of the picture starting at sc: the next start code that is
     * not a slice, an extension or user data. */
    size_t findPictureEnd(size_t from)
    {
        size_t pos = from;
        for (;;) {
            size_t s = findStartCode(pos);
            if (s == NPOS) {
                scanFrom = end >= 3 ? end - 3 : 0;
                if (scanFrom < from)
                    scanFrom = from;
                return NPOS;
            }
            uint8_t c = buf[s + 3];
            if (c == 0x00 || c == 0xB3 || c == 0xB7 || c == 0xB8 || c >= 0xB9)
                return s;
            pos = s + 4;
        }
    }

    int64_t takePts(size_t pos)
    {
        while (!ptsMarks.empty() && ptsMarks.front().stop <= pos)
            ptsMarks.pop_front();
        if (!ptsMarks.empty() && ptsMarks.front().start <= pos) {
            int64_t p = ptsMarks.front().pts;
            ptsMarks.pop_front();
            return p;
        }
        return -1;
    }

    /* One unit of the stream; false when nothing could be done. */
    bool step()
    {
        size_t sc = findStartCode(rd);
        if (sc == NPOS) {
            if (eos) {
                if (!eosFlushed) {
                    eosFlushed = true;
                    rd = end;
                    flushReference();
                    return true;
                }
                return false;
            }
            if (end >= rd + 3)
                rd = end - 3;           /* keep a possible partial prefix */
            return false;
        }
        rd = sc;
        uint8_t code = buf[sc + 3];
        if (code == 0x00) {
            size_t from = scanFrom > sc + 4 ? scanFrom : sc + 4;
            size_t stop = findPictureEnd(from);
            if (stop == NPOS) {
                if (!eos)
                    return false;
                stop = end;
            }
            decodePicture(sc, stop);
            rd = stop;
            scanFrom = 0;
            return true;
        }
        if (code >= 0x01 && code <= 0xAF) {         /* slice outside a picture */
            rd = sc + 4;
            return true;
        }
        if (code == 0xB7) {                         /* sequence_end_code */
            stats.sequenceEnds++;
            flushReference();
            fwd = bwd = -1;
            rd = sc + 4;
            return true;
        }
        size_t next = findStartCode(sc + 4);
        if (next == NPOS) {
            if (!eos)
                return false;
            next = end;
        }
        BitReader br(buf.data() + sc + 4, next - (sc + 4));
        switch (code) {
        case 0xB3:
            sequenceHeader(br);
            break;
        case 0xB5:
            extension(br);
            break;
        case 0xB8:
            stats.gops++;
            break;
        default:                                    /* user data, others */
            break;
        }
        rd = next;
        return true;
    }

    /* -------------------------------------------------------------------- */
    /* Headers                                                              */
    /* -------------------------------------------------------------------- */

    void sequenceHeader(BitReader &br)
    {
        int h = (int)br.get(12);
        int v = (int)br.get(12);
        int ar = (int)br.get(4);
        int frc = (int)br.get(4);
        int bitRate = (int)br.get(18);
        br.skip(1);                                 /* marker */
        br.skip(10);                                /* vbv_buffer_size_value */
        br.skip(1);                                 /* constrained_parameters_flag */
        resetMatrices();
        seq.loadIntraMatrix = br.getBit() != 0;
        if (seq.loadIntraMatrix)
            for (int i = 0; i < 64; i++)
                intraW[t.scan[0][i]] = (uint8_t)br.get(8);
        seq.loadNonIntraMatrix = br.getBit() != 0;
        if (seq.loadNonIntraMatrix)
            for (int i = 0; i < 64; i++)
                nonIntraW[t.scan[0][i]] = (uint8_t)br.get(8);
        for (int i = 0; i < 64; i++) {             /* a zero weight is invalid */
            if (intraW[i] == 0)
                intraW[i] = 1;
            if (nonIntraW[i] == 0)
                nonIntraW[i] = 1;
        }
        bool sizeChange = !seq.valid || (h != (seq.width & 0xFFF)) || (v != (seq.height & 0xFFF));
        if (sizeChange && seq.valid) {
            flushReference();
            fwd = bwd = -1;
        }
        seq.width = h;
        seq.height = v;
        seq.aspectRatioCode = ar;
        seq.frameRateCode = frc;
        seq.frameRate = frame_rate(frc);
        seq.bitRate = bitRate * 400;
        seq.valid = h > 0 && v > 0;
        seq.mpeg2 = false;
        seqExtPending = true;
        stats.sequences++;
        setupSize();
    }

    void extension(BitReader &br)
    {
        int id = (int)br.get(4);
        switch (id) {
        case 1: {                                   /* sequence_extension */
            seq.profileLevel = (int)br.get(8);
            seq.progressiveSequence = br.getBit() != 0;
            seq.chromaFormat = (int)br.get(2);
            int hx = (int)br.get(2);
            int vx = (int)br.get(2);
            int brx = (int)br.get(12);
            br.skip(1);
            br.skip(8);                             /* vbv_buffer_size_extension */
            br.skip(1);                             /* low_delay */
            int frn = (int)br.get(2);
            int frd = (int)br.get(5);
            seq.width = (hx << 12) | (seq.width & 0xFFF);
            seq.height = (vx << 12) | (seq.height & 0xFFF);
            seq.bitRate += brx * 400 * (1 << 18);
            seq.frameRate = frame_rate(seq.frameRateCode) * (frn + 1) / (frd + 1);
            seq.mpeg2 = true;
            seqExtPending = false;
            setupSize();
            break;
        }
        case 3: {                                   /* quant_matrix_extension */
            if (br.getBit())
                for (int i = 0; i < 64; i++)
                    intraW[t.scan[0][i]] = (uint8_t)br.get(8);
            if (br.getBit())
                for (int i = 0; i < 64; i++)
                    nonIntraW[t.scan[0][i]] = (uint8_t)br.get(8);
            for (int i = 0; i < 64; i++) {
                if (intraW[i] == 0)
                    intraW[i] = 1;
                if (nonIntraW[i] == 0)
                    nonIntraW[i] = 1;
            }
            qs = -1;
            break;
        }
        case 8: {                                   /* picture_coding_extension */
            fcode[0][0] = (int)br.get(4);
            fcode[0][1] = (int)br.get(4);
            fcode[1][0] = (int)br.get(4);
            fcode[1][1] = (int)br.get(4);
            dcPrec = (int)br.get(2);
            picStruct = (int)br.get(2);
            tff = br.getBit();
            fpfd = br.getBit();
            cmv = br.getBit();
            qst = br.getBit();
            ivf = br.getBit();
            altScan = br.getBit();
            rff = br.getBit();
            br.skip(1);                             /* chroma_420_type */
            progFrame = br.getBit();
            havePce = true;
            stats.pictureStructures |= 1u << picStruct;
            stats.intraDcPrecisions |= 1u << dcPrec;
            stats.qScaleTypes |= 1u << qst;
            stats.intraVlcFormats |= 1u << ivf;
            stats.alternateScans |= 1u << altScan;
            stats.framePredFrameDct |= 1u << fpfd;
            stats.concealmentVectors |= 1u << cmv;
            stats.progressiveFrames |= 1u << progFrame;
            stats.repeatFirstFields |= 1u << rff;
            stats.topFieldFirsts |= 1u << tff;
            break;
        }
        default:                                    /* display / scalable extensions */
            break;
        }
    }

    /* Macroblock size and frame stores for the sequence. */
    void setupSize()
    {
        if (!seq.valid)
            return;
        /* a damaged header: beyond the 12-bit size fields (was MP@HL,
         * 1920 x 1152: no size limit of the game's own, bigger movies play) */
        if (seq.width > 4096 || seq.height > 4096) {
            error("picture size out of range");
            seq.valid = false;
            return;
        }
        int w = (seq.width + 15) / 16;
        int h = seq.progressiveSequence || !seq.mpeg2 ? (seq.height + 15) / 16 : 2 * ((seq.height + 31) / 32);
        if (w == mbW && h == mbH)
            return;
        mbW = w;
        mbH = h;
        seq.codedWidth = mbW * 16;
        seq.codedHeight = mbH * 16;
        ls = seq.codedWidth;
        cs = seq.codedWidth / 2;
        size_t ySize = (size_t)ls * seq.codedHeight;
        size_t cSize = (size_t)cs * (seq.codedHeight / 2);
        for (Frame &f : frames) {
            f.mem.assign(ySize + 2 * cSize, 0);
            f.y = f.mem.data();
            f.cb = f.y + ySize;
            f.cr = f.cb + cSize;
            std::memset(f.cb, 128, 2 * cSize);
        }
        mbDone.assign((size_t)mbW * mbH, 0);
        outQueue.clear();
        fwd = bwd = -1;
        bwdOutput = true;
    }

    /* -------------------------------------------------------------------- */
    /* Pictures                                                             */
    /* -------------------------------------------------------------------- */

    /* The newer reference, if not output yet, goes out (sequence end, end
     * of stream). */
    void flushReference()
    {
        if (bwd >= 0 && !bwdOutput) {
            outQueue.push_back(bwd);
            bwdOutput = true;
        }
    }

    void decodePicture(size_t sc, size_t stop)
    {
        BitReader br(buf.data() + sc + 4, stop - (sc + 4));
        temporalRef = (int)br.get(10);
        pct = (int)br.get(3);
        br.skip(16);                                /* vbv_delay */
        if (pct == PCT_P || pct == PCT_B) {
            br.skip(1);                             /* full_pel_forward_vector (MPEG-1) */
            fcode[0][0] = fcode[0][1] = (int)br.get(3);
        }
        if (pct == PCT_B) {
            br.skip(1);
            fcode[1][0] = fcode[1][1] = (int)br.get(3);
        }
        while (br.getBit())                         /* extra_bit_picture */
            br.skip(8);
        int64_t pts = takePts(sc);
        havePce = false;
        if (pct >= PCT_I && pct <= PCT_D)
            stats.pictures[pct & 3]++;

        bool started = false;
        bool skip = false;
        while (br.nextStartCode()) {
            if (br.bitsLeft() < 32)
                break;
            uint32_t code = br.get32() & 0xFF;
            if (code == 0xB5) {
                extension(br);
            } else if (code >= 0x01 && code <= 0xAF) {
                if (!started) {
                    started = true;
                    skip = !beginPicture(pts);
                }
                if (skip)
                    break;
                decodeSlice(br, (int)code);
            } else if (code != 0xB2) {
                break;
            }
        }
        if (started && !skip)
            endPicture();
    }

    /* Picks the store and references; false if the picture is skipped. */
    bool beginPicture(int64_t pts)
    {
        if (!seq.valid || mbW == 0) {
            error("picture without a sequence header");
            stats.skippedPictures++;
            return false;
        }
        if (!seq.mpeg2 || !havePce) {
            snprintf(stats.lastError, sizeof(stats.lastError), "MPEG-1 video is not supported");
            stats.unsupported++;
            stats.skippedPictures++;
            return false;
        }
        if (picStruct != PS_FRAME || seq.chromaFormat != 1) {
            snprintf(stats.lastError, sizeof(stats.lastError), "field pictures / non-4:2:0 are not supported");
            stats.unsupported++;
            stats.skippedPictures++;
            return false;
        }
        if (pct == PCT_I || pct == PCT_P) {
            if (pct == PCT_P && bwd < 0) {
                stats.skippedPictures++;
                return false;
            }
            /* mpeg_video_rotate_frames: the older reference's store takes
             * the new picture, the newer becomes the older; the newer one
             * goes out now (display order). */
            int store = fwd;
            if (store < 0) {
                for (int i = 0; i < 3; i++)
                    if (i != bwd) {
                        store = i;
                        break;
                    }
            }
            if (bwd >= 0 && !bwdOutput)
                outQueue.push_back(bwd);
            fwd = bwd;
            bwd = store;
            bwdOutput = false;
            cur = store;
        } else if (pct == PCT_B) {
            if (bwd < 0) {
                stats.skippedPictures++;
                return false;
            }
            int store = 0;
            while (store == fwd || store == bwd)
                store++;
            cur = store;
        } else {
            snprintf(stats.lastError, sizeof(stats.lastError), "D pictures are not supported");
            stats.unsupported++;
            stats.skippedPictures++;
            return false;
        }
        Frame &f = frames[cur];
        f.type = pct;
        f.temporalRef = temporalRef;
        f.pts = pts;
        f.tff = tff != 0;
        f.rff = rff != 0;
        f.progressive = progFrame != 0;
        f.concealed = 0;
        f.decodeIndex = decodeCount++;
        f.sequenceEnds = stats.sequenceEnds;
        std::memset(mbDone.data(), 0, mbDone.size());

        qs = -1;
        intraParams.tabA = t.dctA[ivf];
        intraParams.tabB = t.dctB[ivf];
        intraParams.scan = t.scan[altScan];
        intraParams.wq = wqIntra;
        intraParams.mpeg1 = false;
        nonIntraParams.tabA = t.dctA[0];
        nonIntraParams.tabB = t.dctB[0];
        nonIntraParams.scan = t.scan[altScan];
        nonIntraParams.wq = wqNonIntra;
        nonIntraParams.mpeg1 = false;
        return true;
    }

    void endPicture()
    {
        Frame &f = frames[cur];
        /* Conceal what was not decoded: copy from a reference (or grey). */
        const Frame *ref = nullptr;
        if (pct == PCT_B)
            ref = &frames[bwd];
        else if (fwd >= 0)
            ref = &frames[fwd];
        for (int my = 0; my < mbH; my++) {
            for (int mx = 0; mx < mbW; mx++) {
                if (mbDone[(size_t)my * mbW + mx])
                    continue;
                f.concealed++;
                for (int y = 0; y < 16; y++) {
                    uint8_t *d = f.y + (size_t)(my * 16 + y) * ls + mx * 16;
                    if (ref)
                        std::memcpy(d, ref->y + (d - f.y), 16);
                    else
                        std::memset(d, 128, 16);
                }
                for (int y = 0; y < 8; y++) {
                    size_t o = (size_t)(my * 8 + y) * cs + mx * 8;
                    if (ref) {
                        std::memcpy(f.cb + o, ref->cb + o, 8);
                        std::memcpy(f.cr + o, ref->cr + o, 8);
                    } else {
                        std::memset(f.cb + o, 128, 8);
                        std::memset(f.cr + o, 128, 8);
                    }
                }
            }
        }
        stats.concealedMbs += f.concealed;
        if (pct == PCT_B)
            outQueue.push_back(cur);
    }

    /* -------------------------------------------------------------------- */
    /* Slices and macroblocks                                               */
    /* -------------------------------------------------------------------- */

    void setQscale(int code)
    {
        int q = qst ? t.nonLinearQ[code] : code * 2;
        if (q == qs)
            return;
        qs = q;
        for (int i = 0; i < 64; i++) {
            wqIntra[i] = (uint16_t)(intraW[i] * q);
            wqNonIntra[i] = (uint16_t)(nonIntraW[i] * q);
        }
    }

    void resetDc()
    {
        dcPred[0] = dcPred[1] = dcPred[2] = 1 << (7 + dcPrec);
    }

    void resetPmv()
    {
        std::memset(pmv, 0, sizeof(pmv));
    }

    /* macroblock_address_increment (with escapes); -1 on an invalid code. */
    int readMbaInc(BitReader &br)
    {
        int inc = 0;
        for (;;) {
            const Vlc &e = t.mbaInc[br.peek(11)];
            if (e.len == 0)
                return -1;
            br.skip(e.len);
            if (e.value == MBA_ESCAPE) {
                inc += 33;
                continue;
            }
            if (e.value == MBA_STUFFING)
                continue;
            return inc + e.value;
        }
    }

    bool readMotionCode(BitReader &br, int &code)
    {
        const Vlc &e = t.motionCode[br.peek(10)];
        if (e.len == 0)
            return false;
        br.skip(e.len);
        code = e.value;
        if (code != 0 && br.getBit())
            code = -code;
        return true;
    }

    static int updateMv(int pred, int rsize, int code, int residual)
    {
        if (code == 0)
            return pred;
        int delta = ((code < 0 ? -code : code) - 1) * (1 << rsize) + residual + 1;
        if (code < 0)
            delta = -delta;
        int v = pred + delta;
        int low = -(16 << rsize), high = (16 << rsize) - 1, range = 32 << rsize;
        if (v < low)
            v += range;
        else if (v > high)
            v -= range;
        return v;
    }

    /* motion_vector(r, s): both components into p[2]; fieldInFrame halves
     * the vertical predictor around the update. */
    bool motionVector(BitReader &br, int *p, int s, bool fieldInFrame, bool dmv)
    {
        int rh = fcode[s][0] - 1, rv = fcode[s][1] - 1;
        if (rh < 0 || rh > 8 || rv < 0 || rv > 8)
            return false;
        int code;
        if (!readMotionCode(br, code))
            return false;
        int res = (rh != 0 && code != 0) ? (int)br.get(rh) : 0;
        p[0] = updateMv(p[0], rh, code, res);
        if (dmv)
            br.skip(br.peek(1) ? 2 : 1);            /* dmvector[0] */
        if (!readMotionCode(br, code))
            return false;
        res = (rv != 0 && code != 0) ? (int)br.get(rv) : 0;
        if (fieldInFrame)
            p[1] = updateMv(p[1] >> 1, rv, code, res) * 2;
        else
            p[1] = updateMv(p[1], rv, code, res);
        if (dmv)
            br.skip(br.peek(1) ? 2 : 1);            /* dmvector[1] */
        return true;
    }

    /* motion_vectors(s) of a frame picture for the motion type. */
    bool motionVectors(BitReader &br, int s, int motionType)
    {
        if (motionType == MT_FRAME) {
            if (!motionVector(br, pmv[0][s], s, false, false))
                return false;
            pmv[1][s][0] = pmv[0][s][0];
            pmv[1][s][1] = pmv[0][s][1];
        } else if (motionType == MT_FIELD) {
            mvfs[0][s] = br.getBit();
            if (!motionVector(br, pmv[0][s], s, true, false))
                return false;
            mvfs[1][s] = br.getBit();
            if (!motionVector(br, pmv[1][s], s, true, false))
                return false;
        } else {                                    /* dual prime: parsed, not predicted */
            if (!motionVector(br, pmv[0][s], s, true, true))
                return false;
            pmv[1][s][0] = pmv[0][s][0];
            pmv[1][s][1] = pmv[0][s][1];
        }
        return true;
    }

    /* The source of a w x h prediction at (x, y) of the plane (w / h
     * include the extra half-pel column / row); out-of-range areas are
     * copied with clamped coordinates. */
    const uint8_t *fetch(const Plane &p, int x, int y, int w, int h, int &stride)
    {
        if (x >= 0 && y >= 0 && x + w <= p.w && y + h <= p.h) {
            stride = p.stride;
            return p.base + (size_t)y * p.stride + x;
        }
        stats.mvOutside++;
        for (int j = 0; j < h; j++) {
            const uint8_t *row = p.base + (size_t)clampi(y + j, 0, p.h - 1) * p.stride;
            for (int i = 0; i < w; i++)
                fetchTmp[j * 32 + i] = row[clampi(x + i, 0, p.w - 1)];
        }
        stride = 32;
        return fetchTmp;
    }

    /* Frame prediction of macroblock (mx, my) from ref with vector
     * (vx, vy) in half pels. */
    void predictFrame(const Frame &ref, int mx, int my, int vx, int vy, bool avg)
    {
        Frame &f = frames[cur];
        int stride;
        Plane py = {ref.y, ls, seq.codedWidth, seq.codedHeight};
        int half = (vx & 1) | ((vy & 1) << 1);
        const uint8_t *s = fetch(py, mx * 16 + (vx >> 1), my * 16 + (vy >> 1), 16 + (half & 1), 16 + (half >> 1), stride);
        mc<16>(f.y + (size_t)my * 16 * ls + mx * 16, ls, s, stride, 16, half, avg);

        int cx = vx / 2, cy = vy / 2;               /* truncation toward zero (7.6.3.7) */
        int chalf = (cx & 1) | ((cy & 1) << 1);
        int sx = mx * 8 + (cx >> 1), sy = my * 8 + (cy >> 1);
        int cw = 8 + (chalf & 1), ch = 8 + (chalf >> 1);
        size_t o = (size_t)my * 8 * cs + mx * 8;
        Plane pcb = {ref.cb, cs, seq.codedWidth / 2, seq.codedHeight / 2};
        s = fetch(pcb, sx, sy, cw, ch, stride);
        mc<8>(f.cb + o, cs, s, stride, 8, chalf, avg);
        Plane pcr = {ref.cr, cs, seq.codedWidth / 2, seq.codedHeight / 2};
        s = fetch(pcr, sx, sy, cw, ch, stride);
        mc<8>(f.cr + o, cs, s, stride, 8, chalf, avg);
    }

    /* Field prediction in a frame picture: the lines of field `dstField'
     * of the macroblock from field `srcField' of ref, vector (vx, vy) with
     * vy in field half-lines. */
    void predictField(const Frame &ref, int mx, int my, int dstField, int srcField, int vx, int vy, bool avg)
    {
        Frame &f = frames[cur];
        int stride;
        Plane py = {ref.y + (size_t)srcField * ls, ls * 2, seq.codedWidth, seq.codedHeight / 2};
        int half = (vx & 1) | ((vy & 1) << 1);
        const uint8_t *s = fetch(py, mx * 16 + (vx >> 1), my * 8 + (vy >> 1), 16 + (half & 1), 8 + (half >> 1), stride);
        mc<16>(f.y + (size_t)(my * 16 + dstField) * ls + mx * 16, ls * 2, s, stride, 8, half, avg);

        int cx = vx / 2, cy = vy / 2;
        int chalf = (cx & 1) | ((cy & 1) << 1);
        int sx = mx * 8 + (cx >> 1), sy = my * 4 + (cy >> 1);
        int cw = 8 + (chalf & 1), ch = 4 + (chalf >> 1);
        size_t o = (size_t)(my * 8 + dstField) * cs + mx * 8;
        Plane pcb = {ref.cb + (size_t)srcField * cs, cs * 2, seq.codedWidth / 2, seq.codedHeight / 4};
        s = fetch(pcb, sx, sy, cw, ch, stride);
        mc<8>(f.cb + o, cs * 2, s, stride, 4, chalf, avg);
        Plane pcr = {ref.cr + (size_t)srcField * cs, cs * 2, seq.codedWidth / 2, seq.codedHeight / 4};
        s = fetch(pcr, sx, sy, cw, ch, stride);
        mc<8>(f.cr + o, cs * 2, s, stride, 4, chalf, avg);
    }

    void predictDirection(const Frame &ref, int s, int motionType, int mx, int my, bool avg)
    {
        if (motionType == MT_FIELD) {
            predictField(ref, mx, my, 0, mvfs[0][s], pmv[0][s][0], pmv[0][s][1] >> 1, avg);
            predictField(ref, mx, my, 1, mvfs[1][s], pmv[1][s][0], pmv[1][s][1] >> 1, avg);
        } else {
            predictFrame(ref, mx, my, pmv[0][s][0], pmv[0][s][1], avg);
        }
    }

    /* Prediction of a non-intra macroblock (mpeg_video_motion_compensate,
     * 0x00261D08): forward from the older reference (P pictures: always),
     * backward from the newer, averaged when both. */
    void predict(int mbt, int motionType, int mx, int my)
    {
        const Frame *ref0 = pct == PCT_B ? (fwd >= 0 ? &frames[fwd] : &frames[bwd]) : &frames[fwd];
        bool any = false;
        if ((mbt & MBT_FWD) || pct == PCT_P) {
            predictDirection(*ref0, 0, motionType, mx, my, false);
            any = true;
        }
        if (mbt & MBT_BWD) {
            predictDirection(frames[bwd], 1, motionType, mx, my, any);
            any = true;
        }
        if (!any)                                   /* (cannot happen in valid streams) */
            predictFrame(*ref0, mx, my, 0, 0, false);
    }

    /* Destination of block b of macroblock (mx, my). */
    uint8_t *blockDest(int b, int mx, int my, int dctType, int &stride)
    {
        Frame &f = frames[cur];
        if (b < 4) {
            uint8_t *base = f.y + (size_t)my * 16 * ls + mx * 16 + (b & 1) * 8;
            if (dctType) {
                stride = ls * 2;
                return base + (b >> 1) * ls;
            }
            stride = ls;
            return base + (size_t)(b >> 1) * 8 * ls;
        }
        stride = cs;
        return (b == 4 ? f.cb : f.cr) + (size_t)my * 8 * cs + mx * 8;
    }

    bool intraBlocks(BitReader &br, int mx, int my, int dctType)
    {
        for (int b = 0; b < 6; b++) {
            int cc = b < 4 ? 0 : b - 3;
            bool ok = true;
            int diff = read_dc_diff(br, t, cc, ok);
            if (!ok)
                return false;
            dcPred[cc] += diff;
            blk[0] = (int16_t)(dcPred[cc] << (3 - dcPrec));
            unsigned rowMask;
            int r = decode_intra_ac(br, intraParams, blk, rowMask);
            if (r == BLOCK_ERROR || br.overrun()) {
                clear_block(blk, 0xFF);
                return false;
            }
            int stride;
            uint8_t *d = blockDest(b, mx, my, dctType, stride);
            if (r == BLOCK_DC_ONLY) {
                idct_put_dc(blk[0], d, stride);
                stats.blocksDcOnly++;
            } else {
                idct_put(blk, rowMask, d, stride);
            }
            clear_block(blk, rowMask);
            stats.blocks++;
        }
        return true;
    }

    bool interBlocks(BitReader &br, int mx, int my, int dctType, int cbp)
    {
        for (int b = 0; b < 6; b++) {
            if (!(cbp & (32 >> b)))
                continue;
            unsigned rowMask;
            int r = decode_non_intra(br, nonIntraParams, blk, rowMask);
            if (r == BLOCK_ERROR || br.overrun()) {
                clear_block(blk, 0xFF);
                return false;
            }
            int stride;
            uint8_t *d = blockDest(b, mx, my, dctType, stride);
            if (r == BLOCK_DC_ONLY) {
                idct_add_dc(blk[0], d, stride);
                stats.blocksDcOnly++;
            } else {
                idct_add(blk, rowMask, d, stride);
            }
            clear_block(blk, rowMask);
            stats.blocks++;
        }
        return true;
    }

    /* One coded macroblock at (mx, my). */
    bool macroblock(BitReader &br, int mx, int my)
    {
        const Vlc &e = t.mbType[pct & 3][br.peek(6)];
        if (e.len == 0)
            return false;
        br.skip(e.len);
        int mbt = e.value;
        int motionType = MT_FRAME;
        int dctType = 0;
        if ((mbt & (MBT_FWD | MBT_BWD)) && !fpfd)
            motionType = (int)br.get(2);
        if (!fpfd && (mbt & (MBT_INTRA | MBT_PATTERN)))
            dctType = br.getBit();
        if (mbt & MBT_QUANT) {
            setQscale((int)br.get(5));
            stats.mbQuant++;
        }
        if ((mbt & MBT_FWD) || ((mbt & MBT_INTRA) && cmv)) {
            if (!motionVectors(br, 0, (mbt & MBT_INTRA) ? MT_FRAME : motionType))
                return false;
        }
        if (mbt & MBT_BWD) {
            if (!motionVectors(br, 1, motionType))
                return false;
        }
        if ((mbt & MBT_INTRA) && cmv)
            br.skip(1);                             /* marker_bit */
        if (dctType)
            stats.mbFieldDct++;

        if (mbt & MBT_INTRA) {
            stats.mbIntra++;
            if (!cmv)
                resetPmv();
            if (!intraBlocks(br, mx, my, dctType))
                return false;
        } else {
            stats.mbInter++;
            resetDc();
            if (pct == PCT_P && !(mbt & MBT_FWD)) {
                /* P macroblock without a vector: zero vector, frame */
                pmv[0][0][0] = pmv[0][0][1] = pmv[1][0][0] = pmv[1][0][1] = 0;
                motionType = MT_FRAME;
            }
            if (motionType == MT_DUAL_PRIME) {
                stats.mbDualPrime++;
                stats.unsupported++;
                snprintf(stats.lastError, sizeof(stats.lastError), "dual-prime prediction is not supported");
                return false;
            }
            if (motionType == MT_FIELD)
                stats.mbFieldMotion++;
            else
                stats.mbFrameMotion++;
            int cbp = 0;
            if (mbt & MBT_PATTERN) {
                const Vlc &c = t.cbp[br.peek(9)];
                if (c.len == 0)
                    return false;
                br.skip(c.len);
                cbp = c.value;
            }
            predict(mbt, motionType, mx, my);
            if (!interBlocks(br, mx, my, dctType, cbp))
                return false;
        }
        prevMbType = mbt;
        prevMotionType = motionType;
        return true;
    }

    /* A skipped macroblock: P - zero vector from the reference; B - the
     * previous macroblock's directions and vectors, frame prediction. */
    void skipped(int mx, int my)
    {
        stats.mbSkipped++;
        resetDc();
        if (pct == PCT_P) {
            resetPmv();
            predict(MBT_FWD, MT_FRAME, mx, my);
        } else {
            predict(prevMbType & (MBT_FWD | MBT_BWD), MT_FRAME, mx, my);
        }
    }

    void decodeSlice(BitReader &br, int code)
    {
        stats.slices++;
        int row = code - 1;
        if (seq.height > 2800)
            row += (int)br.get(3) << 7;             /* slice_vertical_position_extension */
        setQscale((int)br.get(5));
        if (br.peek(1)) {                           /* intra_slice_flag, intra_slice, reserved */
            br.skip(9);
            while (br.getBit())
                br.skip(8);
        } else {
            br.skip(1);
        }
        resetDc();
        resetPmv();
        prevMbType = MBT_FWD;
        prevMotionType = MT_FRAME;

        int inc = readMbaInc(br);
        if (inc < 0) {
            error("bad first macroblock address");
            return;
        }
        int addr = row * mbW + inc - 1;
        const int total = mbW * mbH;
        for (;;) {
            if (addr < 0 || addr >= total) {
                error("macroblock address out of range");
                return;
            }
            int mx = addr % mbW, my = addr / mbW;
            if (!macroblock(br, mx, my) || br.overrun()) {
                error("macroblock error");
                return;
            }
            mbDone[addr] = 1;
            if (br.peek(23) == 0) {                 /* next start code: end of the slice */
                /* checks: the slice data was consumed exactly (fewer
                 * than 8 zero bits to the start code) and ends a row */
                BitReader probe = br;
                if (probe.nextStartCode() && probe.bitPos() - br.bitPos() >= 8)
                    stats.sliceLongTails++;
                if (mx != mbW - 1)
                    stats.sliceEndsMidRow++;
                return;
            }
            inc = readMbaInc(br);
            if (inc < 0) {
                error("bad macroblock address increment");
                return;
            }
            for (int k = 1; k < inc; k++) {
                addr++;
                if (addr >= total) {
                    error("skipped macroblocks past the picture");
                    return;
                }
                skipped(addr % mbW, addr / mbW);
                mbDone[addr] = 1;
            }
            addr++;
        }
    }

    void fill(VideoPicture &out, int idx)
    {
        const Frame &f = frames[idx];
        out.y = f.y;
        out.cb = f.cb;
        out.cr = f.cr;
        out.yStride = ls;
        out.cStride = cs;
        out.width = seq.width;
        out.height = seq.height;
        out.codedWidth = seq.codedWidth;
        out.codedHeight = seq.codedHeight;
        out.type = f.type;
        out.temporalReference = f.temporalRef;
        out.pts = f.pts;
        out.topFieldFirst = f.tff;
        out.repeatFirstField = f.rff;
        out.progressiveFrame = f.progressive;
        out.concealedMbs = f.concealed;
        out.decodeIndex = f.decodeIndex;
        out.sequenceEnds = f.sequenceEnds;
        stats.output++;
    }
};

Mpeg2VideoDecoder::Mpeg2VideoDecoder() : impl_(new Impl) {}
Mpeg2VideoDecoder::~Mpeg2VideoDecoder() = default;

void Mpeg2VideoDecoder::reset()
{
    VideoStats keep = impl_->stats;
    impl_.reset(new Impl);
    impl_->stats = keep;
}

void Mpeg2VideoDecoder::push(const uint8_t *data, size_t size, int64_t pts)
{
    impl_->push(data, size, pts);
}

void Mpeg2VideoDecoder::setEndOfStream()
{
    impl_->eos = true;
}

bool Mpeg2VideoDecoder::next(VideoPicture &out)
{
    Impl &m = *impl_;
    for (;;) {
        if (!m.outQueue.empty()) {
            int idx = m.outQueue.front();
            m.outQueue.pop_front();
            m.fill(out, idx);
            return true;
        }
        if (!m.step())
            return false;
    }
}

bool Mpeg2VideoDecoder::finished() const
{
    return impl_->eos && impl_->eosFlushed && impl_->outQueue.empty();
}

bool Mpeg2VideoDecoder::readHeaders()
{
    Impl &m = *impl_;
    for (;;) {
        size_t sc = m.findStartCode(m.rd);
        if (sc == NPOS)
            return false;
        if (m.buf[sc + 3] == 0x00)
            return m.seq.valid;
        if (!m.step())
            return false;
    }
}

const SequenceInfo &Mpeg2VideoDecoder::sequence() const
{
    return impl_->seq;
}

const VideoStats &Mpeg2VideoDecoder::stats() const
{
    return impl_->stats;
}

void Mpeg2VideoDecoder::clearStats()
{
    impl_->stats = VideoStats();
}

size_t Mpeg2VideoDecoder::buffered() const
{
    return impl_->end - impl_->rd;
}

} /* namespace gt3media */
