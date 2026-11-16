/* pc_course_scale.cpp - the course scale (pc_course_scale.h).  No original:
 * new PC code. */
#include "game/app/pc_course_scale.h"
#include "game/app/pc_detail.h"                         /* pc_vision_parts_reset */
#include "game/camera/T172_course_camera.h"             /* CourseCamera, CourseCameraRecord, InterpTable */
#include "game/car/physics/T167_rigid_body.h"           /* WallSeg, GadgetDesc */
#include "game/course/T178_course_env.h"                /* CourseEnv */
#include "game/course/T179_course_data.h"               /* CourseData */
#include "game/course/T453_vision_list.h"               /* VisionList */
#include "game/course/T457_runway.h"                    /* Runway and its records */
#include "game/effects/T159_flare.h"                    /* FlareSource */
#include "game/race/ai/T368_enemy_lines.h"              /* EnemyLines, Line, LineNode */
#include "game/race/ai/enemy_lines_internal.h"          /* the line file's fixed point, LINE_* */
#include "game/race/core/T129_race_base.h"              /* RaceInfoBlock */
#include "game/race/core/T153_race_course.h"            /* RaceCourse */
#include "game/race/display/race_display_draw_internal.h"   /* FlareSourceData, FlareGroupTable */
#include "game/sound/T458_course_sound.h"               /* CourseSound */
#include "engine/gfx/T451_model_set.h"                  /* modelset_get / put_projection_scale */
#include "engine/pgl/G001_flare.h"                      /* pgl_push_matrix, vu0_scale, pgl_set_world_scale */
#include "platform/settings.h"
#include "game/app/pc_challenge.h"                      /* PC (2026-11-14): Course Shrinkage */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <unordered_set>
#include <vector>

namespace {

/* The scale a RaceInfoBlock records in its spare bytes (unk_080, cleared by
 * RaceBase::virtual_7): the tag, then x, y, z in percent (s16 each: the
 * saves before the mirrored scales read the same, 25 .. 400). */
constexpr u32 kBlockTag = 0x4C435343;                   /* "CSCL" */
/* PC (2026-11-14): "CSCT", the same in tenths of a percent (Course
 * Shrinkage's 0.2 % steps; written when an axis is not a whole percent) */
constexpr u32 kBlockTenthsTag = 0x54435343;

/* The lap remap of a scaled runway: over one lap, old distances (from,
 * ascending, from[0] = 0) to new ones (to); linear between them. */
struct LapMap {
    std::vector<f32> from, to;
    f32 lap = 0.0f, newLap = 0.0f;

