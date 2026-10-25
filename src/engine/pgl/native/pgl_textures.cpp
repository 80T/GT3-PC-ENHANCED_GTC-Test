/* pgl_textures.cpp - the VRAM address registry and the texture cache
 * (docs/graphics.md 5 and 6, docs/renderer.md "Textures").
 *
 * Nothing here stores GS memory.  Every object the game would have put into
 * VRAM is registered under the blocks it covers - a Tex1 set's uploads (an
 * upload group: the live Tex1Image), an image upload, a render target - in
 * a block owner map of two layers (the RGB / Z bytes and the top byte of
 * 32-bit words).  A texture (TEX0) is resolved through the owners of its
 * blocks: a render target is sampled as it is, uploads are decoded with
 * gstex (laid out the way the GS lays them out, read in the texture's
 * format, through the CLUT buffer state) and cached.  The GS CLUT buffer is
 * modelled by gstex::ClutBuffer, loaded at every TEX0 / TEX2 write with
 * CLD != 0 from the owner of CBP's blocks.
 *
 * Cost per frame (the game re-uploads everything it draws every frame):
 *   - a Tex1 set seen before is recognised by its header and upload table
 *     (no parse); only its small uploads (CLUTs, animated) are hashed; its
 *     layout is relative to the base it was first laid out at, so a set the
 *     VRAM allocator moves is not laid out again.  Each upload is placed at
 *     the base the set had when it was recorded (a set moves several times
 *     a frame: evicted and reloaded, a wheel face per wheel), and every block
 *     keeps the base of the copy that wrote it, which the draws recorded
 *     before a move sample;
 *   - PSMCT32 image uploads (movie frames and IPU pictures in 16 x 16
 *     macroblocks, icons, CLUT images) go into a CPU image of their buffer
 *     (a "surface", one per DBP / DBW) - a memcpy each - and the surface's
 *     GPU texture is updated once, when a draw uses it;
 *   - decoded textures are cached by content (set identity, TEX0 fields
 *     relative to the set, the palette, TEXA where it is used);
 *   - PC: a set's textures are decoded ahead on a worker as the game loads
 *     the set (pgl_prewarm.cpp; TexJob below), under the keys a draw works
 *     out for them. */
#include "engine/pgl/native/pgl_d3d.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <vector>

#include <windows.h>

#include "engine/pgl/native/gs_tex1.h"
#include "engine/pgl/native/gs_tex_decode.h"
#include "engine/pgl/native/gs_tex_layout.h"
#include "engine/pgl/native/gs_tex_space.h"
#include "platform/log.h"

namespace pglr {

namespace {

using gstex::BlockRange;

uint64_t hash_bytes(const void *p, size_t n, uint64_t h = 0x9E3779B97F4A7C15ull)
{
    const uint8_t *s = static_cast<const uint8_t *>(p);
    while (n >= 8) {
        uint64_t w;
        memcpy(&w, s, 8);
        h = (h ^ w) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
        s += 8;
        n -= 8;
    }
    while (n-- > 0)
        h = (h ^ *s++) * 0x100000001B3ull;
    return h ^ (h >> 29);
}

inline uint64_t mix(uint64_t h, uint64_t v)
{
    return h ^ (v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2));
}

/* PC: a fingerprint of a big upload's bytes - 32 words of 8 bytes spread
 * over it, its first and last included - cheap enough for every upload of
 * every frame: a set's texels written again in place (no new load stamp)
 * change it (group_for; docs/renderer.md 6, "Changed in place"). */
uint64_t sample_hash(const void *p, size_t n)
{
    const uint8_t *s = static_cast<const uint8_t *>(p);
    if (n < 32 * 8)
        return hash_bytes(p, n, 0x5A3C96E1ull);
    uint64_t h = 0x5A3C96E1ull ^ n;
    const size_t step = (n - 8) / 31;
    for (size_t i = 0; i < 32; i++) {
        uint64_t w;
        memcpy(&w, s + (i == 31 ? n - 8 : i * step), 8);
        h = (h ^ w) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
    }
    return h;
}

/* PC: a stronger combine (one-to-one in v, multiply and shift) for the keys
 * that stand for content: the block tags, the CLUT loads, the CLUT rows and
 * the palettes (CPU time, docs/renderer.md 6). */
inline uint64_t comb(uint64_t h, uint64_t v)
{
    h = (h ^ v) * 0xFF51AFD7ED558CCDull;
    return h ^ (h >> 32);
}

const uint32_t BLOCK_MASK = gstex::GS_MEM_BLOCKS - 1;

/* Owner-map layers. */
enum : uint8_t { LAYER_RGB = 1, LAYER_TOP = 2 };

uint8_t layers_of(uint32_t psm)
{
    switch (psm) {
    case gstex::PSMCT24: case gstex::PSMZ24:
        return LAYER_RGB;
    case gstex::PSMT8H: case gstex::PSMT4HL: case gstex::PSMT4HH:
        return LAYER_TOP;
    default:
        return LAYER_RGB | LAYER_TOP;
    }
}

bool psm_top(uint32_t psm)
{
    return psm == gstex::PSMT8H || psm == gstex::PSMT4HL || psm == gstex::PSMT4HH;
}

/* A Tex1Image as the game holds it (G001_flare.h, 32-bit). */
struct RawTex1 {
    uint32_t magic, self, ptr08, size;
    uint16_t vramBase, vramSize, nTex, nXfer;
    uint32_t tex, xfer, ptrTable, clutAnim;
    uint8_t  unk28[8];
};
struct RawXfer {
    uint32_t data;
    uint16_t dbp;
    uint8_t  dbw, dpsm;
    uint16_t w, h;
};
static_assert(sizeof(RawTex1) == 0x30 && sizeof(RawXfer) == 0xC, "Tex1Image layout");

/* OBJ_PLANE: a byte plane of pfx_split_rgb_planes (the afterimage,
 * graphics.md 8.6): one colour channel of a captured frame held in the top
 * bytes of a PSMCT32 buffer's words (layer 1 only), read as PSMT8H / T4HL /
 * T4HH through the CLUT. */
enum ObjKind : uint8_t { OBJ_GROUP, OBJ_IMAGE, OBJ_SURFACE, OBJ_TARGET, OBJ_PLANE };

struct GroupUpload {
    BlockRange range;           /* at layoutBase */
    uint64_t hash = 0;          /* the data at the last upload: small ones (CLUTs) hashed whole, PC: big ones
                                   by sample_hash */
    bool isSmall = false;
};

struct Obj {
    uint32_t id = 0;
    ObjKind kind = OBJ_IMAGE;
    uint64_t serial = 0;        /* when it last wrote its blocks */
    uint64_t lastFrame = 0;
    double lastUse = 0.0;       /* PC: g_execSeconds when last uploaded (a group: kept that long unowned) */
    /* group: set, space, uploads and marks are at layoutBase; the set is
     * at curBase now (delta() moves blocks between the two) */
    const void *img = nullptr;
    uint64_t stamp = 0;
    uint32_t contentVer = 0;    /* PC: its data changed (parsed again, a small upload): g_blockGen */
    bool live = false;          /* PC: (registry_end_frame) it owns a block */
    gstex::Tex1Set set;
    gstex::TexSpace space;
    bool laidOut = false;
    uint32_t layoutBase = 0, curBase = 0;
    RawTex1 rawHdr = {};
    std::vector<RawXfer> rawXfer;
    std::vector<GroupUpload> uploads;
    std::vector<std::pair<uint32_t, uint8_t>> marks;    /* block, layers */
    /* image */
    gstex::Transfer t;
    std::vector<uint8_t> data;
    uint64_t key = 0;
    /* surface: the CT32 pixels of buffer (sdbp, sdbw), sw x sh */
    uint32_t sdbp = 0, sdbw = 0;
    int sw = 0, sh = 0;
    std::vector<uint32_t> spx;
    int dx0 = 0, dy0 = 0, dx1 = 0, dy1 = 0;     /* dirty since the GPU copy */
    Com<ID3D11Texture2D> stex;
    Com<ID3D11ShaderResourceView> ssrv;
    int texW = 0, texH = 0;
    /* target */
    Target *target = nullptr;
    bool needRemark[2] = {true, true};
    BlockRange targetRange;
    int markW = 0, markH = 0;                   /* the GS geometry targetRange was marked with */
    uint32_t markFbw = 0;
    /* surface: the region (hx, hy, hw, hh) holds the pixels of a high-res
     * image (g_hires, key hkey; 0: none) - registry_hires_readback */
    uint64_t hkey = 0;
    int hx = 0, hy = 0, hw = 0, hh = 0;
    /* plane: the top bytes of the pw x ph PSMCT32 buffer (pbp, pfbw) hold
     * channel pch of the capture pcap (its native pixels, capW x capH); the
     * texture last read through a palette (key: palette, PSM, serial) */
    uint32_t pbp = 0, pfbw = 0;
    int pw = 0, ph = 0, pch = 0;
    Com<ID3D11ShaderResourceView> pcap;
    int capW = 0, capH = 0;
    Com<ID3D11Texture2D> pdec;
    Com<ID3D11RenderTargetView> pdecRtv;
    Com<ID3D11ShaderResourceView> pdecSrv;
    uint64_t pdecKey = 0;

