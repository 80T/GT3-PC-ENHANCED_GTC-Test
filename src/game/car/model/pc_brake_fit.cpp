/* pc_brake_fit.cpp - PC (2026-11-13, no original): the brake moved back
 * until the wheel in front of it clears it (pc_brake_fit.h). */
#include "game/car/model/pc_brake_fit.h"
#include "engine/gfx/pc_model_walk.h"
#include "engine/gfx/T451_model_set.h"
#include "engine/pgl/native/vif_model.h"    /* VifModelHeader, VifPartRecord (the wheels' stamp) */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const f32 kPcBrakeClear = 0.004f;
const f32 kPcBrakeMaxPush = 0.080f;
const f32 kPcBrakeKeepIn = 0.020f;

namespace {

enum { kMaxRows = 96, kMaxCols = 256 };     /* a triangle's sample rows, and points in a row, at most */
const f32 kFaceCos = 0.9f;                  /* a face: its normal within 26 degrees of the axle */
const f32 kFlat = 1.0e-9f;                  /* twice a triangle's area, squared: below, no triangle */
const f32 kX0 = -0.5f;                      /* the cells along x: from here... */
const f32 kCell = 1.0f / 256.0f;            /* ...this wide (1 mm of a 0.25 m rim), kCells of them: to +1.5 */
enum { kGap = 5 };                          /* empty cells a piece may have between two filled ones (5 mm) */
const f32 kFaceBand = 0.03f;                /* the brake's face: its rings within this of its least x */
enum { kInner = 5 };                        /* the rings nearest the axle (out to 0.1 rim radii) */
const f32 kNone = 100.0f;

enum { kFace, kSide, kRound };

struct Fill {
    PcRadial *p;
    /* the triangle being read: its kind, its x range in each ring it is in */
    int kind;
    f32 lo[PcRadial::kBins], hi[PcRadial::kBins];
    u64 rings;
};

int cell_of(f32 x)
{
    const int c = (int)floorf((x - kX0) / kCell);
    return c < 0 ? 0 : c >= PcRadial::kCells ? PcRadial::kCells - 1 : c;
}

/* The direction of (y, z) about the axle as one of kSectors: by the diamond
 * angle (y / (|y| + |z|) per quadrant: no arc tangent; the sectors are 7 to
 * 14 degrees wide). */
int sector_of(f32 y, f32 z)
{
    const f32 d = fabsf(y) + fabsf(z);
    if (d <= 0.0f)
        return 0;
    const f32 t = y / d;
    const f32 a = z >= 0.0f ? 1.0f - t : 3.0f + t;          /* 0 .. 4 once round */
    const int s = (int)(a * (f32)(PcRadial::kSectors / 4));
    return s < 0 ? 0 : s >= PcRadial::kSectors ? PcRadial::kSectors - 1 : s;
}

void put(Fill *f, f32 x, f32 y, f32 z)
{
    PcRadial *p = f->p;
    const f32 r = sqrtf(y * y + z * z);
    const int bin = (int)(r / p->step);
    if (bin < 0 || bin >= PcRadial::kBins)
        return;
    if (!p->has[bin] || x < p->lo[bin])
        p->lo[bin] = x;
    if (!p->has[bin] || x > p->hi[bin])
        p->hi[bin] = x;
    p->has[bin] = 1;
    const u64 bit = 1ull << bin;
    if (!(f->rings & bit) || x < f->lo[bin])
        f->lo[bin] = x;
    if (!(f->rings & bit) || x > f->hi[bin])
        f->hi[bin] = x;
    f->rings |= bit;
    if (f->kind == kFace) {
        const int s = sector_of(y, z);
        if (!(p->faced[bin] & (1u << s)) || x < p->front[bin][s])
            p->front[bin][s] = x;
        p->faced[bin] |= 1u << s;
    }
}

/* A triangle into the rings: its kind by its normal at its centre, then its
 * points on rows parallel to its longest edge (as seen along the axle), half
 * a ring apart and half a ring between the points of a row - so no ring it
 * crosses is stepped over, and the work goes by its area; a face's or a
 * side's span of x in each ring into that ring's cells. */
void triangle(const float *v0, const float *v1, const float *v2, void *user)
{
    Fill *f = (Fill *)user;
    PcRadial *p = f->p;
    p->triangles++;
    const f32 e1[3] = {v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2]}, e2[3] = {v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2]};
    const f32 n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
    const f32 len2 = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
    if (len2 < kFlat)
        return;                             /* (no area: it draws nothing) */
    int kind = kFace;
    if (n[0] * n[0] < kFaceCos * kFaceCos * len2) {
        /* the normal's part along the radius at the centre, and across */
        const f32 cy = (v0[1] + v1[1] + v2[1]) / 3.0f, cz = (v0[2] + v1[2] + v2[2]) / 3.0f;
        const f32 cr = sqrtf(cy * cy + cz * cz);
        kind = kRound;
        if (cr > 1.0e-6f) {
            const f32 nr = (n[1] * cy + n[2] * cz) / cr, nt = (n[2] * cy - n[1] * cz) / cr;
            if (fabsf(nt) > fabsf(nr))
                kind = kSide;
        }
        if (kind == kRound)
            p->round++;
    }
    /* b - c the longest edge seen along the axle, a the corner across it */
    const float *vs[3] = {v0, v1, v2};
    int far = 0;
    f32 longest = -1.0f;
    for (int k = 0; k < 3; k++) {
        const float *s = vs[(k + 1) % 3], *t = vs[(k + 2) % 3];
        const f32 dy = t[1] - s[1], dz = t[2] - s[2];
        const f32 d = dy * dy + dz * dz;
        if (d > longest) {
            longest = d;
            far = k;
        }
    }
    const float *a = vs[far], *b = vs[(far + 1) % 3], *c = vs[(far + 2) % 3];
    longest = sqrtf(longest);
    const f32 h = p->step * 0.5f;
    /* (n[0] is twice its area seen along the axle: its height over that edge) */
    const f32 height = longest > 1.0e-6f ? fabsf(n[0]) / longest : 0.0f;
    int rows = (int)(height / h) + 1;
    if (rows > kMaxRows)
        rows = kMaxRows;
    f->kind = kind;
    f->rings = 0;
    for (int i = 0; i <= rows; i++) {
        const f32 t = (f32)i / (f32)rows;   /* 0: on the edge b - c; 1: at a */
        const f32 p0[3] = {b[0] + (a[0] - b[0]) * t, b[1] + (a[1] - b[1]) * t, b[2] + (a[2] - b[2]) * t};
        const f32 p1[3] = {c[0] + (a[0] - c[0]) * t, c[1] + (a[1] - c[1]) * t, c[2] + (a[2] - c[2]) * t};
        int cols = (int)(longest * (1.0f - t) / h) + 1;
        if (cols > kMaxCols)
            cols = kMaxCols;
        for (int j = 0; j <= cols; j++) {
            const f32 s = (f32)j / (f32)cols;
            put(f, p0[0] + (p1[0] - p0[0]) * s, p0[1] + (p1[1] - p0[1]) * s, p0[2] + (p1[2] - p0[2]) * s);
        }
    }
    if (kind == kRound)
        return;
    for (int bin = 0; bin < PcRadial::kBins; bin++) {
        if (!(f->rings & (1ull << bin)))
            continue;
        const int c0 = cell_of(f->lo[bin]), c1 = cell_of(f->hi[bin]);
        for (int k = c0; k <= c1; k++)
            p->cells[bin][k >> 6] |= 1ull << (k & 63);
    }
}