    f32 map(f32 d) const
    {
        if (!(lap > 0.0f) || from.empty())
            return d;
        const f32 laps = floorf(d / lap);
        const f32 x = d - laps * lap;
        size_t i = (size_t)(std::upper_bound(from.begin(), from.end(), x) - from.begin());
        i = i == 0 ? 0 : i - 1;
        const f32 x1 = i + 1 < from.size() ? from[i + 1] : lap;
        const f32 y1 = i + 1 < from.size() ? to[i + 1] : newLap;
        const f32 span = x1 - from[i];
        const f32 y = span > 0.0f ? to[i] + (x - from[i]) * (y1 - to[i]) / span : to[i];
        return y + laps * newLap;
    }
};

/* A loaded course and its scale. */
struct Entry {
    const CourseData *cd = nullptr;
    const Runway *runway = nullptr;
    const RaceMiniMapData *map = nullptr;
    f32 s[3] = {1.0f, 1.0f, 1.0f};
    bool scaled = false;
    LapMap lap;
};
Entry g_entries[4];
int g_nextEntry;

/* A race organization's course and the scale it loads at (settings: the
 * settings' as they are when it loads; then the scale it took). */
struct Bind {
    const RaceCourse *course = nullptr;
    f32 s[3] = {1.0f, 1.0f, 1.0f};
    bool settings = false;
};
Bind g_binds[8];
int g_nextBind;

/* The course's models being drawn scaled (pc_course_draw_begin / _end):
 * per level, whether it is scaled and the projection scale it found. */
struct DrawLevel {
    bool scaled;
    f32 projection;
};
DrawLevel g_draw[8];
int g_drawDepth, g_drawScaled;

const Entry *entry_of(const CourseData *cd)
{
    for (const Entry &e : g_entries)
        if (cd != nullptr && e.cd == cd)
            return &e;
    return nullptr;
}

const Bind *bind_of(const RaceCourse *course)
{
    for (const Bind &b : g_binds)
        if (course != nullptr && b.course == course)
            return &b;
    return nullptr;
}

bool is_one(const f32 s[3])
{
    return s[0] == 1.0f && s[1] == 1.0f && s[2] == 1.0f;
}

/* The largest stretch (a mirrored axis by its size). */
f32 largest(const f32 s[3])
{
    return std::max(fabsf(s[0]), std::max(fabsf(s[1]), fabsf(s[2])));
}

/* The ground's stretch (x and z: the geometric mean of their sizes). */
f32 flat_of(const f32 s[3])
{
    return sqrtf(fabsf(s[0] * s[2]));
}

/* Mirrored on the ground: one of x, z negative (a left turn a right one; a
 * y alone flips no handedness on the ground). */
bool mirrored_xz(const f32 s[3])
{
    return s[0] * s[2] < 0.0f;
}

/* The settings' scale. */
void settings_scale(f32 s[3])
{
    /* PC (2026-11-14): the challenge Course Shrinkage in a GT mode race
     * (game/app/pc_challenge.h): every axis by the day, in place of the
     * settings' */
    const int tenths = pc_challenge_race_course();
    if (tenths != 0) {
        for (int a = 0; a < 3; a++)
            s[a] = (f32)tenths / 1000.0f;
        return;
    }
    for (int a = 0; a < 3; a++)
        s[a] = (f32)settings::course_scale(a) / 100.0f;
}

/* PC (2026-11-14): tenths of a percent held to what any axis could once be
 * set to (clamp_course_scale_recorded's range, x 10) */
int clamp_tenths_recorded(int tenths)
{
    const int lo = settings::kCourseScaleMin * 10, hi = settings::kCourseScaleMax * 10;
    const int nearest = settings::kCourseScaleNearest * 10;
    int t = tenths < lo ? lo : (tenths > hi ? hi : tenths);
    if (t > -nearest && t < nearest)
        t = t < 0 ? -nearest : nearest;
    return t;
}

/* The scale blk recorded (none: 100 %); false when none.  Held to the range
 * any axis could once be set to, not the setting's own (a replay made before
 * the height's floor, upside down, plays as it was run). */
bool block_scale(const RaceInfoBlock *blk, f32 s[3])
{
    s[0] = s[1] = s[2] = 1.0f;
    u32 tag;
    s16 pct[3];
    memcpy(&tag, blk->unk_080, 4);
    memcpy(pct, blk->unk_080 + 4, sizeof(pct));
    if (tag == kBlockTenthsTag) {
        /* PC (2026-11-14): tenths of a percent */
        for (int a = 0; a < 3; a++)
            s[a] = (f32)clamp_tenths_recorded(pct[a]) / 1000.0f;
        return true;
    }
    if (tag != kBlockTag)
        return false;
    for (int a = 0; a < 3; a++)
        s[a] = (f32)settings::clamp_course_scale_recorded(pct[a]) / 100.0f;
    return true;
}

void scale3(f32 *v, const f32 s[3])
{
    v[0] *= s[0];
    v[1] *= s[1];
    v[2] *= s[2];
}

/* A direction through the scale, normalised (as it was, if 0). */
void scale_dir(f32 *v, const f32 s[3])
{
    const f32 x = v[0] * s[0], y = v[1] * s[1], z = v[2] * s[2];
    const f32 l = sqrtf(x * x + y * y + z * z);
    if (l > 0.0f) {
        v[0] = x / l;
        v[1] = y / l;
        v[2] = z / l;
    }
}

/* A heading turned with the stretch: its forwards (-sin h, cos h) on (x, z)
 * - or the lines' (x, -z), the same - through the scale. */
f32 scale_heading(f32 h, const f32 s[3])
{
    return atan2f(s[0] * sinf(h), s[2] * cosf(h));
}

/* ---- the runway ---- */

/* The lap remap: each lap line at its distance with the stretch of the road
 * there (its normal (a, b) is the road's way on (x, z)); between two lines
 * their mean, before the first and after the last the mean of the last and
 * the first on a looped course (else their own).  Without lines: by the
 * mean of x and z. */
void build_lap_map(const Runway *r, const f32 s[3], LapMap *m)
{
    m->from.clear();
    m->to.clear();
    m->lap = r->lapLength;
    const f32 flat = flat_of(s);
    m->newLap = r->lapLength * flat;
    m->from.push_back(0.0f);
    m->to.push_back(0.0f);
    if (!(r->lapLength > 0.0f) || r->lineCount <= 0 || r->lines == nullptr)
        return;
    struct Point {
        f32 d, k;
    };
    std::vector<Point> pts;
    for (int i = 0; i < r->lineCount; i++) {
        const RunwayLine &l = r->lines[i];
        f32 d = fmodf(l.dist, r->lapLength);
        if (d < 0.0f)
            d += r->lapLength;
        const f32 n = sqrtf(l.a * l.a + l.b * l.b);
        const f32 k = n > 0.0f ? sqrtf(s[0] * l.a * s[0] * l.a + s[2] * l.b * s[2] * l.b) / n : flat;
        pts.push_back({d, k});
    }
    std::sort(pts.begin(), pts.end(), [](const Point &a, const Point &b) { return a.d < b.d; });
    const Point &first = pts.front(), &last = pts.back();
    const f32 wrap = (first.k + last.k) * 0.5f;
    const f32 head = r->looped ? wrap : first.k, tail = r->looped ? wrap : last.k;
    f32 acc = first.d * head;
    if (first.d > 0.0f) {
        m->from.push_back(first.d);
        m->to.push_back(acc);
    }
    for (size_t i = 1; i < pts.size(); i++) {
        acc += (pts[i].d - pts[i - 1].d) * (pts[i - 1].k + pts[i].k) * 0.5f;
        if (pts[i].d > m->from.back()) {
            m->from.push_back(pts[i].d);
            m->to.push_back(acc);
        }
    }
    m->newLap = acc + (r->lapLength - last.d) * tail;
}

/* An xz cell map's bounds (x ones, then z ones).  A mirrored axis's bounds
 * come out descending: reversed (runway_find_interval searches ascending
 * ones), and the cells with them (cell (i, j) at j x (nx - 1) + i). */
void scale_cell_map(RunwayCellMap *m, const f32 s[3])
{
    const int nx = m->nx, nz = m->nz;
    for (int i = 0; i < nx; i++)
        m->bounds[i] *= s[0];
    for (int i = 0; i < nz; i++)
        m->bounds[nx + i] *= s[2];
    if (nx < 2 || nz < 2)
        return;
    if (s[0] < 0.0f) {
        std::reverse(m->bounds, m->bounds + nx);
        for (int j = 0; j < nz - 1; j++)
            std::reverse(m->cells + j * (nx - 1), m->cells + (j + 1) * (nx - 1));
    }
    if (s[2] < 0.0f) {
        std::reverse(m->bounds + nx, m->bounds + nx + nz);
        for (int j = 0; j < (nz - 1) / 2; j++)
            std::swap_ranges(m->cells + j * (nx - 1), m->cells + (j + 1) * (nx - 1),
                             m->cells + (nz - 2 - j) * (nx - 1));
    }
}

/* fn(sector, item) for each item of the cell map's cells. */
template <class Fn> void for_each_item(const RunwayCellMap *m, Fn fn)
{
    const int cells = (m->nx - 1) * (m->nz - 1);
    for (int c = 0; c < cells; c++) {
        const u16 *p = m->cells[c];
        if (p == nullptr)
            continue;
        for (; p[0] != 0; p += p[0] + 2)
            for (int k = 0; k < p[0]; k++)
                fn(p[1], p[2 + k]);
    }
}

/* A count word of the runway's unknown bytes (? names: unk_28 the grid's
 * records, unk_38 the walls - 6 and the walls the pointMap lists in the
 * files looked at, docs/launcher.md "Course scale"). */
int runway_word(const u8 *p)
{
    int v;
    memcpy(&v, p, 4);
    return v;
}

/* The walls on a mirrored ground (mirrored_xz): T376 / T167 keep the road
 * on one side of every segment by the sign of a cross product, which the
 * mirror turns - so each polyline's order is reversed.  A loop (the first
 * vertex's link +n, the last's -n: every wall of the US courses) keeps its
 * first vertex, the rest backwards, the links staying with the places; any
 * other run is reversed whole.  A vertex keeps its own flags but the soft
 * one, which belongs to the segment it starts (to the new start of that
 * segment).  The pointMap's wall indices follow (each list once). */
void reverse_walls(Runway *r, int walls)
{
    if (walls <= 1 || r->walls == nullptr)
        return;
    const std::vector<WallSeg> old(r->walls, r->walls + walls);
    std::vector<int> newOf((size_t)walls);
    for (int i = 0; i < walls; i++)
        newOf[(size_t)i] = i;
    for (int f = 0; f < walls;) {
        int l = f;
        while (l < walls - 1 && old[(size_t)l].link >= 0)
            l++;
        const int n = l - f + 1;
        const bool loop = n > 1 && old[(size_t)f].link == n && old[(size_t)l].link == -n;
        for (int p = 0; p < n; p++) {
            /* the old vertex now at f + p, and the start of the old segment
             * that is now this vertex's (none for an open run's last) */
            const int from = loop ? f + (n - p) % n : l - p;
            const int segFrom = loop ? f + (n - p - 1) % n : (p < n - 1 ? l - p - 1 : from);
            WallSeg &w = r->walls[f + p];
            w.x = old[(size_t)from].x;
            w.y = old[(size_t)from].y;
            w.z = old[(size_t)from].z;
            w.flags = (u16)((old[(size_t)from].flags & ~WALL_SOFT) | (old[(size_t)segFrom].flags & WALL_SOFT));
            newOf[(size_t)from] = f + p;
        }
        f = l + 1;
    }
    RunwayCellMap *m = r->pointMap;
    if (m == nullptr || m->nx < 2 || m->nz < 2)
        return;
    std::unordered_set<u16 *> done;         /* (each index once: cells may share their lists) */
    const int cells = (m->nx - 1) * (m->nz - 1);
    for (int c = 0; c < cells; c++) {
        u16 *p = m->cells[c];
        if (p == nullptr)
            continue;
        for (; p[0] != 0; p += p[0] + 2)
            for (int k = 0; k < p[0]; k++)
                if (p[2 + k] < walls && done.insert(&p[2 + k]).second)
                    p[2 + k] = (u16)newOf[p[2 + k]];
    }
}

void scale_runway(Runway *r, const f32 s[3], LapMap *lm)
{
    build_lap_map(r, s, lm);
    /* the lap lines: x and z; a x + b z + c holds for the scaled points
     * with a / sx, b / sz (not normalised: a point between two lines keeps
     * its fraction, so its distance follows the remap); the distances */
    const bool mirror = mirrored_xz(s);
    for (int i = 0; i < r->lineCount; i++) {
        RunwayLine &l = r->lines[i];
        l.x0 *= s[0];
        l.x1 *= s[0];
        l.z0 *= s[2];
        l.z1 *= s[2];
        l.a /= s[0];
        l.b /= s[2];
        l.dist = lm->map(l.dist);
        /* mirrored: the ends swapped (runway_sector_lap_distance tells the
         * sides of the stretch between two lines by the cross products of
         * their first and second ends' joins, which the mirror turns) */
        if (mirror) {
            std::swap(l.x0, l.x1);
            std::swap(l.z0, l.z1);
        }
    }
    if (r->unk_1C != 0.0f)
        r->unk_1C = lm->map(r->unk_1C);
    for (int i = 0; i < r->markerCount; i++)
        if (r->markers[i].type == 2)
            r->markers[i].value = lm->map(r->markers[i].value);
    r->lapLength = lm->newLap;

    /* the road: the vertices of the triangles the triMap lists (the queries
     * reach no others), each once; mirrored, each triangle's winding turned
     * back (runway_query's plane is the cross product of its edges: up again,
     * as the contacts' plane, which is up by construction) */
    std::unordered_set<RunwayVertex *> done;
    std::unordered_set<RunwayTri *> turned;
    for_each_item(r->triMap, [&](u16 sector, u16 item) {
        if (sector >= r->sectorCount)
            return;
        const RunwaySector &sec = r->sectors[sector];
        RunwayTri &t = sec.tris[item];
        for (int k = 0; k < 3; k++) {
            RunwayVertex *v = &sec.verts[t.v[k]];
            if (done.insert(v).second) {
                v->x *= s[0];
                v->y *= s[1];
                v->z *= s[2];
            }
        }
        if (mirror && turned.insert(&t).second)
            std::swap(t.v[1], t.v[2]);
    });
    scale_cell_map(r->triMap, s);
    if (r->pointMap != r->triMap)
        scale_cell_map(r->pointMap, s);

    /* the walls: as many as the pointMap reaches (unk_38 when it agrees) */
    int walls = 0;
    for_each_item(r->pointMap, [&](u16, u16 item) { walls = std::max(walls, item + 1); });
    const int stated = runway_word(r->unk_38);
    if (stated >= walls && stated <= 0x10000)
        walls = stated;
    for (int i = 0; i < walls; i++) {
        WallSeg &w = r->walls[i];
        w.x *= s[0];
        w.y *= s[1];
        w.z *= s[2];
    }
    if (mirror)
        reverse_walls(r, walls);

    /* the start grid (unk_28 records, 1 at least) */
    int grid = runway_word(r->unk_28);
    grid = grid < 1 ? 1 : grid > 64 ? 64 : grid;
    for (int i = 0; i < grid; i++) {
        RunwayGrid &g = r->grid[i];
        g.x *= s[0];
        g.y *= s[1];
        g.z *= s[2];
        g.angle = scale_heading(g.angle, s);
    }

    /* the gadgets' places (their shapes keep their size), the lights along
     * the road (radius 0: a direction) */
    for (u32 i = 0; i < r->objectCount; i++)
        scale3(&r->objects[i].pos.x, s);
    for (int i = 0; i < r->lightCount; i++) {
        RunwayLight &l = r->lights[i];
        if (l.radius == 0.0f)
            continue;
        scale3(l.pos, s);
        scale_dir(l.dir, s);
    }
}

/* ---- the other blocks ---- */

void scale_vision(VisionList *v, const f32 s[3])
{
    for (int i = 0; i < v->nPoints; i++) {
        VisionPoint &p = v->points[i];
        p.x *= s[0];
        p.y *= s[1];
        p.z *= s[2];
    }
    /* the grid: (x - minX) x nx x scaleX picks the cell */
    v->minX *= s[0];
    v->minZ *= s[2];
    v->scaleX /= s[0];
    v->scaleZ /= s[2];
}

void scale_flares(FlareSourceData *f, const f32 s[3])
{
    if (f->table == nullptr)
        return;
    std::unordered_set<FlareGroup *> done;
    for (int g = 0; g < f->table->count; g++) {
        FlareGroup *group = f->table->group[g];
        if (group == nullptr || !done.insert(group).second)
            continue;
        for (int i = 0; i < group->count; i++) {
            scale3(&group->src[i].pos.x, s);
            scale_dir(&group->src[i].dir.x, s);
        }
    }
}

void scale_sound(CourseSound *snd, const f32 s[3])
{
    std::unordered_set<SoundSource *> done;
    for (SoundSourceList *list : snd->groups) {
        if (list == nullptr)
            continue;
        for (int i = 0; i < list->count; i++) {
            SoundSource *src = list->items[i];
            if (src == nullptr || !done.insert(src).second)
                continue;
            for (u32 k = 0; k < src->emitterCount; k++)
                scale3(&src->emitters[k].x, s);
            for (SoundSectionEnd &e : src->ends)
                scale3(&e.x, s);
        }
    }
}

/* The far clip, the fogs' start (a distance: the fog of a point at a
 * scaled distance stays), the reach of the lights along the road and of the
 * flares; a positional light's place. */
void scale_env(CourseEnv *env, const f32 s[3])
{
    /* (sizes: a mirrored axis by its size) */
    const f32 far = std::max(1.0f, largest(s)), flat = flat_of(s), mean = cbrtf(fabsf(s[0] * s[1] * s[2]));
    env->farClip *= far;
    env->fog.nearZ *= flat;
    env->mirrorFog.nearZ *= flat;
    env->reflFog.nearZ *= flat;
    for (CourseEnvLightA &l : env->lightA) {
        l.att[1] /= mean;
        l.att[2] /= mean * mean;
        l.glowSize *= mean;
        if (l.pos[3] != 0.0f)
            scale3(l.pos, s);
    }
    for (CourseEnvLightB &l : env->lightB)
        if (l.pos[3] != 0.0f)
            scale3(l.pos, s);
    /* FlareClass::setFlareType's v[4..13]: size, att1, att2, far */
    for (CourseEnvFlareType &t : env->flareTypes) {
        t.v[5] *= mean;
        t.v[9] /= mean;
        t.v[10] /= mean * mean;
        t.v[12] *= far;
    }
}

/* The course cameras: their ranges on the lap remapped; the points of
 * those in course coordinates (mode 0) scaled; of these, those that aim at
 * a car keep it framed - their zoom limits (and their zoom table, unless the
 * zoom follows the distance already) times the distance's stretch. */
void scale_cameras(CourseCamera *c, const f32 s[3], const LapMap &lm)
{
    const f32 k = flat_of(s);
    std::unordered_set<InterpTable *> done;
    auto table = [&](InterpTable *t, f32 f) {
        if (t == nullptr || !done.insert(t).second)
            return;
        for (int i = 0; i < t->n; i++)
            t->pts[i].y *= f;
    };
    auto records = [&](CourseCameraRecord *rec, int n) {
        for (int i = 0; rec != nullptr && i < n; i++) {
            CourseCameraRecord &r = rec[i];
            r.start = lm.map(r.start);
            r.end = lm.map(r.end);
            if ((r.flags & 3) != 0)
                continue;                   /* (relative to the car) */
            for (int a = 0; a < 3; a++) {
                table(r.pos[a], s[a]);
                table(r.target[a], s[a]);
            }
            if (r.flags & 0xC) {
                r.zoomMin *= k;
                r.zoomMax *= k;
                if (!(r.flags & 0x10))
                    table(r.zoom, k);
            }
        }
    };
    c->length = lm.map(c->length);
    c->end = lm.map(c->end);
    records(c->records, c->count);
    records(c->records2, c->count2);
    for (u32 i = 0; c->zones != nullptr && i < c->zoneCount; i++) {
        c->zones[i].a = lm.map(c->zones[i].a);
        c->zones[i].b = lm.map(c->zones[i].b);
    }
}

} // namespace

