/* mdldump - loads a GT3 model file as extracted from gt3.vol, relocates it
 * as the game does, decodes its prebuilt VU1 packets with the native
 * decoder (src/engine/pgl/native/vif_model.cpp) and writes
 *   <out>.obj / <out>.mtl  the geometry the model's command streams draw
 *                          (LOD / switch / morph choices below), in the
 *                          model's space (the matrices of the streams applied)
 *   <out>.txt              statistics: per packet the header's nVerts /
 *                          nPrims against the decoded vertices / triangles,
 *                          modes, PRIMs, warnings
 *
 *   mdldump FILE [-o OUTBASE] [--lod N] [--switch N] [--morph T] [--raw]
 *               [--set NAME] [--part N] [--no-obj]
 *
 * --raw draws every packet once (no command streams); --set / --part keep
 * one model set (by name) / one part; --no-obj writes the statistics only.
 *
 * File kinds (by their headers): a ModelSet "GTM1" (gadget/*.mdl), a wheel
 * file "GTTW" (wheel/*, menu/wheel/*), a car model file (cars/day|eve|night/*,
 * menu/cars/*: CarModelFile, the body ModelSet and the wheel file), a course
 * (courses/<region>/*: CourseData, its ModelSets and the SpriteModel's
 * packets), a course preview (menu/course/*.cdt: the ModelSet at +4), a
 * bare VifModel (race/flare.bin).  "GTTR" tyre files only hold
 * the tread texture: the tyre geometry is built at run time (T462).
 *
 * The relocation is the game's (modelset_relocate 0x00224600,
 * vif_model_relocate 0x00248D48, carmodelfile_relocate 0x002272B0,
 * wheel_file_relocate 0x0021F4D0, course_data_relocate 0x001A8D18 for the
 * models): offsets become 32-bit addresses in place, so the tool is built
 * 32-bit like the game.  The command interpreter is modelset_run_part /
 * modelset_exec (T451) without the frustum tests and render states. */
#include "engine/pgl/native/vif_model.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

static_assert(sizeof(void *) == 4, "mdldump relocates 32-bit pointers in place: build it 32-bit (tools\\msvc.cmd)");

using namespace gt3vif;

/* ------------------------------------------------------------------------ */
/* Little helpers                                                           */
/* ------------------------------------------------------------------------ */

typedef uint32_t Addr;          /* a relocated pointer of the file */

static inline uint8_t *P(Addr a) { return (uint8_t *)(uintptr_t)a; }
static inline Addr A(const void *p) { return (Addr)(uintptr_t)p; }

static inline uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline int16_t rds16(const uint8_t *p) { return (int16_t)rd16(p); }
static inline float rdf(const uint8_t *p) { float v; memcpy(&v, p, 4); return v; }
static inline void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

/* p += delta for a stored offset (U102_RELOC) */
static inline void reloc(uint8_t *field, int32_t delta)
{
    wr32(field, rd32(field) + (uint32_t)delta);
}

/* ------------------------------------------------------------------------ */
/* The file structures (32-bit layouts of the decomp's headers)            */
/* ------------------------------------------------------------------------ */

/* ModelSet "GTM1" (T451_model_set.h) */
struct ModelSetView {
    uint8_t *m;
    uint16_t nParts() const { return rd16(m + 0x10); }
    uint16_t nPackets() const { return rd16(m + 0x12); }
    uint16_t nMaterials() const { return rd16(m + 0x14); }
    uint16_t nTex() const { return rd16(m + 0x16); }
    uint16_t nTexSets() const { return rd16(m + 0x18); }
    uint16_t nMaterialSets() const { return rd16(m + 0x1A); }
    Addr parts() const { return rd32(m + 0x20); }
    Addr packets() const { return rd32(m + 0x24); }
    Addr materials() const { return rd32(m + 0x28); }
    Addr bounds() const { return rd32(m + 0x30); }
    Addr anim() const { return rd32(m + 0x3C); }
    uint8_t *part(int i) const { Addr t = parts(); return t ? P(rd32(P(t) + 4 * i)) : nullptr; }
    uint8_t *packet(int i) const { Addr t = packets(); return t ? P(rd32(P(t) + 4 * i)) : nullptr; }
};

/* vif_model_relocate (tex1.cpp 0x00248D48): delta = m - m->self, self = m,
 * ptr04 if not NULL, every part's data. */
static void vifmodel_relocate(uint8_t *vm)
{
    int32_t delta = (int32_t)(A(vm) - rd32(vm));
    wr32(vm, A(vm));
    if (rd32(vm + 4) != 0)
        reloc(vm + 4, delta);
    uint16_t n = rd16(vm + 0x0A);
    for (int i = 0; i < n; i++)
        reloc(vm + 0x10 + 8 * i, delta);
}

/* modelset_relocate (T451 0x00224600), the parts the geometry needs: the
 * command streams, the packets, the material offsets, the bounds.  The
 * texture images (tex1_relocate) and the animation tracks are left as
 * offsets: nothing here reads them. */
