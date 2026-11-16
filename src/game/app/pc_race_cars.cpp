/* pc_race_cars.cpp - races of more cars (pc_race_cars.h).  No original:
 * new PC code. */
#include "game/app/pc_race_cars.h"
#include "game/race/core/T153_race_course.h"            /* RaceCourse */
#include "game/course/T179_course_data.h"               /* CourseData (its runway) */
#include "game/course/T457_runway.h"                    /* Runway, RunwayGrid */
#include "game/race/dynamics/T305_dynamics_conductor.h" /* racingLinePoint, getLapLength */
#include "game/race/race_cars.h"                        /* PC_RACE_CARS_MAX */
#include "game/race/core/T129_race_base.h"              /* RaceInfoBlock, RaceInfoSlot */
#include "game/car/physics/T302_car_state.h"            /* RaceCarState (pc_car_runaway) */
#include "game/race/dynamics/pc_physics_threads.h"      /* pc_physics_threads (logged) */
#include "game/car/physics/T347_ground_surface.h"       /* ground_is_off_course (a made grid place) */
#include "game/car/physics/T167_rigid_body.h"           /* WallSeg, WALL_SOFT (a made grid place) */
#include "platform/settings.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

namespace {

/* The grid as the racing line sees it, for the course last asked about:
 * each record's distance along the line (m; negative: behind the start
 * line) and its offset across it (m, along (cos yaw, sin yaw)), the rows'
 * spacing (between a place and the one two behind it). */
struct GridModel {
    const void *key = nullptr;                          /* the runway's grid records */
    f32 lap = 0.0f;
    int count = 0;
    bool line = false;                                  /* the racing line placed them */
    f32 d[16];
    f32 lat[16];
    f32 rowGap = 0.0f;
    int logged = 0;                                     /* the places made, logged */
};
GridModel g_grid;

constexpr f32 kPi = 3.14159265f;

/* A made place's footprint against the walls: from the car's front, the
 * longest and widest of the game's cars with a little room. */
constexpr f32 kCarLength = 5.4f, kCarHalfWidth = 1.2f, kCarFrontRoom = 0.4f;

/* The racing line `dist' m from the start line: x, y, ground height,
 * heading; false without a racing line. */
bool line_at(DynamicsConductor *dc, int dist, f32 *x, f32 *y, f32 *h, f32 *yaw)
{
    int sector = -1;
    return dc->racingLinePoint(&sector, x, y, h, yaw, dist) != 0;
}

/* (place_on_road's walls) */
struct PlaceWalls {
    f32 x, y;                                           /* the racing line there */
    f32 fx, fy, sx, sy;                                 /* forward, across */
    f32 acrossMin, acrossMax;                           /* the strip, from the line */
    bool hit;
};

/* The wall segment w -> its next against the strip: its ends in the line's
 * frame, clipped to the rectangle axis by axis.  (A RunwayPointFunc; the box
 * plane's (x, y) is the wall's (x, -z), as T376's.) */
void place_wall_cb(const WallSeg *w, void *ctx)
{
    PlaceWalls *p = static_cast<PlaceWalls *>(ctx);
    if (p->hit || (w->flags & WALL_SOFT))
        return;
    const WallSeg *n = w + 1;
    if (w->link < 0)
        n += w->link;
    const f32 ax = w->x - p->x, ay = -w->z - p->y, bx = n->x - p->x, by = -n->z - p->y;
    const f32 from[2] = {ax * p->fx + ay * p->fy, ax * p->sx + ay * p->sy};
    const f32 to[2] = {bx * p->fx + by * p->fy, bx * p->sx + by * p->sy};
    const f32 lo[2] = {-kCarLength, p->acrossMin}, hi[2] = {kCarFrontRoom, p->acrossMax};
    f32 t0 = 0.0f, t1 = 1.0f;
    for (int k = 0; k < 2; k++) {
        const f32 d = to[k] - from[k];
        if (d == 0.0f) {
            if (from[k] < lo[k] || hi[k] < from[k])
                return;
            continue;
        }
        f32 a = (lo[k] - from[k]) / d, b = (hi[k] - from[k]) / d;
        if (b < a) {
            const f32 s = a;
            a = b;
            b = s;
        }
        if (t0 < a)
            t0 = a;
        if (b < t1)
            t1 = b;
        if (t1 < t0)
            return;
    }
    p->hit = true;
}

/* A car's place, its front `lat' m across the racing line's point x, y
 * (heading a, the line's height h), on the course's road: its footprint - the
 * front, 4 m back, 1 m to each side 2 m back - on the runway and on none of
 * its off-course surfaces (grass, sand, gravel: ground_is_off_course), and no
 * solid wall across the car or between it and the line.  The runway runs on
 * under some walls and past them: a car in a wall is pushed out of it at once
 * and its first frames depend on what it was left with; a car past one is
 * clear of it but outside the course, and the line never is.  The walls of
 * the sector of the line there and its neighbours, as a car's own
 * collisions. */
bool place_on_road(RaceCourse *course, f32 x, f32 y, f32 lat, f32 h, f32 a)
{
    static const f32 probe[4][2] = {{0.0f, 0.0f}, {-4.0f, 0.0f}, {-2.0f, -1.0f}, {-2.0f, 1.0f}};
    const f32 fx = -sinf(a), fy = cosf(a);              /* forward */
    const f32 sx = cosf(a), sy = sinf(a);               /* across */
    for (const f32 *p : probe) {
        const f32 across = lat + p[1];
        RunwayContact c;
        memset(&c, 0, sizeof(c));
        c.x = x + fx * p[0] + sx * across;
        c.y = h + 1.0f;
        c.z = -(y + fy * p[0] + sy * across);
        c.sector = -1;
        course->getContact(&c);
        if (c.offRoad != 0 || ground_is_off_course(&c))
            return false;
    }
    RunwayContact line;
    memset(&line, 0, sizeof(line));
    line.x = x;
    line.y = h + 1.0f;
    line.z = -y;
    line.sector = -1;
    course->getContact(&line);
    PlaceWalls walls = {x, y, fx, fy, sx, sy, (lat < 0.0f ? lat : 0.0f) - kCarHalfWidth,
                        (lat > 0.0f ? lat : 0.0f) + kCarHalfWidth, false};
    const f32 reach = kCarLength + kCarHalfWidth + fabsf(lat);
    course->forEachPoint(place_wall_cb, &walls, line.sector, x - reach, -y - reach, x + reach, -y + reach);
    return !walls.hit;
}

/* The grid of `course' against the racing line (once per course). */
void model_grid(DynamicsConductor *dc, const Runway *rw)
{
    const f32 lap = dc->getLapLength();
    if (g_grid.key == rw->grid && g_grid.lap == lap)
        return;
    g_grid = GridModel();
    g_grid.key = rw->grid;
    g_grid.lap = lap;
    int n = *(const int *)rw->unk_28;                   /* the grid's record count */
    g_grid.count = n < 0 ? 0 : n > 16 ? 16 : n;
    if (g_grid.count == 0)
        return;
    f32 x, y, h, yaw;
    if (!line_at(dc, 0, &x, &y, &h, &yaw))
        return;                                         /* no racing line */
    const int lo = -(int)(lap - 30.0f < 800.0f ? lap - 30.0f : 800.0f), hi = 60;
    for (int i = 0; i < g_grid.count; i++) {
        const f32 gx = rw->grid[i].x, gy = -rw->grid[i].z;
        f32 best = 3.4e38f;
        int bd = 0;
        for (int d = lo; d <= hi; d += 2)
            if (line_at(dc, d, &x, &y, &h, &yaw)) {
                const f32 e = (gx - x) * (gx - x) + (gy - y) * (gy - y);
                if (e < best) {
                    best = e;
                    bd = d;
                }
            }
        for (int d = bd - 2; d <= bd + 2; d++)
            if (line_at(dc, d, &x, &y, &h, &yaw)) {
                const f32 e = (gx - x) * (gx - x) + (gy - y) * (gy - y);
                if (e < best) {
                    best = e;
                    bd = d;
                }
            }
        line_at(dc, bd, &x, &y, &h, &yaw);
        /* forward (-sin, cos), across (cos, sin): DynCar_initialize's axes */
        const f32 along = (gx - x) * -sinf(yaw) + (gy - y) * cosf(yaw);
        g_grid.d[i] = (f32)bd + along;
        g_grid.lat[i] = (gx - x) * cosf(yaw) + (gy - y) * sinf(yaw);
    }
    /* the places from the front back (a course may list them out of order:
     * a user's log had its 3rd place behind its 4th) */
    for (int i = 1; i < g_grid.count; i++)
        for (int k = i; k > 0 && g_grid.d[k] > g_grid.d[k - 1]; k--) {
            const f32 d = g_grid.d[k], lat = g_grid.lat[k];
            g_grid.d[k] = g_grid.d[k - 1];
            g_grid.lat[k] = g_grid.lat[k - 1];
            g_grid.d[k - 1] = d;
            g_grid.lat[k - 1] = lat;
        }
    f32 gap = 16.0f;
    if (g_grid.count >= 3) {
        f32 sum = 0.0f;
        for (int i = 0; i + 2 < g_grid.count; i++)
            sum += g_grid.d[i] - g_grid.d[i + 2];
        gap = sum / (f32)(g_grid.count - 2);
    } else if (g_grid.count == 2) {
        gap = 2.0f * (g_grid.d[0] - g_grid.d[1]);
    }
    if (!(gap >= 4.0f && gap <= 60.0f))
        gap = 16.0f;
    g_grid.rowGap = gap;
    g_grid.line = true;
    printf("PC: grid: %d places on the course; the racing line puts them (front to back) at", g_grid.count);
    for (int i = 0; i < g_grid.count; i++)
        printf(" %.1f m (%+.1f m across)%s", g_grid.d[i], g_grid.lat[i], i + 1 < g_grid.count ? "," : "");
    printf("; rows %.1f m apart\n", gap);
}

} // namespace

