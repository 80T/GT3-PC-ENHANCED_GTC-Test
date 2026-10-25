/* vif_model.h - the native decoder of GT3's prebuilt VU1 models.
 *
 * On the PS2 a prebuilt model (G001's VifModel: the model sets of T451, the
 * car / course / wheel files, race/flare.bin, the strip packets the game
 * builds with TStripBuilder, T448) is a list of VIF1 packets.  model_draw
 * (render.c 0x0024D048) sends each part (VifPart) to VU1 by a DMA ref tag
 * and kicks the model program of the VU1 microcode (g_gsVu1Microcode,
 * gs_driver_core.c) on it; the program converts the vertex format, lights
 * the vertices, transforms them and hands one GS triangle-strip packet to
 * the GIF (XGKICK).  docs/models.md describes the microcode.
 *
 * This decoder reads the VIF packets once into vertex and index arrays (it
 * interprets the VIF codes: UNPACK with STCYCL / STMASK / STMOD / STROW /
 * STCOL, the data layout the microcode expects in VU1 data memory; it runs
 * no VU code).  What the program computed per frame (transform, lighting,
 * environment-map coordinates, morphing, fog) is the renderer's shaders'
 * job; docs/models.md gives their math.
 *
 * The layout a part leaves in VU1 data memory (addresses relative to the
 * double-buffer base TOPS, VIF UNPACK with FLG):
 *   0x00 + i  xyz of vertex i (V3-32 floats; format 3: integers)
 *   0x20 + i  formats 1 / 2: the morph target's xyz (at most 32 vertices)
 *   0x40      w: the number of strips; 0x41 + k w: 3 * (length - 2) of
 *             strip k (UNPACK S-8, before the masked stream below)
 *   0x40 + i  xy: texture coordinates S, T (V2-32, masked: w kept) -
 *             lighting modes 2 / 3: the normal (V3-32, masked)
 *   0x60 + i  formats 1 / 2: the morph target's 0x40 stream
 *   0x80 + i  RGBA (V4-8, 0..255; 0x80 = 1.0) - lighting modes 1 / 4: the
 *             normal (V3-32)
 *   0xA0 + i  formats 1 / 2: the morph target's 0x80 stream
 *   0xC0      the GIF tag (NLOOP = vertices, EOP, PRE, PRIM, PACKED,
 *             NREG 3, REGS ST RGBAQ XYZF2)
 * The program sets ADC (no drawing kick) on the first two vertices of each
 * strip: the part is one GS triangle strip of several strips.  The
 * decoder turns it into a triangle list, every triangle in the winding of
 * its strip's first triangle (v0 v1 v2, then v2 v1 v3, ...: front faces are
 * counter-clockwise in GL's y-up NDC) and starting with the vertex that
 * kicked it on the GS (whose colour a flat-shaded GS triangle takes; the
 * leading vertex of Direct3D 11's flat interpolation).
 *
 * Plain C++17, no Windows or game dependency. */
#ifndef GT3_PGL_NATIVE_VIF_MODEL_H
#define GT3_PGL_NATIVE_VIF_MODEL_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gt3vif {

/* ------------------------------------------------------------------------ */
/* VifModel.mode                                                            */
/* ------------------------------------------------------------------------ */

/* Bits 0..4: the lighting mode (vifmodel_setup_lighting 0x0024DF28, the
 * VU1L_* programs of pgl_vu1_internal.h). */
enum LightMode : uint8_t {
    LMODE_NONE   = 0,   /* vertex colours as they are (VU1L_DEFAULT) */
    LMODE_LIGHTS = 1,   /* OpenGL lights, normals at 0x80 (VU1L_DIFFUSE, _SPEC2*, _SPEC4*) */
    LMODE_ENVMTX = 2,   /* sphere-map texgen + reflection weight (VU1L_ENVMTX): normals at 0x40, colours at 0x80 */
    LMODE_ENVMAP = 3,   /* panorama texgen + reflection weight (VU1L_ENVMAP): normals at 0x40, colours at 0x80 */
    LMODE_LOCAL  = 4,   /* specular-only pass, local viewer (VU1L_LOCAL*): normals at 0x80 */
    LMODE_CONST  = 5    /* colour * constVector + constColor (VU1L_CONST) */
};

/* Bits 5..7: the vertex format (the ITOP offsets -2 / -4 / -6 of
 * vifmodel_setup_lighting; the converters at VU1 0x02B / 0x015 / 0x009). */
enum VertexFormat : uint8_t {
    FMT_FLOAT       = 0,    /* floats as unpacked */
    FMT_MORPH_COLOR = 1,    /* two key frames (0x00 / 0x20, 0x40 / 0x60, 0x80 / 0xA0); 0x80 integer colours */
    FMT_MORPH_FLOAT = 2,    /* two key frames; 0x80 floats (normals) */
    FMT_FIXED16     = 3     /* xyz integers (ITOF0), S T integers / 4096 (ITOF12) */
};