void pc_course_scale_bind(const RaceCourse *course, const RaceInfoBlock *blk)
{
    Bind *b = nullptr;
    for (Bind &x : g_binds)
        if (x.course == course)
            b = &x;
    if (b == nullptr) {
        b = &g_binds[g_nextBind];
        g_nextBind = (g_nextBind + 1) % (int)(sizeof(g_binds) / sizeof(g_binds[0]));
    }
    b->course = course;
    b->settings = blk == nullptr;
    if (blk == nullptr) {
        settings_scale(b->s);
        /* PC (2026-11-14; the game's printf has no usable %g) */
        if (const int tenths = pc_challenge_race_course())
            printf("PC: course scale: Course Shrinkage, %d.%d %% on every axis\n", tenths / 10, tenths % 10);
        return;
    }
    if (block_scale(blk, b->s) && !is_one(b->s))
        printf("PC: course scale: the saved race's own, x %g y %g z %g\n", b->s[0], b->s[1], b->s[2]);
}

void pc_course_scale_apply(const RaceCourse *course, CourseData *cd)
{
    if (cd == nullptr)
        return;
    pc_vision_parts_reset();                /* (another course may be at this address) */
    f32 s[3];
    Bind *b = const_cast<Bind *>(bind_of(course));
    if (b != nullptr && b->settings) {
        /* the settings as they are now (the F2 menu may have changed them);
         * kept from then on: the organization's course loaded again (its
         * replay after a save borrowed the arena) stays at that scale */
        settings_scale(b->s);
        b->settings = false;
    }
    if (b != nullptr)
        memcpy(s, b->s, sizeof(s));
    else
        settings_scale(s);
    Entry *e = nullptr;
    for (Entry &x : g_entries)
        if (x.cd == cd)
            e = &x;
    if (e == nullptr) {
        e = &g_entries[g_nextEntry];
        g_nextEntry = (g_nextEntry + 1) % (int)(sizeof(g_entries) / sizeof(g_entries[0]));
    }
    *e = Entry();
    e->cd = cd;
    e->runway = cd->runway;
    e->map = cd->miniMap;
    memcpy(e->s, s, sizeof(s));
    e->scaled = !is_one(s);
    if (!e->scaled)
        return;

    const f32 lapBefore = cd->runway != nullptr ? cd->runway->lapLength : 0.0f;
    if (cd->runway != nullptr)
        scale_runway(cd->runway, s, &e->lap);
    /* the vision lists (one block may stand in several slots) */
    std::unordered_set<VisionList *> lists;
    for (VisionList *v : {cd->visionList, cd->visionMirror, cd->visionSub, cd->visionMirrorAlt, cd->envVision,
                          cd->vis28, cd->vis38})
        if (v != nullptr && lists.insert(v).second)
            scale_vision(v, s);
    if (cd->flares != nullptr)
        scale_flares(cd->flares, s);
    if (cd->sound != nullptr)
        scale_sound(cd->sound, s);
    if (cd->env != nullptr)
        scale_env(cd->env, s);
    if (cd->replay != nullptr)
        scale_cameras(cd->replay, s, e->lap);
    printf("PC: course scale x %g y %g z %g: lap %.1f m -> %.1f m\n", s[0], s[1], s[2], lapBefore,
           cd->runway != nullptr ? cd->runway->lapLength : 0.0f);
}