/* The block's own bytes (retail's size word) and its inline slots. */
constexpr int kInfoSize = 0x191C;
constexpr int kInfoSlots = 8;
static_assert(sizeof(RaceInfoSlot) == 0x280, "RaceInfoSlot");
static_assert(offsetof(RaceInfoBlock, slots) + kInfoSlots * sizeof(RaceInfoSlot) + 4 == kInfoSize, "the block");

int pc_race_info_size(int cars)
{
    cars = cars > PC_RACE_CARS_MAX ? PC_RACE_CARS_MAX : cars;
    return kInfoSize + (cars > kInfoSlots ? (cars - kInfoSlots) * (int)sizeof(RaceInfoSlot) : 0);
}

RaceInfoSlot *pc_race_info_slot(RaceInfoBlock *blk, int i)
{
    if (i < kInfoSlots)
        return &blk->slots[i];
    return (RaceInfoSlot *)((u8 *)blk + kInfoSize + (i - kInfoSlots) * (int)sizeof(RaceInfoSlot));
}

int pc_race_info_cars(const RaceInfoBlock *blk)
{
    const int n = blk->carCount;
    return n < 0 ? 0 : n > PC_RACE_CARS_MAX ? PC_RACE_CARS_MAX : n;
}

u8 *pc_race_info_end(const RaceInfoBlock *blk)
{
    /* by the cars, not the size word: Manager::save appends the records at
     * the size word and adds them to it */
    return (u8 *)blk + pc_race_info_size(pc_race_info_cars(blk));
}

