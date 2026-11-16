/* pc_brake_fit.h - PC (2026-11-13, no original): the brake moved back until
 * the wheel in front of it clears it (user: "Some cars have brake caliper /
 * rotor models that aren't placed well, and some wheels clip into it ...
 * make wheel models push the rotor / caliper model back so they don't
 * touch").
 *
 * A wheel and its brake are drawn in one frame: x along the axle from the
 * tyre's outer face inward, one unit the rim radius (TirePacket's matrix:
 * moved by -width / 2, PGL_CAP_SCALE = the rim).  Both turn about x, so what
 * matters is each one's shape by radius: a PcRadial holds, per ring of
 * radius, where along x a model's triangles are (modelset_pc_walk,
 * engine/gfx/pc_model_walk.h).
 *
 * Of a wheel, not everything is in a brake's way:
 *   - by a triangle's normal (along the axle, along the radius, across
 *     both): a FACE looks along the axle (within 26 degrees: the dish a race
 *     wheel's spokes are painted on, a menu wheel's spoke fronts and backs,
 *     its hub cap); a SIDE looks across more than along the radius (a
 *     spoke's flanks, which carry its depth); the rest is ROUND, a tube or a
 *     cone about the axle - the hub's bore (a race wheel's reaches 8 cm in,
 *     through the disc's middle), the rim's barrel and the funnel of its
 *     well - and a brake sits inside those: they are left out;
 *   - the faces and sides fill cells along x in each ring; cells that
 *     touch, in a ring (no more than kGap empty between them) or from one
 *     ring to the next, are one PIECE.  A piece that mostly has something
 *     else of the wheel in front of it, ring by ring, is a LAYER BEHIND -
 *     the plate that closes a menu wheel's back, a flange at its inner end -
 *     and the brake belongs in front of it; the other pieces are the
 *     wheel's BODY (the spokes, the hub cap, the lip);
 *   - a ring the body's faces close all round (every one of kSectors
 *     directions has one: a hub, a hub cap - and all of a race wheel's dish,
 *     whose openings are in its picture) hides what is behind them: there
 *     the brake only has to be behind the deepest of the faces seen first.
 *     In an open ring it shows between the spokes, and has to be behind all
 *     of the body.
 * That is the wheel's back in a ring; the brake's face is the least x of
 * all its triangles (a menu car's brake model: the body's part 2 / 3) or of
 * its flat pieces (a race car's disc and caliper quads).  The push is the
 * least move along +x that leaves kPcBrakeClear between the two in every
 * ring both are in - at most kPcBrakeMaxPush, never to within
 * kPcBrakeKeepIn of the tyre's inner face, and never past the wheel's next
 * layer (its back plate would hide the brake).
 *
 * The log has a line for each brake moved ("PC: brake fit: ..."):
 * RaceTireModel::pcFitBrake's (T449) and MenuTireModel::pcFitBrake's (T450),
 * an axle's first tyre's.  GT3PC_BRAKE_FIT=0 in the environment: no push
 * (the game's own places); GT3PC_BRAKE_FIT_LOG=1: a line for every fit, the
 * ones that move nothing too (the distance found). */
#ifndef GAME_CAR_MODEL_PC_BRAKE_FIT_H
#define GAME_CAR_MODEL_PC_BRAKE_FIT_H

#include "game/game.h"

class ModelSet;                             /* T451 */

struct PcRadial {
    enum { kBins = 64, kCells = 512, kWords = kCells / 64, kSectors = 32 };
    f32 step;                               /* a ring's width */
    f32 lo[kBins], hi[kBins];               /* every triangle: the least / greatest x in the ring (where has) */
    u8  has[kBins];
    u64 cells[kBins][kWords];               /* the faces and sides: the cells along x they are in */
    f32 front[kBins][kSectors];             /* the faces: the least x in each direction of the ring (where faced) */
    u32 faced[kBins];
    /* (pc_radial_model's last step) the wheel's back in the ring (where
     * hasBack), and where the first layer behind its body starts (back +
     * 100: none) */
    f32 back[kBins], behind[kBins];
    u8  hasBack[kBins];
    u8  closed[kBins];                      /* the ring is closed all round (back = its closing faces') */
    int triangles;                          /* triangles read (the flat ones too) */
    int round;                              /* of them, round ones */
    int pieces, layers;                     /* pieces found; of them, layers behind */
};

/* How far out the fits' rings go, in rim radii (past the rim's lip). */
const f32 kPcRadialMax = 1.28f;

/* The rings out to rMax (cleared), then m's part at level lod into them. */
void pc_radial_clear(PcRadial *out, f32 rMax);
void pc_radial_model(PcRadial *out, ModelSet *m, int part, int lod);
/* A wheel's rings out to kPcRadialMax, kept: the last few wheels read are
 * not read again (a car's four tyres share one or two wheel parts; reading
 * one takes a millisecond or two).  A wheel is known by its model's address,
 * part and level and by a sum over its packets' headers and the part's
 * commands - a wheel file read over another at the same address (the wheel
 * shop) is another wheel.  NULL: no model.  The pointer holds until the
 * next call; the game thread's. */
const PcRadial *pc_radial_wheel(ModelSet *m, int part, int lod);
/* A flat piece at x between the radii r0 and r1 (a race car's disc: 0 to its
 * radius; its caliper: the quad's nearest and farthest points). */
void pc_radial_flat(PcRadial *out, f32 x, f32 r0, f32 r1);
/* How far the brake must move along +x for the wheel's back to clear its
 * face by `clear' in every ring both are in (0: it clears, or they share no
 * ring).  *gap: the least distance found, before the move (clear + 1 when
 * they share no ring); *room: how far the brake's face can move before the
 * wheel's next layer (100 when there is none). */
f32 pc_radial_push(const PcRadial *wheel, const PcRadial *brake, f32 clear, f32 *gap, f32 *room);

/* The wheel model carries a brake of its own where this one is (the '67
 * Cobra's menu wheel: a disc and a caliper inside it, the car's brake model
 * a copy at the same place): in more than half the rings both are in, a
 * surface of the wheel starts at the brake's face.  Nothing is moved then. */
bool pc_radial_same(const PcRadial *wheel, const PcRadial *brake);

/* The limits, in metres. */
extern const f32 kPcBrakeClear;             /* left between the wheel and the brake */
extern const f32 kPcBrakeMaxPush;           /* a push at most */
extern const f32 kPcBrakeKeepIn;            /* the brake's face stays this far from the tyre's inner face */

/* GT3PC_BRAKE_FIT (1 unless set to 0), GT3PC_BRAKE_FIT_LOG (0 unless set). */
bool pc_brake_fit_on(void);
bool pc_brake_fit_log(void);

/* What a fit found, for its log line. */
struct PcBrakeFit {
    f32  gap;                               /* the wheel's back to the brake's face before the move, metres
                                               (negative: the wheel is in the brake) */
    bool limited;                           /* the move is less than the wheel asks for (a limit) */
    bool own;                               /* the wheel carries this brake itself (pc_radial_same) */
    bool apart;                             /* they share no ring */
};

/* The push in metres of a brake whose rings are brake (rim radii) behind
 * wheel, for a tyre of this rim radius and width: pc_radial_push with the
 * limits; 0 with GT3PC_BRAKE_FIT=0.  found (may be NULL): for the log. */
f32 pc_brake_fit(const PcRadial *wheel, const PcRadial *brake, f32 rim, f32 width, PcBrakeFit *found);

#endif /* GAME_CAR_MODEL_PC_BRAKE_FIT_H */