bool cell_set(const PcRadial *p, int bin, int k)
{
    return (p->cells[bin][k >> 6] >> (k & 63)) & 1;
}

/* A ring's filled cells as runs (no more than kGap empty inside one). */
enum { kRuns = 8 };                         /* per ring at most (the rest joins the last) */
struct Run {
    short first, last;                      /* cells */
    short piece;                            /* its piece (the index of a run: the piece's root) */
};

struct Runs {
    Run run[PcRadial::kBins][kRuns];
    int count[PcRadial::kBins];
    short runsOf[PcRadial::kBins * kRuns], hidden[PcRadial::kBins * kRuns];
};

int root_of(Runs *r, int id)
{
    for (;;) {
        Run *x = &r->run[id / kRuns][id % kRuns];
        if (x->piece == id)
            return id;
        id = x->piece;
    }
}

/* The pieces of the filled cells, which of them are layers behind, and the
 * rings closed all round: the wheel's back and the first layer behind its
 * body, per ring. */
void layers(PcRadial *p)
{
    Runs *r = new Runs;
    for (int bin = 0; bin < PcRadial::kBins; bin++) {
        int n = 0, last = -1000;
        for (int k = 0; k < PcRadial::kCells; k++) {
            if (!cell_set(p, bin, k))
                continue;
            if (n == 0 || (k - last - 1 > kGap && n < kRuns)) {
                r->run[bin][n].first = (short)k;
                r->run[bin][n].piece = (short)(bin * kRuns + n);
                n++;
            }
            r->run[bin][n - 1].last = (short)k;
            last = k;
        }
        r->count[bin] = n;
    }
    /* runs of neighbouring rings that reach each other (kGap apart at most) are one piece */
    for (int bin = 0; bin + 1 < PcRadial::kBins; bin++) {
        for (int i = 0; i < r->count[bin]; i++) {
            for (int j = 0; j < r->count[bin + 1]; j++) {
                const Run &a = r->run[bin][i], &b = r->run[bin + 1][j];
                if (a.first > b.last + kGap + 1 || b.first > a.last + kGap + 1)
                    continue;
                const int ra = root_of(r, bin * kRuns + i), rb = root_of(r, (bin + 1) * kRuns + j);
                if (ra != rb)
                    r->run[rb / kRuns][rb % kRuns].piece = (short)ra;
            }
        }
    }
    /* a run has something else in front of it when the ring's first run is
     * another piece's, or anything at all (a round triangle too) lies more
     * than kGap in front of the ring's first run */
    memset(r->runsOf, 0, sizeof(r->runsOf));
    memset(r->hidden, 0, sizeof(r->hidden));
    for (int bin = 0; bin < PcRadial::kBins; bin++) {
        if (r->count[bin] == 0)
            continue;
        const int first = root_of(r, bin * kRuns);
        const bool roundInFront = p->has[bin] && cell_of(p->lo[bin]) < r->run[bin][0].first - kGap;
        for (int i = 0; i < r->count[bin]; i++) {
            const int piece = root_of(r, bin * kRuns + i);
            r->runsOf[piece]++;
            if (roundInFront || piece != first)
                r->hidden[piece]++;
        }
    }
    p->pieces = p->layers = 0;
    for (int id = 0; id < PcRadial::kBins * kRuns; id++) {
        if (r->runsOf[id] == 0)
            continue;
        p->pieces++;
        if (r->hidden[id] * 2 > r->runsOf[id])
            p->layers++;
    }
    for (int bin = 0; bin < PcRadial::kBins; bin++) {
        p->hasBack[bin] = 0;
        p->closed[bin] = 0;
        int last = -1;
        for (int i = 0; i < r->count[bin]; i++) {
            const int piece = root_of(r, bin * kRuns + i);
            if (r->hidden[piece] * 2 > r->runsOf[piece])
                continue;
            if (r->run[bin][i].last > last)
                last = r->run[bin][i].last;
        }
        if (last < 0)
            continue;
        p->hasBack[bin] = 1;
        p->back[bin] = kX0 + (f32)(last + 1) * kCell;   /* (the cell's far edge: up to a cell too deep, never too
                                                         * shallow) */
        p->behind[bin] = p->back[bin] + kNone;
        for (int i = 0; i < r->count[bin]; i++) {
            const int piece = root_of(r, bin * kRuns + i);
            if (r->hidden[piece] * 2 > r->runsOf[piece] && r->run[bin][i].first > last) {
                p->behind[bin] = kX0 + (f32)r->run[bin][i].first * kCell;
                break;
            }
        }
        /* closed all round: every direction's first face is the body's */
        if (p->faced[bin] != 0xFFFFFFFFu)
            continue;
        bool closed = true;
        f32 deepest = 0.0f;
        for (int s = 0; s < PcRadial::kSectors && closed; s++) {
            const int c = cell_of(p->front[bin][s]);
            int run = -1;
            for (int i = 0; i < r->count[bin]; i++)
                if (c >= r->run[bin][i].first && c <= r->run[bin][i].last)
                    run = i;
            if (run < 0) {
                closed = false;
                break;
            }
            const int piece = root_of(r, bin * kRuns + run);
            if (r->hidden[piece] * 2 > r->runsOf[piece])
                closed = false;             /* (this way one sees through to a layer behind) */
            if (s == 0 || p->front[bin][s] > deepest)
                deepest = p->front[bin][s];
        }
        if (closed && deepest < p->back[bin]) {
            p->closed[bin] = 1;
            p->back[bin] = deepest;
        }
    }
    delete r;
}

