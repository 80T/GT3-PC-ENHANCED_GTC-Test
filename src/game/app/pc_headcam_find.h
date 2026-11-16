/* pc_headcam_find.h - PC: where the driver's head is in a car model
 * (pc_headcam.h).  No original: new PC code.  No game headers: the offline
 * check over the car files builds this unit too (ROADMAP 57).
 *
 * The driver is drawn by the body packets' VU1 batches (VifParts), and
 * nothing names or flags him: on most cars the helmet is a small batch of
 * its own - a few planes carrying a helmet silhouette texture, or a rounded
 * mesh - beside a torso batch; on others (ho0064, the rally cars' two
 * drivers) he is merged into a batch with the seats, 1.5 m wide, and only
 * his vertices tell.  So, per seat side (x = +-0.33 of the centre line, the
 * "Driver" mount's eye for the height and depth: the bumper view's, at head
 * height on the centre line):
 *   - a batch of its own: a box at least 0.18 m on every side and at most
 *     0.6 across, 0.45 high and 0.4 deep (a head, or a head with its
 *     shoulders; a seat is taller and deeper, a wiper wider, a mirror
 *     thinner), on that side, its centre within 0.25 m in height and depth
 *     of the eye; of several the most like a modelled head - 16 vertices
 *     or more, about as deep as high (a headrest or a roll hoop behind the
 *     driver is a card of 6 .. 8 vertices, thin, and reached higher than
 *     ty2056's helmet) - then the one reaching highest (the shoulders sit
 *     beside the eye too, under the head's top);
 *   - else a merged one: the batch with the most vertices within 0.25 m of
 *     the seat's head point (6 at least, their box 0.12 .. 0.5 m a side: a
 *     blob, not a card), the head's box being those vertices' - the seat's
 *     top with them.
 * The head found first is a batch of its own anywhere (the open-wheel cars'
 * helmet sits on the centre line), else the fuller merged one of the two
 * sides; a second (a second cockpit camera: the rally cars' other seat -
 * which goes first in the result, user request: the driver's) where the
 * other side has a batch of its own too that is like the first's head (as
 * many vertices, sizes within half again; a hoop mirrored on the passenger
 * side is not), or the first's box mirrored across
 * the centre line (grown 3 cm) holds vertices whose span is a head's: at
 * least 0.15 m across, 0.20 high and 0.08 deep (op0010's second driver is a
 * figure on a card, 10 cm deep), at most 0.45 each way (a passenger seat's
 * top, a visor or a mirror is not).
 * The eye: 10 cm under the head box's top (a helmet's eye line; the centre
 * sat too low, lower still in a head drawn with its neck or shoulders), or
 * 13 cm under a merged box's top (the seat is in it); a batch sitting on
 * the head's top (a helmet's crown or visor drawn apart: wl1001) is taken
 * into the box first.  Units: metres, +y up, -z forward, +x the car's
 * right, the origin on the ground on the centre line. */
#ifndef GAME_APP_PC_HEADCAM_FIND_H
#define GAME_APP_PC_HEADCAM_FIND_H

namespace headcam {

struct HeadBox {
    float centre[3];        /* the box's centre */
    float bmin[3], bmax[3]; /* the head's box (the car's frame) */
    float eye[3];           /* where the camera goes */
    bool merged;            /* in a batch with the seats (else a batch of its own) */
    int verts;              /* the vertices that are it */
};

constexpr int kMaxGlass = 16;

struct Ref {
    const void *model;      /* a VifModel (a body packet) */
    int part;               /* a VifPart (batch) in it */
    float lo[3], hi[3];     /* PC (2026-11-14): its box (a custom camera's eye in it: pc_headcam_hide_at) */
};

struct Heads {
    int count;              /* 0, 1 or 2 */
    HeadBox head[2];        /* with two, the one found second first (user request: the driver's seat
                             * on the cars tried), the first-found as "Cockpit 2" */
    /* The translucent shells round the first head's eye wound to face it
     * (ho0030's cabin glass: a tint the eye in the cabin sees, where every
     * other car's glass faces outward and culls from inside): not drawn in
     * the cockpit view. */
    int glassCount;
    Ref glass[kMaxGlass];
};

/* packets: the body ModelSet's VifModels, relocated in place (the game's
 * 32-bit case: the data words are pointers); eye: the Driver mount's eye in
 * the car's frame, or NULL for a file without a mount table (some colour
 * variants' files): the cars' mean eye is taken, 0.90 m up and 0.15 m back,
 * with windows half again as wide. */
void find_heads(const void *const *packets, int packetCount, const float eye[3], Heads *out);

/* Every batch of the packets, with its box: fn(model, part, lo, hi, user)
 * (the offline check's listing). */
typedef void (*BatchFn)(const void *model, int part, const float lo[3], const float hi[3], void *user);
void list_batches(const void *const *packets, int packetCount, BatchFn fn, void *user);

} // namespace headcam

#endif /* GAME_APP_PC_HEADCAM_FIND_H */