void pc_car_runaway(const RaceCarState *car, const char *what, f32 value)
{
    /* once a car a race (its organization), a few lines at most a run */
    static const RaceCarState *logged[PC_RACE_CARS_MAX * 2];
    static const void *loggedOrg[PC_RACE_CARS_MAX * 2];
    static int lines;
    for (int i = 0; i < PC_RACE_CARS_MAX * 2; i++)
        if (logged[i] == car && loggedOrg[i] == car->org)
            return;
    if (lines >= 64)
        return;
    for (int i = 0; i < PC_RACE_CARS_MAX * 2; i++)
        if (logged[i] == NULL || loggedOrg[i] != car->org) {
            logged[i] = car;
            loggedOrg[i] = car->org;
            break;
        }
    lines++;
    const DynCarState *st = &car->st;
    printf("PC: car %d's physics ran away: %s %g (driver %d, control %d, rank %d, lap %d at %.1f m; "
           "at %.1f %.1f %.1f, speed %.1f m/s; wheels' speed %.1f %.1f %.1f %.1f m/s on surfaces %d %d %d %d, "
           "loads %.0f %.0f %.0f %.0f N; gear %d, rpm %d; physics threads %d)\n",
           st->carIndex, what, value, st->driver, st->ctrlMode, st->rank, st->lap, st->lapDistance,
           st->pos[0], st->pos[1], st->pos[2], st->speedX, st->wheel[0].spin, st->wheel[1].spin,
           st->wheel[2].spin, st->wheel[3].spin, st->wheel[0].surface, st->wheel[1].surface,
           st->wheel[2].surface, st->wheel[3].surface, st->wheel[0].load, st->wheel[1].load,
           st->wheel[2].load, st->wheel[3].load, st->gear, (int)st->rpm, pc_physics_threads());
}

