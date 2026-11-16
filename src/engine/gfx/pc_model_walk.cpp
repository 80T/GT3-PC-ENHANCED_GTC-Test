/* pc_model_walk.cpp - PC (2026-11-13, no original): the triangles a ModelSet
 * part draws, read without drawing it (pc_model_walk.h). */
#include "engine/gfx/pc_model_walk.h"
#include "engine/gfx/T451_model_set.h"
#include "engine/pgl/native/pgl_math.h"
#include "engine/pgl/native/vif_model.h"

#include <string.h>
#include <vector>

namespace {

enum { kMaxDepth = 8, kMaxSteps = 20000 };  /* parts within parts; commands of one part (a bad stream ends) */

struct Walk {
    ModelSet *m;
    int lod;
    ModelTriFn fn;
    void *user;
    ModelWalkStats st;
    pgl::Vu0 vu;
    int mode;                               /* the matrix mode (0x0E) */
    gt3vif::Mesh mesh;
    std::vector<float> pts;
};

int rd_u16(const u8 *p) { return (p[1] << 8) | p[0]; }
int rd_s16(const u8 *p) { return ((s8)p[1] << 8) | p[0]; }
f32 rd_f32(const u8 *p) { f32 v; memcpy(&v, p, 4); return v; }

/* The operand bytes of the commands with nothing to read here (T451
 * modelset_run_part / modelset_exec); -1: handled by the walk. */
int operand_bytes(u8 op)
{
    switch (op) {
    case 0x0E: case 0x19: case 0x1F: case 0x29: case 0x2A: case 0x2B: case 0x2F:
        return 1;
    case 0x1C: case 0x20: case 0x2C:
        return 2;
    case 0x21: case 0x24: case 0x27: case 0x32: case 0x34: case 0x36:
        return 4;
    case 0x31:
        return 12;
    case 0x28: case 0x33: case 0x37:
        return 16;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A:
    case 0x0C: case 0x0D: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15:
    case 0x16: case 0x35:
        return -1;
    default:
        return 0;                           /* (0x01, 0x0B, the on / off states, 0x30, above 0x37) */
    }
}

void packet(Walk &w, int n)
{
    if (n < 0 || n >= w.m->nPackets || w.m->packets == NULL || w.m->packets[n] == NULL)
        return;
#if UINTPTR_MAX == 0xFFFFFFFFu
    w.mesh.clear();
    if (!gt3vif::decodeModel(w.m->packets[n], w.mesh))
        return;
#else
    return;                                 /* (the game is 32-bit: the packets' data words are pointers) */
#endif
    w.st.packets++;
    for (const gt3vif::Batch &b : w.mesh.batches) {
        if (b.vertexCount == 0 || b.indexCount < 3 || b.firstVertex + b.vertexCount > w.mesh.vertices.size())
            continue;
        w.pts.resize((size_t)b.vertexCount * 3);
        for (uint32_t v = 0; v < b.vertexCount; v++) {
            const float *p = w.mesh.vertices[b.firstVertex + v].pos;
            alignas(16) float src[4] = {p[0], p[1], p[2], 1.0f}, dst[4];
            pgl::vu0ApplyMatrix(w.vu, dst, src);
            w.pts[3 * v] = dst[0];
            w.pts[3 * v + 1] = dst[1];
            w.pts[3 * v + 2] = dst[2];
        }
        for (uint32_t t = 0; t + 2 < b.indexCount; t += 3) {
            uint32_t i[3];
            bool ok = true;
            for (int k = 0; k < 3; k++) {
                i[k] = w.mesh.indices[b.firstIndex + t + k] - b.firstVertex;
                if (i[k] >= b.vertexCount)
                    ok = false;
            }
            if (!ok)
                continue;
            w.fn(&w.pts[3 * i[0]], &w.pts[3 * i[1]], &w.pts[3 * i[2]], w.user);
            w.st.triangles++;
        }
    }
}

void run(Walk &w, int part, int depth)
{
    if (depth > kMaxDepth || part < 0 || part >= w.m->nParts || w.m->parts == NULL)
        return;
    const u8 *p = w.m->parts[part];
    if (p == NULL)
        return;
    w.st.parts++;
    for (int steps = 0; steps < kMaxSteps; steps++) {
        const u8 op = *p++;
        if (op == 0)
            return;
        switch (op) {
        case 0x02:
            run(w, p[0], depth + 1);
            p += 1;
            break;
        case 0x03:
            run(w, rd_u16(p), depth + 1);
            p += 2;
            break;
        case 0x04:
            packet(w, p[0]);
            p += 1;
            break;
        case 0x05:
            packet(w, rd_u16(p));
            p += 2;
            break;
        case 0x35:                          /* a morphing packet: its first, the first key frame */
            packet(w, rd_u16(p));
            p += 4;
            break;
        case 0x06: {                        /* a switch: branch 0 (as without a callback) */
            p += 2;
            const int n = p[0];
            p += 1;
            if (n == 0)
                break;
            p += rd_s16(p);
            break;
        }
        case 0x07: {                        /* a LOD: the level asked for, at most its last */
            p += 0x10;
            const int n = p[0];
            p += 1;
            if (n == 0)
                break;                      /* (the game would read a level that is not there) */
            const int k = w.lod < n ? (w.lod < 0 ? 0 : w.lod) : n - 1;
            p += k * 2;
            p += rd_s16(p);
            break;
        }
        case 0x08:
            p += (s8)p[0];
            break;
        case 0x09:
            p += rd_s16(p);
            break;
        case 0x0A:                          /* a frustum test: in view */
            p += 1 + p[0] * 12 + 2;
            break;
        case 0x0C:
            pgl::vu0PushMatrix(w.vu, w.mode);
            break;
        case 0x0D:
            pgl::vu0PopMatrix(w.vu, w.mode);
            break;
        case 0x0E:
            pgl::vu0SwitchMatrixMode(w.vu, w.mode, p[0]);
            w.mode = p[0];
            p += 1;
            break;
        case 0x0F:
        case 0x10: {
            alignas(16) float m[16];
            memcpy(m, p, sizeof(m));
            if (op == 0x0F)
                pgl::vu0LoadMatrix(w.vu, m);
            else
                pgl::vu0MultMatrix(w.vu, m);
            p += 0x40;
            w.st.matrixOps++;
            break;
        }
        case 0x11:
            pgl::vu0Translate(w.vu, rd_f32(p), rd_f32(p + 4), rd_f32(p + 8));
            p += 12;
            w.st.matrixOps++;
            break;
        case 0x12:
            pgl::vu0Scale(w.vu, rd_f32(p), rd_f32(p + 4), rd_f32(p + 8));
            p += 12;
            w.st.matrixOps++;
            break;
        case 0x13:
            pgl::vu0Rotate(w.vu, rd_f32(p), rd_f32(p + 4), rd_f32(p + 8), rd_f32(p + 12));
            p += 16;
            w.st.matrixOps++;
            break;
        case 0x14:
            pgl::vu0RotateX(w.vu, rd_f32(p));
            p += 4;
            w.st.matrixOps++;
            break;
        case 0x15:
            pgl::vu0RotateY(w.vu, rd_f32(p));
            p += 4;
            w.st.matrixOps++;
            break;
        case 0x16:
            pgl::vu0RotateZ(w.vu, rd_f32(p));
            p += 4;
            w.st.matrixOps++;
            break;
        default:
            p += operand_bytes(op);
            break;
        }
    }
}

} // namespace

int modelset_pc_walk(ModelSet *m, int part, int lod, ModelTriFn fn, void *user, ModelWalkStats *stats)
{
    if (stats != NULL)
        memset(stats, 0, sizeof(*stats));
    if (m == NULL || fn == NULL)
        return 0;
    /* (a Vu0 holds its 4 KB of data memory: not on the game thread's stack
     * twice over, and the mesh's vectors are kept between packets) */
    Walk *w = new Walk;
    w->m = m;
    w->lod = lod;
    w->fn = fn;
    w->user = user;
    memset(&w->st, 0, sizeof(w->st));
    w->mode = pgl::MTX_MODELVIEW;
    pgl::vu0Reset(w->vu);
    pgl::vu0InitMatrices(w->vu);
    run(*w, part, 0);
    const int n = w->st.triangles;
    if (stats != NULL)
        *stats = w->st;
    delete w;
    return n;
}