void pc_course_scale_store(RaceInfoBlock *blk, const RaceCourse *course)
{
    f32 s[3];
    if (const Bind *b = bind_of(course))
        memcpy(s, b->s, sizeof(s));
    else
        settings_scale(s);
    if (is_one(s))
        return;                             /* (nothing: as the original's block) */
    /* whole percents: "CSCL"; else (PC 2026-11-14: Course Shrinkage's 0.2 %
     * steps) tenths of a percent: "CSCT" */
    s16 v[3];
    bool tenths = false;
    for (int a = 0; a < 3; a++) {
        const long t = lroundf(s[a] * 1000.0f);
        tenths = tenths || t % 10 != 0;
        v[a] = (s16)t;
    }
    if (!tenths)
        for (int a = 0; a < 3; a++)
            v[a] = (s16)(v[a] / 10);
    memcpy(blk->unk_080, tenths ? &kBlockTenthsTag : &kBlockTag, 4);
    memcpy(blk->unk_080 + 4, v, sizeof(v));
}

void pc_course_scale_check_ghost(const RaceInfoBlock *blk, const RaceCourse *course)
{
    f32 g[3], s[3];
    block_scale(blk, g);
    if (const Bind *b = bind_of(course))
        memcpy(s, b->s, sizeof(s));
    else
        settings_scale(s);
    if (memcmp(g, s, sizeof(s)) != 0)
        printf("PC: course scale: the ghost was saved at x %g y %g z %g, the course is at x %g y %g z %g: it "
               "will not follow the road\n",
               g[0], g[1], g[2], s[0], s[1], s[2]);
}