    uint32_t delta() const { return (curBase - layoutBase) & BLOCK_MASK; }
};

uint32_t g_own[2][gstex::GS_MEM_BLOCKS];       /* [0] RGB / Z layer, [1] top byte */
/* PC (CPU time): what each block holds, as a tag of its last write (for a
 * Tex1 set its id, base and data version; for an image upload its content
 * key; for a target its id - the same each time they mark the same blocks
 * again; a new g_markGen for a surface, a plane, a release): a CLUT load is
 * known by its blocks' tags (registry_clut_load's memo).  The front caches
 * (CLUT loads, texture binds) hold while the regions of VRAM they read keep
 * their generation (g_regionGen: 256 blocks each, moved when a tag - so an
 * owner - changes in it; the uploads of a frame touch a few of them) and
 * g_tagEpoch stays (it moves when a cache they point into drops entries). */
uint64_t g_blockGen[2][gstex::GS_MEM_BLOCKS];
uint64_t g_markGen;
uint64_t g_tagEpoch = 1;
constexpr uint32_t kRegionShift = 8, kRegions = gstex::GS_MEM_BLOCKS >> kRegionShift;
uint32_t g_regionGen[kRegions];

/* The regions (a bit each) whose tags changed: a generation further. */
void regions_changed(uint64_t mask)
{
    static_assert(kRegions <= 64, "a region mask of 64 bits");
    for (uint32_t r = 0; mask != 0; r++, mask >>= 1)
        if (mask & 1) {
            g_regionGen[r]++;
            g_profCount[PCOUNT_REGIONS]++;  /* (the verbose monitor's counts) */
        }
}

/* The generations of up to four regions, as a front cache entry saw them. */
struct RegionSnap {
    uint8_t n = 0;
    uint8_t r[4] = {};
    uint32_t gen[4] = {};
    bool add(uint32_t region)
    {
        for (uint8_t i = 0; i < n; i++)
            if (r[i] == region)
                return true;
        if (n == 4)
            return false;
        r[n] = (uint8_t)region;
        gen[n] = g_regionGen[region];
        n++;
        return true;
    }
    /* the regions of blocks first .. first + count - 1 (the VRAM wraps):
     * false when more than four */
    bool add_blocks(uint32_t first, uint32_t count)
    {
        if (count == 0)
            return true;
        const uint32_t mask = gstex::GS_MEM_BLOCKS - 1;
        const uint32_t r0 = (first & mask) >> kRegionShift;
        const uint32_t r1 = ((first + count - 1) & mask) >> kRegionShift;
        for (uint32_t r = r0, k = 0; k < kRegions; r = (r + 1) % kRegions, k++) {
            if (!add(r))
                return false;
            if (r == r1)
                return true;
        }
        return true;
    }
    bool same() const
    {
        for (uint8_t i = 0; i < n; i++)
            if (g_regionGen[r[i]] != gen[i])
                return false;
        return true;
    }
};
/* For blocks a Tex1 set owns: the VRAM base the set was uploaded at when it
 * wrote the block.  A set the allocator moves within a frame leaves its
 * earlier copies in VRAM (the GS keeps the bytes), and draws recorded while
 * it was there sample them: their delta is the one of the copy, not of the
 * set's latest upload. */
uint16_t g_ownBase[2][gstex::GS_MEM_BLOCKS];
uint64_t g_serial = 0;
uint32_t g_nextId = 1;
std::unordered_map<uint32_t, std::unique_ptr<Obj>> g_objs;
std::unordered_map<const void *, uint64_t> g_stamps;    /* Tex1Image -> load stamp */
uint64_t g_nextStamp = 1;
std::unordered_map<const void *, uint32_t> g_groupByImg;
std::unordered_map<uint64_t, uint32_t> g_imageByKey;
std::unordered_map<uint64_t, uint32_t> g_surfaceByKey;
std::unordered_map<uint32_t, uint32_t> g_planeByBlock;  /* plane objects by their first block */
std::vector<uint32_t> g_targetObjs;

/* The frames pfx_split_rgb_planes captured, by the first plane's block (the
 * three planes of a split share one capture). */
struct Capture {
    Com<ID3D11Texture2D> tex;
    Com<ID3D11ShaderResourceView> srv;
    int w = 0, h = 0;
};
std::unordered_map<uint32_t, Capture> g_captures;

/* PC: the high-resolution images (registry_hires_readback): the native
 * pixels of a readback, by the content of its GS pixels (hires_key); at most
 * kHiresMax, the least recently used dropped. */
struct Hires {
    Com<ID3D11Texture2D> tex;
    Com<ID3D11ShaderResourceView> srv;
    int w = 0, h = 0;                           /* its GS size */
    uint64_t used = 0;
};
constexpr size_t kHiresMax = 64;
std::unordered_map<uint64_t, Hires> g_hires;
uint64_t g_hiresClock = 0;

uint64_t hires_key(const void *pixels, int w, int h)
{
    return mix(hash_bytes(pixels, (size_t)w * (size_t)h * 4), (uint32_t)w | (uint64_t)(uint32_t)h << 32) | 1;
}

/* Debug aid: GT3PC_AFTERIMAGE_LOG=1 logs (first 400 lines, with the frame
 * number) every plane split and every PSMT8H / T4HL / T4HH texture with the
 * owner of its block's top byte. */
bool afterimage_log()
{
    static int state = -1;
    static int lines = 0;
    if (state < 0) {
        const char *e = getenv("GT3PC_AFTERIMAGE_LOG");
        state = (e != nullptr && atoi(e) > 0) ? 1 : 0;
        if (state)
            pclog::write("pgl: logging the afterimage planes and the top-byte textures (GT3PC_AFTERIMAGE_LOG,"
                         " 400 lines)");
    }
    return state == 1 && lines++ < 400;
}

const char *kind_name(ObjKind k)
{
    switch (k) {
    case OBJ_GROUP: return "a Tex1 set";
    case OBJ_IMAGE: return "an image upload";
    case OBJ_SURFACE: return "a PSMCT32 surface";
    case OBJ_TARGET: return "a render target";
    case OBJ_PLANE: return "a plane";
    }
    return "?";
}
std::vector<uint8_t> g_mask;                    /* mark_rect's scratch */

/* The GS CLUT buffer: one 1 KB store of 512 halfwords, as the GS has it - a
 * CT16 / CT16S entry e is halfword e, a CT32 entry e (0..255) has its low
 * half at halfword e and its high half at e + 256.  A load writes it in its
 * own CPSM (through a ClutBuffer per load; loads repeated within a frame -
 * every textured model part reloads its CLUT - come from a memo of what the
 * load wrote), a texture reads it in its TEX0's CPSM.  (Two arrays and the
 * last load's CPSM, as before, read a CT16 palette from the CT32 array once
 * a CT32 load had come between: the menu font's CT16 palette after a kanji
 * glyph's CT32 one - a car name went brown and blotchy after its "・",
 * user report.) */
struct Clut {
    uint16_t h[512] = {};
} g_clut;

struct ClutLoad {
    uint32_t cpsm = 0, base = 0, n = 0;
    uint64_t key = 0;                   /* its memo key (what it writes: g_clutRow) */
    uint32_t nRows = 0;                 /* the rows it writes and what it writes there (clut_rows_init) */
    uint8_t rowIdx[32];
    uint8_t rowSrc[32];                 /* where: entries 16 j .. 16 j + 15 (j << 1), their high halves (| 1) */
    uint64_t rowId[32];
    uint32_t raw[256];
};
/* PC (CPU time): the memo is keyed by what a load reads - CBP, CPSM, CSM,
 * CSA, T8 and the tags of its blocks (g_blockGen) - so it holds across
 * uploads elsewhere and across frames (at most 4096 loads, then emptied). */
std::unordered_map<uint64_t, ClutLoad> g_clutMemo;
/* PC (CPU time): the key of the last load (the same load again writes what
 * the buffer holds), and what each 16-halfword row of the buffer holds (the
 * key of the load that wrote it and its row there; 0 the zeros it starts
 * with): a load that would write what is there already is skipped (model
 * parts take turns with a few CLUTs), and the palettes texture_bind reads
 * are kept by their rows (g_palMemo). */
uint64_t g_clutLastKey;
bool g_clutLastValid;
uint64_t g_clutRow[32];
/* PC (CPU time): a load only notes, row by row, which load's entries the
 * row holds now (g_clutRowSrc, the row's bit in g_clutDirty); the halfwords
 * are copied into g_clut when they are read (clut_lookup: a palette worked
 * out, rare) - parts taking turns with a few CLUTs copied all 256 entries
 * at nearly every load (CLUT LOAD 0.56 ms with its key checks cut short). */
struct ClutRowSrc {
    const ClutLoad *load = nullptr;
    uint8_t src = 0;
};
ClutRowSrc g_clutRowSrc[32];
uint32_t g_clutDirty;
/* The palettes texture_bind read, by their rows' contents, entries, CSA,
 * CPSM and TEXA (direct-mapped) */
struct PalMemo {
    uint64_t key = 0;
    uint64_t hash = 0;
    bool valid = false;
    uint32_t pal[256];
};
constexpr uint32_t kPalMemos = 64;
PalMemo g_palMemo[kPalMemos];
/* PC (CPU time): the loads by their TEX0 fields (CBP, CPSM, CSM, CSA, T8),
 * direct-mapped, with the CLUT's blocks (first, count): checked by its key
 * worked out again from those blocks' tags and CBP's owner (clut_key: a few
 * multiplies; the VRAM's other regions change all the time - the wheel
 * faces, the course's sets streamed in - and do not matter), while
 * g_tagEpoch stays. */
struct ClutFront {
    uint32_t fields = ~0u;
    uint64_t epoch = 0;
    uint32_t first = 0, count = 0;
    const ClutLoad *load = nullptr;
};
/* (2048: a race frame loads several hundred CLUTs, in the same order every
 * frame - 256 slots missed half of them, each evicting another) */
constexpr uint32_t kClutFronts = 2048;
ClutFront g_clutFront[kClutFronts];

/* (CLUT entries are CT32, CT16 or CT16S: the 16-bit ones by their CPSM.) */
inline bool clut_16bit(uint32_t cpsm)
{
    return cpsm == gstex::PSMCT16 || cpsm == gstex::PSMCT16S;
}

/* The rows of the buffer load l writes and what it writes there: CT32
 * entries are two rows (their low halves at row r, the high at r + 16). */
void clut_rows_init(ClutLoad &l)
{
    l.nRows = 0;
    const uint32_t rows = std::min<uint32_t>((l.n + 15) / 16, 16);
    for (uint32_t j = 0; j < rows; j++) {
        const uint64_t c = comb(l.key, j + 1) | 1;
        if (!clut_16bit(l.cpsm)) {
            const uint32_t r = (l.base / 16 + j) & 15;
            l.rowIdx[l.nRows] = (uint8_t)r;
            l.rowSrc[l.nRows] = (uint8_t)(j << 1);
            l.rowId[l.nRows++] = c;
            l.rowIdx[l.nRows] = (uint8_t)(r + 16);
            l.rowSrc[l.nRows] = (uint8_t)(j << 1 | 1);
            l.rowId[l.nRows++] = comb(c, 0x48) | 1;
        } else {
            l.rowIdx[l.nRows] = (uint8_t)((l.base / 16 + j) & 31);
            l.rowSrc[l.nRows] = (uint8_t)(j << 1);
            l.rowId[l.nRows++] = c;
        }
    }
}

void clut_apply(const ClutLoad &l)
{
    bool same = true;
    for (uint32_t i = 0; i < l.nRows && same; i++)
        same = g_clutRow[l.rowIdx[i]] == l.rowId[i];
    if (same)
        return;                         /* PC: the buffer holds it already */
    for (uint32_t i = 0; i < l.nRows; i++) {
        const uint32_t r = l.rowIdx[i];
        g_clutRow[r] = l.rowId[i];
        g_clutRowSrc[r].load = &l;
        g_clutRowSrc[r].src = l.rowSrc[i];
        g_clutDirty |= 1u << r;
    }
}

/* The rows noted since into g_clut: row r is halfwords 16 r .. 16 r + 15,
 * entries 16 j .. of its load (their low or high halves). */
void clut_materialize()
{
    for (uint32_t r = 0; g_clutDirty != 0; r++) {
        if (!(g_clutDirty & (1u << r)))
            continue;
        g_clutDirty &= ~(1u << r);
        const ClutRowSrc &s = g_clutRowSrc[r];
        const uint32_t *raw = s.load->raw + (s.src >> 1) * 16;
        uint16_t *h = &g_clut.h[r * 16];
        if (s.src & 1) {
            for (int k = 0; k < 16; k++)
                h[k] = (uint16_t)(raw[k] >> 16);
        } else {
            for (int k = 0; k < 16; k++)
                h[k] = (uint16_t)raw[k];
        }
    }
}

/* PC: what the n entries a texture reads at CSA in cpsm hold (the rows'
 * contents; TEXA expands CT16 entries). */
uint64_t palette_key(uint32_t n, bool ct16, uint32_t csa, uint64_t texaKey)
{
    uint64_t k = comb(comb(0x3C6EF372FE94F82Bull, n | (uint64_t)ct16 << 16 | (uint64_t)csa << 24), texaKey);
    const uint32_t rows = (n + 15) / 16;
    for (uint32_t j = 0; j < rows; j++) {
        if (!ct16) {
            const uint32_t r = (csa + j) & 15;
            k = comb(comb(k, g_clutRow[r]), g_clutRow[r + 16]);
        } else {
            k = comb(k, g_clutRow[(csa + j) & 31]);
        }
    }
    return k;
}

/* Index i of a psm texture with CLUT offset csa read in cpsm (the texture's
 * TEX0.CPSM). */
uint32_t clut_lookup(uint32_t psm, uint32_t cpsm, uint32_t csa, uint32_t i, const gstex::Texa &texa)
{
    if (g_clutDirty != 0)
        clut_materialize();             /* PC: the rows the loads noted */
    const bool t8 = psm == gstex::PSMT8 || psm == gstex::PSMT8H;
    const uint32_t e = csa * 16 + (t8 ? (i & 255) : (i & 15));
    if (!clut_16bit(cpsm))
        return (uint32_t)g_clut.h[e & 255] | (uint32_t)g_clut.h[(e & 255) + 256] << 16;
    return gstex::expand_ct16(g_clut.h[e & 511], texa);
}

/* The indices decode_rect returns without a CLUT (index x 0x01010101)
 * through the palette. */
void apply_palette(std::vector<uint32_t> &px, const uint32_t *palette, uint32_t n)
{
    for (uint32_t &p : px)
        p = palette[(p & 0xFF) % n];
}

struct CachedTex {
    Com<ID3D11Texture2D> tex;
    Com<ID3D11ShaderResourceView> srv;
    gstex::Rect rect;
    uint64_t lastFrame = 0;
    double lastUse = 0.0;       /* PC: g_execSeconds when last bound */
    uint32_t mean = 0;          /* the texels' mean (TexBinding::mean: GT3PC_PGL_TRACE) */
    bool pre = false;           /* PC: decoded ahead (pgl_prewarm.cpp), not bound yet */
};
/* PC: the decoded textures are kept while they fit in kTexBudget bytes (the
 * least recently bound go first beyond it, never one bound in the last
 * kTexKeep seconds) or until unused for kTexExpire seconds: a course's
 * textures stay decoded from lap to lap (at 300 frames unused they went in
 * about a second at a high frame rate, decoded again every lap). */
constexpr size_t kTexBudget = 512u << 20;
constexpr double kTexKeep = 5.0, kTexExpire = 600.0;
size_t g_texBytes;

/* The mean of decoded texels (R | G << 8 | B << 16 | A << 24). */
uint32_t texel_mean(const std::vector<uint32_t> &px)
{
    if (px.empty())
        return 0;
    uint64_t s[4] = {0, 0, 0, 0};
    for (uint32_t p : px)
        for (int c = 0; c < 4; c++)
            s[c] += (p >> (8 * c)) & 0xFF;
    uint32_t m = 0;
    for (int c = 0; c < 4; c++)
        m |= (uint32_t)((s[c] + px.size() / 2) / px.size()) << (8 * c);
    return m;
}
std::unordered_map<uint64_t, CachedTex> g_cache;
uint64_t g_registryFrame = 0;

/* PC (CPU time): obj's lookups, direct-mapped by id (a slot cleared as its
 * object goes: obj_forget) */
struct ObjSlot {
    uint32_t id = 0;
    Obj *o = nullptr;
};
ObjSlot g_objSlot[256];

Obj *obj(uint32_t id)
{
    if (id == 0)
        return nullptr;
    ObjSlot &s = g_objSlot[id & 255];
    if (s.id == id)
        return s.o;
    auto it = g_objs.find(id);
    Obj *o = it == g_objs.end() ? nullptr : it->second.get();
    if (o != nullptr) {
        s.id = id;
        s.o = o;
    }
    return o;
}

void obj_forget(uint32_t id)
{
    ObjSlot &s = g_objSlot[id & 255];
    if (s.id == id)
        s = ObjSlot();
}

Obj *new_obj(ObjKind kind)
{
    std::unique_ptr<Obj> o(new Obj());
    o->id = g_nextId++;
    if (g_nextId == 0)
        g_nextId = 1;
    o->kind = kind;
    Obj *p = o.get();
    g_objs[p->id] = std::move(o);
    return p;
}

BlockRange shifted(const BlockRange &r, uint32_t d)
{
    BlockRange s = r;
    if (!r.empty()) {
        s.first = r.first + d;
        s.last = r.last + d;
    }
    return s;
}

/* ---- decoded-texture keys (texture_bind; the textures decoded ahead) ---- */

/* The formats whose texels TEXA expands (CT24, CT16 and their Z forms):
 * the others decode the same under any TEXA (an indexed one's palette has
 * it in its content). */
bool texa_used(uint32_t psm)
{
    return psm == gstex::PSMCT24 || psm == gstex::PSMZ24 || psm == gstex::PSMCT16 || psm == gstex::PSMCT16S ||
           psm == gstex::PSMZ16 || psm == gstex::PSMZ16S;
}

/* What a decoded texture is of: PSM, TW, TH, TBW, the rectangle, the
 * palette's content, TEXA (PC: where it is used - a set's textures decoded
 * ahead are known without it), CSA. */
uint64_t tex_key(const gstex::Tex0 &t, const gstex::Rect &rect, uint64_t palHash, const gstex::Texa &ta)
{
    uint64_t key = mix(mix(0x7E, t.psm | (uint64_t)t.tw << 8 | (uint64_t)t.th << 12 | (uint64_t)t.tbw << 16),
                       rect.x | (uint64_t)rect.y << 16 | (uint64_t)rect.w << 32 | (uint64_t)rect.h << 48);
    const uint64_t texaKey = texa_used(t.psm) ? (ta.ta0 | ta.aem << 8 | (uint64_t)ta.ta1 << 16) : 0;
    return mix(mix(key, palHash), texaKey | (uint64_t)t.csa << 24);
}

/* ... read from a Tex1 set: the set by its image and load stamp (PC: not
 * the object's id - a set dropped and met again finds its decoded
 * textures), TBP0's place in it (blocks from its base) and the uploads under
 * the footprint brL (laid out at the set's base) by their data's hashes: the
 * small ones (animated CLUT data, small textures), PC: and the big ones'
 * fingerprints (texels written again in place, docs/renderer.md 6). */
uint64_t group_key(uint64_t key, const void *img, uint64_t stamp, uint32_t offset,
                   const std::vector<GroupUpload> &uploads, const BlockRange &brL)
{
    key = mix(mix(key, 0x6), mix((uint64_t)(uintptr_t)img, stamp));
    key = mix(key, offset & BLOCK_MASK);
    for (const GroupUpload &gu : uploads)
        if (gu.range.overlaps(brL) || gu.range.overlaps(shifted(brL, gstex::GS_MEM_BLOCKS)) ||
            shifted(gu.range, gstex::GS_MEM_BLOCKS).overlaps(brL))
            key = mix(key, gu.hash);
    return key;
}

/* An upload's data hash: a small one's whole, a big one's fingerprint. */
uint64_t upload_hash(const gstex::Tex1Upload &u)
{
    if (u.data == nullptr)
        return 0;
    return u.bytes <= 4096 ? hash_bytes(u.data, u.bytes) : sample_hash(u.data, u.bytes);
}

/* A set's uploads as GroupUploads (their blocks; their data hashes). */
void uploads_of(const gstex::Tex1Set &set, std::vector<GroupUpload> &out)
{
    out.clear();
    for (const gstex::Tex1Upload &u : set.uploads) {
        GroupUpload gu;
        gu.range = gstex::image_blocks(u.t.dpsm, u.t.dbp, u.t.dbw, 0, 0, u.t.w, u.t.h);
        gu.isSmall = u.bytes <= 4096;
        gu.hash = upload_hash(u);
        out.push_back(gu);
    }
}

/* Targets whose blocks another object wrote must re-mark at their next draw. */
void touch_targets(const BlockRange &r, uint32_t writer)
{
    for (uint32_t id : g_targetObjs) {
        if (id == writer)
            continue;
        Obj *o = obj(id);
        if (o == nullptr)
            continue;
        /* ranges may pass 0x3FFF: compare both wrapped forms */
        BlockRange a = o->targetRange, b = r;
        a.first &= BLOCK_MASK;
        a.last = a.first + (o->targetRange.last - o->targetRange.first);
        b.first &= BLOCK_MASK;
        b.last = b.first + (r.last - r.first);
        if (a.overlaps(b) || shifted(a, gstex::GS_MEM_BLOCKS).overlaps(b) ||
            a.overlaps(shifted(b, gstex::GS_MEM_BLOCKS))) {
            o->needRemark[0] = true;
            o->needRemark[1] = true;
        }
    }
}

/* Calls fn(block) for every block the rectangle touches: one block_number
 * per block cell of the format's block grid (a block holds one blockW x
 * blockH rectangle of a page), not per pixel. */
template <class Fn> void for_each_block(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t w,
                                        uint32_t h, Fn fn)
{
    if (w == 0 || h == 0)
        return;
    const gstex::PsmGeom g = gstex::psm_geom(psm);
    const uint32_t bx0 = x / g.blockW, bx1 = (x + w - 1) / g.blockW;
    const uint32_t by0 = y / g.blockH, by1 = (y + h - 1) / g.blockH;
    for (uint32_t by = by0; by <= by1; by++)
        for (uint32_t bx = bx0; bx <= bx1; bx++)
            fn(gstex::block_number(psm, bp, bw, bx * g.blockW, by * g.blockH) & BLOCK_MASK);
}

/* Marks the exact block set of a rectangle for o. */
void mark_rect(Obj *o, uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
               uint8_t layers, std::vector<std::pair<uint32_t, uint8_t>> *record)
{
    BlockRange r = gstex::image_blocks(psm, bp, bw, x, y, w, h);
    if (r.empty())
        return;
    const uint32_t id = o->id;
    /* (the tag: an image's bytes never change, a target's blocks read as
     * zeros wherever a texture or CLUT is composed: their tags stay the
     * same each time they mark the same blocks; surfaces, planes: new) */
    uint64_t gen;
    if (o->kind == OBJ_IMAGE)
        gen = (comb(comb(0x510E527FADE682D1ull, o->key), id) & ~(3ull << 62)) | (1ull << 62);
    else if (o->kind == OBJ_TARGET)
        gen = (comb(0x9B05688C2B3E6C1Full, id) & ~(7ull << 61)) | (1ull << 61);
    else
        gen = ++g_markGen;
    uint64_t changed = 0;                   /* (the regions whose tags changed) */
    bool planeLost = false;
    uint32_t lostBlock = 0;
    for_each_block(psm, bp, bw, x, y, w, h, [&](uint32_t b) {
        if (layers & LAYER_RGB) {
            g_own[0][b] = id;
            if (g_blockGen[0][b] != gen) {
                g_blockGen[0][b] = gen;
                changed |= 1ull << (b >> kRegionShift);
            }
        }
        if (layers & LAYER_TOP) {
            if (g_blockGen[1][b] != gen) {
                g_blockGen[1][b] = gen;
                changed |= 1ull << (b >> kRegionShift);
            }
            /* (GT3PC_AFTERIMAGE_LOG: a target taking an afterimage plane's
             * bytes before drawAfterimage read them) */
            if (!planeLost && o->kind == OBJ_TARGET && g_own[1][b] != id) {
                const Obj *prev = obj(g_own[1][b]);
                if (prev != nullptr && prev->kind == OBJ_PLANE) {
                    planeLost = true;
                    lostBlock = prev->pbp;
                }
            }
            g_own[1][b] = id;
        }
        if (record != nullptr)
            record->push_back({b, layers});
    });
    regions_changed(changed);
    if (planeLost && o->target != nullptr && afterimage_log())
        pclog::write("pgl afterimage: frame %llu: the plane at block 0x%X loses its bytes to a write of the target at"
                     " block 0x%X (created as psm 0x%02X, GS %dx%d, FBW %u)",
                     (unsigned long long)frame_number(), lostBlock, o->target->base, o->target->psm,
                     o->target->gsW, o->target->gsH, o->target->fbw);
    touch_targets(r, o->id);
}

/* ---- groups ------------------------------------------------------------ */

void group_prepare_uploads(Obj *g)
{
    g->marks.clear();
    uploads_of(g->set, g->uploads);
}

void group_layout(Obj *g)
{
    if (g->laidOut)
        return;
    gstex::tex1_layout(g->set, g->space);
    g->laidOut = true;
}

void group_mark(Obj *g)
{
    if (g->marks.empty()) {
        for (const gstex::Tex1Upload &u : g->set.uploads) {
            /* the marks at layoutBase, recorded, then applied at curBase */
            const uint8_t layers = layers_of(u.t.dpsm);
            for_each_block(u.t.dpsm, u.t.dbp, u.t.dbw, 0, 0, u.t.w, u.t.h,
                           [&](uint32_t b) { g->marks.push_back({b, layers}); });
        }
    }
    const uint32_t d = g->delta();
    const uint16_t base = (uint16_t)(g->curBase & BLOCK_MASK);
    /* (the tag: bit 63 a set's, 62 an image's, 61 a target's, else a
     * g_markGen) */
    const uint64_t gen = comb(comb(comb(0x6A09E667F3BCC908ull, g->id), base), g->contentVer) | (1ull << 63);
    uint64_t changed = 0;                   /* (the regions whose tags changed) */
    for (const auto &m : g->marks) {
        const uint32_t b = (m.first + d) & BLOCK_MASK;
        if (m.second & LAYER_RGB) {
            g_own[0][b] = g->id;
            g_ownBase[0][b] = base;
            if (g_blockGen[0][b] != gen) {
                g_blockGen[0][b] = gen;
                changed |= 1ull << (b >> kRegionShift);
            }
        }
        if (m.second & LAYER_TOP) {
            g_own[1][b] = g->id;
            g_ownBase[1][b] = base;
            if (g_blockGen[1][b] != gen) {
                g_blockGen[1][b] = gen;
                changed |= 1ull << (b >> kRegionShift);
            }
        }
    }
    regions_changed(changed);
    for (const GroupUpload &gu : g->uploads)
        touch_targets(shifted(gu.range, d), g->id);
    g->serial = ++g_serial;
    g->lastFrame = g_registryFrame;
    g->lastUse = g_execSeconds;
}

/* The delta (base - layoutBase) of the copy of set g that block `block' of
 * layer `layer' holds (g owns the block). */
uint32_t group_delta_at(const Obj *g, int layer, uint32_t block)
{
    return ((uint32_t)g_ownBase[layer][block & BLOCK_MASK] - g->layoutBase) & BLOCK_MASK;
}

/* The set at img is the one g holds (moved at most): no parse needed. */
bool group_same(const Obj *g, const RawTex1 &h, const RawXfer *x)
{
    const RawTex1 &o = g->rawHdr;
    if (h.self != o.self || h.ptr08 != o.ptr08 || h.size != o.size || h.vramSize != o.vramSize ||
        h.nTex != o.nTex || h.nXfer != o.nXfer || h.tex != o.tex || h.xfer != o.xfer ||
        h.ptrTable != o.ptrTable || h.clutAnim != o.clutAnim || g->rawXfer.size() != h.nXfer)
        return false;
    for (uint32_t i = 0; i < h.nXfer; i++) {
        const RawXfer &a = x[i], &b = g->rawXfer[i];
        if (a.data != b.data || a.dbw != b.dbw || a.dpsm != b.dpsm || a.w != b.w || a.h != b.h ||
            (uint16_t)(a.dbp - h.vramBase) != (uint16_t)(b.dbp - o.vramBase))
            return false;
    }
    return true;
}

void group_remember(Obj *g, const RawTex1 &h, const RawXfer *x)
{
    g->rawHdr = h;
    g->rawXfer.assign(x, x + h.nXfer);
}

/* ---- surfaces ---------------------------------------------------------- */

void surface_upload(Obj *s, int dx, int dy, int w, int h, const uint8_t *src, size_t bytes)
{
    const int needW = std::max({s->sw, (int)s->sdbw * 64, dx + w});
    const int needH = std::max(s->sh, dy + h);
    if (needW != s->sw || needH != s->sh) {
        std::vector<uint32_t> px((size_t)needW * needH, 0);
        for (int y = 0; y < s->sh; y++)
            memcpy(&px[(size_t)y * needW], &s->spx[(size_t)y * s->sw], (size_t)s->sw * 4);
        s->spx.swap(px);
        s->sw = needW;
        s->sh = needH;
        s->dx0 = 0;
        s->dy0 = 0;
        s->dx1 = needW;
        s->dy1 = needH;
    }
    const size_t rowBytes = (size_t)w * 4;
    for (int y = 0; y < h; y++) {
        if ((size_t)(y + 1) * rowBytes > bytes)
            break;
        memcpy(&s->spx[(size_t)(dy + y) * s->sw + dx], src + (size_t)y * rowBytes, rowBytes);
    }
    if (s->dx1 <= s->dx0 || s->dy1 <= s->dy0) {
        s->dx0 = dx;
        s->dy0 = dy;
        s->dx1 = dx + w;
        s->dy1 = dy + h;
    } else {
        s->dx0 = std::min(s->dx0, dx);
        s->dy0 = std::min(s->dy0, dy);
        s->dx1 = std::max(s->dx1, dx + w);
        s->dy1 = std::max(s->dy1, dy + h);
    }
}

/* The surface's GPU texture, brought up to date. */
ID3D11ShaderResourceView *surface_srv(Obj *s)
{
    if (!s->stex || s->texW != s->sw || s->texH != s->sh) {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = (UINT)s->sw;
        d.Height = (UINT)s->sh;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init = {};
        init.pSysMem = s->spx.data();
        init.SysMemPitch = (UINT)s->sw * 4;
        s->stex.reset();
        s->ssrv.reset();
        if (FAILED(dev()->CreateTexture2D(&d, &init, s->stex.put())) ||
            FAILED(dev()->CreateShaderResourceView(s->stex.get(), nullptr, s->ssrv.put())))
            return nullptr;
        s->texW = s->sw;
        s->texH = s->sh;
        s->dx1 = s->dx0;
        return s->ssrv.get();
    }
    if (s->dx1 > s->dx0 && s->dy1 > s->dy0) {
        /* (queued: the D3D thread, the rows written copied along) */
        const D3D11_BOX box = {(UINT)s->dx0, (UINT)s->dy0, 0, (UINT)s->dx1, (UINT)s->dy1, 1};
        const UINT rowBytes = (box.right - box.left) * 4, rows = box.bottom - box.top;
        static std::vector<uint8_t> packed;
        packed.resize((size_t)rowBytes * rows);
        for (UINT y = 0; y < rows; y++)
            memcpy(&packed[(size_t)y * rowBytes], &s->spx[(size_t)(s->dy0 + (int)y) * s->sw + s->dx0], rowBytes);
        ID3D11Texture2D *tex = s->stex.get();
        q::run_with(packed.data(), packed.size(), [tex, box, rowBytes](const uint8_t *p) {
            qdc()->UpdateSubresource(tex, 0, &box, p, rowBytes, 0);
        });
        s->dx1 = s->dx0;
    }
    return s->ssrv.get();
}

/* A space of the block range holding what the owners wrote there, in the
 * order they wrote it (the composition path). */
void compose_space(const BlockRange &r, gstex::TexSpace &space, uint64_t *key)
{
    g_profCount[PCOUNT_COMPOSES]++;         /* (the verbose monitor's profile) */
    const uint32_t count = std::min<uint32_t>(r.count(), gstex::GS_MEM_BLOCKS);
    space.reset(r.first & BLOCK_MASK, count);
    std::vector<Obj *> owners;
    std::set<uint32_t> seen;
    std::unordered_map<uint32_t, uint32_t> groupDelta;  /* a set's copy here (the first block it owns) */
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t b = (r.first + i) & BLOCK_MASK;
        for (int l = 0; l < 2; l++) {
            const uint32_t id = g_own[l][b];
            if (id != 0 && seen.insert(id).second) {
                Obj *o = obj(id);
                if (o != nullptr) {
                    owners.push_back(o);
                    if (o->kind == OBJ_GROUP)
                        groupDelta[id] = group_delta_at(o, l, b);
                }
            }
        }
    }
    std::sort(owners.begin(), owners.end(), [](const Obj *a, const Obj *b) { return a->serial < b->serial; });
    uint64_t k = 0xC0;
    for (Obj *o : owners) {
        k = mix(mix(k, o->id), o->serial);
        if (o->kind == OBJ_GROUP) {
            const uint32_t d = groupDelta[o->id];
            k = mix(k, d);
            for (size_t i = 0; i < o->set.uploads.size(); i++) {
                const gstex::Tex1Upload &u = o->set.uploads[i];
                if (u.data == nullptr)
                    continue;
                if (i < o->uploads.size())
                    k = mix(k, o->uploads[i].hash);  /* PC: the big ones' fingerprints too */
                gstex::Transfer t = u.t;
                t.dbp = (t.dbp + d) & BLOCK_MASK;
                space.upload(t, u.data, u.bytes);
            }
        } else if (o->kind == OBJ_IMAGE) {
            space.upload(o->t, o->data.data(), o->data.size());
        } else if (o->kind == OBJ_SURFACE) {
            gstex::Transfer t;
            t.dbp = o->sdbp;
            t.dbw = o->sdbw;
            t.dpsm = gstex::PSMCT32;
            t.w = (uint32_t)o->sw;
            t.h = (uint32_t)o->sh;
            space.upload(t, o->spx.data(), o->spx.size() * 4);
            k = mix(k, hash_bytes(o->spx.data(), o->spx.size() * 4));
        } else {
            log_once("pgl-compose-target", "pgl: a texture mixes uploads and render-target (or afterimage plane)"
                     " blocks: the target part reads as zeros");
        }
    }
    if (key != nullptr)
        *key = k;
}

