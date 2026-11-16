/* pc_detail.cpp - the detail and distance settings of the races
 * (pc_detail.h).  No original: new PC code. */
#include "game/app/pc_detail.h"
#include "game/course/T453_vision_list.h"
#include "engine/gfx/T451_model_set.h"         /* modelset_pc_full_detail */
#include "engine/pgl/G001_flare.h"              /* pgl_get_matrix, vu0_scale */
#include "platform/settings.h"
#include "platform/settings_menu.h"             /* notice */

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <unordered_map>
#include <vector>

void pc_fov_apply(int replay, int cockpit)
{
    if (cockpit)
        return;                             /* PC (2026-11-09): the cockpit camera's own (pc_detail.h) */
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

f32 pc_fov_percent(void)
{
    return (f32)settings::fov_percent();
}

int pc_lod(void)
{
    /* PC: as the models' (T451 modelset_pc_full_detail: [Graphics] Lod, or
     * the road reflection's own) */
    return modelset_pc_full_detail() ? 0 : 1;
}

int pc_reflection_full(void)
{
    return settings::reflection_detail() == settings::ReflectionDetail::full ? 1 : 0;
}

void pc_reflection_scale(int screenW, int screenH, int reflW, int reflH, f32 *dx, f32 *dy)
{
    *dx = 1.0f;
    *dy = 1.0f;
    const settings::ReflectionResolution r = settings::reflection_resolution();
    if (r == settings::ReflectionResolution::normal || reflW <= 0 || reflH <= 0)
        return;
    const f32 share = r == settings::ReflectionResolution::half ? 0.5f : 1.0f;
    const f32 x = (f32)screenW * share / (f32)reflW, y = (f32)screenH * share / (f32)reflH;
    *dx = x > 1.0f ? x : 1.0f;
    *dy = y > 1.0f ? y : 1.0f;
}

int pc_replay_blur_steps(void)
{
    switch (settings::replay_depth_of_field()) {
    case settings::ReplayDepthOfField::reduced:
        return 1;
    case settings::ReplayDepthOfField::off:
        return 0;
    default:
        return 2;                           /* the original's ReplayBlur::farSteps */
    }
}

int pc_rear_view_bumper(void)
{
    return settings::rear_view(settings::RearViewCamera::bumper) ? 1 : 0;
}

int pc_rear_view_chase(void)
{
    return settings::rear_view(settings::RearViewCamera::chase) ? 1 : 0;
}

int pc_extra_cameras(void)
{
    return settings::extra_cameras() ? 1 : 0;
}

void pc_camera_notice(const char *name)
{
    char text[32];
    snprintf(text, sizeof(text), "CAMERA: %s", name);
    for (char *c = text; *c != '\0'; c++)
        *c = (char)toupper((unsigned char)*c);
    settings_menu::notice(text, 1.5);
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

void pc_vision_parts_reset(void)
{
    s_unions.clear();
    s_list = NULL;
    s_extra = 0.0f;
}

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
