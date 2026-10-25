/* vif_model.cpp - the native decoder of GT3's prebuilt VU1 models (see
 * vif_model.h; the microcode and the math of each program:
 * docs/models.md).
 *
 * Two steps per part:
 *   1. the VIF codes of the part are interpreted into an image of the VU1
 *      data buffer the part lands in (VIF1 UNPACK semantics: the vector
 *      formats S / V2 / V3 / V4 x 32 / 16 / 8 and V4-5, USN, the write
 *      cycle STCYCL with skipping and filling writes, the STMASK write mask
 *      with the STROW / STCOL registers, the STMOD offset / difference
 *      modes);
 *   2. the image is read as the model program reads it: the GIF tag at
 *      0xC0, the strip table in the w of 0x40.., the vertex streams at 0x00
 *      / 0x40 / 0x80 (and the key-frame copies at 0x20 / 0x60 / 0xA0) as
 *      the vertex format converters (VU1 0x009 / 0x015 / 0x02B) and the
 *      lighting mode take them; the strips become triangles. */
#include "engine/pgl/native/vif_model.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace gt3vif {

namespace {

/* The VU1 data memory is 0x400 quadwords; a double buffer of the model
 * programs is 0x1B3 (gfx_begin_packet: BASE 0, OFFSET 0x1B3).  The image
 * covers the whole memory: an UNPACK address wraps at 0x400. */
const uint32_t VU_QWORDS = 0x400;

/* Addresses the model programs read (TOPS relative). */
const uint32_t VU_XYZ      = 0x00;
const uint32_t VU_XYZ2     = 0x20;
const uint32_t VU_STRIPS   = 0x40;     /* also S T / mode 2-3 normals */
const uint32_t VU_ST2      = 0x60;
const uint32_t VU_RGBA     = 0x80;     /* also mode 1-4 normals */
const uint32_t VU_RGBA2    = 0xA0;
const uint32_t VU_GIFTAG   = 0xC0;

struct VuImage {
    uint32_t w[VU_QWORDS][4];
    uint8_t  written[VU_QWORDS];        /* lanes written (bit per lane) */