/* Where texel (0, 0) of a texture at tbp lies in a target (pixels). */
bool target_offset(const Target &tg, uint32_t tbp, int &ox, int &oy)
{
    if (tbp == tg.base) {
        ox = oy = 0;
        return true;
    }
    static std::unordered_map<uint64_t, std::pair<int, int>> cache;
    /* (gsH too: a depth target's GS geometry follows the frame that wrote it) */
    const uint64_t key = ((uint64_t)tg.base << 40) ^ ((uint64_t)tg.fbw << 32) ^ ((uint64_t)tg.psm << 24) ^
                         ((uint64_t)tg.gsW << 14) ^ ((uint64_t)tg.gsH << 54) ^ tbp;
    auto it = cache.find(key);
    if (it != cache.end()) {
        ox = it->second.first;
        oy = it->second.second;
        return ox >= 0;
    }
    const uint32_t psm = tg.depth ? (tg.psm & 0x3F) : (tg.psm & 0x0F);
    gstex::PsmGeom g = gstex::psm_geom(psm);
    ox = oy = -1;
    const uint32_t fbw = tg.fbw ? tg.fbw : (uint32_t)((tg.gsW + 63) / 64);
    for (int y = 0; y < tg.gsH && ox < 0; y += (int)g.blockH) {
        for (int x = 0; x < tg.gsW; x += (int)g.blockW) {
            if ((gstex::block_number(psm, tg.base, fbw, (uint32_t)x, (uint32_t)y) & BLOCK_MASK) == tbp) {
                ox = x;
                oy = y;
                break;
            }
        }
    }
    cache[key] = {ox, oy};
    return ox >= 0;
}