int env_int(const char *name, int otherwise)
{
    const char *e = getenv(name);
    return e != NULL && e[0] != '\0' ? atoi(e) : otherwise;
}

u32 sum(u32 h, const void *data, size_t n)
{
    const u8 *p = (const u8 *)data;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 16777619u;         /* FNV-1a */
    return h;
}

/* What tells one wheel model from another at the same address: its counts,
 * every packet's header and part records (vertex and primitive counts, the
 * parts' sizes), the part's command stream (its first bytes). */
u32 stamp_of(ModelSet *m, int part)
{
    u32 h = 2166136261u;
    h = sum(h, &m->nParts, sizeof(u16) * 6);
    if (m->packets != NULL) {
        for (int i = 0; i < m->nPackets; i++) {
            const gt3vif::VifModelHeader *v = (const gt3vif::VifModelHeader *)m->packets[i];
            if (v == NULL)
                continue;
            h = sum(h, v, sizeof(*v));
            if (v->nParts <= 64)
                h = sum(h, v + 1, sizeof(gt3vif::VifPartRecord) * v->nParts);
        }
    }
    if (m->parts != NULL && part >= 0 && part < m->nParts && m->parts[part] != NULL) {
        const u8 *p = m->parts[part];
        size_t n = 0;
        while (n < 64 && p[n] != 0)
            n++;
        h = sum(h, p, n);
    }
    return h;
}