    void reset() { memset(this, 0, sizeof(*this)); }
    bool laneWritten(uint32_t q, int lane) const { return (written[q & (VU_QWORDS - 1)] >> lane) & 1; }
    bool qwordWritten(uint32_t q) const { return written[q & (VU_QWORDS - 1)] != 0; }
    float f(uint32_t q, int lane) const
    {
        float v;
        memcpy(&v, &w[q & (VU_QWORDS - 1)][lane], 4);
        return v;
    }
    uint32_t u(uint32_t q, int lane) const { return w[q & (VU_QWORDS - 1)][lane]; }
};

/* The VIF1 registers an UNPACK uses. */
struct VifRegs {
    uint32_t cl, wl;        /* STCYCL */
    uint32_t mode;          /* STMOD */
    uint32_t mask;          /* STMASK */
    uint32_t row[4];        /* STROW */
    uint32_t col[4];        /* STCOL */
};

inline uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* One element of an UNPACK's data (sign- or zero-extended); out of the
 * available bytes: 0. */
uint32_t readElem(const uint8_t *data, size_t avail, uint32_t index, uint32_t bits, bool usn)
{
    size_t byteOfs = (size_t)index * bits / 8;
    if (byteOfs + bits / 8 > avail)
        return 0;
    const uint8_t *p = data + byteOfs;
    switch (bits) {
    case 32:
        return rd32(p);
    case 16: {
        uint16_t v = (uint16_t)(p[0] | p[1] << 8);
        return usn ? (uint32_t)v : (uint32_t)(int32_t)(int16_t)v;
    }
    default: {
        uint8_t v = p[0];
        return usn ? (uint32_t)v : (uint32_t)(int32_t)(int8_t)v;
    }
    }
}

/* The four lanes of vector `vec' of an UNPACK (vn 0..3 = S, V2, V3, V4;
 * vl 0..3 = 32, 16, 8, 5 bits).  As the VIF writes them: S fills the four
 * lanes, V2 writes x y x y, V3 takes w from the next element (the hardware
 * runs V3 through the V4 path), V4-5 expands 5:5:5:1. */
void readVector(const uint8_t *data, size_t avail, uint32_t vec, int vn, int vl, bool usn,
                uint32_t out[4])
{
    if (vl == 3) {                                      /* V4-5 */
        uint32_t v = readElem(data, avail, vec, 16, true);
        out[0] = (v & 0x001F) << 3;
        out[1] = (v & 0x03E0) >> 2;
        out[2] = (v & 0x7C00) >> 7;
        out[3] = (v & 0x8000) >> 8;
        return;
    }
    uint32_t bits = 32 >> vl;
    uint32_t n = (uint32_t)vn + 1;
    uint32_t e = vec * n;
    switch (vn) {
    case 0:
        out[0] = out[1] = out[2] = out[3] = readElem(data, avail, e, bits, usn);
        break;
    case 1:
        out[0] = out[2] = readElem(data, avail, e, bits, usn);
        out[1] = out[3] = readElem(data, avail, e + 1, bits, usn);
        break;
    case 2:
        out[0] = readElem(data, avail, e, bits, usn);
        out[1] = readElem(data, avail, e + 1, bits, usn);
        out[2] = readElem(data, avail, e + 2, bits, usn);
        out[3] = readElem(data, avail, e + 3, bits, usn);   /* the next element */
        break;
    default:
        for (int i = 0; i < 4; i++)
            out[i] = readElem(data, avail, e + i, bits, usn);
        break;
    }
}

/* VIF UNPACK: cmd (0x60..0x7F), num, imm; data / avail the bytes after the
 * VIFcode (avail: up to the end of the part, the V3 w may read past the
 * UNPACK's own data).  Returns the UNPACK's data size in bytes. */
size_t doUnpack(VuImage &img, VifRegs &r, uint32_t cmd, uint32_t num, uint32_t imm,
                const uint8_t *data, size_t avail)
{
    int vn = (cmd >> 2) & 3;
    int vl = cmd & 3;
    bool masked = (cmd & 0x10) != 0;
    bool usn = (imm & 0x4000) != 0;
    uint32_t addr = imm & 0x3FF;                        /* FLG: relative to TOPS, as everything here */
    uint32_t n = num ? num : 256;
    uint32_t wl = r.wl ? r.wl : 256;
    uint32_t cl = r.cl;
    bool fill = cl < wl;

    /* vectors taken from the data */
    uint32_t nread;
    if (!fill)
        nread = n;
    else
        nread = cl * (n / wl) + std::min(n % wl, cl);
    uint32_t vecBits = (vl == 3) ? 16 : (32u >> vl) * (uint32_t)(vn + 1);
    size_t size = ((size_t)nread * vecBits + 31) / 32 * 4;

    uint32_t cyc = 0;
    uint32_t src = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v[4];
        readVector(data, avail, src, vn, vl, usn, v);
        uint32_t q = addr & (VU_QWORDS - 1);
        uint32_t maskRow = std::min(cyc, 3u);
        for (int lane = 0; lane < 4; lane++) {
            uint32_t m = masked ? (r.mask >> (maskRow * 8 + lane * 2)) & 3 : 0;
            switch (m) {
            case 0:
                if (vl == 3) {                          /* V4-5 ignores STMOD */
                    img.w[q][lane] = v[lane];
                } else {
                    switch (r.mode) {
                    case 1:                             /* offset */
                        img.w[q][lane] = v[lane] + r.row[lane];
                        break;
                    case 2:                             /* difference: the row accumulates */
                        r.row[lane] += v[lane];
                        img.w[q][lane] = r.row[lane];
                        break;
                    case 3:
                        r.row[lane] = v[lane];
                        img.w[q][lane] = v[lane];
                        break;
                    default:
                        img.w[q][lane] = v[lane];
                        break;
                    }
                }
                img.written[q] |= (uint8_t)(1 << lane);
                break;
            case 1:
                img.w[q][lane] = r.row[lane];
                img.written[q] |= (uint8_t)(1 << lane);
                break;
            case 2:
                img.w[q][lane] = r.col[maskRow];
                img.written[q] |= (uint8_t)(1 << lane);
                break;
            default:                                    /* write protected */
                break;
            }
        }
        addr++;
        cyc++;
        if (fill) {
            /* filling write: the first CL of every WL quadwords come from
             * the data, the others repeat the next vector (the mask makes
             * them row / column values) */
            if (cyc <= cl)
                src++;
            else if (cyc == wl)
                cyc = 0;
        } else {
            src++;
            if (cyc >= wl) {
                addr += cl - wl;
                cyc = 0;
            }
        }
    }
    return size;
}