/* ---- planes ------------------------------------------------------------ */

/* A plane read as a texture of psm (PSMT8H / T4HL / T4HH) through palette
 * (n entries, CT32 words as clut_lookup gives them): the plane's bytes are
 * the indices, the texture holds the palette's colours at the capture's
 * native size.  A full-screen pass (ps_plane) from the capture into the
 * plane's texture, done again when the palette, the PSM or the capture
 * changed.  The renderer's bound state is forgotten (d3d_invalidate): the
 * caller's draw binds its own. */
ID3D11ShaderResourceView *plane_texture(Obj *o, uint32_t psm, const uint32_t *palette, uint32_t n)
{
    const uint64_t key = mix(mix(hash_bytes(palette, (size_t)n * 4), psm), o->serial);
    if (o->pdecSrv && o->pdecKey == key)
        return o->pdecSrv.get();
    D3D11_TEXTURE2D_DESC have = {};
    if (o->pdec)
        o->pdec->GetDesc(&have);
    if (!o->pdec || have.Width != (UINT)o->capW || have.Height != (UINT)o->capH) {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = (UINT)o->capW;
        d.Height = (UINT)o->capH;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        o->pdecSrv.reset();
        o->pdecRtv.reset();
        if (FAILED(dev()->CreateTexture2D(&d, nullptr, o->pdec.put())) ||
            FAILED(dev()->CreateRenderTargetView(o->pdec.get(), nullptr, o->pdecRtv.put())) ||
            FAILED(dev()->CreateShaderResourceView(o->pdec.get(), nullptr, o->pdecSrv.put()))) {
            o->pdec.reset();
            o->pdecRtv.reset();
            o->pdecSrv.reset();
            log_once("pgl-plane-create", "pgl: could not create a %dx%d plane texture", o->capW, o->capH);
            return nullptr;
        }
    }
    Com<ID3D11Texture2D> ptex;
    Com<ID3D11ShaderResourceView> psrv;
    if (!make_texture((int)n, 1, palette, ptex, psrv))
        return nullptr;
    /* (queued: the D3D thread, pgl_queue.h; the state objects found here) */
    d3d_invalidate();
    ID3D11RenderTargetView *rtv = o->pdecRtv.get();
    const D3D11_VIEWPORT vp = {0, 0, (float)o->capW, (float)o->capH, 0, 1};
    ID3D11RasterizerState *rs = raster_state(0, false);
    ID3D11BlendState *bs = blend_plain(pgl::gs::WRITE_RGBA);
    ID3D11DepthStencilState *ds = depth_state(false, pgl::gs::Compare::Always, false);
    q::run([rtv, vp, rs, bs, ds] {
        ID3D11ShaderResourceView *none[2] = {nullptr, nullptr};
        qdc()->PSSetShaderResources(0, 2, none);
        qdc()->OMSetRenderTargets(1, &rtv, nullptr);
        qdc()->RSSetViewports(1, &vp);
        qdc()->RSSetState(rs);
        qdc()->OMSetBlendState(bs, nullptr, 0xFFFFFFFFu);
        qdc()->OMSetDepthStencilState(ds, 0);
    });
    bind_vs(Vs::Full);
    bind_ps(Ps::Plane);
    CbPlane cb = {};
    cb.channel = o->pch;
    cb.shift = psm == gstex::PSMT4HH ? 4 : 0;
    cb.mask = psm == gstex::PSMT8H ? 0xFF : 0xF;
    bind_ps_cb(&cb, sizeof(cb));
    ID3D11ShaderResourceView *srv0 = o->pcap.get(), *srv1 = psrv.get();
    q::run([srv0, srv1] {
        ID3D11ShaderResourceView *srvs[2] = {srv0, srv1};
        ID3D11ShaderResourceView *none[2] = {nullptr, nullptr};
        qdc()->PSSetShaderResources(0, 2, srvs);
        qdc()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        qdc()->Draw(3, 0);
        qdc()->PSSetShaderResources(0, 2, none);
        qdc()->OMSetRenderTargets(0, nullptr, nullptr);
    });
    d3d_invalidate();
    o->pdecKey = key;
    return o->pdecSrv.get();
}

}  /* namespace */