enum { kKept = 4 };
struct Kept {
    ModelSet *m;
    int part, lod;
    u32 stamp;
    u32 used;
    PcRadial rings;
};
Kept g_kept[kKept];
u32 g_use;

} // namespace

const PcRadial *pc_radial_wheel(ModelSet *m, int part, int lod)
{
    if (m == NULL)
        return NULL;
    const u32 stamp = stamp_of(m, part);
    int oldest = 0;
    for (int i = 0; i < kKept; i++) {
        Kept &k = g_kept[i];
        if (k.m == m && k.part == part && k.lod == lod && k.stamp == stamp) {
            k.used = ++g_use;
            return &k.rings;
        }
        if (k.used < g_kept[oldest].used)
            oldest = i;
    }
    Kept &k = g_kept[oldest];
    k.m = m;
    k.part = part;
    k.lod = lod;
    k.stamp = stamp;
    k.used = ++g_use;
    pc_radial_clear(&k.rings, kPcRadialMax);
    pc_radial_model(&k.rings, m, part, lod);
    return &k.rings;
}

void pc_radial_clear(PcRadial *out, f32 rMax)
{
    memset(out, 0, sizeof(*out));
    out->step = rMax / (f32)PcRadial::kBins;
}

void pc_radial_model(PcRadial *out, ModelSet *m, int part, int lod)
{
    Fill *f = new Fill;
    f->p = out;
    f->kind = kFace;
    f->rings = 0;
    modelset_pc_walk(m, part, lod, triangle, f, NULL);
    delete f;
    layers(out);
}

void pc_radial_flat(PcRadial *out, f32 x, f32 r0, f32 r1)
{
    int b0 = (int)(r0 / out->step), b1 = (int)(r1 / out->step);
    if (b0 < 0)
        b0 = 0;
    if (b1 >= PcRadial::kBins)
        b1 = PcRadial::kBins - 1;
    for (int b = b0; b <= b1; b++) {
        if (!out->has[b] || x < out->lo[b])
            out->lo[b] = x;
        if (!out->has[b] || x > out->hi[b])
            out->hi[b] = x;
        out->has[b] = 1;
    }
}