void pc_course_draw_begin(const CourseData *cd)
{
    const Entry *e = entry_of(cd);
    DrawLevel &d = g_draw[std::min(g_drawDepth, 7)];
    g_drawDepth++;
    d.scaled = e != nullptr && e->scaled;
    if (!d.scaled)
        return;
    pgl_push_matrix();
    vu0_scale(e->s[0], e->s[1], e->s[2]);   /* (first: the world scaled, then the view) */
    pgl_set_world_scale(1, e->s[0], e->s[1], e->s[2]);
    g_drawScaled++;
    /* the LODs' projected sizes: the parts are farther by up to the scale */
    d.projection = modelset_get_projection_scale();
    modelset_put_projection_scale(d.projection * largest(e->s));
}

void pc_course_draw_end(const CourseData *cd)
{
    (void)cd;
    if (g_drawDepth == 0)
        return;
    g_drawDepth--;
    const DrawLevel &d = g_draw[std::min(g_drawDepth, 7)];
    if (!d.scaled)
        return;
    pgl_pop_matrix();
    if (--g_drawScaled == 0)
        pgl_set_world_scale(0, 1.0f, 1.0f, 1.0f);
    modelset_put_projection_scale(d.projection);
}

f32 pc_course_far_factor(const CourseData *cd)
{
    const Entry *e = entry_of(cd);
    return e != nullptr && e->scaled ? std::max(1.0f, largest(e->s)) : 1.0f;
}