namespace {

/* ---- decoded ahead (pgl_prewarm.cpp) ------------------------------------- */

/* The textures the worker decoded, under their keys, until the executor
 * puts them in the cache (tex_pre_drain: at a texture's miss, at a frame's
 * end).  (Never freed, the lock has no destructor: the worker may still run
 * while the process exits.) */
struct PreTex {
    uint64_t key = 0;
    CachedTex ct;
};
SRWLOCK g_preTexLock = SRWLOCK_INIT;
std::vector<PreTex> *g_preTex = new std::vector<PreTex>();
std::atomic<uint32_t> g_preTexCount{0};
/* TEXA of the last texture bound from a set (the CT16 palettes and texels
 * of the sets loaded next are expanded with it; the PS2's reset value until
 * then) */
std::atomic<uint64_t> g_preTexa{0x80ull << 32};

/* A Tex1 set as the game loaded it - parsed, its uploads' bytes copied (on
 * the game thread: registry_tex1_loaded, before any link) - and its load
 * stamp.  Each texture is decoded as texture_bind decodes it from the set
 * at a draw (its CLUT loaded the way pgl loads it, pgl_clut_load: the
 * palette that draw reads), under the key it works out there. */
struct TexJob : pre::Job {
    const void *img = nullptr;
    uint64_t stamp = 0;
    gstex::Tex1Set set;
    std::vector<uint8_t> bytes;
    gstex::Texa texa;
    void run() override;
};

/* (the worker) */
void TexJob::run()
{
    gstex::TexSpace space;
    gstex::tex1_layout(set, space);
    std::vector<GroupUpload> uploads;
    uploads_of(set, uploads);
    const uint32_t base = set.vramBase & BLOCK_MASK;
    std::vector<PreTex> out;
    size_t made = 0;
    for (const gstex::TexRegs &regs : set.tex) {
        const gstex::Tex0 t = gstex::Tex0::decode(regs.tex0);
        const gstex::Clamp c = gstex::Clamp::decode(regs.clamp);
        if (!gstex::psm_valid(t.psm))
            continue;
        const uint32_t tbp = t.tbp0 & BLOCK_MASK;
        const bool indexed = gstex::psm_indexed(t.psm);
        const uint32_t palN = (t.psm == gstex::PSMT8 || t.psm == gstex::PSMT8H) ? 256 : 16;
        uint32_t pal[256];
        uint64_t palHash = 0;
        if (indexed) {
            /* (what clut_lookup gives after registry_clut_load of that
             * TEX2: the CT32 entries as they are, CT16 ones by TEXA) */
            gstex::ClutBuffer cb;
            cb.load(space, gstex::pgl_clut_load(t));
            const bool ct16 = clut_16bit(t.cpsm);
            for (uint32_t i = 0; i < palN; i++) {
                const uint32_t raw = cb.lookup_raw(t.psm, t.csa, i);
                pal[i] = ct16 ? gstex::expand_ct16(raw & 0xFFFF, texa) : raw;
            }
            palHash = hash_bytes(pal, palN * 4);
        }
        const gstex::Rect rect = gstex::decode_region(t, c);
        const BlockRange br = gstex::image_blocks(t.psm, tbp, t.tbw, rect.x, rect.y, rect.w, rect.h);
        const uint64_t key = group_key(tex_key(t, rect, palHash, texa), img, stamp, tbp - base, uploads, br);
        bool dup = false;
        for (const PreTex &p : out)
            dup = dup || p.key == key;
        if (dup)
            continue;
        std::vector<uint32_t> px((size_t)rect.w * rect.h, 0);
        gstex::decode_rect(space, t.psm, tbp, t.tbw, rect.x, rect.y, rect.w, rect.h, nullptr, t.csa, texa,
                           px.data(), rect.w);
        if (indexed)
            apply_palette(px, pal, palN);
        PreTex p;
        p.key = key;
        p.ct.rect = rect;
        p.ct.mean = texel_mean(px);
        p.ct.pre = true;
        if (!make_texture((int)rect.w, (int)rect.h, px.data(), p.ct.tex, p.ct.srv))
            continue;
        made += px.size() * 4;
        out.push_back(std::move(p));
    }
    pre::made(pre::TEXTURE, (uint32_t)out.size(), made);
    if (out.empty())
        return;
    AcquireSRWLockExclusive(&g_preTexLock);
    for (PreTex &p : out)
        g_preTex->push_back(std::move(p));
    g_preTexCount.store((uint32_t)g_preTex->size(), std::memory_order_release);
    ReleaseSRWLockExclusive(&g_preTexLock);
}

/* The textures decoded ahead into the cache (the executor's thread; one
 * decoded at a draw meanwhile keeps its own); false: none were waiting. */
bool tex_pre_drain()
{
    if (g_preTexCount.load(std::memory_order_acquire) == 0)
        return false;
    std::vector<PreTex> ready;
    AcquireSRWLockExclusive(&g_preTexLock);
    ready.swap(*g_preTex);
    g_preTexCount.store(0, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&g_preTexLock);
    for (PreTex &p : ready) {
        if (g_cache.count(p.key) != 0)
            continue;
        p.ct.lastFrame = g_registryFrame;
        p.ct.lastUse = g_execSeconds;
        g_texBytes += (size_t)p.ct.rect.w * p.ct.rect.h * 4;
        g_cache.emplace(p.key, std::move(p.ct));
    }
    return true;
}

/* A set just loaded at img, its stamp given: its textures to the worker. */
void tex_prewarm(const void *img, uint64_t stamp)
{
    std::unique_ptr<TexJob> j(new TexJob());
    /* (a set without uploads of its own reads another's blocks: its
     * textures are that set's at a draw) */
    if (!gstex::tex1_from_memory(img, j->set) || j->set.tex.empty() || j->set.uploads.empty())
        return;
    size_t total = 0;
    for (const gstex::Tex1Upload &u : j->set.uploads)
        if (u.data != nullptr)
            total += (u.bytes + 15) & ~(size_t)15;
    j->bytes.resize(total);
    size_t at = 0;
    for (gstex::Tex1Upload &u : j->set.uploads) {
        if (u.data == nullptr)
            continue;
        memcpy(j->bytes.data() + at, u.data, u.bytes);
        u.data = j->bytes.data() + at;
        at += (u.bytes + 15) & ~(size_t)15;
    }
    j->set.anims.clear();                   /* (they point into the game's memory) */
    j->img = img;
    j->stamp = stamp;
    j->texa = gstex::Texa::decode(g_preTexa.load(std::memory_order_relaxed));
    pre::post(j.release());
}

}  /* namespace */

/* ======================================================================== */
/* Registration                                                             */
/* ======================================================================== */

void registry_init()
{
    memset(g_own, 0, sizeof(g_own));
    memset(g_ownBase, 0, sizeof(g_ownBase));
    g_tagEpoch++;
}

void registry_tex1_loaded(void *img)
{
    const uint64_t stamp = ++g_nextStamp;
    g_stamps[img] = stamp;
    if (pre::on())
        tex_prewarm(img, stamp);            /* PC: decoded ahead (pgl_prewarm.cpp) */
}

uint32_t registry_tex1_base(const void *img)
{
    return img != nullptr ? (uint32_t)static_cast<const RawTex1 *>(img)->vramBase & BLOCK_MASK : 0;
}

/* PC: a big upload of a known set whose bytes changed without a new load
 * (group_for: decoded again).  Always logged, the first 40 times: the
 * evidence of a menu car's garbled reflection (docs/renderer.md 11). */
static void tex1_log_changed(const void *img, size_t upload, size_t bytes)
{
    static int lines = 0;
    if (lines >= 40)
        return;
    lines++;
    pclog::write("pgl tex1: frame %llu: set %p's upload %u (%u bytes) changed in place (no new load): decoded "
                 "again%s", (unsigned long long)frame_number(), img, (unsigned)upload, (unsigned)bytes,
                 lines == 40 ? " (no more of these lines)" : "");
}

/* Debug aid: GT3PC_TEX1_LOG=1 logs (first 4000 lines, with the frame number)
 * every Tex1 set uploaded at a second VRAM base within one frame (the
 * allocator evicted and reloaded it, or TexSet::load moved it): the draws
 * recorded before the move sample the earlier copy. */
static void tex1_log_move(const void *img, uint32_t id, uint32_t base, uint32_t blocks)
{
    static int state = -1;
    static int lines = 0;
    static std::unordered_map<uint32_t, std::pair<uint64_t, uint32_t>> last;    /* group: frame, base */
    if (state < 0) {
        const char *e = getenv("GT3PC_TEX1_LOG");
        state = (e != nullptr && atoi(e) > 0) ? 1 : 0;
        if (state)
            pclog::write("pgl: logging Tex1 sets uploaded at two VRAM bases in one frame (GT3PC_TEX1_LOG, 4000"
                         " lines)");
    }
    if (state != 1)
        return;
    auto it = last.find(id);
    if (it != last.end() && it->second.first == g_registryFrame && it->second.second != base && lines < 4000) {
        lines++;
        pclog::write("pgl tex1: frame %llu: set %p (%u blocks) uploaded at block 0x%X, earlier this frame at"
                     " 0x%X", (unsigned long long)frame_number(), img, blocks, base, it->second.second);
    }
    last[id] = {g_registryFrame, base};
}

namespace {

/* The group of the set at img placed at vramBase (masked): recognised by
 * its header and upload table, else parsed (laid out as linked now);
 * nullptr if it does not parse.  Nothing marked. */
Obj *group_for(const void *img, uint32_t vramBase)
{
    auto st = g_stamps.find(img);
    if (st == g_stamps.end())
        st = g_stamps.emplace(img, ++g_nextStamp).first;
    const uint64_t stamp = st->second;
    const RawTex1 &hdr = *static_cast<const RawTex1 *>(img);
    const RawXfer *xfer = reinterpret_cast<const RawXfer *>((uintptr_t)hdr.xfer);
    if (hdr.nXfer != 0 && xfer == nullptr)
        return nullptr;

    Obj *g = nullptr;
    auto gi = g_groupByImg.find(img);
    if (gi != g_groupByImg.end()) {
        g = obj(gi->second);
        if (g != nullptr && g->stamp != stamp)
            g = nullptr;
    }
    if (g != nullptr && group_same(g, hdr, xfer)) {
        /* the same set again (relinked elsewhere at most): its small
         * uploads may have changed (CLUT animations).  PC: and its big ones,
         * by their fingerprints: texels written again in place keep the
         * load stamp, and the textures decoded from the old bytes stayed
         * until the set was loaded again (user report: a menu car's
         * reflection garbled until the menu was entered again); the keys
         * follow the hashes (group_key) */
        g->curBase = vramBase;
        for (size_t i = 0; i < g->uploads.size() && i < g->set.uploads.size(); i++) {
            GroupUpload &gu = g->uploads[i];
            const gstex::Tex1Upload &u = g->set.uploads[i];
            if (u.data == nullptr)
                continue;
            const uint64_t h = upload_hash(u);
            if (h != gu.hash) {
                if (!gu.isSmall)
                    tex1_log_changed(img, i, u.bytes);
                gu.hash = h;
                g->contentVer++;
                if (g->laidOut)
                    g->space.upload(u.t, u.data, u.bytes);
            }
        }
        group_remember(g, hdr, xfer);
        return g;
    }

    gstex::Tex1Set set;
    if (!gstex::tex1_from_memory(img, set)) {
        log_once("pgl-tex1-parse", "pgl: a Tex1 set at %p does not parse: not uploaded", img);
        return nullptr;
    }
    if (g == nullptr) {
        g = new_obj(OBJ_GROUP);
        g->img = img;
        g->stamp = stamp;
        g_groupByImg[img] = g->id;
    }
    g->set = std::move(set);
    g->laidOut = false;
    g->contentVer++;
    /* laid out as linked now, placed where the upload was recorded */
    g->layoutBase = g->set.vramBase & BLOCK_MASK;
    g->curBase = vramBase;
    group_prepare_uploads(g);
    group_remember(g, hdr, xfer);
    return g;
}

/* PC (ROADMAP 18, no memory limits; registry_texture_source): the texture
 * last named as a VRAM-less set's - the set's group (never marked: it owns
 * no block), the base it is linked for, its TEX0 (CLD aside) and CLUT
 * fields. */
struct TexSource {
    Obj *obj = nullptr;
    uint32_t base = 0;
    uint64_t tex0 = 0;
    uint32_t cbp = 0, cpsm = 0, csm = 0;
};
TexSource g_texSrc;

}  /* namespace */

/* vramBase: where the set was linked when its upload was recorded
 * (pglr_tex1_upload); the header read here may already be linked for a
 * later copy of the frame.  The layout stays the one of the first parse,
 * curBase - layoutBase moves it. */
void registry_tex1_upload(const void *img, uint32_t vramBase)
{
    if (img == nullptr)
        return;
    vramBase &= BLOCK_MASK;
    Obj *g = group_for(img, vramBase);
    if (g == nullptr)
        return;
    tex1_log_move(img, g->id, vramBase, static_cast<const RawTex1 *>(img)->vramSize);
    group_mark(g);
}

void registry_texture_source(const void *img, uint32_t vramBase, uint64_t tex0)
{
    g_texSrc = TexSource();
    if (img == nullptr)
        return;
    Obj *g = group_for(img, vramBase & BLOCK_MASK);
    if (g == nullptr)
        return;
    g->lastFrame = g_registryFrame;
    g->lastUse = g_execSeconds;             /* (kept while drawn: it owns no block) */
    const gstex::Tex0 t = gstex::Tex0::decode(tex0);
    g_texSrc.obj = g;
    g_texSrc.base = vramBase & BLOCK_MASK;
    g_texSrc.tex0 = tex0 & ~(7ull << 61);
    g_texSrc.cbp = t.cbp & BLOCK_MASK;
    g_texSrc.cpsm = t.cpsm;
    g_texSrc.csm = t.csm;
}

/* The native pixels of src's GS rectangle (x, y, w, h), when it has more
 * than one per GS pixel, copied into an image of their own under the key of
 * the GS pixels read back (the least recently used dropped beyond
 * kHiresMax). */
