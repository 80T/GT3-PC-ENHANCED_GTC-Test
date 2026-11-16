/* pc_model_walk.h - PC (2026-11-13, no original): the triangles a ModelSet
 * part draws, read without drawing it.
 *
 * modelset_run_part (T451) runs a part's command stream on the graphics
 * library: packets, other parts, matrix and render-state commands, the
 * switch and LOD branches.  This reads the same stream for its geometry
 * alone: the packets are decoded (engine/pgl/native/vif_model.h, as the
 * renderer decodes them), their vertices put through the matrix the stream
 * builds - on a VU0 register file of its own, with the library's own matrix
 * routines (pgl_math.h): the game's is not touched - and every triangle
 * handed to the caller in the part's space.
 *   - a LOD command (0x07) takes `lod' (at most its last level), a switch
 *     (0x06) branch 0 (no callback's), a frustum test (0x0A) never skips;
 *   - a morphing packet command (0x35) gives its first packet's first key;
 *   - the render-state and texture commands are stepped over.
 * The brake fit (game/car/model/pc_brake_fit.h) measures a wheel and a
 * brake model with it. */
#ifndef ENGINE_GFX_PC_MODEL_WALK_H
#define ENGINE_GFX_PC_MODEL_WALK_H

class ModelSet;

struct ModelWalkStats {
    int packets;                /* packets decoded */
    int triangles;              /* triangles handed over */
    int matrixOps;              /* matrix commands met (0x0F..0x16) */
    int parts;                  /* parts entered (the first too) */
};

/* a, b, c: three floats each, in the part's space. */
typedef void (*ModelTriFn)(const float *a, const float *b, const float *c, void *user);

/* The triangles of m's part.  Returns their number (0: no such part, an
 * empty one); stats (may be NULL) is filled either way. */
int modelset_pc_walk(ModelSet *m, int part, int lod, ModelTriFn fn, void *user, ModelWalkStats *stats);

#endif /* ENGINE_GFX_PC_MODEL_WALK_H */