f32 pc_course_runway_reach(const Runway *r)
{
    /* (8 m: the cells' size, wall vertices up to twice that apart) */
    for (const Entry &e : g_entries)
        if (r != nullptr && e.runway == r && e.scaled)
            return 8.0f * std::max(1.0f, std::max(fabsf(e.s[0]), fabsf(e.s[2])));
    return 8.0f;
}

void pc_course_map_unscale(const RaceMiniMapData *m, f32 *x, f32 *z)
{
    for (const Entry &e : g_entries)
        if (m != nullptr && e.map == m && e.scaled) {
            *x /= e.s[0];
            *z /= e.s[2];
            return;
        }
}

f32 pc_course_lap_distance(const CourseData *cd, f32 dist)
{
    const Entry *e = entry_of(cd);
    return e != nullptr && e->scaled ? e->lap.map(dist) : dist;
}

f32 pc_course_lap_ratio(const CourseData *cd)
{
    const Entry *e = entry_of(cd);
    return e != nullptr && e->scaled && e->lap.lap > 0.0f ? e->lap.newLap / e->lap.lap : 1.0f;
}

int pc_course_scale_lines(const CourseData *cd, EnemyLines *file)
{
    const Entry *e = entry_of(cd);
    if (e == nullptr || !e->scaled)
        return 0;
    const f32 *s = e->s;
    /* mirrored: a left turn is a right one (the radius's sign), and the two
     * side lines of the racing line change sides - their slots swapped, so
     * LINE_SIDE_1 stays on the side it was on */
    const bool mirror = mirrored_xz(s);
    const f32 turn = mirror ? -1.0f : 1.0f;
    if (mirror)
        std::swap(file->line[LINE_SIDE_1], file->line[LINE_SIDE_2]);
    for (int k = 0; k < 8; k++) {
        Line *line = file->line[k];
        if (line == nullptr)
            continue;
        LineNode *nodes = line->node;
        const int count = line->count;
        /* converted as lines_prepare does (the line frame: x, y = -z) */
        std::vector<f32> radius((size_t)count);
        for (int n = 0; n < count; n++) {
            union {
                f32 f;
                s32 i;
            } x, y, a, r;
            x.f = nodes[n].x;
            y.f = nodes[n].y;
            a.f = nodes[n].angle;
            r.f = nodes[n].radius;
            nodes[n].x = -(f32)x.i * U087_FIXED_ONE;
            nodes[n].angle = (f32)a.i * U087_TWO_PI * U087_FIXED_ONE + U087_PI;
            nodes[n].radius = (f32)r.i * U087_FIXED_ONE;
            nodes[n].y = -(f32)y.i * U087_FIXED_ONE;
            radius[(size_t)n] = nodes[n].radius;
            nodes[n].x *= s[0];
            nodes[n].y *= s[2];
            nodes[n].angle = scale_heading(nodes[n].angle, s);
        }
        /* the arcs: x and z alike in size, the radius times it (the turn's
         * sign by the mirror); else the circle tangent to the node's new
         * heading through the next node (the arc's centre is the node -
         * radius (cos h, sin h)); the last node's (no segment follows it) by
         * the mean */
        for (int n = 0; n < count; n++) {
            const f32 r = radius[(size_t)n];
            if (r == 0.0f)
                continue;
            if (fabsf(s[0]) == fabsf(s[2])) {
                nodes[n].radius = r * fabsf(s[0]) * turn;
                continue;
            }
            f32 refit = r * flat_of(s) * turn;
            if (n + 1 < count) {
                const int next = n + 1;
                const f32 dx = nodes[next].x - nodes[n].x, dy = nodes[next].y - nodes[n].y;
                const f32 h = nodes[n].angle;
                const f32 du = dx * cosf(h) + dy * sinf(h);
                const f32 d2 = dx * dx + dy * dy;
                if (fabsf(du) > 1e-3f * sqrtf(d2) && d2 > 0.0f) {
                    const f32 f = -d2 / (2.0f * du);
                    if ((f > 0.0f) == (r * turn > 0.0f))
                        refit = f;
                }
            }
            nodes[n].radius = refit;
        }
    }
    return 1;
}