void registry_hires_readback(Target &src, int x, int y, int w, int h, const void *pixels)
{
    if (src.depth || pixels == nullptr || w <= 0 || h <= 0 || w > 256 || h > 256)
        return;
    if (src.sx <= 1.01f && src.sy <= 1.01f)
        return;                             /* (GS resolution: nothing more to keep) */
    const int nx0 = std::max(0, (int)std::lround((float)x * src.sx));
    const int ny0 = std::max(0, (int)std::lround((float)y * src.sy));
    const int nx1 = std::min(src.nw, (int)std::lround((float)(x + w) * src.sx));
    const int ny1 = std::min(src.nh, (int)std::lround((float)(y + h) * src.sy));
    if (nx1 <= nx0 || ny1 <= ny0)
        return;
    ID3D11Texture2D *from = target_tex1(src);
    if (from == nullptr)
        return;
    const uint64_t key = hires_key(pixels, w, h);
    Hires &hr = g_hires[key];
    D3D11_TEXTURE2D_DESC have = {};
    if (hr.tex)
        hr.tex->GetDesc(&have);
    if (!hr.tex || hr.w != w || hr.h != h || have.Width != (UINT)(nx1 - nx0) || have.Height != (UINT)(ny1 - ny0)) {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = (UINT)(nx1 - nx0);
        d.Height = (UINT)(ny1 - ny0);
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        hr.srv.reset();
        hr.tex.reset();
        if (FAILED(dev()->CreateTexture2D(&d, nullptr, hr.tex.put())) ||
            FAILED(dev()->CreateShaderResourceView(hr.tex.get(), nullptr, hr.srv.put()))) {
            g_hires.erase(key);
            return;
        }
        hr.w = w;
        hr.h = h;
    }
    const D3D11_BOX box = {(UINT)nx0, (UINT)ny0, 0, (UINT)nx1, (UINT)ny1, 1};
    ID3D11Texture2D *to = hr.tex.get();
    q::run([to, from, box] { qdc()->CopySubresourceRegion(to, 0, 0, 0, 0, from, 0, &box); });
    hr.used = ++g_hiresClock;
    while (g_hires.size() > kHiresMax) {
        auto oldest = g_hires.begin();
        for (auto it = g_hires.begin(); it != g_hires.end(); ++it)
            if (it->second.used < oldest->second.used)
                oldest = it;
        g_hires.erase(oldest);
    }
    char key64[48];
    snprintf(key64, sizeof(key64), "pgl-hires-%dx%d", w, h);
    log_once(key64, "pgl: a %dx%d readback kept at %dx%d native pixels (high-res car icons: docs/renderer.md)", w, h,
             nx1 - nx0, ny1 - ny0);
}

void registry_upload(uint32_t dbp, uint32_t dbw, uint32_t dpsm, int dx, int dy, int w, int h,
                     const void *data, size_t bytes)
{
    if (data == nullptr || w <= 0 || h <= 0 || bytes == 0)
        return;
    dbp &= BLOCK_MASK;
    if (dpsm == gstex::PSMCT32 && dx >= 0 && dy >= 0 && dx + w <= 2048 && dy + h <= 2048) {
        /* into the surface of buffer (dbp, dbw) */
        const uint64_t skey = (uint64_t)dbp | ((uint64_t)dbw << 16);
        Obj *s = nullptr;
        auto it = g_surfaceByKey.find(skey);
        if (it != g_surfaceByKey.end())
            s = obj(it->second);
        if (s == nullptr) {
            s = new_obj(OBJ_SURFACE);
            s->sdbp = dbp;
            s->sdbw = dbw;
            g_surfaceByKey[skey] = s->id;
        }
        surface_upload(s, dx, dy, w, h, static_cast<const uint8_t *>(data), bytes);
        mark_rect(s, dpsm, dbp, dbw, (uint32_t)dx, (uint32_t)dy, (uint32_t)w, (uint32_t)h, LAYER_RGB | LAYER_TOP,
                  nullptr);
        /* PC: the pixels of a high-res image (registry_hires_readback): that
         * region is sampled from it; another upload over it ends that */
        uint64_t hk = 0;
        if (!g_hires.empty() && w <= 256 && h <= 256 && bytes >= (size_t)w * (size_t)h * 4) {
            hk = hires_key(data, w, h);
            if (g_hires.find(hk) == g_hires.end())
                hk = 0;
        }
        if (hk != 0) {
            s->hkey = hk;
            s->hx = dx;
            s->hy = dy;
            s->hw = w;
            s->hh = h;
        } else if (s->hkey != 0 && dx < s->hx + s->hw && s->hx < dx + w && dy < s->hy + s->hh && s->hy < dy + h) {
            s->hkey = 0;
        }
        s->serial = ++g_serial;
        s->lastFrame = g_registryFrame;
        return;
    }
    uint64_t key = hash_bytes(data, bytes);
    key = mix(mix(mix(key, dbp | (uint64_t)dbw << 16 | (uint64_t)dpsm << 24), (uint32_t)dx | (uint64_t)(uint32_t)dy << 32),
              (uint32_t)w | (uint64_t)(uint32_t)h << 32);
    Obj *o = nullptr;
    auto it = g_imageByKey.find(key);
    if (it != g_imageByKey.end())
        o = obj(it->second);
    if (o == nullptr) {
        o = new_obj(OBJ_IMAGE);
        o->t.dbp = dbp;
        o->t.dbw = dbw;
        o->t.dpsm = dpsm;
        o->t.dsax = (uint32_t)dx;
        o->t.dsay = (uint32_t)dy;
        o->t.w = (uint32_t)w;
        o->t.h = (uint32_t)h;
        o->data.assign(static_cast<const uint8_t *>(data), static_cast<const uint8_t *>(data) + bytes);
        o->key = key;
        g_imageByKey[key] = o->id;
    }
    mark_rect(o, dpsm, dbp, dbw, (uint32_t)dx, (uint32_t)dy, (uint32_t)w, (uint32_t)h, layers_of(dpsm), nullptr);
    o->serial = ++g_serial;
    o->lastFrame = g_registryFrame;
}

void registry_target_written(Target &t, bool rgb, bool top)
{
    Obj *o = obj(t.objId);
    if (o == nullptr) {
        o = new_obj(OBJ_TARGET);
        o->target = &t;
        t.objId = o->id;
        g_targetObjs.push_back(o->id);
    }
    const uint32_t psm = t.depth ? (0x30 | (t.psm & 0x0F)) : (t.psm & 0x0F);
    const uint32_t fbw = t.fbw ? t.fbw : (uint32_t)((t.gsW + 63) / 64);
    const uint8_t own = layers_of(psm);
    if (t.gsW != o->markW || t.gsH != o->markH || fbw != o->markFbw) {
        /* a depth target that took another frame's GS geometry
         * (depth_target_written) covers other blocks now */
        o->needRemark[0] = true;
        o->needRemark[1] = true;
    }
    uint8_t layers = 0;
    if (rgb && (own & LAYER_RGB) && o->needRemark[0])
        layers |= LAYER_RGB;
    if (top && (own & LAYER_TOP) && o->needRemark[1])
        layers |= LAYER_TOP;
    if (layers == 0)
        return;
    o->targetRange = gstex::image_blocks(psm, t.base, fbw, 0, 0, (uint32_t)t.gsW, (uint32_t)t.gsH);
    o->markW = t.gsW;
    o->markH = t.gsH;
    o->markFbw = fbw;
    mark_rect(o, psm, t.base, fbw, 0, 0, (uint32_t)t.gsW, (uint32_t)t.gsH, layers, nullptr);
    if (layers & LAYER_RGB)
        o->needRemark[0] = false;
    if (layers & LAYER_TOP)
        o->needRemark[1] = false;
    o->serial = ++g_serial;
}

/* pfx_split_rgb_planes (VU1 micro program 0x78D, graphics.md 8.6) read the
 * frame's bytes as PSMT8 through an identity CLUT and wrote its R, G and B
 * into the alpha bytes of three planes (RacePS2Base::captureAfterimage: the
 * unused top bytes of the CT24 frame buffers and of a Z24 buffer).  Native:
 * the frame's native pixels are copied once into a capture; each plane is a
 * registry object owning the top-byte layer of its blocks that stands for
 * one channel of it, so the PSMT8H draws of drawAfterimage read it (through
 * their CLUT, plane_texture) until something else writes those bytes. */
void registry_split_planes(Target &src, const uint32_t blocks[3], int w, int h)
{
    if (src.depth || !src.tex || w <= 0 || h <= 0)
        return;
    /* the native pixels of src's GS pixels 0..w-1 x 0..h-1 */
    const int cw = std::min(src.nw, std::max(1, (int)std::lround((float)w * src.sx)));
    const int ch = std::min(src.nh, std::max(1, (int)std::lround((float)h * src.sy)));
    Capture &cap = g_captures[blocks[0] & BLOCK_MASK];
    if (!cap.tex || cap.w != cw || cap.h != ch) {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = (UINT)cw;
        d.Height = (UINT)ch;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        cap.srv.reset();
        if (FAILED(dev()->CreateTexture2D(&d, nullptr, cap.tex.put())) ||
            FAILED(dev()->CreateShaderResourceView(cap.tex.get(), nullptr, cap.srv.put()))) {
            cap.tex.reset();
            cap.srv.reset();
            log_once("pgl-plane-capture", "pgl: could not create a %dx%d frame capture (afterimage)", cw, ch);
            return;
        }
        cap.w = cw;
        cap.h = ch;
    }
    const D3D11_BOX box = {0, 0, 0, (UINT)cw, (UINT)ch, 1};
    ID3D11Texture2D *to = cap.tex.get(), *from = target_tex1(src);
    q::run([to, from, box] { qdc()->CopySubresourceRegion(to, 0, 0, 0, 0, from, 0, &box); });
    const uint32_t fbw = (uint32_t)((w + 63) / 64);
    for (int c = 0; c < 3; c++) {
        const uint32_t bp = blocks[c] & BLOCK_MASK;
        Obj *o = nullptr;
        auto it = g_planeByBlock.find(bp);
        if (it != g_planeByBlock.end())
            o = obj(it->second);
        if (o == nullptr) {
            o = new_obj(OBJ_PLANE);
            g_planeByBlock[bp] = o->id;
        }
        o->pbp = bp;
        o->pfbw = fbw;
        o->pw = w;
        o->ph = h;
        o->pch = c;
        o->pcap = cap.srv;
        o->capW = cw;
        o->capH = ch;
        mark_rect(o, gstex::PSMCT32, bp, fbw, 0, 0, (uint32_t)w, (uint32_t)h, LAYER_TOP, nullptr);
        o->serial = ++g_serial;
        o->lastFrame = g_registryFrame;
    }
    if (afterimage_log())
        pclog::write("pgl afterimage: frame %llu: the frame at block 0x%X (GS %dx%d, native %dx%d) split into the"
                     " planes at blocks 0x%X / 0x%X / 0x%X (%dx%d, FBW %u, native %dx%d)",
                     (unsigned long long)frame_number(), src.base, src.gsW, src.gsH, src.nw, src.nh,
                     blocks[0] & BLOCK_MASK, blocks[1] & BLOCK_MASK, blocks[2] & BLOCK_MASK, w, h, fbw, cw, ch);
}

void registry_target_released(Target &t)
{
    if (t.objId == 0)
        return;
    const uint64_t gen = ++g_markGen;
    uint64_t changed = 0;
    for (int l = 0; l < 2; l++)
        for (uint32_t b = 0; b < gstex::GS_MEM_BLOCKS; b++)
            if (g_own[l][b] == t.objId) {
                g_own[l][b] = 0;
                g_blockGen[l][b] = gen;
                changed |= 1ull << (b >> kRegionShift);
            }
    regions_changed(changed);
    g_targetObjs.erase(std::remove(g_targetObjs.begin(), g_targetObjs.end(), t.objId), g_targetObjs.end());
    obj_forget(t.objId);
    g_objs.erase(t.objId);
    t.objId = 0;
}

Target *registry_colour_target_at(uint32_t block, int &ox, int &oy)
{
    Obj *o = obj(g_own[0][block & BLOCK_MASK]);
    if (o == nullptr || o->kind != OBJ_TARGET || o->target == nullptr || o->target->depth)
        return nullptr;
    if (!target_offset(*o->target, block & BLOCK_MASK, ox, oy))
        return nullptr;
    return o->target;
}