void warn(Mesh &out, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (out.warnings.size() < 256)
        out.warnings.push_back(buf);
}

/* The streams a lighting mode reads, and what they hold. */
struct ModeLayout {
    bool stAt40;            /* S T at 0x40 (else normals) */
    bool colorAt80;         /* RGBA at 0x80 (else normals) */
};

ModeLayout layoutOf(LightMode lm)
{
    switch (lm) {
    case LMODE_LIGHTS:
    case LMODE_LOCAL:
        return { true, false };
    case LMODE_ENVMTX:
    case LMODE_ENVMAP:
        return { false, true };
    case LMODE_NONE:
    case LMODE_CONST:
    default:
        return { true, true };
    }
}

/* Reads the batch in img the way the model program does; appends to out. */
void emitBatch(const VuImage &img, uint8_t mode, uint16_t tex, uint16_t partIndex,
               uint32_t extraFlags, bool extPos, Mesh &out)
{
    Batch b = {};
    b.firstVertex = (uint32_t)out.vertices.size();
    b.firstIndex = (uint32_t)out.indices.size();
    b.firstStrip = (uint32_t)out.strips.size();
    b.tex = tex;
    b.part = partIndex;
    b.flags = extraFlags;

    if (!img.qwordWritten(VU_GIFTAG)) {
        b.flags |= BATCH_NO_GIFTAG;
        warn(out, "part %u: no GIF tag at 0xC0", partIndex);
        out.batches.push_back(b);
        return;
    }
    uint32_t t0 = img.u(VU_GIFTAG, 0), t1 = img.u(VU_GIFTAG, 1), t2 = img.u(VU_GIFTAG, 2);
    uint32_t nloop = t0 & 0x7FFF;
    bool pre = (t1 >> 14) & 1;
    uint32_t prim = (t1 >> 15) & 0x7FF;
    uint32_t flg = (t1 >> 26) & 3;
    uint32_t nreg = t1 >> 28;
    b.nloop = (uint16_t)nloop;
    b.prim = (uint16_t)prim;
    b.regs = t2;
    if (!pre)
        b.flags |= BATCH_NO_PRE;
    if (flg != 0 || nreg != 3 || (t2 & 0xFFF) != GIF_REGS_ST_RGBAQ_XYZF2) {
        b.flags |= BATCH_BAD_REGS;
        warn(out, "part %u: GIF tag FLG %u NREG %u REGS %03X (expected PACKED, 3, 412)",
             partIndex, flg, nreg, t2 & 0xFFF);
    }

    LightMode lm = modeLighting(mode);
    VertexFormat fmt = modeFormat(mode);
    ModeLayout lay = layoutOf(lm);
    bool morph = (fmt == FMT_MORPH_COLOR || fmt == FMT_MORPH_FLOAT);
    uint32_t maxVerts = morph ? 32 : 64;
    if (nloop > maxVerts) {
        b.flags |= BATCH_TOO_MANY;
        warn(out, "part %u: NLOOP %u > %u", partIndex, nloop, maxVerts);
    }
    /* the pairs the microcode gets wrong: format 1 lerps 0x80 as integers
     * (normals of modes 1 / 4 are floats), format 2 as floats (colours
     * are integers), format 3 converts 0x40 xy only (normals of modes 2 /
     * 3 need z) and has no converter in front of VU1L_LOCAL_S12 /
     * VU1L_LOCAL (their "-6" entries are the previous variant's JR) */
    if ((fmt == FMT_MORPH_COLOR && !lay.colorAt80) || (fmt == FMT_MORPH_FLOAT && lay.colorAt80) ||
        (fmt == FMT_FIXED16 && (!lay.stAt40 || lm == LMODE_LOCAL)) || lm > LMODE_CONST) {
        b.flags |= BATCH_BROKEN_FORMAT;
        warn(out, "part %u: mode 0x%02X (lighting %u, format %u) is not a working pair in GT3's microcode",
             partIndex, mode, lm, fmt);
    }

    /* the strip table: count in the w of 0x40, then 3 * (len - 2) (ILW:
     * the low 16 bits of the field) */
    uint32_t nStrips = img.u(VU_STRIPS, 3) & 0xFFFF;
    uint32_t total = 0;
    for (uint32_t k = 0; k < nStrips; k++) {
        uint32_t v = img.u(VU_STRIPS + 1 + k, 3) & 0xFFFF;
        if (v % 3 != 0)
            b.flags |= BATCH_BAD_STRIP;
        uint32_t len = v / 3 + 2;
        out.strips.push_back((uint16_t)len);
        total += len;
    }
    b.stripCount = nStrips;
    if (total != nloop) {
        b.flags |= BATCH_STRIP_MISMATCH;
        warn(out, "part %u: strips add up to %u vertices, NLOOP %u", partIndex, total, nloop);
    }

    /* the vertices */
    uint32_t nv = std::min<uint32_t>(nloop, 64);
    bool unwritten = false;
    for (uint32_t i = 0; i < nv; i++) {
        Vertex v = {};
        /* xyz */
        if (!img.qwordWritten(VU_XYZ + i))
            unwritten = true;
        for (int c = 0; c < 3; c++) {
            if (fmt == FMT_FIXED16)
                v.pos[c] = (float)(int32_t)img.u(VU_XYZ + i, c);      /* ITOF0 */
            else
                v.pos[c] = img.f(VU_XYZ + i, c);
            v.pos2[c] = morph ? img.f(VU_XYZ2 + i, c) : v.pos[c];
        }
        /* 0x40: S T or the normal */
        if (lay.stAt40) {
            for (int c = 0; c < 2; c++) {
                if (fmt == FMT_FIXED16)
                    v.st[c] = (float)(int32_t)img.u(VU_STRIPS + i, c) * (1.0f / 4096.0f);  /* ITOF12 */
                else
                    v.st[c] = img.f(VU_STRIPS + i, c);
                v.st2[c] = morph ? img.f(VU_ST2 + i, c) : v.st[c];
            }
        } else {
            for (int c = 0; c < 3; c++) {
                v.nrm[c] = img.f(VU_STRIPS + i, c);
                v.nrm2[c] = morph ? img.f(VU_ST2 + i, c) : v.nrm[c];
            }
        }
        /* (S T only matter with TME: untextured packets carry none) */
        if (!img.laneWritten(VU_STRIPS + i, 0) && (!lay.stAt40 || (prim & PRIM_TME)))
            unwritten = true;
        /* 0x80: RGBA or the normal */
        if (lay.colorAt80) {
            for (int c = 0; c < 4; c++) {
                v.rgba[c] = (uint8_t)img.u(VU_RGBA + i, c);
                v.rgba2[c] = morph ? (uint8_t)img.u(VU_RGBA2 + i, c) : v.rgba[c];
            }
        } else {
            for (int c = 0; c < 3; c++) {
                v.nrm[c] = img.f(VU_RGBA + i, c);
                v.nrm2[c] = morph ? img.f(VU_RGBA2 + i, c) : v.nrm[c];
            }
        }
        if (!img.qwordWritten(VU_RGBA + i))
            unwritten = true;
        out.vertices.push_back(v);
    }
    if (unwritten) {
        b.flags |= BATCH_UNWRITTEN;
        warn(out, "part %u: a vertex stream the mode reads was not unpacked", partIndex);
    }
    b.vertexCount = nv;

    out.attribs |= ATTR_POS;
    if (extPos)
        out.attribs |= ATTR_EXT_POS;
    if (morph)
        out.attribs |= ATTR_MORPH;
    if (lay.stAt40)
        out.attribs |= ATTR_ST;
    else
        out.attribs |= ATTR_NORMAL;
    if (lay.colorAt80)
        out.attribs |= ATTR_COLOR;
    else
        out.attribs |= ATTR_NORMAL;

    /* The triangles.  The program sets ADC on the first two vertices of
     * each strip (L392 / L419 ...: +2048 to the fog lane, bit 15 of the
     * XYZF2 word after FTOI4); the GS draws a triangle at every vertex
     * without ADC from the last three vertices it got. */
    uint32_t type = prim & PRIM_TYPE_MASK;
    if (type != PRIM_TRISTRIP && type != PRIM_TRIANGLE && type != PRIM_TRIFAN) {
        b.flags |= BATCH_NOT_TRIANGLES;
        warn(out, "part %u: PRIM type %u is not a triangle type", partIndex, type);
    } else {
        std::vector<uint8_t> adc(nv, 0);
        std::vector<uint32_t> stripStart(nv, 0);
        uint32_t s = 0;
        for (uint32_t k = 0; k < nStrips && s < nv; k++) {
            uint32_t len = out.strips[b.firstStrip + k];
            for (uint32_t j = 0; j < len && s + j < nv; j++) {
                adc[s + j] = j < 2;
                stripStart[s + j] = s;
            }
            s += len;
        }
        for (uint32_t i = s; i < nv; i++) {             /* vertices past the strip table: never kicked */
            adc[i] = 1;
            stripStart[i] = i;
        }
        /* Each triangle starts with its kicking vertex (the rotation keeps
         * the winding): with IIP clear the GS takes the colour of that last
         * vertex, Direct3D 11's flat interpolation the first. */
        uint32_t base = b.firstVertex;
        for (uint32_t i = 0; i < nv; i++) {
            if (adc[i])
                continue;
            if (type == PRIM_TRISTRIP) {
                if (i < 2)
                    continue;
                uint32_t parity = (i - stripStart[i]) & 1;    /* triangle k = i - 2 - start */
                if (parity == 0) {                  /* (i-2, i-1, i) */
                    out.indices.push_back(base + i);
                    out.indices.push_back(base + i - 2);
                    out.indices.push_back(base + i - 1);
                } else {                            /* (i-1, i-2, i) */
                    out.indices.push_back(base + i);
                    out.indices.push_back(base + i - 1);
                    out.indices.push_back(base + i - 2);
                }
            } else if (type == PRIM_TRIANGLE) {
                /* the GS completes a triangle at every third vertex since
                 * PRIM was set */
                if (i % 3 != 2)
                    continue;
                out.indices.push_back(base + i);
                out.indices.push_back(base + i - 2);
                out.indices.push_back(base + i - 1);
            } else {
                /* fan: the centre is the first vertex since PRIM (ADC does
                 * not reset it); (centre, i-1, i) */
                if (i < 2)
                    continue;
                out.indices.push_back(base + i);
                out.indices.push_back(base);
                out.indices.push_back(base + i - 1);
            }
        }
    }
    b.indexCount = (uint32_t)out.indices.size() - b.firstIndex;
    out.batches.push_back(b);
}

} // namespace

