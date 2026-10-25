/* pc_detail.cpp - the detail and distance settings of the races
 * (pc_detail.h).  No original: new PC code. */
#include "game/app/pc_detail.h"
#include "game/course/T453_vision_list.h"
#include "engine/pgl/G001_flare.h"              /* pgl_get_matrix, vu0_scale */
#include "platform/settings.h"

#include <math.h>
#include <unordered_map>
#include <vector>

void pc_fov_apply(int replay)
{
    const int pct = replay ? settings::replay_fov_percent() : settings::fov_percent();
    if (pct == 100)
        return;
    f32 m[16];
    pgl_get_matrix(m);
    const f32 sy = fabsf(m[5]);
    if (!(sy > 0.0f))
        return;
    const f32 half = atanf(1.0f / sy);
    f32 want = half * (f32)pct / 100.0f;
    const f32 lo = 5.0f * 3.14159265f / 180.0f, hi = 85.0f * 3.14159265f / 180.0f;
    want = want < lo ? lo : want > hi ? hi : want;
    const f32 k = tanf(half) / tanf(want);
    vu0_scale(k, k, 1.0f);
}

int pc_lod(void)
{
    return settings::lod_enabled() ? 1 : 0;
}

int pc_rear_view_bumper(void)
{
    return settings::rear_view(settings::RearViewCamera::bumper) ? 1 : 0;
}

int pc_rear_view_chase(void)
{
    return settings::rear_view(settings::RearViewCamera::chase) ? 1 : 0;
}

f32 pc_render_scale(void)
{
    return (f32)settings::render_distance() / 100.0f;
}

/* The unions built, per point, for one list and one extra distance (a new
 * list or distance starts over). */
static const VisionList *s_list;
static f32 s_extra;
static std::unordered_map<const VisionPoint *, std::vector<u16>> s_unions;

const u16 *pc_vision_parts(VisionList *v, f32 x, f32 y, f32 z, f32 extra)
{
    VisionPoint *near = vision_nearest(v, x, y, z);
    if (near == NULL || near->parts == NULL || extra <= 0.0f)
        return near != NULL ? near->parts : NULL;
    if (s_list != v || s_extra != extra) {
        s_unions.clear();
        s_list = v;
        s_extra = extra;
    }
    std::vector<u16> &out = s_unions[near];
    if (!out.empty())
        return out.data();

    /* the point's own parts first marked, then the other points' within
     * reach, nearest first in the file's order; drawn farther first */
    static std::vector<u8> seen;
    seen.assign(65536 / 8, 0);
    const u16 *own = near->parts;
    const int nOwn = own[0];
    for (int i = 1; i <= nOwn; i++)
        seen[own[i] >> 3] |= (u8)(1u << (own[i] & 7));
    std::vector<u16> extraParts;
    const f32 r2 = extra * extra;
    for (int i = 0; i < v->nPoints; i++) {
        const VisionPoint *pt = &v->points[i];
        if (pt == near || pt->parts == NULL)
            continue;
        const f32 dx = pt->x - near->x, dy = pt->y - near->y, dz = pt->z - near->z;
        if (dx * dx + dy * dy + dz * dz > r2)
            continue;
        const int n = pt->parts[0];
        for (int k = 1; k <= n; k++) {
            const u16 part = pt->parts[k];
            u8 &bits = seen[part >> 3];
            const u8 bit = (u8)(1u << (part & 7));
            if (bits & bit)
                continue;
            bits |= bit;
            extraParts.push_back(part);
        }
    }
    out.reserve(1 + extraParts.size() + (size_t)nOwn);
    out.push_back((u16)(extraParts.size() + (size_t)nOwn));
    out.insert(out.end(), extraParts.begin(), extraParts.end());
    out.insert(out.end(), own + 1, own + 1 + nOwn);
    return out.data();
}