void registry_clut_load(uint64_t tex0v)
{
    ProfScope prof(PROF_CLUT);              /* (the verbose monitor's profile) */
    g_profCount[PCOUNT_CLUTS]++;
    gstex::Tex0 t = gstex::Tex0::decode(tex0v);
    if (!gstex::psm_indexed(t.psm))
        return;
    const uint32_t cbp = t.cbp & BLOCK_MASK;
    const bool t8 = t.psm == gstex::PSMT8 || t.psm == gstex::PSMT8H;
    /* PC (CPU time): these fields loaded since the last owner change */
    const uint32_t fields = cbp | (t.cpsm & 15) << 14 | (t.csm & 1) << 18 | (t.csa & 31) << 19 | (uint32_t)t8 << 24;
    /* the memo key: the fields, CBP's owner, the tags of the CLUT's blocks */
    const uint64_t fieldKey = cbp | (uint64_t)t.cpsm << 16 | (uint64_t)t.csm << 20 | (uint64_t)t.csa << 24 |
                              (uint64_t)t8 << 32;
    auto clut_key = [&](uint32_t first, uint32_t count) {
        uint64_t k = comb(comb(0xBB67AE8584CAA73Bull, fieldKey), g_own[0][cbp]);
        for (uint32_t i = 0; i < count; i++) {
            const uint32_t b = (first + i) & BLOCK_MASK;
            k = comb(comb(k, g_blockGen[0][b]), g_blockGen[1][b]);
        }
        return k;
    };
    /* the load of the CLUT cb holds, memoized under key */
    auto add_load = [&](uint64_t key, const gstex::ClutBuffer &cb) -> const ClutLoad & {
        ClutLoad l;
        l.key = key;
        l.cpsm = cb.cpsm();
        l.base = t.csa * 16;
        l.n = t8 ? 256 : 16;
        for (uint32_t i = 0; i < l.n; i++)
            l.raw[i] = cb.lookup_raw(t.psm, t.csa, i);
        clut_rows_init(l);
        if (g_clutMemo.size() > 4096) {
            clut_materialize();             /* (the rows still noted by their loads) */
            g_clutMemo.clear();
            g_tagEpoch++;                   /* (the front's loads went with it) */
        }
        return g_clutMemo.emplace(key, l).first->second;
    };
    /* PC (ROADMAP 18, no memory limits): the CLUT of a VRAM-less set's
     * texture (registry_texture_source) is the set's: by the set, its data
     * version and CBP's place in it - no front entry (it owns no block) */
    if (g_texSrc.obj != nullptr && cbp == g_texSrc.cbp && t.cpsm == g_texSrc.cpsm && t.csm == g_texSrc.csm) {
        Obj *g = g_texSrc.obj;
        const uint32_t ofs = (cbp - g_texSrc.base) & BLOCK_MASK;
        const uint64_t key = comb(comb(comb(0x9E3779B97F4A7C15ull, fieldKey),
                                       comb((uint64_t)(uintptr_t)g->img, g->stamp)),
                                  comb(g->contentVer, ofs));
        auto it = g_clutMemo.find(key);
        const ClutLoad *load = it != g_clutMemo.end() ? &it->second : nullptr;
        if (load == nullptr) {
            group_layout(g);
            gstex::Tex0 tl = t;
            tl.cbp = (g->layoutBase + ofs) & BLOCK_MASK;
            gstex::ClutBuffer cb;
            cb.load(g->space, tl);
            load = &add_load(key, cb);
        }
        if (!g_clutLastValid || key != g_clutLastKey) {
            g_clutLastKey = key;
            g_clutLastValid = true;
            clut_apply(*load);
        }
        return;
    }
    ClutFront &front = g_clutFront[(uint32_t)(comb(0x243F6A8885A308D3ull, fields) >> 40) % kClutFronts];
    if (front.load != nullptr && front.fields == fields && front.epoch == g_tagEpoch &&
        clut_key(front.first, front.count) == front.load->key) {
        if (!g_clutLastValid || front.load->key != g_clutLastKey) {
            g_clutLastKey = front.load->key;
            g_clutLastValid = true;
            clut_apply(*front.load);
        }
        return;
    }
    Obj *o = obj(g_own[0][cbp]);
    if (o == nullptr) {
        log_once("pgl-clut-none", "pgl: a CLUT load from block 0x%X that nothing wrote (TEX0 psm %s cpsm 0x%X"
                 " csa %u; kept the old CLUT)", cbp, gstex::psm_name(t.psm), t.cpsm, t.csa);
        return;
    }
    if (o->kind == OBJ_TARGET) {
        log_once("pgl-clut-target", "pgl: a CLUT load from a render target (block 0x%X, TEX0 psm %s tbp 0x%X"
                 " cpsm 0x%X csa %u): not native, the old CLUT kept", cbp, gstex::psm_name(t.psm), t.tbp0, t.cpsm,
                 t.csa);
        return;
    }
    BlockRange r = gstex::clut_blocks(t, false);
    if (r.empty())
        r.first = r.last = cbp;
    /* PC (CPU time): the key by what the load reads, its blocks' tags (the
     * front's entry checks it with its blocks; a long run of them is not
     * kept there) */
    const uint32_t count = std::min<uint32_t>(r.count(), gstex::GS_MEM_BLOCKS);
    const uint64_t key = clut_key(r.first, count);
    const bool frontable = count <= 16;
    g_profCount[PCOUNT_CLUTSLOW]++;
    auto it = g_clutMemo.find(key);
    if (it != g_clutMemo.end()) {
        front.fields = fields;
        front.epoch = g_tagEpoch;
        front.first = r.first;
        front.count = count;
        front.load = frontable ? &it->second : nullptr;
        if (g_clutLastValid && key == g_clutLastKey)
            return;                         /* PC: the buffer holds this load already */
        g_clutLastKey = key;
        g_clutLastValid = true;
        clut_apply(it->second);
        return;
    }
    g_clutLastKey = key;
    g_clutLastValid = true;
    g_profCount[PCOUNT_CLUTMISS]++;
    gstex::ClutBuffer cb;
    if (o->kind == OBJ_GROUP) {
        group_layout(o);
        gstex::Tex0 tl = t;
        tl.cbp = (cbp - group_delta_at(o, 0, cbp)) & BLOCK_MASK;     /* the copy at CBP */
        cb.load(o->space, tl);
    } else {
        gstex::TexSpace space;
        compose_space(r, space, nullptr);
        cb.load(space, t);
    }
    const ClutLoad &added = add_load(key, cb);
    front.fields = fields;
    front.epoch = g_tagEpoch;
    front.first = r.first;
    front.count = count;
    front.load = frontable ? &added : nullptr;
    clut_apply(added);
}

/* ======================================================================== */
/* Resolving TEX0                                                           */
/* ======================================================================== */

namespace {

/* PC (CPU time): the bindings of the decoded-texture path (texture_bind's
 * last one) by TEX0 (CLD aside), CLAMP, TEXA, an alpha-only draw and the
 * palette's content, direct-mapped, while g_tagEpoch stays (no decoded
 * texture dropped) and what was read is as it was: a Tex1 set's texture
 * (most of a race's) is the set's data at the copy TBP0's block holds - its
 * two tags (the set, its base, its data version) say it all (exact); an
 * image's or a composed one, by the regions of TBP0's block and of the
 * texture's blocks (no tag changed there). */
struct TexFront {
    uint64_t tex0 = 0, clamp = 0, texa = 0, pal = 0, epoch = 0;
    bool alphaOnly = false;
    bool exact = false;
    uint64_t tag0 = 0, tag1 = 0;
    RegionSnap regions;
    CachedTex *ct = nullptr;
    TexBinding out;
};
/* (4096: several hundred textures a race frame, in the same order every
 * frame - 256 slots missed half the binds) */
constexpr uint32_t kTexFronts = 4096;
TexFront g_texFront[kTexFronts];

}  /* namespace */