inline LightMode modeLighting(uint8_t mode) { return LightMode(mode & 0x1F); }
inline VertexFormat modeFormat(uint8_t mode) { return VertexFormat(mode >> 5); }

/* VifModel.scaleFlags: the lit colour's scale (GsContext.colorScale /
 * alphaScale): 255 with the bit, else 128 (the GS's 1.0). */
enum : uint8_t {
    SCALE_COLOR_255 = 0x01,
    SCALE_ALPHA_255 = 0x02
};

/* ------------------------------------------------------------------------ */
/* GS PRIM (the GIF tag's PRIM field, PRE set)                              */
/* ------------------------------------------------------------------------ */

enum : uint16_t {
    PRIM_TYPE_MASK = 0x007,
    PRIM_POINT     = 0,
    PRIM_LINE      = 1,
    PRIM_LINESTRIP = 2,
    PRIM_TRIANGLE  = 3,
    PRIM_TRISTRIP  = 4,
    PRIM_TRIFAN    = 5,
    PRIM_SPRITE    = 6,
    PRIM_IIP       = 0x008,     /* Gouraud (else flat: the colour of the last vertex) */
    PRIM_TME       = 0x010,     /* textured */
    PRIM_FGE       = 0x020,     /* fogged */
    PRIM_ABE       = 0x040,     /* alpha blended (GS ALPHA) */
    PRIM_AA1       = 0x080,     /* antialiased edges */
    PRIM_FST       = 0x100,     /* UV (texels) instead of STQ */
    PRIM_CTXT      = 0x200,     /* GS context 2 */
    PRIM_FIX       = 0x400      /* fixed fragment value */
};

/* The GIF tag's register list the model programs expect: ST, RGBAQ, XYZF2. */
const uint32_t GIF_REGS_ST_RGBAQ_XYZF2 = 0x412;

/* ------------------------------------------------------------------------ */
/* The decoded mesh                                                         */
/* ------------------------------------------------------------------------ */

/* Mesh::attribs: which Vertex fields carry data (the others are zero). */
enum : uint32_t {
    ATTR_POS     = 0x01,    /* pos */
    ATTR_MORPH   = 0x02,    /* pos2, and st2 / nrm2 / rgba2 of the present ones (formats 1 / 2) */
    ATTR_NORMAL  = 0x04,    /* nrm: lighting modes 1..4 (as stored: not normalised) */
    ATTR_ST      = 0x08,    /* st: lighting modes 0, 1, 4, 5 (2 / 3 generate them in the shader) */
    ATTR_COLOR   = 0x10,    /* rgba: lighting modes 0, 2, 3, 5 */
    ATTR_EXT_POS = 0x20     /* the positions came from DecodeOptions::preXyz (no xyz in the packet) */
};

/* One vertex as the VU1 program read it from data memory, 72 bytes. */
struct Vertex {
    float   pos[3];         /* model space (format 3: the integers as floats) */
    float   pos2[3];        /* morph target (formats 1 / 2), else = pos */
    float   nrm[3];         /* normal */
    float   nrm2[3];        /* morph target normal */
    float   st[2];          /* S, T: normalised texture coordinates (Q = 1/w is the GS's) */
    float   st2[2];         /* morph target S, T */
    uint8_t rgba[4];        /* R, G, B, A as the GS takes them (the low byte of each word) */
    uint8_t rgba2[4];       /* morph target colour (format 1) */
};
static_assert(sizeof(Vertex) == 72, "Vertex layout");

/* Batch::flags: what did not look like a model packet (the decoder still
 * does what the hardware would do). */
enum : uint32_t {
    BATCH_NO_GIFTAG      = 0x0001,  /* nothing unpacked to 0xC0: nothing drawn */
    BATCH_BAD_REGS       = 0x0002,  /* REGS not ST RGBAQ XYZF2 / not PACKED / NREG != 3 */
    BATCH_NO_PRE         = 0x0004,  /* PRE clear: the GS keeps its PRIM register */
    BATCH_STRIP_MISMATCH = 0x0008,  /* the strip lengths do not add up to NLOOP */
    BATCH_TOO_MANY       = 0x0010,  /* more vertices than the format holds (64, 32 when morphing) */
    BATCH_NOT_TRIANGLES  = 0x0020,  /* PRIM is not a triangle type: no indices */
    BATCH_BAD_STRIP      = 0x0040,  /* a strip length not of the form 3 * (n - 2) */
    BATCH_BROKEN_FORMAT  = 0x0080,  /* a mode / format pair the microcode gets wrong (see docs/models.md) */
    BATCH_UNWRITTEN      = 0x0100,  /* a stream the mode reads was not unpacked */
    BATCH_EXTRA_KICK     = 0x0200   /* the packet itself holds an MSCAL / MSCNT */
};

/* One VU1 run: one part (VifPart), or a piece of one cut by an MSCAL in its
 * data. */