void Mesh::clear()
{
    mode = 0;
    scaleFlags = 0;
    nVertsHeader = 0;
    nPrimsHeader = 0;
    attribs = 0;
    vertices.clear();
    indices.clear();
    strips.clear();
    batches.clear();
    warnings.clear();
}

bool decodePart(const void *dataV, uint32_t qwc, uint16_t tex, uint16_t partIndex,
                uint8_t mode, const DecodeOptions &opt, Mesh &out)
{
    const uint8_t *data = (const uint8_t *)dataV;
    if (data == nullptr || qwc == 0) {
        warn(out, "part %u: no data", partIndex);
        return false;
    }
    static thread_local VuImage img;
    img.reset();
    VifRegs r = {};
    r.cl = opt.stcyclCl;
    r.wl = opt.stcyclWl;
    r.mode = opt.stmod;
    r.mask = opt.stmask;

    bool extPos = false;
    if (opt.preXyz != nullptr && opt.preXyzCount > 0) {
        /* vu1_unpack_xyz: UNPACK V3-32 (n rounded up to 4) to 0, unmasked */
        int n = std::min(opt.preXyzCount, 64);
        for (int i = 0; i < n; i++) {
            for (int c = 0; c < 3; c++)
                memcpy(&img.w[i][c], &opt.preXyz[i * 3 + c], 4);
            img.written[i] = 0x0F;
        }
        extPos = true;
    }

    size_t size = (size_t)qwc * 16;
    size_t p = 0;
    bool ok = true;
    uint32_t extra = 0;
    int batches = 0;
    while (p + 4 <= size) {
        uint32_t code = rd32(data + p);
        p += 4;
        uint32_t cmd = (code >> 24) & 0x7F;
        uint32_t num = (code >> 16) & 0xFF;
        uint32_t imm = code & 0xFFFF;
        if (cmd >= 0x60) {
            size_t used = doUnpack(img, r, cmd, num, imm, data + p, size - p);
            if (p + used > size) {
                warn(out, "part %u: UNPACK %02X past the end of the packet", partIndex, cmd);
                ok = false;
                break;
            }
            p += used;
            continue;
        }
        switch (cmd) {
        case 0x00:                                      /* NOP */
        case 0x02:                                      /* OFFSET */
        case 0x03:                                      /* BASE */
        case 0x04:                                      /* ITOP */
        case 0x06:                                      /* MSKPATH3 */
        case 0x07:                                      /* MARK */
        case 0x10:                                      /* FLUSHE */
        case 0x11:                                      /* FLUSH */
        case 0x13:                                      /* FLUSHA */
            break;
        case 0x01:                                      /* STCYCL */
            r.cl = imm & 0xFF;
            r.wl = imm >> 8;
            break;
        case 0x05:                                      /* STMOD */
            r.mode = imm & 3;
            break;
        case 0x14:                                      /* MSCAL */
        case 0x15:                                      /* MSCALF */
        case 0x17:                                      /* MSCNT */
            /* a kick inside the part: the program runs on what is there,
             * then the VIF goes on in the other buffer */
            warn(out, "part %u: VIF %02X inside the packet", partIndex, cmd);
            emitBatch(img, mode, tex, partIndex, extra | BATCH_EXTRA_KICK, extPos, out);
            batches++;
            img.reset();
            extPos = false;
            extra = BATCH_EXTRA_KICK;
            break;
        case 0x20:                                      /* STMASK */
            if (p + 4 > size) { ok = false; break; }
            r.mask = rd32(data + p);
            p += 4;
            break;
        case 0x30:                                      /* STROW */
        case 0x31:                                      /* STCOL */
            if (p + 16 > size) { ok = false; break; }
            for (int i = 0; i < 4; i++)
                (cmd == 0x30 ? r.row : r.col)[i] = rd32(data + p + i * 4);
            p += 16;
            break;
        case 0x4A: {                                    /* MPG: microcode, skipped */
            size_t n = (num ? num : 256) * 8u;
            warn(out, "part %u: MPG in the packet (skipped)", partIndex);
            p += n;
            break;
        }
        case 0x50:                                      /* DIRECT / DIRECTHL: GIF data, skipped */
        case 0x51: {
            size_t n = (imm ? imm : 0x10000u) * 16u;
            p = (p + 15) & ~(size_t)15;
            warn(out, "part %u: DIRECT in the packet (skipped)", partIndex);
            p += n;
            break;
        }
        default:
            warn(out, "part %u: unknown VIFcode %08X at +0x%X", partIndex, code, (unsigned)(p - 4));
            ok = false;
            break;
        }
        if (!ok)
            break;
    }
    if (img.qwordWritten(VU_GIFTAG) || batches == 0) {
        emitBatch(img, mode, tex, partIndex, extra, extPos, out);
        batches++;
    }
    return ok;
}