static void modelset_relocate(uint8_t *m)
{
    int32_t delta = (int32_t)(A(m) - rd32(m + 4));
    if (delta == 0)
        return;
    wr32(m + 4, A(m));
    ModelSetView v = { m };
    if (rd32(m + 0x20) != 0) {
        reloc(m + 0x20, delta);
        for (int i = 0; i < v.nParts(); i++) {
            uint8_t *f = P(v.parts()) + 4 * i;
            if (rd32(f) != 0)
                reloc(f, delta);
        }
    }
    if (rd32(m + 0x24) != 0) {
        reloc(m + 0x24, delta);
        for (int i = 0; i < v.nPackets(); i++) {
            uint8_t *f = P(v.packets()) + 4 * i;
            if (rd32(f) != 0) {
                reloc(f, delta);
                vifmodel_relocate(P(rd32(f)));
            }
        }
    }
    if (rd32(m + 0x28) != 0) {
        reloc(m + 0x28, delta);
        for (int i = 0; i < v.nMaterials(); i++) {
            uint8_t *f = P(v.materials()) + 0x50 * i + 0x48;
            if (rd32(f) != 0)
                reloc(f, delta);
        }
    }
    if (rd32(m + 0x30) != 0)
        reloc(m + 0x30, delta);
}

/* SpriteModel (T454): spriteset_relocate, the packets only. */
static void spriteset_relocate(uint8_t *m)
{
    int32_t delta = (int32_t)(A(m) - rd32(m + 4));
    wr32(m + 4, A(m));
    if (rd32(m + 0x28) != 0) {
        reloc(m + 0x28, delta);
        uint16_t n = rd16(m + 0x16);
        for (int i = 0; i < n; i++) {
            uint8_t *f = P(rd32(m + 0x28)) + 4 * i;
            if (rd32(f) != 0) {
                reloc(f, delta);
                vifmodel_relocate(P(rd32(f)));
            }
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Matrices (pgl: row vectors, v' = v * M, row 3 the translation)           */
/* ------------------------------------------------------------------------ */

struct Mat { float m[4][4]; };

static Mat identity()
{
    Mat r = {};
    for (int i = 0; i < 4; i++)
        r.m[i][i] = 1.0f;
    return r;
}

/* a * b: a applied first */
static Mat mul(const Mat &a, const Mat &b)
{
    Mat r = {};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 4; k++)
                r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}

static void xformPoint(const Mat &M, const float in[3], float out[3])
{
    for (int j = 0; j < 3; j++)
        out[j] = in[0] * M.m[0][j] + in[1] * M.m[1][j] + in[2] * M.m[2][j] + M.m[3][j];
}

static void xformDir(const Mat &M, const float in[3], float out[3])
{
    for (int j = 0; j < 3; j++)
        out[j] = in[0] * M.m[0][j] + in[1] * M.m[1][j] + in[2] * M.m[2][j];
}

/* glRotate in the row-vector layout (pgl_rotate 0x00257CBC, vu0_rotate_x
 * / _y / _z): R applied first, M' = R * M. */
static Mat rotation(float deg, float x, float y, float z)
{
    float len = sqrtf(x * x + y * y + z * z);
    Mat r = identity();
    if (len == 0.0f)
        return r;
    x /= len; y /= len; z /= len;
    float a = deg * 3.14159265f / 180.0f;
    float c = cosf(a), s = sinf(a), t = 1.0f - c;
    /* the column-vector GL matrix, transposed */
    float g[3][3] = {
        { t * x * x + c,     t * x * y - s * z, t * x * z + s * y },
        { t * x * y + s * z, t * y * y + c,     t * y * z - s * x },
        { t * x * z - s * y, t * y * z + s * x, t * z * z + c     },
    };
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            r.m[i][j] = g[j][i];
    return r;
}

/* ------------------------------------------------------------------------ */
/* The command interpreter (modelset_run_part / modelset_exec, T451)        */
/* ------------------------------------------------------------------------ */

struct Options {
    int lod = 0;            /* command 0x07: the branch taken (0: the most detailed) */
    int sw = 0;             /* command 0x06: the branch taken */
    float morph = 0.0f;     /* command 0x35's position */
    bool raw = false;       /* every packet once, no command streams */
    bool noObj = false;     /* statistics only */
    int onlyPart = -1;
};

struct DrawCall {
    int set;                /* model set index in the file */
    int packet;
    Mat mtx;
    int texTable;           /* the ModelSet texture chosen by command 0x2B / 0x2C (-1 none) */
    float morphWeight;
    int part;               /* the command stream that drew it */
};

struct ModelSetInfo {
    std::string name;
    uint8_t *m;
    bool sprite;            /* a SpriteModel (packets only) */
};

struct Interp {
    const Options &opt;
    std::vector<DrawCall> &calls;
    int setIndex;
    ModelSetView ms;
    Mat stack[3][64];
    int sp[3] = { 0, 0, 0 };
    int matrixMode = 0;
    int texTable = -1;
    int depth = 0;
    int unknownOps = 0;

    Interp(const Options &o, std::vector<DrawCall> &c, int set, uint8_t *m)
        : opt(o), calls(c), setIndex(set), ms{ m }
    {
        for (int i = 0; i < 3; i++)
            stack[i][0] = identity();
    }
    Mat &cur() { return stack[matrixMode][sp[matrixMode]]; }

    void draw(int packet, float w, int part)
    {
        if (packet < 0 || packet >= ms.nPackets())
            return;
        DrawCall d;
        d.set = setIndex;
        d.packet = packet;
        d.mtx = stack[0][sp[0]];
        d.texTable = texTable;
        d.morphWeight = w;
        d.part = part;
        calls.push_back(d);
    }

    void runPart(int part)
    {
        if (part < 0 || part >= ms.nParts() || depth > 16)
            return;
        uint8_t *p = ms.part(part);
        if (p == nullptr)
            return;
        depth++;
        int saveTex = texTable;
        for (int guard = 0; guard < 1000000; guard++) {
            uint8_t op = *p++;
            if (op == 0)
                break;
            switch (op) {
            case 0x02: runPart(p[0]); p += 1; break;
            case 0x03: runPart(rd16(p)); p += 2; break;
            case 0x04: draw(p[0], 0.0f, part); p += 1; break;
            case 0x05: draw(rd16(p), 0.0f, part); p += 2; break;
            case 0x35: {
                int first = rd16(p), count = rd16(p + 2);
                p += 4;
                float pos = opt.morph * (float)count;
                int k = (int)pos;
                if (!(k < count))
                    k = count - 1;
                draw(first + k, pos - (float)k, part);
                break;
            }
            case 0x34: p += 4; break;
            case 0x0A: {                        /* frustum test: taken as passed */
                int n = p[0];
                p += 1 + n * 12 + 2;
                break;
            }
            case 0x0B: break;
            case 0x2B: texTable = p[0]; p += 1; break;
            case 0x2C: texTable = rd16(p); p += 2; break;
            case 0x21: p += 4; break;
            case 0x22: case 0x23: break;
            /* modelset_exec */
            case 0x08: p += (int8_t)p[0]; break;
            case 0x09: p += rds16(p); break;
            case 0x06: {
                p += 2;
                unsigned n = p[0];
                p += 1;
                unsigned k = (unsigned)opt.sw;
                if (k >= n)
                    p += n * 2;
                else {
                    p += k * 2;
                    p += rds16(p);
                }
                break;
            }
            case 0x07: {
                p += 0x10;
                int n = p[0];
                p += 1;
                int k = opt.lod;
                if (!(k < n))
                    k = n - 1;
                p += k * 2;
                p += rds16(p);
                break;
            }
            case 0x0C:
                if (sp[matrixMode] < 63) {
                    stack[matrixMode][sp[matrixMode] + 1] = cur();
                    sp[matrixMode]++;
                }
                break;
            case 0x0D: if (sp[matrixMode] > 0) sp[matrixMode]--; break;
            case 0x0E: matrixMode = p[0] < 3 ? p[0] : 0; p += 1; break;
            case 0x0F: {
                Mat m;
                for (int i = 0; i < 16; i++)
                    m.m[i / 4][i % 4] = rdf(p + 4 * i);
                cur() = m;
                p += 0x40;
                break;
            }
            case 0x10: {
                Mat m;
                for (int i = 0; i < 16; i++)
                    m.m[i / 4][i % 4] = rdf(p + 4 * i);
                cur() = mul(m, cur());              /* pgl_mult_matrix: m applied first */
                p += 0x40;
                break;
            }
            case 0x11: {
                Mat t = identity();
                t.m[3][0] = rdf(p); t.m[3][1] = rdf(p + 4); t.m[3][2] = rdf(p + 8);
                cur() = mul(t, cur());
                p += 12;
                break;
            }
            case 0x12: {
                Mat s = identity();
                s.m[0][0] = rdf(p); s.m[1][1] = rdf(p + 4); s.m[2][2] = rdf(p + 8);
                cur() = mul(s, cur());
                p += 12;
                break;
            }
            case 0x13:
                cur() = mul(rotation(rdf(p), rdf(p + 4), rdf(p + 8), rdf(p + 12)), cur());
                p += 16;
                break;
            case 0x14: cur() = mul(rotation(rdf(p), 1, 0, 0), cur()); p += 4; break;
            case 0x15: cur() = mul(rotation(rdf(p), 0, 1, 0), cur()); p += 4; break;
            case 0x16: cur() = mul(rotation(rdf(p), 0, 0, 1), cur()); p += 4; break;
            case 0x19: case 0x1F: case 0x29: case 0x2A: case 0x2F:
                p += 1;
                break;
            case 0x1C: case 0x20: p += 2; break;
            case 0x24: case 0x27: case 0x32: case 0x36: p += 4; break;
            case 0x31: p += 12; break;
            case 0x28: case 0x33: case 0x37: p += 16; break;
            default:
                if (op > 0x37 || op == 0x01 || op == 0x30)
                    unknownOps++;
                break;                          /* the rest: no operands */
            }
        }
        texTable = saveTex;
        depth--;
    }
};

/* ------------------------------------------------------------------------ */
/* The file                                                                 */
/* ------------------------------------------------------------------------ */

static bool isGTM1(const uint8_t *p) { return memcmp(p, "GTM1", 4) == 0; }

static std::vector<ModelSetInfo> openFile(uint8_t *buf, size_t size, std::string &kind,
                                          std::vector<uint8_t *> &bareModels)
{
    std::vector<ModelSetInfo> sets;
    auto addSet = [&](const char *name, uint32_t ofs) {
        if (ofs + 0x40 > size)
            return;
        uint8_t *m = buf + ofs;
        if (!isGTM1(m)) {
            fprintf(stderr, "%s at 0x%X: not GTM1\n", name, ofs);
            return;
        }
        modelset_relocate(m);
        sets.push_back({ name, m, false });
    };
    if (isGTM1(buf)) {
        kind = "ModelSet (GTM1)";
        addSet("model", 0);             /* (a container's NULL offsets are skipped by their callers) */
    } else if (memcmp(buf, "GTTW", 4) == 0) {
        /* wheel_file_relocate: model at +0x1C */
        kind = "wheel file (GTTW)";
        addSet("wheel", rd32(buf + 0x1C));
    } else if (memcmp(buf, "GTTR", 4) == 0) {
        kind = "tyre file (GTTR): the tread texture only, the geometry is built at run time (T462)";
    } else if (rd32(buf) == 0 && rd32(buf + 4) == 0x40 && memcmp(buf + 0x40, "GTCI", 4) == 0) {
        /* CarModelFile: info +4, body +8, tire +0xC, wheel +0x10 */
        kind = "car model file (CarModelFile)";
        if (rd32(buf + 8) != 0)
            addSet("body", rd32(buf + 8));
        uint32_t wheel = rd32(buf + 0x10);
        if (wheel != 0 && wheel + 0x20 < size && memcmp(buf + wheel, "GTTW", 4) == 0)
            addSet("wheel", wheel + rd32(buf + wheel + 0x1C));
    } else if (rd32(buf) == 0 && rd32(buf + 4) == 0x40 && isGTM1(buf + 0x40)) {
        /* menu/course/*.cdt (the course previews): +4 a ModelSet, +8 a
         * table of points, +0xC a 'GTCM' block */
        kind = "course preview (.cdt: +4 ModelSet)";
        addSet("preview", 0x40);
    } else if (rd32(buf) == 0 && rd32(buf + 4) >= 0xA4 && rd32(buf + 4) < size &&
               isGTM1(buf + rd32(buf + 4))) {
        kind = "course (CourseData)";
        static const struct { uint32_t ofs; const char *name; } courseSets[] = {
            { 0x04, "model04" }, { 0x14, "envModel" }, { 0x24, "model24" }, { 0x34, "model34" },
            { 0x54, "model54" }, { 0x58, "model58" }, { 0x5C, "envSky" }, { 0x60, "model60" },
            { 0x6C, "model6C" },
        };
        for (const auto &c : courseSets)
            if (rd32(buf + c.ofs) != 0)
                addSet(c.name, rd32(buf + c.ofs));
        uint32_t spr = rd32(buf + 0x78);
        if (spr != 0 && spr + 0x2C < size) {
            spriteset_relocate(buf + spr);
            sets.push_back({ "model78 (SpriteModel)", buf + spr, true });
        }
    } else {
        /* a bare VifModel (race/flare.bin): self 0, nParts, parts at +0x10 */
        uint16_t n = rd16(buf + 0x0A);
        if (rd32(buf) == 0 && n > 0 && n < 256 && 0x10u + 8u * n <= size) {
            kind = "bare VifModel";
            vifmodel_relocate(buf);
            bareModels.push_back(buf);
        } else {
            kind = "unknown";
        }
    }
    return sets;
}

/* ------------------------------------------------------------------------ */
/* Output                                                                   */
/* ------------------------------------------------------------------------ */

struct PacketStats {
    uint32_t verts = 0, tris = 0, batches = 0, flags = 0;
};

/* --exact: the positions as stored (before the draw's matrix) at full
 * precision, and each draw's matrix as hex floats ("# mtx"): what decides
 * whether two draws give the GPU bit-identical vertices (two copies of a
 * triangle tie in depth only then) */
static bool g_exact = false;

static void writeObj(FILE *obj, FILE *mtl, const std::vector<DrawCall> &calls,
                     const std::vector<ModelSetInfo> &sets, std::map<uint64_t, Mesh> &cache,
                     uint32_t &vOut, uint32_t &tOut)
{
    std::map<std::string, bool> mtls;
    uint32_t base = 1;
    vOut = tOut = 0;
    int callNo = 0;
    for (const DrawCall &d : calls) {
        uint64_t key = ((uint64_t)d.set << 32) | (uint32_t)d.packet;
        const Mesh &mesh = cache[key];
        if (mesh.vertices.empty())
            continue;
        fprintf(obj, "g set%d_part%d_pk%d_%d\n", d.set, d.part, d.packet, callNo++);
        if (g_exact) {
            /* --exact: the draw's matrix, as hex floats (two draws give the
             * GPU bit-identical vertices only with the same matrix and positions) */
            fprintf(obj, "# mtx");
            for (int i = 0; i < 16; i++)
                fprintf(obj, " %a", d.mtx.m[i / 4][i % 4]);
            fprintf(obj, "\n");
        }
        for (const Vertex &v : mesh.vertices) {
            float p[3], q[3];
            /* the morph weight as the program blends: (1 - w) a + w b */
            for (int c = 0; c < 3; c++)
                q[c] = v.pos[c] * (1.0f - d.morphWeight) + v.pos2[c] * d.morphWeight;
            xformPoint(d.mtx, q, p);
            if (g_exact)                    /* (the positions as stored: the GPU's input) */
                fprintf(obj, "v %.9g %.9g %.9g\n", q[0], q[1], q[2]);
            else if (mesh.attribs & ATTR_COLOR)
                fprintf(obj, "v %.6f %.6f %.6f %.4f %.4f %.4f\n", p[0], p[1], p[2],
                        std::min(1.0f, v.rgba[0] / 128.0f), std::min(1.0f, v.rgba[1] / 128.0f),
                        std::min(1.0f, v.rgba[2] / 128.0f));
            else
                fprintf(obj, "v %.6f %.6f %.6f\n", p[0], p[1], p[2]);
        }
        for (const Vertex &v : mesh.vertices)
            fprintf(obj, "vt %.6f %.6f\n", v.st[0], 1.0f - v.st[1]);
        for (const Vertex &v : mesh.vertices) {
            float n[3];
            xformDir(d.mtx, v.nrm, n);
            fprintf(obj, "vn %.5f %.5f %.5f\n", n[0], n[1], n[2]);
        }
        for (const Batch &b : mesh.batches) {
            char name[96];
            /* the part's texture (texmat_select: bits 0..8 the texture of
             * the table chosen by 0x2B / 0x2C, 9..15 the material) */
            snprintf(name, sizeof(name), "m%02X_t%d_tex%d_mat%d_prim%03X",
                     mesh.mode, d.texTable, b.tex & 0x1FF, b.tex >> 9, b.prim);
            if (!mtls[name]) {
                mtls[name] = true;
                fprintf(mtl, "newmtl %s\n# set %s mode %s prim %s\nKd 0.8 0.8 0.8\n\n", name,
                        sets[d.set].name.c_str(), modeToString(mesh.mode).c_str(),
                        primToString(b.prim).c_str());
            }
            fprintf(obj, "usemtl %s\n", name);
            for (uint32_t i = 0; i < b.indexCount; i += 3) {
                uint32_t a = mesh.indices[b.firstIndex + i] + base;
                uint32_t bb = mesh.indices[b.firstIndex + i + 1] + base;
                uint32_t c = mesh.indices[b.firstIndex + i + 2] + base;
                fprintf(obj, "f %u/%u/%u %u/%u/%u %u/%u/%u\n", a, a, a, bb, bb, bb, c, c, c);
            }
            tOut += b.indexCount / 3;
        }
        base += (uint32_t)mesh.vertices.size();
        vOut += (uint32_t)mesh.vertices.size();
    }
}

/* ------------------------------------------------------------------------ */
/* --selftest: synthetic packets for the VIF paths the files do not use     */
/* ------------------------------------------------------------------------ */

struct PacketWriter {
    std::vector<uint32_t> w;
    void code(uint32_t c) { w.push_back(c); }
    void f(float v) { uint32_t u; memcpy(&u, &v, 4); w.push_back(u); }
    void u(uint32_t v) { w.push_back(v); }
    /* GIF tag: NLOOP n, EOP, PRE, prim, PACKED, NREG 3, REGS ST RGBAQ XYZF2 */
    void giftag(uint32_t n, uint32_t prim)
    {
        code(0x6C01C0C0);                                   /* UNPACK V4-32 1 -> 0xC0 (FLG USN) */
        u(n | 0x8000);
        u(0x30004000 | (prim << 15));
        u(0x412);
        u(0);
    }
    void strips(std::initializer_list<int> lens)
    {
        uint32_t n = (uint32_t)lens.size();
        code(0x6200C040 | ((n + 1) << 16));                 /* UNPACK S-8 n+1 -> 0x40 */
        std::vector<uint8_t> b;
        b.push_back((uint8_t)n);
        for (int l : lens)
            b.push_back((uint8_t)((l - 2) * 3));
        while (b.size() % 4)
            b.push_back(0);
        for (size_t i = 0; i < b.size(); i += 4)
            u(b[i] | b[i + 1] << 8 | b[i + 2] << 16 | (uint32_t)b[i + 3] << 24);
    }
    uint32_t qwc()
    {
        while (w.size() % 4)
            w.push_back(0);
        return (uint32_t)(w.size() / 4);
    }
};

static int g_fails = 0;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fails++; } } while (0)

static bool near(float a, float b) { return fabsf(a - b) < 1e-4f; }

static int selftest()
{
    DecodeOptions opt;
    /* 1. pgl_put_const_color's constant colour: STCYCL 0/4 (a filling write
     * without data), STROW, STMASK 0x55555555, UNPACK_M V4-32 -> 0x80 */
    {
        PacketWriter p;
        p.giftag(4, PRIM_TRISTRIP | PRIM_IIP);
        p.code(0x6804C000);                                 /* V3-32 4 -> 0 */
        float xyz[12] = { 0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0 };
        for (float v : xyz) p.f(v);
        p.strips({ 4 });
        p.code(0x01000400);                                 /* STCYCL cl 0, wl 4 */
        p.code(0x30000000); p.u(0x10); p.u(0x20); p.u(0x30); p.u(0x80);    /* STROW */
        p.code(0x20000000); p.u(0x55555555);                /* STMASK: every field from the row */
        p.code(0x7C04C080);                                 /* UNPACK_M V4-32 4 -> 0x80, no data */
        p.code(0x01000404);                                 /* STCYCL 4 / 4 */
        Mesh m;
        bool ok = decodePart(p.w.data(), p.qwc(), 0, 0, 0x00, opt, m);
        CHECK(ok && m.vertices.size() == 4 && m.triangleCount() == 2);
        for (const Vertex &v : m.vertices)
            CHECK(v.rgba[0] == 0x10 && v.rgba[1] == 0x20 && v.rgba[2] == 0x30 && v.rgba[3] == 0x80);
        CHECK(m.warnings.empty());
        printf("const colour fill: %s\n", g_fails ? "fail" : "ok");
    }
    /* 2. format 3 (tri_draw16): V4-16 xyzw signed, V2-16 S T signed / 4096,
     * PRIM TRIANGLE, strips of 3 */
    {
        int before = g_fails;
        PacketWriter p;
        p.giftag(6, PRIM_TRIANGLE | PRIM_IIP | PRIM_TME);
        p.code(0x6D068000);                                 /* UNPACK V4-16 6 -> 0 (FLG, signed: USN clear) */
        int16_t xyzw[24] = { -100, 200, 300, 0, 1, 2, 3, 0, 4, 5, 6, 0,
                             7, 8, 9, 0, 10, 11, 12, 0, 13, 14, 15, 0 };
        for (int i = 0; i < 24; i += 2)
            p.u((uint16_t)xyzw[i] | (uint32_t)(uint16_t)xyzw[i + 1] << 16);
        p.strips({ 3, 3 });
        p.code(0x75068040);                                 /* UNPACK_M V2-16 6 -> 0x40 (signed) */
        int16_t st[12] = { 4096, 2048, -4096, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        for (int i = 0; i < 12; i += 2)
            p.u((uint16_t)st[i] | (uint32_t)(uint16_t)st[i + 1] << 16);
        p.code(0x6E06C080);                                 /* V4-8 6 -> 0x80 */
        for (int i = 0; i < 6; i++) p.u(0x80808080);
        Mesh m;
        decodePart(p.w.data(), p.qwc(), 0, 0, 0x60, opt, m);
        CHECK(m.vertices.size() == 6 && m.triangleCount() == 2);
        CHECK(near(m.vertices[0].pos[0], -100) && near(m.vertices[0].pos[1], 200) && near(m.vertices[0].pos[2], 300));
        CHECK(near(m.vertices[0].st[0], 1.0f) && near(m.vertices[0].st[1], 0.5f) && near(m.vertices[1].st[0], -1.0f));
        CHECK(m.vertices[0].rgba[0] == 0x80);
        /* the strip winding / kick order: list triangle (0 1 2) as (2 0 1) */
        CHECK(m.indices.size() == 6 && m.indices[0] == 2 && m.indices[1] == 0 && m.indices[2] == 1);
        printf("format 3 / TRIANGLE: %s\n", g_fails > before ? "fail" : "ok");
    }
    /* 3. morph format 1: xyz at 0 / 0x20, colours at 0x80 / 0xA0; a strip of
     * 5: triangles (0 1 2) (2 1 3) (2 3 4) in kick-first order */
    {
        int before = g_fails;
        PacketWriter p;
        p.giftag(5, PRIM_TRISTRIP | PRIM_IIP);
        p.code(0x6805C000);
        for (int i = 0; i < 15; i++) p.f((float)i);
        p.code(0x6805C020);
        for (int i = 0; i < 15; i++) p.f((float)(100 + i));
        p.strips({ 5 });
        p.code(0x6E05C080);
        for (int i = 0; i < 5; i++) p.u(0x01020304u * (i + 1));
        p.code(0x6E05C0A0);
        for (int i = 0; i < 5; i++) p.u(0xFFFFFFFF);
        Mesh m;
        decodePart(p.w.data(), p.qwc(), 0, 0, 0x20, opt, m);
        CHECK(m.vertices.size() == 5 && m.triangleCount() == 3 && (m.attribs & ATTR_MORPH));
        CHECK(near(m.vertices[1].pos[0], 3) && near(m.vertices[1].pos2[0], 103));
        CHECK(m.vertices[1].rgba[0] == 0x08 && m.vertices[1].rgba2[3] == 0xFF);
        static const uint32_t want[9] = { 2, 0, 1, 3, 2, 1, 4, 2, 3 };
        for (int i = 0; i < 9 && i < (int)m.indices.size(); i++)
            CHECK(m.indices[i] == want[i]);
        printf("morph format 1 / strip order: %s\n", g_fails > before ? "fail" : "ok");
    }
    /* 4. V4-5, a skipping write (STCYCL cl 2 wl 1), a fan */
    {
        int before = g_fails;
        PacketWriter p;
        p.giftag(3, PRIM_TRIFAN | PRIM_IIP);
        p.code(0x6803C000);                                 /* V3-32 3 -> 0: w from the next element */
        for (int i = 0; i < 9; i++) p.f((float)(i + 1));
        p.strips({ 3 });
        p.code(0x01000102);                                 /* STCYCL cl 2, wl 1: every other qword */
        p.code(0x6F02C080);                                 /* V4-5 2 -> 0x80, 0x82 */
        p.u(0x7FFF | 0x8000u << 16);
        p.code(0x01000404);
        p.code(0x6F01C081);                                 /* V4-5 1 -> 0x81 */
        p.u(0x001F);
        Mesh m;
        decodePart(p.w.data(), p.qwc(), 0, 0, 0x00, opt, m);
        CHECK(m.vertices.size() == 3 && m.triangleCount() == 1);
        CHECK(m.vertices[0].rgba[0] == 0xF8 && m.vertices[0].rgba[3] == 0);
        CHECK(m.vertices[2].rgba[0] == 0 && m.vertices[2].rgba[3] == 0x80);
        CHECK(m.vertices[1].rgba[0] == 0xF8 && m.vertices[1].rgba[1] == 0);
        printf("V4-5 / skipping write / fan: %s\n", g_fails > before ? "fail" : "ok");
    }
    /* 5. positions from vu1_unpack_xyz (preXyz) */
    {
        int before = g_fails;
        PacketWriter p;
        p.giftag(4, PRIM_TRISTRIP | PRIM_IIP | PRIM_TME);
        p.strips({ 4 });
        p.code(0x6804C080);
        for (int i = 0; i < 12; i++) p.f(i == 1 || i == 4 || i == 7 || i == 10 ? 1.0f : 0.0f);
        p.code(0x7404C040);                                 /* UNPACK_M V2-32 4 -> 0x40 */
        for (int i = 0; i < 8; i++) p.f(0.25f * i);
        float pre[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
        DecodeOptions o2;
        o2.preXyz = pre;
        o2.preXyzCount = 4;
        Mesh m;
        decodePart(p.w.data(), p.qwc(), 0, 0, 0x01, o2, m);
        CHECK(m.vertices.size() == 4 && (m.attribs & ATTR_EXT_POS) && near(m.vertices[3].pos[2], 12));
        CHECK(near(m.vertices[1].nrm[1], 1.0f) && near(m.vertices[3].st[1], 1.75f));
        CHECK(m.warnings.empty());
        printf("preXyz (disc / caliper quads): %s\n", g_fails > before ? "fail" : "ok");
    }
    printf("selftest: %d failure(s)\n", g_fails);
    return g_fails ? 1 : 0;
}

static int usage()
{
    fprintf(stderr,
            "usage: mdldump FILE [-o OUTBASE] [--lod N] [--switch N] [--morph T] [--raw]\n"
            "               [--set NAME] [--part N] [--no-obj]\n"
            "       mdldump --selftest\n");
    return 2;
}

int main(int argc, char **argv)
{
    Options opt;
    const char *in = nullptr;
    std::string outBase;
    std::string onlySet;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-o" && i + 1 < argc) outBase = argv[++i];
        else if (a == "--lod" && i + 1 < argc) opt.lod = atoi(argv[++i]);
        else if (a == "--switch" && i + 1 < argc) opt.sw = atoi(argv[++i]);
        else if (a == "--morph" && i + 1 < argc) opt.morph = (float)atof(argv[++i]);
        else if (a == "--raw") opt.raw = true;
        else if (a == "--no-obj") opt.noObj = true;
        else if (a == "--exact") g_exact = true;
        else if (a == "--set" && i + 1 < argc) onlySet = argv[++i];
        else if (a == "--part" && i + 1 < argc) opt.onlyPart = atoi(argv[++i]);
        else if (a == "--selftest") return selftest();
        else if (a[0] == '-') return usage();
        else in = argv[i];
    }
    if (in == nullptr)
        return usage();
    if (outBase.empty())
        outBase = in;

    FILE *f = fopen(in, "rb");
    if (f == nullptr) {
        fprintf(stderr, "cannot open %s\n", in);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    size_t size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    /* 64-byte aligned, as the game's load buffers */
    std::vector<uint8_t> storage(size + 64 + 16);
    uint8_t *buf = (uint8_t *)(((uintptr_t)storage.data() + 63) & ~(uintptr_t)63);
    if (fread(buf, 1, size, f) != size) {
        fprintf(stderr, "read error\n");
        return 1;
    }
    fclose(f);

    std::string kind;
    std::vector<uint8_t *> bare;
    std::vector<ModelSetInfo> sets = openFile(buf, size, kind, bare);

    std::string txtPath = outBase + ".txt";
    FILE *txt = fopen(txtPath.c_str(), "w");
    if (txt == nullptr) {
        fprintf(stderr, "cannot write %s\n", txtPath.c_str());
        return 1;
    }
    fprintf(txt, "file %s (%zu bytes): %s\n", in, size, kind.c_str());

    /* a bare VifModel: one pseudo set */
    std::map<uint64_t, Mesh> cache;
    std::vector<DrawCall> calls;
    uint32_t totalPk = 0, okPk = 0, totalWarn = 0;
    uint32_t sumHdrV = 0, sumHdrP = 0, sumV = 0, sumT = 0;

    auto decodePacket = [&](int set, int index, uint8_t *vm, const char *setName) {
        uint64_t key = ((uint64_t)set << 32) | (uint32_t)index;
        Mesh &mesh = cache[key];
        DecodeOptions dop;
        decodeModel(vm, mesh, dop);
        uint32_t nloop = 0, flags = 0;
        for (const Batch &b : mesh.batches) {
            nloop += b.nloop;
            flags |= b.flags;
        }
        bool match = nloop == mesh.nVertsHeader && mesh.triangleCount() == mesh.nPrimsHeader;
        totalPk++;
        okPk += match;
        sumHdrV += mesh.nVertsHeader;
        sumHdrP += mesh.nPrimsHeader;
        sumV += nloop;
        sumT += mesh.triangleCount();
        fprintf(txt, "  %s packet %4d: %-18s parts %3zu  verts %5u/%5u  tris %5u/%5u %s",
                setName, index, modeToString(mesh.mode).c_str(), mesh.batches.size(), nloop,
                mesh.nVertsHeader, mesh.triangleCount(), mesh.nPrimsHeader, match ? "ok" : "MISMATCH");
        if (mesh.scaleFlags)
            fprintf(txt, " scale %u", mesh.scaleFlags);
        std::map<uint16_t, int> prims;
        for (const Batch &b : mesh.batches)
            prims[b.prim]++;
        for (auto &pr : prims)
            fprintf(txt, " [%s x%d]", primToString(pr.first).c_str(), pr.second);
        if (flags)
            fprintf(txt, " flags %X", flags);
        fprintf(txt, "\n");
        for (const std::string &w : mesh.warnings) {
            fprintf(txt, "      ! %s\n", w.c_str());
            totalWarn++;
        }
    };

    for (size_t s = 0; s < sets.size(); s++) {
        const ModelSetInfo &si = sets[s];
        uint8_t *m = si.m;
        if (!onlySet.empty() && si.name.find(onlySet) == std::string::npos)
            continue;
        if (si.sprite) {
            uint16_t n = rd16(m + 0x16);
            fprintf(txt, "set %zu %s: %u packets\n", s, si.name.c_str(), n);
            for (int i = 0; i < n; i++) {
                uint8_t *vm = P(rd32(P(rd32(m + 0x28)) + 4 * i));
                decodePacket((int)s, i, vm, si.name.c_str());
                /* spriteset_draw_part draws its packets in the current
                 * matrix: each once, as they are */
                DrawCall d = { (int)s, i, identity(), -1, 0.0f, -1 };
                calls.push_back(d);
            }
            continue;
        }
        ModelSetView v = { m };
        fprintf(txt, "set %zu %s: parts %u packets %u materials %u tex %u texSets %u materialSets %u anim %s\n",
                s, si.name.c_str(), v.nParts(), v.nPackets(), v.nMaterials(), v.nTex(), v.nTexSets(),
                v.nMaterialSets(), v.anim() ? "yes" : "no");
        for (int i = 0; i < v.nPackets(); i++) {
            uint8_t *vm = v.packet(i);
            if (vm != nullptr)
                decodePacket((int)s, i, vm, si.name.c_str());
        }
        if (opt.raw) {
            for (int i = 0; i < v.nPackets(); i++) {
                DrawCall d = { (int)s, i, identity(), -1, 0.0f, -1 };
                calls.push_back(d);
            }
        } else {
            Interp it(opt, calls, (int)s, m);
            for (int p = 0; p < v.nParts(); p++) {
                if (opt.onlyPart >= 0 && p != opt.onlyPart)
                    continue;
                it.runPart(p);
            }
            if (it.unknownOps)
                fprintf(txt, "  (%d commands without a known operand size)\n", it.unknownOps);
        }
    }
    for (size_t b = 0; b < bare.size(); b++) {
        int s = (int)sets.size();
        sets.push_back({ "vifmodel", bare[b], false });
        decodePacket(s, 0, bare[b], "vifmodel");
        DrawCall d = { s, 0, identity(), -1, 0.0f, -1 };
        calls.push_back(d);
    }

    fprintf(txt, "packets %u, header counts matched %u; header verts %u prims %u, decoded verts %u tris %u; warnings %u\n",
            totalPk, okPk, sumHdrV, sumHdrP, sumV, sumT, totalWarn);

    if (opt.noObj) {
        fclose(txt);
        printf("%s: %s; %u packets (%u match their header counts), %u warnings\n", in, kind.c_str(),
               totalPk, okPk, totalWarn);
        return 0;
    }
    std::string objPath = outBase + ".obj", mtlPath = outBase + ".mtl";
    FILE *obj = fopen(objPath.c_str(), "w");
    FILE *mtl = fopen(mtlPath.c_str(), "w");
    if (obj == nullptr || mtl == nullptr) {
        fprintf(stderr, "cannot write %s\n", objPath.c_str());
        return 1;
    }
    size_t cut = mtlPath.find_last_of("/\\");
    std::string mtlName = cut == std::string::npos ? mtlPath : mtlPath.substr(cut + 1);
    fprintf(obj, "# mdldump %s: %s (lod %d, switch %d, morph %.2f%s)\nmtllib %s\n", in, kind.c_str(),
            opt.lod, opt.sw, opt.morph, opt.raw ? ", every packet" : "", mtlName.c_str());
    uint32_t vOut, tOut;
    writeObj(obj, mtl, calls, sets, cache, vOut, tOut);
    fclose(obj);
    fclose(mtl);
    fprintf(txt, "obj: %zu draws, %u vertices, %u triangles\n", calls.size(), vOut, tOut);
    fclose(txt);
    printf("%s: %s; %u packets (%u match their header counts), %u warnings; obj %zu draws, %u vertices, %u triangles\n",
           in, kind.c_str(), totalPk, okPk, totalWarn, calls.size(), vOut, tOut);
    return 0;
}