int pc_race_cars(int own)
{
    /* the 6-car races; with the override, any race with opponents (the
     * rally 2, the wet courses 4) */
    if (own != 6 && !(settings::race_cars_override() && own >= 2 && own < 6))
        return own;
    const int n = settings::race_cars();
    return n > PC_RACE_CARS_MAX ? PC_RACE_CARS_MAX : n < 6 ? 6 : n;
}

void pc_grid_start(DynamicsConductor *dc, RaceCourse *course, int order, f32 pos[3], f32 *yaw)
{
    const Runway *rw = course->data->runway;
    const int n = *(const int *)rw->unk_28;
    if (order < n || n <= 0) {
        /* the course's own place (retail: every heading record 0's) */
        const RunwayGrid &g = rw->grid[order < n ? order : 0];
        pos[0] = g.x;
        pos[1] = -g.z;
        pos[2] = g.y;
        *yaw = course->getGridAngle(0);
        return;
    }
    model_grid(dc, rw);
    const int e = order - g_grid.count;                 /* the made places' index */
    if (g_grid.line) {
        const int j = g_grid.count >= 2 ? g_grid.count - 2 + (e % 2) : 0;
        const int row = g_grid.count >= 2 ? e / 2 + 1 : e + 1;
        const f32 dist = g_grid.d[j] - (f32)row * g_grid.rowGap;
        if (dist > -(g_grid.lap - 30.0f)) {
            const int di = (int)floorf(dist + 0.5f);
            const f32 frac = dist - (f32)di;
            f32 x, y, h, a;
            if (line_at(dc, di, &x, &y, &h, &a)) {
                /* the grid's offset across the line, nearer the line while
                 * that would put the car off the road, in a wall or past one
                 * (the line may run by an edge behind the grid), then a
                 * little past it; none of them: on the line */
                static const f32 kShares[] = {1.0f, 0.75f, 0.5f, 0.25f, 0.0f, -0.25f, -0.5f};
                const f32 own = g_grid.count >= 2 ? g_grid.lat[j] : 0.0f;
                const f32 lineX = x - frac * sinf(a), lineY = y + frac * cosf(a);
                f32 lat = 0.0f;
                bool clear = false;
                for (f32 share : kShares) {
                    const f32 l = own * share;
                    if (place_on_road(course, lineX, lineY, l, h, a)) {
                        lat = l;
                        clear = true;
                        break;
                    }
                }
                pos[0] = lineX + lat * cosf(a);
                pos[1] = lineY + lat * sinf(a);
                pos[2] = h;
                *yaw = a;
                if (order + 1 > g_grid.logged) {
                    g_grid.logged = order + 1;
                    if (!clear)
                        printf("PC: grid: place %d made %.1f m along the racing line, on it: no clear place across "
                               "the road there\n", order + 1, dist);
                    else if (lat != own)
                        printf("PC: grid: place %d made %.1f m along the racing line, %+.1f m across (the grid's "
                               "%+.1f m: off the road, in a wall or past one there)\n", order + 1, dist, lat, own);
                    else if (order + 1 == PC_RACE_CARS_MAX || order + 1 == settings::race_cars())
                        printf("PC: grid: place %d made %.1f m along the racing line, %+.1f m across\n", order + 1,
                               dist, lat);
                }
                return;
            }
        }
    }
    /* no racing line, or past the lap: an existing place */
    const RunwayGrid &g = rw->grid[order % n];
    pos[0] = g.x;
    pos[1] = -g.z;
    pos[2] = g.y;
    *yaw = course->getGridAngle(0);
    if (order + 1 > g_grid.logged) {
        g_grid.logged = order + 1;
        printf("PC: grid: place %d at the course's place %d (no racing line for it)\n", order + 1, order % n + 1);
    }
}