bool decodeModel(const void *model, ResolveFn resolve, void *user,
                 const DecodeOptions &opt, Mesh &out)
{
    out.clear();
    if (model == nullptr)
        return false;
    VifModelHeader h;
    memcpy(&h, model, sizeof(h));
    out.mode = h.mode;
    out.scaleFlags = h.scaleFlags;
    out.nVertsHeader = h.nVerts;
    out.nPrimsHeader = h.nPrims;
    const uint8_t *parts = (const uint8_t *)model + sizeof(VifModelHeader);
    bool ok = true;
    for (uint32_t i = 0; i < h.nParts; i++) {
        VifPartRecord pr;
        memcpy(&pr, parts + i * sizeof(VifPartRecord), sizeof(pr));
        const void *data = resolve ? resolve(pr.data, user) : nullptr;
        if (data == nullptr) {
            warn(out, "part %u: data 0x%08X not resolved", i, pr.data);
            ok = false;
            continue;
        }
        if (!decodePart(data, pr.qwc, pr.tex, (uint16_t)i, h.mode, opt, out))
            ok = false;
    }
    return ok;
}

#if UINTPTR_MAX == 0xFFFFFFFFu
static const void *resolveAddress(uint32_t word, void *)
{
    return (const void *)(uintptr_t)word;
}