struct Batch {
    uint32_t firstVertex;   /* into Mesh::vertices */
    uint32_t vertexCount;   /* = NLOOP */
    uint32_t firstIndex;    /* into Mesh::indices */
    uint32_t indexCount;    /* 3 per triangle */
    uint32_t firstStrip;    /* into Mesh::strips */
    uint32_t stripCount;
    uint16_t prim;          /* the GIF tag's PRIM (PRIM_*) */
    uint16_t tex;           /* VifPart.tex: the part-texture callback's id (texmat_select: tex | mat << 9), 0 none */
    uint16_t part;          /* the part's index in its VifModel */
    uint16_t nloop;         /* the GIF tag's NLOOP */
    uint32_t regs;          /* the GIF tag's REGS (low word) */
    uint32_t flags;         /* BATCH_* */
    /* PC: its vertices' box (pos and pos2), filled by the renderer after the
     * decode (pgl_models.cpp: where a draw lands on screen) */
    float bmin[3], bmax[3];
    bool hasBounds;
};

struct Mesh {
    uint8_t  mode = 0;          /* VifModel.mode */
    uint8_t  scaleFlags = 0;    /* VifModel.scaleFlags */
    uint16_t nVertsHeader = 0;  /* VifModel.nVerts (statistics in the file) */
    uint16_t nPrimsHeader = 0;  /* VifModel.nPrims */
    uint32_t attribs = 0;       /* ATTR_* of the vertices */
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;  /* triangle list (absolute vertex numbers) */
    std::vector<uint16_t> strips;   /* strip lengths in vertices, per batch (Batch::firstStrip) */
    std::vector<Batch>    batches;
    std::vector<std::string> warnings;

    void clear();
    uint32_t triangleCount() const { return (uint32_t)(indices.size() / 3); }
};

/* ------------------------------------------------------------------------ */
/* Decoding                                                                 */
/* ------------------------------------------------------------------------ */

/* The VIF state and VU1 data a part finds when it arrives. */
struct DecodeOptions {
    /* VIF STMASK: vifmodel_draw / vifmodel_draw_lit (and vif_model_draw,
     * flare.c) make it 0xC0C0C0C0 (w write-protected) before the parts;
     * STCYCL is 4 / 4 and STMOD 0 from the list start (gfx_begin_packet). */
    uint32_t stmask = 0xC0C0C0C0u;
    uint8_t  stcyclCl = 4;
    uint8_t  stcyclWl = 4;
    uint8_t  stmod = 0;
    /* xyz written to VU1 0x000.. in front of the part by vu1_unpack_xyz
     * (render.c 0x0024D0C0): RaceTirePacket's disc and caliper quads
     * (T449 setDiscVertices / setCaliperVertices) are built without
     * positions and get 4 per frame this way.  preXyzCount floats * 3. */
    const float *preXyz = nullptr;
    int preXyzCount = 0;
};

/* Decodes one part: qwc quadwords of VIF codes at data (VifPart.data /
 * .qwc; tex = VifPart.tex), for a model of the given mode byte.  Appends
 * vertices, indices, strips and one batch (more if the data holds an
 * MSCAL) to out.  Returns false when nothing could be decoded (out may
 * have warnings). */
bool decodePart(const void *data, uint32_t qwc, uint16_t tex, uint16_t partIndex,
                uint8_t mode, const DecodeOptions &opt, Mesh &out);

/* A VifModel in memory (G001_flare.h, 32-bit layout): self, ptr04, mode,
 * scaleFlags, nParts, nVerts, nPrims, then nParts VifPart {data, qwc,
 * tex}.  The data words are addresses once vif_model_relocate has run. */
struct VifModelHeader {
    uint32_t self;
    uint32_t ptr04;
    uint8_t  mode;
    uint8_t  scaleFlags;
    uint16_t nParts;
    uint16_t nVerts;
    uint16_t nPrims;
};
struct VifPartRecord {
    uint32_t data;
    uint16_t qwc;
    uint16_t tex;
};
static_assert(sizeof(VifModelHeader) == 0x10 && sizeof(VifPartRecord) == 8, "VifModel layout");

/* Turns a part's data word into a pointer (an address in the game; a tool
 * reading a file resolves it its own way).  NULL: skip the part. */
typedef const void *(*ResolveFn)(uint32_t dataWord, void *user);

/* Decodes every part of a VifModel into out (cleared first). */
bool decodeModel(const void *model, ResolveFn resolve, void *user,
                 const DecodeOptions &opt, Mesh &out);

#if UINTPTR_MAX == 0xFFFFFFFFu
/* The game's case (32-bit, relocated in place): the data words are
 * pointers. */
bool decodeModel(const void *model, Mesh &out, const DecodeOptions &opt = DecodeOptions());
#endif

/* "TRISTRIP IIP TME FGE ..." */
std::string primToString(uint16_t prim);
/* "LIGHTS/FLOAT" ... */
std::string modeToString(uint8_t mode);

} // namespace gt3vif

#endif /* GT3_PGL_NATIVE_VIF_MODEL_H */
