/* pc_headcam_find.cpp - PC: where the driver's head is in a car model
 * (pc_headcam_find.h). */
#include "game/app/pc_headcam_find.h"
#include "engine/pgl/native/vif_model.h"

#include <math.h>
#include <vector>
#ifdef HEADCAM_CHECK
#include <stdio.h>                          /* (the offline check's listing only) */
#endif

namespace headcam {
namespace {

constexpr float kSideMin = 0.18f;                       /* a head batch's box, each side at least, metres */
constexpr float kSideMax[3] = {0.60f, 0.45f, 0.40f};    /* and at most: across (with its shoulders), high, deep */
constexpr float kOffCentre = 0.60f;                     /* a head batch's centre: |x| at most */
constexpr float kNearEye = 0.25f;                       /* its centre's height / depth from the Driver eye */
constexpr float kSeatX = 0.33f;                         /* a seat's head point: this far off the centre line... */
constexpr float kSeatZ = 0.10f;                         /* ...and behind the Driver eye (the heads' mean) */
constexpr float kClusterR = 0.25f;                      /* the vertices within this of it are a merged head's */
constexpr int kClusterMin = 6;                          /* at least this many */
constexpr float kClusterMax = 0.50f;                    /* their box at most this a side */
constexpr float kEyeDown = 0.13f;                       /* the eye under a merged box's top (the seat's top is in it) */
constexpr float kEyeOwn = 0.10f;                        /* the eye under a head batch's top: the eye line of a helmet
                                                           (the box's centre sat too low - user, wl1001 - and in a head
                                                           with its neck or shoulders lower still) */
constexpr float kCapBelow = 0.10f, kCapAbove = 0.03f;   /* a batch whose bottom is between this far under the head's
                                                           top and this far over it, reaching higher... */
constexpr float kCapHigh = 0.15f;                       /* ...at most this high (a helmet's crown or visor)... */
constexpr float kCapWide = 1.3f;                        /* ...no wider or deeper than the head by more than this... */
constexpr float kCapOverlap = 0.5f;                     /* ...over this much of its own footprint: the head's too */
constexpr float kSameBox = 0.002f;                      /* tops alike within this: the nearer wins */
constexpr float kSideOf = 0.15f;                        /* a head this far off the centre line sits on a side */
constexpr float kTwinGrow = 0.03f;                      /* the first head's box mirrored, grown by this: a twin's */
constexpr float kBlobMin[3] = {0.12f, 0.12f, 0.08f};    /* a merged head's vertices span at least this across, high, deep... */
constexpr float kTwinMin[3] = {0.15f, 0.20f, 0.08f};    /* ...a twin's at least this (a figure on a card is flat, but a
                                                           head high; a visor, a mirror, a headrest's top is not) */
constexpr float kTwinMax = 0.45f;                       /* and at most this each way (not a seat) */
constexpr int kHeadVerts = 16;                          /* a modelled head's vertices at least (head_score) */
constexpr float kHeadDeep = 0.72f;                      /* a head's depth over its height at least (a hoop is thin) */
constexpr float kLikeSize = 1.5f;                       /* a second driver's own batch: sizes within this of the first's */
constexpr float kGlassIn = 0.02f;                       /* a glass shell's box holds the eye this far in */
constexpr float kGlassToward = 0.7f;                    /* ...and this part of its triangles face the eye */
constexpr float kGlassFloor = 0.40f;                    /* ...and it starts this high (the cabin's glass, not the
                                                           shadow's copy of the car, which reaches the ground) */
constexpr float kGlassLong = 3.5f;                      /* ...and is at most this long (the shadow spans the car) */

/* (the windows in use: the constants', wider without a mount table) */
float g_nearEye = kNearEye, g_clusterR = kClusterR;

struct Box {
    float lo[3], hi[3];
};

struct Seen {
    const void *model;
    int part;
    Box box;
    int near[2];            /* the vertices within kClusterR of each side's head point (0: left, 1: right) */
    Box nearBox[2];
    std::vector<float> pts; /* its vertices, x y z each */
    std::vector<uint32_t> tris;     /* its triangles, three vertex numbers each (into pts) */
    bool blended;           /* PRIM.ABE: drawn translucent */
};

void grow(Box *b, const float *p, bool first)
{
    for (int k = 0; k < 3; k++) {
        if (first || p[k] < b->lo[k])
            b->lo[k] = p[k];
        if (first || p[k] > b->hi[k])
            b->hi[k] = p[k];
    }
}

bool in_box(const Box &b, const float *p)
{
    for (int k = 0; k < 3; k++)
        if (p[k] < b.lo[k] || p[k] > b.hi[k])
            return false;
    return true;
}

/* Every batch's box, once, with its vertices near each seat's head point. */
void all_boxes(const void *const *packets, int packetCount, const float eye[3], std::vector<Seen> *seen)
{
    gt3vif::Mesh mesh;
    for (int i = 0; i < packetCount; i++) {
        if (packets[i] == nullptr)
            continue;
        mesh.clear();
#if UINTPTR_MAX == 0xFFFFFFFFu
        if (!gt3vif::decodeModel(packets[i], mesh))
            continue;
#else
        continue;                           /* (the game is 32-bit; the check is built so too) */
#endif
        for (const gt3vif::Batch &b : mesh.batches) {
            if (b.vertexCount == 0 || b.firstVertex + b.vertexCount > mesh.vertices.size())
                continue;
            Seen s;
            s.model = packets[i];
            s.part = b.part;
            s.near[0] = s.near[1] = 0;
            s.blended = (b.prim & 0x40) != 0;
            s.tris.reserve(b.indexCount);
            for (uint32_t t = 0; t < b.indexCount; t++) {
                const uint32_t v = mesh.indices[b.firstIndex + t];
                s.tris.push_back(v >= b.firstVertex && v < b.firstVertex + b.vertexCount ? v - b.firstVertex : 0);
            }
            s.pts.reserve(b.vertexCount * 3);
            for (uint32_t v = 0; v < b.vertexCount; v++) {
                const float *p = mesh.vertices[b.firstVertex + v].pos;
                s.pts.push_back(p[0]);
                s.pts.push_back(p[1]);
                s.pts.push_back(p[2]);
                grow(&s.box, p, v == 0);
                for (int side = 0; side < 2; side++) {
                    const float dx = p[0] - (side == 0 ? -kSeatX : kSeatX), dy = p[1] - eye[1],
                                dz = p[2] - (eye[2] + kSeatZ);
                    if (dx * dx + dy * dy + dz * dz > g_clusterR * g_clusterR)
                        continue;
                    grow(&s.nearBox[side], p, s.near[side] == 0);
                    s.near[side]++;
                }
            }
            seen->push_back(s);
        }
    }
}

/* How much a batch looks like a modelled head: 2 for kHeadVerts vertices or
 * more (the pseudo head has 32, a helmet 40 .. 64; a headrest or a roll
 * hoop behind the driver, a card or a box, 6 .. 8 - ty2056's reached higher
 * than its helmet and took the eye), 1 more for a box about as deep as
 * high (a head; a hoop is thin). */
int head_score(const Seen &s)
{
    const Box &b = s.box;
    int score = s.pts.size() / 3 >= (size_t)kHeadVerts ? 2 : 0;
    if (b.hi[2] - b.lo[2] >= kHeadDeep * (b.hi[1] - b.lo[1]))
        score++;
    return score;
}

/* A head as a batch of its own: the best one (the most like a head, then
 * reaching highest, then the nearest to the eye) among those on the side of
 * x `side' (-1 left, 1 right, 0 any: the centre too), or -1. */
int own_batch(const std::vector<Seen> &seen, int side, const float eye[3])
{
    int best = -1, bestScore = 0;
    float bestTop = 0.0f, bestDist = 0.0f;
    for (size_t i = 0; i < seen.size(); i++) {
        const Box &b = seen[i].box;
        float c[3];
        bool ok = true;
        for (int k = 0; k < 3; k++) {
            const float s = b.hi[k] - b.lo[k];
            if (s < kSideMin || s > kSideMax[k])
                ok = false;
            c[k] = 0.5f * (b.lo[k] + b.hi[k]);
        }
        if (!ok || fabsf(c[0]) > kOffCentre)
            continue;
        if (side != 0 && (fabsf(c[0]) < kSideOf || (c[0] < 0.0f) != (side < 0)))
            continue;
        const float dy = c[1] - eye[1], dz = c[2] - eye[2];
        if (fabsf(dy) > g_nearEye || fabsf(dz) > g_nearEye)
            continue;
        const float d = dy * dy + dz * dz;
        const int score = head_score(seen[i]);
        if (best < 0 || score > bestScore ||
            (score == bestScore &&
             (b.hi[1] > bestTop + kSameBox || (fabsf(b.hi[1] - bestTop) <= kSameBox && d < bestDist)))) {
            best = (int)i;
            bestScore = score;
            bestTop = b.hi[1];
            bestDist = d;
        }
    }
    return best;
}

/* The other side's own batch is a second driver only when it is like the
 * first's head (as many vertices, or kHeadVerts; sizes within kLikeSize of
 * it): a roll hoop or a headrest mirrored on the passenger side is not. */
bool like_head(const Seen &s, const Seen &first)
{
    const size_t n = s.pts.size() / 3, n1 = first.pts.size() / 3;
    if (n < (size_t)kHeadVerts && n * 2 < n1)
        return false;
    for (int k = 0; k < 3; k++) {
        const float a = s.box.hi[k] - s.box.lo[k], b = first.box.hi[k] - first.box.lo[k];
        if (a > b * kLikeSize || b > a * kLikeSize)
            return false;
    }
    return true;
}

/* The side's head merged in a batch: the batch with the most vertices near
 * the seat's head point whose box fits a head, or -1. */
int merged_batch(const std::vector<Seen> &seen, int side)
{
    int best = -1;
    for (size_t i = 0; i < seen.size(); i++) {
        const Seen &s = seen[i];
        if (s.near[side] < kClusterMin)
            continue;
        bool ok = true;
        for (int k = 0; k < 3; k++) {
            const float span = s.nearBox[side].hi[k] - s.nearBox[side].lo[k];
            if (span > kClusterMax || span < kBlobMin[k])
                ok = false;                 /* (a card or a bar is not a head) */
        }
        if (ok && (best < 0 || s.near[side] > seen[best].near[side]))
            best = (int)i;
    }
    return best;
}

/* The overlap of two spans as a part of the shorter. */
float overlap(float lo0, float hi0, float lo1, float hi1)
{
    const float o = (hi0 < hi1 ? hi0 : hi1) - (lo0 > lo1 ? lo0 : lo1);
    const float s = (hi0 - lo0 < hi1 - lo1 ? hi0 - lo0 : hi1 - lo1);
    return s > 0.0f ? o / s : 0.0f;
}

/* The box grown by the batches sitting on its top (a helmet's crown or
 * visor drawn apart from it: wl1001), so that the eye line and the hide box
 * take them in. */
void with_caps(const std::vector<Seen> &seen, Box *b)
{
    const Box head = *b;                    /* (the caps against the head itself, not one another: a roll
                                             * cage grew the box batch by batch on sb0097) */
    for (const Seen &s : seen) {
        const Box &c = s.box;
        if (c.hi[1] - c.lo[1] > kCapHigh || c.hi[1] <= head.hi[1] || c.lo[1] < head.hi[1] - kCapBelow ||
            c.lo[1] > head.hi[1] + kCapAbove)
            continue;
        if (c.hi[0] - c.lo[0] > kCapWide * (head.hi[0] - head.lo[0]) ||
            c.hi[2] - c.lo[2] > kCapWide * (head.hi[2] - head.lo[2]))
            continue;
        if (overlap(head.lo[0], head.hi[0], c.lo[0], c.hi[0]) < kCapOverlap ||
            overlap(head.lo[2], head.hi[2], c.lo[2], c.hi[2]) < kCapOverlap)
            continue;
        for (int k = 0; k < 3; k++) {
            if (c.lo[k] < b->lo[k])
                b->lo[k] = c.lo[k];
            if (c.hi[k] > b->hi[k])
                b->hi[k] = c.hi[k];
        }
    }
}

void fill(HeadBox *h, const Box &b, bool merged, int verts)
{
    h->merged = merged;
    h->verts = verts;
    for (int k = 0; k < 3; k++) {
        h->bmin[k] = b.lo[k];
        h->bmax[k] = b.hi[k];
        h->centre[k] = 0.5f * (b.lo[k] + b.hi[k]);
        h->eye[k] = h->centre[k];
    }
    h->eye[1] = b.hi[1] - (merged ? kEyeDown : kEyeOwn);
}

/* A twin of the first head: the vertices of any batch inside its box
 * mirrored across the centre line (grown a little): a second driver there
 * (the rally cars), nothing on a passenger seat.  Their box into *box;
 * false when too few or too flat. */
bool twin(const std::vector<Seen> &seen, const HeadBox &first, Box *box, int *verts, bool *merged)
{
    Box m;
    m.lo[0] = -first.bmax[0] - kTwinGrow;
    m.hi[0] = -first.bmin[0] + kTwinGrow;
    for (int k = 1; k < 3; k++) {
        m.lo[k] = first.bmin[k] - kTwinGrow;
        m.hi[k] = first.bmax[k] + kTwinGrow;
    }
    int n = 0;
    std::vector<int> holders;               /* the batches with vertices in the seed */
    for (size_t b = 0; b < seen.size(); b++) {
        const Seen &s = seen[b];
        int in = 0;
        for (size_t i = 0; i + 2 < s.pts.size(); i += 3)
            if (in_box(m, &s.pts[i])) {
                grow(box, &s.pts[i], n == 0);
                n++;
                in++;
            }
        if (in > 0)
            holders.push_back((int)b);
    }
    *verts = n;
    *merged = false;
    if (n < kClusterMin)
        return false;
    for (int k = 0; k < 3; k++) {
        const float span = box->hi[k] - box->lo[k];
        if (span < kTwinMin[k] || span > kTwinMax)
            return false;                   /* (a sun visor, a mirror, a seat's top: not a head) */
    }
    /* The mirrored box is only a seed: the other driver need not sit where
     * the first does (pe2007's sits 16 cm further forward, and the hide box
     * missed the front of his helmet - user report).  Of the batches with
     * vertices in the seed, the fullest cluster of vertices round the seed's
     * centre is added to the seed - sideways, forward and down, not up: the
     * seed's top is the first driver's helmet top, and a cluster reaching
     * higher took in the roll cage over pe2007's and put the eye too high
     * (user report); the seed keeps the figure above a seat's cluster
     * (op0010). */
    float c[3];
    for (int k = 0; k < 3; k++)
        c[k] = 0.5f * (box->lo[k] + box->hi[k]);
    const float top = box->hi[1];
    int best = -1, bestN = 0;
    Box bestBox;
    for (int b : holders) {
        const Seen &s = seen[b];
        Box g;
        int ng = 0;
        for (size_t i = 0; i + 2 < s.pts.size(); i += 3) {
            const float dx = s.pts[i] - c[0], dy = s.pts[i + 1] - c[1], dz = s.pts[i + 2] - c[2];
            if (dx * dx + dy * dy + dz * dz > g_clusterR * g_clusterR)
                continue;
            grow(&g, &s.pts[i], ng == 0);
            ng++;
        }
        bool fits = ng >= kClusterMin;
        for (int k = 0; k < 3 && fits; k++)
            if (g.hi[k] - g.lo[k] > kClusterMax)
                fits = false;
        if (fits && ng > bestN) {
            best = b;
            bestBox = g;
            bestN = ng;
        }
    }
    if (best >= 0) {
        for (int k = 0; k < 3; k++) {
            if (bestBox.lo[k] < box->lo[k])
                box->lo[k] = bestBox.lo[k];
            if (bestBox.hi[k] > box->hi[k])
                box->hi[k] = bestBox.hi[k];
        }
        box->hi[1] = top;
        *verts = bestN;
        /* (in a batch bigger than a head - the seats' - the seat's top is in
         * the cluster: the eye goes by its top, as a merged head's) */
        const Seen &s = seen[best];
        for (int k = 0; k < 3; k++)
            if (s.box.hi[k] - s.box.lo[k] > kSideMax[k])
                *merged = true;
    }
    return true;
}

/* The triangles of a batch whose winding normal, (b - a) x (c - a), points
 * away from `eye' and toward it. */
void winding(const Seen &s, const float eye[3], int *away, int *toward)
{
    *away = *toward = 0;
    for (size_t t = 0; t + 2 < s.tris.size(); t += 3) {
        const float *a = &s.pts[3 * s.tris[t]], *b = &s.pts[3 * s.tris[t + 1]], *c = &s.pts[3 * s.tris[t + 2]];
        float e1[3], e2[3], n[3], d[3];
        for (int k = 0; k < 3; k++) {
            e1[k] = b[k] - a[k];
            e2[k] = c[k] - a[k];
            d[k] = (a[k] + b[k] + c[k]) / 3.0f - eye[k];
        }
        n[0] = e1[1] * e2[2] - e1[2] * e2[1];
        n[1] = e1[2] * e2[0] - e1[0] * e2[2];
        n[2] = e1[0] * e2[1] - e1[1] * e2[0];
        if (n[0] * d[0] + n[1] * d[1] + n[2] * d[2] < 0.0f)
            (*toward)++;
        else
            (*away)++;
    }
}

/* The translucent batches round `eye' wound to face it: their box holds the
 * eye (a margin in), and most of their triangles are wound the other way
 * from the car's opaque faces round the eye (the body's, which face out
 * and cull from inside): a shell the eye in the cabin sees, as ho0030's
 * cabin tint, where every other car's glass is wound as its body. */
void glass_round(const std::vector<Seen> &seen, const float eye[3], Heads *out)
{
    out->glassCount = 0;
    /* the body's winding: of the opaque batches, which way most triangles go */
    int bodyAway = 0, bodyToward = 0;
    for (const Seen &s : seen) {
        if (s.blended || s.tris.size() < 3)
            continue;
        int away, toward;
        winding(s, eye, &away, &toward);
        bodyAway += away;
        bodyToward += toward;
    }
    const bool bodyFacesAway = bodyAway >= bodyToward;
    for (const Seen &s : seen) {
        if (!s.blended || s.tris.size() < 3)
            continue;
        bool holds = true;
        for (int k = 0; k < 3; k++)
            if (eye[k] < s.box.lo[k] + kGlassIn || eye[k] > s.box.hi[k] - kGlassIn)
                holds = false;
        if (!holds || s.box.lo[1] < kGlassFloor || s.box.hi[2] - s.box.lo[2] > kGlassLong)
            continue;                       /* (the shadow's and the ground passes' copies of the car go the
                                             * other way too, but reach the ground and span the car) */
        int away, toward;
        winding(s, eye, &away, &toward);
        const int other = bodyFacesAway ? toward : away;
#ifdef HEADCAM_CHECK
        printf("    glass? part %d: away %d toward %d (body away %d toward %d) box (%.2f %.2f %.2f)-(%.2f %.2f %.2f)%s\n",
               s.part, away, toward, bodyAway, bodyToward, s.box.lo[0], s.box.lo[1], s.box.lo[2], s.box.hi[0],
               s.box.hi[1], s.box.hi[2], other < kGlassToward * (away + toward) ? "" : " TAKEN");
#endif
        if (other < kGlassToward * (away + toward))
            continue;
        if (out->glassCount < kMaxGlass) {
            out->glass[out->glassCount].model = s.model;
            out->glass[out->glassCount].part = s.part;
            for (int k = 0; k < 3; k++) {       /* PC (2026-11-14) */
                out->glass[out->glassCount].lo[k] = s.box.lo[k];
                out->glass[out->glassCount].hi[k] = s.box.hi[k];
            }
            out->glassCount++;
        }
    }
}

} // namespace

void list_batches(const void *const *packets, int packetCount, BatchFn fn, void *user)
{
    const float eye[3] = {0.0f, 0.0f, 0.0f};
    std::vector<Seen> seen;
    all_boxes(packets, packetCount, eye, &seen);
    for (const Seen &s : seen)
        fn(s.model, s.part, s.box.lo, s.box.hi, user);
}

void find_heads(const void *const *packets, int packetCount, const float eyeIn[3], Heads *out)
{
    out->count = 0;
    out->glassCount = 0;
    /* (no mount table: the cars' mean Driver eye - 0.90 m up, 0.15 m back
     * over the 262 US cars with one - and the windows half again as wide) */
    const float meanEye[3] = {0.0f, 0.90f, 0.15f};
    const float *eye = eyeIn != nullptr ? eyeIn : meanEye;
    g_nearEye = eyeIn != nullptr ? kNearEye : kNearEye * 1.5f;
    g_clusterR = eyeIn != nullptr ? kClusterR : kClusterR * 1.5f;
    std::vector<Seen> seen;
    all_boxes(packets, packetCount, eye, &seen);
    /* the driver: a batch of its own anywhere (the open-wheel cars' helmet
     * sits on the centre line), else the fuller merged cluster of a side */
    const int own = own_batch(seen, 0, eye);
    if (own >= 0) {
        Box b = seen[own].box;
        with_caps(seen, &b);
        fill(&out->head[0], b, false, (int)(seen[own].pts.size() / 3));
    } else {
        const int left = merged_batch(seen, 0), right = merged_batch(seen, 1);
        int side = -1;
        if (left >= 0 && right >= 0)
            side = seen[right].near[1] >= seen[left].near[0] ? 1 : 0;
        else if (left >= 0 || right >= 0)
            side = left >= 0 ? 0 : 1;
        if (side < 0)
            return;
        const Seen &s = seen[side == 0 ? left : right];
        fill(&out->head[0], s.nearBox[side], true, s.near[side]);
    }
    out->count = 1;
    glass_round(seen, out->head[0].eye, out);
    /* a second driver: a batch of its own on the other side, else a twin of
     * the first across the centre line */
    const HeadBox &h = out->head[0];
    if (fabsf(h.centre[0]) < kSideOf)
        return;
    const int other = own_batch(seen, h.centre[0] < 0.0f ? 1 : -1, eye);
    if (other >= 0 && (own < 0 || like_head(seen[other], seen[own]))) {
        Box b = seen[other].box;
        with_caps(seen, &b);
        fill(&out->head[1], b, false, (int)(seen[other].pts.size() / 3));
        out->count = 2;
        return;
    }
    Box tb;
    int n = 0;
    bool merged = false;
    if (twin(seen, h, &tb, &n, &merged)) {
        fill(&out->head[1], tb, merged, n);
        out->count = 2;
    }
    /* (user, 2026-11-07: "Make Cockpit 2 the 1st / default one when a car
     * has two drivers" - the head found second, the other side's, is the
     * driver's seat on the cars tried: it goes first) */
    if (out->count == 2) {
        const HeadBox t = out->head[0];
        out->head[0] = out->head[1];
        out->head[1] = t;
        glass_round(seen, out->head[0].eye, out);
    }
}

} // namespace headcam