bool decodeModel(const void *model, Mesh &out, const DecodeOptions &opt)
{
    return decodeModel(model, resolveAddress, nullptr, opt, out);
}
#endif

std::string primToString(uint16_t prim)
{
    static const char *const types[8] = {
        "POINT", "LINE", "LINESTRIP", "TRIANGLE", "TRISTRIP", "TRIFAN", "SPRITE", "PRIM7"
    };
    std::string s = types[prim & 7];
    static const struct { uint16_t bit; const char *name; } bits[] = {
        { PRIM_IIP, "IIP" }, { PRIM_TME, "TME" }, { PRIM_FGE, "FGE" }, { PRIM_ABE, "ABE" },
        { PRIM_AA1, "AA1" }, { PRIM_FST, "FST" }, { PRIM_CTXT, "CTXT2" }, { PRIM_FIX, "FIX" },
    };
    for (const auto &b : bits) {
        if (prim & b.bit) {
            s += ' ';
            s += b.name;
        }
    }
    return s;
}

std::string modeToString(uint8_t mode)
{
    static const char *const lights[6] = { "NONE", "LIGHTS", "ENVMTX", "ENVMAP", "LOCAL", "CONST" };
    static const char *const fmts[8] = {
        "FLOAT", "MORPH_COLOR", "MORPH_FLOAT", "FIXED16", "FMT4", "FMT5", "FMT6", "FMT7"
    };
    char buf[64];
    unsigned lm = mode & 0x1F;
    if (lm < 6)
        snprintf(buf, sizeof(buf), "%s/%s", lights[lm], fmts[mode >> 5]);
    else
        snprintf(buf, sizeof(buf), "LMODE%u/%s", lm, fmts[mode >> 5]);
    return buf;
}

} // namespace gt3vif