f32 pc_radial_push(const PcRadial *wheel, const PcRadial *brake, f32 clear, f32 *gap, f32 *room)
{
    bool any = false, anyBrake = false;
    f32 least = 0.0f, face = 0.0f;
    for (int b = 0; b < PcRadial::kBins; b++) {
        if (brake->has[b] && (!anyBrake || brake->lo[b] < face))
            face = brake->lo[b];
        anyBrake = anyBrake || brake->has[b];
    }
    f32 free = kNone;
    for (int b = 0; b < PcRadial::kBins; b++) {
        if (!brake->has[b] || !wheel->hasBack[b])
            continue;
        const f32 back = wheel->back[b], behind = wheel->behind[b];
        const f32 g = brake->lo[b] - back;
        if (!any || g < least)
            least = g;
        any = true;
        /* the wheel's next layer behind the brake's face (a part of the
         * brake already behind it - its own back plate - is not its face),
         * where the brake shows: not in a ring closed all round, nor in the
         * innermost ones (a hub's own little plate behind its cap stopped
         * the 180SX's disc 4 mm short of a wheel 11 mm into it; so near the
         * axle the directions are too narrow to tell a closed ring) */
        if (b >= kInner && !wheel->closed[b] && brake->lo[b] <= face + kFaceBand && behind > brake->lo[b] &&
            behind - brake->lo[b] < free)
            free = behind - brake->lo[b];
    }
    if (room != NULL)
        *room = free;
    if (!any) {
        if (gap != NULL)
            *gap = clear + 1.0f;
        return 0.0f;
    }
    if (gap != NULL)
        *gap = least;
    return least < clear ? clear - least : 0.0f;
}

bool pc_radial_same(const PcRadial *wheel, const PcRadial *brake)
{
    int shared = 0, same = 0;
    for (int b = 0; b < PcRadial::kBins; b++) {
        if (!brake->has[b] || !wheel->has[b])
            continue;
        shared++;
        const int c = cell_of(brake->lo[b]);
        for (int k = c - 1; k <= c + 1; k++) {
            if (k < 2 || k >= PcRadial::kCells || !cell_set(wheel, b, k))
                continue;
            if (!cell_set(wheel, b, k - 1) && !cell_set(wheel, b, k - 2)) {
                same++;                     /* (a surface of the wheel starts at the brake's face) */
                break;
            }
        }
    }
    return shared > 0 && same * 2 > shared;
}

bool pc_brake_fit_on(void)
{
    static int on = -1;
    if (on < 0)
        on = env_int("GT3PC_BRAKE_FIT", 1);
    return on != 0;
}

bool pc_brake_fit_log(void)
{
    static int log = -1;
    if (log < 0)
        log = env_int("GT3PC_BRAKE_FIT_LOG", 0);
    return log != 0;
}

f32 pc_brake_fit(const PcRadial *wheel, const PcRadial *brake, f32 rim, f32 width, PcBrakeFit *found)
{
    PcBrakeFit none;
    if (found == NULL)
        found = &none;
    found->gap = 0.0f;
    found->limited = found->own = found->apart = false;
    if (!pc_brake_fit_on() || wheel == NULL || rim <= 0.0f)
        return 0.0f;
    if (pc_radial_same(wheel, brake)) {
        found->own = true;                  /* (the wheel's own brake is where the car's is: nothing to clear) */
        return 0.0f;
    }
    const f32 clear = kPcBrakeClear / rim;
    f32 gap = 0.0f, free = 0.0f;
    const f32 need = pc_radial_push(wheel, brake, clear, &gap, &free) * rim;
    found->gap = gap * rim;
    found->apart = gap > clear + 0.5f;
    if (need <= 0.0f)
        return 0.0f;
    f32 push = need < kPcBrakeMaxPush ? need : kPcBrakeMaxPush;
    /* the brake's face (its least x anywhere) stays inside the tyre... */
    f32 face = 0.0f;
    bool any = false;
    for (int b = 0; b < PcRadial::kBins; b++) {
        if (brake->has[b] && (!any || brake->lo[b] < face))
            face = brake->lo[b];
        any = any || brake->has[b];
    }
    f32 most = (width - kPcBrakeKeepIn) - face * rim;
    /* ...and in front of the wheel's next layer */
    if (free < kNone * 0.5f && free * rim - kPcBrakeClear < most)
        most = free * rim - kPcBrakeClear;
    if (push > most)
        push = most > 0.0f ? most : 0.0f;
    found->limited = push < need;
    return push;
}