bool texture_bind(uint64_t tex0v, uint64_t tex1v, uint64_t clampv, uint64_t texav, uint8_t writeMask,
                  Target *drawing, TexBinding &out)
{
    const gstex::Tex0 t = gstex::Tex0::decode(tex0v);
    const gstex::Clamp c = gstex::Clamp::decode(clampv);
    const gstex::Texa ta = gstex::Texa::decode(texav);
    const gstex::Tex1 t1 = gstex::Tex1::decode(tex1v);
    const uint32_t tbp = t.tbp0 & BLOCK_MASK;
    const bool top = psm_top(t.psm);
    out.hasMean = false;
    out.linear = false;

    /* the palette's content (PC, CPU time: what the CLUT buffer's rows it
     * reads hold) and this binding again (TexFront) */
    const bool indexed = gstex::psm_indexed(t.psm);
    const uint32_t palN = (t.psm == gstex::PSMT8 || t.psm == gstex::PSMT8H) ? 256 : 16;
    uint64_t palKey = 0;
    if (indexed) {
        const bool ct16 = clut_16bit(t.cpsm);
        palKey = palette_key(palN, ct16, t.csa, ct16 ? (ta.ta0 | ta.aem << 8 | (uint64_t)ta.ta1 << 16) : 0);
    }
    const uint64_t tex0Key = tex0v & ~(7ull << 61);
    const bool alphaOnly = (writeMask & pgl::gs::WRITE_RGB) == 0;
    /* PC (ROADMAP 18, no memory limits): the texture last named as a
     * VRAM-less set's is that set's, whatever owns its blocks (no front
     * entry: its blocks say nothing of it) */
    const bool virt = g_texSrc.obj != nullptr && tex0Key == g_texSrc.tex0;
    TexFront &front = g_texFront[(uint32_t)(comb(comb(tex0Key, clampv), palKey) >> 40) % kTexFronts];
    if (!virt && front.ct != nullptr && front.epoch == g_tagEpoch && front.tex0 == tex0Key && front.clamp == clampv &&
        front.texa == texav && front.pal == palKey && front.alphaOnly == alphaOnly &&
        (front.exact ? front.tag0 == g_blockGen[0][tbp] && front.tag1 == g_blockGen[1][tbp]
                     : front.regions.same())) {
        front.ct->lastFrame = g_registryFrame;
        front.ct->lastUse = g_execSeconds;
        out = front.out;
        return true;
    }
    g_profCount[PCOUNT_TEXSLOW]++;

    if (!gstex::psm_valid(t.psm)) {
        log_once("pgl-tex-psm", "pgl: texture with an invalid PSM 0x%X: not drawn", t.psm);
        return false;
    }
    if (t1.mxl != 0)
        log_once("pgl-tex-mip", "pgl: mipmapped textures (MXL %u): level 0 only", t1.mxl);

    Obj *o = virt ? g_texSrc.obj : obj(g_own[top ? 1 : 0][tbp]);
    /* A PSMCT32 texel is both layers: its RGB is what the owner of the RGB
     * layer wrote, its alpha what the owner of the top byte wrote.  A draw
     * that writes no colour channel uses nothing of it but the alpha (the GS
     * never mixes texel RGB into the alpha it writes or tests), so the top
     * byte's owner decides when that is a colour target: a target drawn
     * alpha-only owns only that layer.  CarIconMaker::drawIcon (T184) clears
     * and draws its mask under FBMSK 0x00FFFFFF and halves it alpha-only into
     * the icon; read through the RGB layer, the mask was whatever had last
     * written those blocks' RGB (an old frame, an earlier car's textures)
     * and the icons' alpha came from there. */
    if (!virt && !top && t.psm == gstex::PSMCT32 && (writeMask & pgl::gs::WRITE_RGB) == 0) {
        Obj *a = obj(g_own[1][tbp]);
        if (a != nullptr && a != o && a->kind == OBJ_TARGET && a->target != nullptr && !a->target->depth)
            o = a;
    }
    if (top && afterimage_log())
        pclog::write("pgl afterimage: frame %llu: %s texture at block 0x%X (TBW %u, CSA %u): the top byte is %s%s",
                     (unsigned long long)frame_number(), gstex::psm_name(t.psm), tbp, t.tbw, t.csa,
                     o == nullptr ? "unwritten" : kind_name(o->kind),
                     o != nullptr && o->kind == OBJ_PLANE ? (o->pch == 0 ? " (R)" : o->pch == 1 ? " (G)" : " (B)")
                                                          : "");
    if (o == nullptr) {
        char key[48];
        snprintf(key, sizeof(key), "pgl-tex-none-%X-%X", tbp, t.psm);
        log_once(key, "pgl: texture at block 0x%X (%s %ux%u) has no source: not drawn", tbp,
                 gstex::psm_name(t.psm), t.width(), t.height());
        return false;
    }

    /* ---- a plane of pfx_split_rgb_planes (the afterimage): its bytes as
     * the indices of a PSMT8H / T4HL / T4HH texture at its first block ---- */
    if (o->kind == OBJ_PLANE) {
        if (!top || tbp != o->pbp || t.tbw != o->pfbw) {
            log_once("pgl-tex-plane-ofs", "pgl: a %s texture at block 0x%X (TBW %u) inside a plane at 0x%X (FBW %u):"
                     " not native", gstex::psm_name(t.psm), tbp, t.tbw, o->pbp, o->pfbw);
            return false;
        }
        const uint32_t n = t.psm == gstex::PSMT8H ? 256 : 16;
        uint32_t pal[256];
        for (uint32_t i = 0; i < n; i++)
            pal[i] = clut_lookup(t.psm, t.cpsm, t.csa, i, ta);
        ID3D11ShaderResourceView *srv = plane_texture(o, t.psm, pal, n);
        if (srv == nullptr)
            return false;
        out.srv = srv;
        out.x0 = 0;
        out.y0 = 0;
        out.w = (float)o->pw;
        out.h = (float)o->ph;
        out.alphaMode = 0;
        out.target = nullptr;
        out.depthTex = false;
        return true;
    }

    /* ---- a render target ---- */
    if (o->kind == OBJ_TARGET) {
        Target *tg = o->target;
        if (gstex::psm_depth(t.psm) != tg->depth) {
            log_once("pgl-tex-target-kind", "pgl: a %s texture of a %s buffer (block 0x%X): not native",
                     gstex::psm_name(t.psm), tg->depth ? "depth" : "colour", tbp);
            return false;
        }
        if (gstex::psm_indexed(t.psm)) {
            log_once("pgl-tex-target-index", "pgl: an indexed (%s) texture of a render target (block 0x%X):"
                     " not native", gstex::psm_name(t.psm), tbp);
            return false;
        }
        int ox = 0, oy = 0;
        if (!target_offset(*tg, tbp, ox, oy)) {
            log_once("pgl-tex-target-ofs", "pgl: texture at block 0x%X inside a render target at 0x%X:"
                     " offset not found", tbp, tg->base);
            return false;
        }
        if (tg->fbw != 0 && t.tbw != tg->fbw)
            log_once("pgl-tex-target-tbw", "pgl: render target at 0x%X sampled with TBW %u (FBW %u)", tg->base,
                     t.tbw, tg->fbw);
        out.srv = (tg == drawing) ? target_copy_srv(*tg) : target_srv(*tg);
        out.x0 = -(float)ox;
        out.y0 = -(float)oy;
        out.w = (float)tg->gsW;
        out.h = (float)tg->gsH;
        out.alphaMode = 0;
        if (t.psm == gstex::PSMCT24)
            out.alphaMode = 1;
        else if (t.psm == gstex::PSMCT16 || t.psm == gstex::PSMCT16S)
            out.alphaMode = 2;
        out.target = tg;
        out.depthTex = tg->depth;
        return true;
    }

    /* ---- uploads ---- */
    const gstex::Rect rect = gstex::decode_region(t, c);
    const BlockRange br = gstex::image_blocks(t.psm, tbp, t.tbw, rect.x, rect.y, rect.w, rect.h);
    /* The owner of TBP0's block decides when it is a Tex1 set or a surface:
     * the 2^TW x 2^TH space usually reaches past the set's (or the movie
     * buffer's) blocks into other objects' VRAM, which the game never
     * samples - composing it would decode the texture anew every frame.
     * A lone image upload must own the whole footprint for the linear
     * path; anything else is composed. */
    bool single = o->kind == OBJ_GROUP || o->kind == OBJ_SURFACE;
    if (!single) {
        single = true;
        const uint32_t count = std::min<uint32_t>(br.count(), gstex::GS_MEM_BLOCKS);
        const int layer = top ? 1 : 0;
        for (uint32_t i = 0; i < count && single; i++) {
            const uint32_t id = g_own[layer][(br.first + i) & BLOCK_MASK];
            if (id != o->id && id != 0)
                single = false;
        }
    }

    /* a CT32 surface read as itself: its own GPU texture */
    if (single && o->kind == OBJ_SURFACE && t.psm == gstex::PSMCT32 && o->sdbp == tbp && o->sdbw == t.tbw) {
        /* PC: the region of a high-res image (registry_hires_readback) read
         * as exactly itself: the image, bilinear (a car icon) */
        if (o->hkey != 0 && (int)rect.x == o->hx && (int)rect.y == o->hy && (int)rect.w == o->hw &&
            (int)rect.h == o->hh) {
            auto hi = g_hires.find(o->hkey);
            if (hi != g_hires.end() && hi->second.srv) {
                hi->second.used = ++g_hiresClock;
                out.srv = hi->second.srv.get();
                out.x0 = (float)o->hx;
                out.y0 = (float)o->hy;
                out.w = (float)o->hw;
                out.h = (float)o->hh;
                out.alphaMode = 0;
                out.target = nullptr;
                out.depthTex = false;
                out.linear = true;
                return true;
            }
        }
        ID3D11ShaderResourceView *srv = surface_srv(o);
        if (srv == nullptr)
            return false;
        out.srv = srv;
        out.x0 = 0;
        out.y0 = 0;
        out.w = (float)o->sw;
        out.h = (float)o->sh;
        out.alphaMode = 0;
        out.target = nullptr;
        out.depthTex = false;
        return true;
    }

    /* the palette the texture reads (PC, CPU time: from the memo, by its
     * content, palKey) */
    const uint32_t *palette = nullptr;
    uint64_t palHash = 0;
    if (indexed) {
        PalMemo &m = g_palMemo[(palKey ^ (palKey >> 17)) % kPalMemos];
        if (!m.valid || m.key != palKey) {
            for (uint32_t i = 0; i < palN; i++)
                m.pal[i] = clut_lookup(t.psm, t.cpsm, t.csa, i, ta);
            m.hash = hash_bytes(m.pal, palN * 4);
            m.key = palKey;
            m.valid = true;
        }
        palette = m.pal;
        palHash = m.hash;
    }
    const bool linear = single && o->kind == OBJ_IMAGE && o->t.dpsm == t.psm && o->t.dbp == tbp &&
                        o->t.dbw == t.tbw && o->t.dsax == 0 && o->t.dsay == 0;

    uint64_t key = tex_key(t, rect, palHash, ta);
    uint64_t composeKey = 0;
    gstex::TexSpace composed;
    const bool group = single && o->kind == OBJ_GROUP;
    uint32_t tbpLayout = tbp;
    if (group) {
        /* the copy of the set at TBP0 (an earlier one of the frame, if the
         * set has moved since); the small uploads under the footprint there */
        const uint32_t d = virt ? ((g_texSrc.base - o->layoutBase) & BLOCK_MASK)   /* PC: (linked there) */
                                : group_delta_at(o, top ? 1 : 0, tbp);
        tbpLayout = (tbp - d) & BLOCK_MASK;
        key = group_key(key, o->img, o->stamp, tbpLayout - o->layoutBase, o->uploads,
                        shifted(br, (0u - d) & BLOCK_MASK));
        g_preTexa.store(texav, std::memory_order_relaxed);  /* PC: (the sets decoded ahead next) */
    } else if (linear) {
        key = mix(mix(key, 0x1), o->key);
    } else {
        compose_space(br, composed, &composeKey);
        key = mix(mix(mix(key, 0xC), composeKey), tbp);
    }

    auto it = g_cache.find(key);
    if (it == g_cache.end() && tex_pre_drain())
        it = g_cache.find(key);             /* PC: decoded ahead since (pgl_prewarm.cpp) */
    if (it != g_cache.end() && it->second.pre) {
        it->second.pre = false;
        pre::taken(pre::TEXTURE);
    }
    if (it == g_cache.end()) {
        ProfScope prof(PROF_DECODE);        /* (the verbose monitor's profile) */
        g_profCount[PCOUNT_DECODES]++;
        if (group && pre::on())
            pre::late(pre::TEXTURE);
        std::vector<uint32_t> px((size_t)rect.w * rect.h, 0);
        if (group) {
            group_layout(o);
            gstex::decode_rect(o->space, t.psm, tbpLayout, t.tbw, rect.x, rect.y, rect.w, rect.h, nullptr, t.csa,
                               ta, px.data(), rect.w);
            if (indexed)
                apply_palette(px, palette, palN);
        } else if (linear) {
            const uint32_t iw = o->t.w, ih = o->t.h;
            std::vector<uint32_t> img((size_t)iw * ih, 0);
            gstex::decode_linear(t.psm, o->data.data(), o->data.size(), iw, ih, indexed ? palette : nullptr, ta,
                                 img.data(), iw);
            for (uint32_t y = 0; y < rect.h; y++) {
                const uint32_t sy = rect.y + y;
                if (sy >= ih)
                    break;
                for (uint32_t x = 0; x < rect.w; x++) {
                    const uint32_t sx = rect.x + x;
                    if (sx < iw)
                        px[(size_t)y * rect.w + x] = img[(size_t)sy * iw + sx];
                }
            }
        } else {
            gstex::decode_rect(composed, t.psm, tbp, t.tbw, rect.x, rect.y, rect.w, rect.h, nullptr, t.csa, ta,
                               px.data(), rect.w);
            if (indexed)
                apply_palette(px, palette, palN);
        }
        CachedTex ct;
        ct.rect = rect;
        ct.mean = texel_mean(px);
        if (flicker_on())
            flicker_event("texture decoded: 0x%X %s TBW %u (%u,%u) %ux%u from %s, mean %08X (%u cached)", tbp,
                          gstex::psm_name(t.psm), t.tbw, rect.x, rect.y, rect.w, rect.h,
                          group ? "a Tex1 set" : linear ? "an image upload" : "composed VRAM", ct.mean,
                          (unsigned)g_cache.size() + 1);
        if (!make_texture((int)rect.w, (int)rect.h, px.data(), ct.tex, ct.srv)) {
            log_once("pgl-tex-create", "pgl: could not create a %ux%u texture", rect.w, rect.h);
            return false;
        }
        it = g_cache.emplace(key, std::move(ct)).first;
        g_texBytes += (size_t)rect.w * rect.h * 4;
    }
    it->second.lastFrame = g_registryFrame;
    it->second.lastUse = g_execSeconds;
    out.srv = it->second.srv.get();
    out.x0 = (float)rect.x;
    out.y0 = (float)rect.y;
    out.w = (float)rect.w;
    out.h = (float)rect.h;
    out.alphaMode = 0;
    out.target = nullptr;
    out.depthTex = false;
    out.mean = it->second.mean;
    out.hasMean = true;
    /* the front's entry: what was read - a set's: TBP0's tags; else TBP0's
     * block and the texture's blocks by the regions they lie in (none if
     * over four) */
    RegionSnap regions;
    const bool snapped = group || (regions.add_blocks(tbp, 1) &&
                                   regions.add_blocks(br.first, std::min<uint32_t>(br.count(), gstex::GS_MEM_BLOCKS)));
    front.tex0 = tex0Key;
    front.clamp = clampv;
    front.texa = texav;
    front.pal = palKey;
    front.alphaOnly = alphaOnly;
    front.epoch = g_tagEpoch;
    front.exact = group;
    front.tag0 = g_blockGen[0][tbp];
    front.tag1 = g_blockGen[1][tbp];
    front.regions = regions;
    front.ct = snapped && !virt ? &it->second : nullptr;
    front.out = out;
    return true;
}

/* ======================================================================== */
/* Housekeeping                                                             */
/* ======================================================================== */

void registry_end_frame()
{
    g_registryFrame++;
    tex_pre_drain();                        /* PC: the textures decoded ahead (pgl_prewarm.cpp) */
    if ((g_registryFrame % 64) != 0)
        return;
    /* decoded textures unused for kTexExpire seconds; then, over the budget,
     * the least recently bound (not within kTexKeep seconds: composed ones,
     * whose keys follow the uploads, are among the first) */
    bool dropped = false;
    const double now = g_execSeconds;
    std::vector<std::pair<double, uint64_t>> old;
    for (auto it = g_cache.begin(); it != g_cache.end();) {
        if (it->second.lastUse + kTexExpire < now) {
            g_texBytes -= (size_t)it->second.rect.w * it->second.rect.h * 4;
            it = g_cache.erase(it);
            dropped = true;
            continue;
        }
        if (it->second.lastUse + kTexKeep < now)
            old.push_back({it->second.lastUse, it->first});
        ++it;
    }
    if (g_texBytes > kTexBudget) {
        std::sort(old.begin(), old.end());
        for (const auto &o : old) {
            if (g_texBytes <= kTexBudget)
                break;
            auto it = g_cache.find(o.second);
            if (it == g_cache.end())
                continue;
            g_texBytes -= (size_t)it->second.rect.w * it->second.rect.h * 4;
            g_cache.erase(it);
            dropped = true;
        }
    }
    if (dropped)
        g_tagEpoch++;                       /* PC: (the texture binds' front pointed into it) */
    /* objects that own no block any more (PC, CPU time: flagged through the
     * owner map, an object looked up where the owner changes - a std::set
     * of the 32768 entries was a spike every 64 frames) */
    for (auto &e : g_objs)
        e.second->live = false;
    for (int l = 0; l < 2; l++) {
        uint32_t last = 0;
        for (uint32_t b = 0; b < gstex::GS_MEM_BLOCKS; b++) {
            const uint32_t id = g_own[l][b];
            if (id != 0 && id != last) {
                if (Obj *o = obj(id))
                    o->live = true;
                last = id;
            }
        }
    }
    for (auto it = g_objs.begin(); it != g_objs.end();) {
        Obj *o = it->second.get();
        bool drop = false;
        if (o->kind == OBJ_IMAGE || o->kind == OBJ_SURFACE || o->kind == OBJ_PLANE)
            drop = !o->live;
        else if (o->kind == OBJ_GROUP)
            drop = !o->live && o->lastUse + kTexExpire < now;  /* PC: by time (it was 600 frames) */
        if (drop) {
            obj_forget(o->id);
            if (g_texSrc.obj == o)
                g_texSrc = TexSource();     /* PC: (registry_texture_source's) */
            if (o->kind == OBJ_IMAGE) {
                auto k = g_imageByKey.find(o->key);
                if (k != g_imageByKey.end() && k->second == o->id)
                    g_imageByKey.erase(k);
            } else if (o->kind == OBJ_PLANE) {
                auto k = g_planeByBlock.find(o->pbp);
                if (k != g_planeByBlock.end() && k->second == o->id)
                    g_planeByBlock.erase(k);
            } else if (o->kind == OBJ_SURFACE) {
                auto k = g_surfaceByKey.find((uint64_t)o->sdbp | ((uint64_t)o->sdbw << 16));
                if (k != g_surfaceByKey.end() && k->second == o->id)
                    g_surfaceByKey.erase(k);
            } else {
                auto k = g_groupByImg.find(o->img);
                if (k != g_groupByImg.end() && k->second == o->id)
                    g_groupByImg.erase(k);
            }
            it = g_objs.erase(it);
        } else {
            ++it;
        }
    }
    /* captures whose first plane is gone */
    for (auto it = g_captures.begin(); it != g_captures.end();) {
        if (g_planeByBlock.count(it->first) == 0)
            it = g_captures.erase(it);
        else
            ++it;
    }
}

}  /* namespace pglr */
