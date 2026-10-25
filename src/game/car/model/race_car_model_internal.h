/* race_car_model_internal.h - Gran Turismo 3 A-Spec US (SCUS-97102) main executable, unit u060,
 * 0x001A9B90..0x001AC870: translation units T180, T181.
 *
 *   T180  RaceCarModel: the drawn model of a race car (RaceEntryCar +0x11F8)
 *         and the car model arena g_002C2F50 (include/game/
 *         T180_race_car_model.h).  Also T180's weak copies of the
 *         UnitArena<64> template members.
 *   T181  CarRenderCtx: the lighting and shadow passes of one car for a
 *         frame (include/game/car/model/T181_car_lighting.h).
 *
 * Every shared class, function and global is declared by the canonical
 * headers, which each file includes itself (in the order that gives
 * retail's type names: the data pass, ref/data/u060/notes.md).  This header
 * keeps only what is local to the unit: named constants, T180's .rodata
 * table g_00330B48, and static helpers for the VU0 register moves of the
 * original.
 *
 * Offsets in comments are the ORIGINAL ones (egcs / GCC 2.9x ee layout).
 */
#ifndef U060_H
#define U060_H

#include "types.h"
#include "vu0asm.h"                         /* VU0_ACC, VU0_Q */
#include "engine/pgl/native/vu0_game.h"     /* PC: the VU0 register file and its operations (the asm below) */

/* ======================================================================== */
/* Named constants                                                          */
/* ======================================================================== */

#define RAD_TO_DEG          57.29578f       /* 180 / pi (0x42652EE1 as stored) */
#define KMH_TO_MS_SQ        0.07716049f     /* 1 / 3.6^2: (km/h)^2 to (m/s)^2 */

/* The car model arena (RaceCarModel::initArena / setArena): one block per
 * car file, 7 blocks (+ 0x80 for the 64-byte alignment of the first). */
enum {
    CAR_MODEL_BLOCK_SIZE = 0xA8000,
    CAR_MODEL_ARENA_SIZE = 0x498080
};
/* PC: the tyres' work space of a car model (RaceCarModel::setupTires: the
 * four RaceTireModels' three levels of packets).  Their geometry is the
 * code's (18 / 12 / 8 segments), not the files': about 0x14000 bytes for
 * four F1 tyres with brakes, so this leaves more than 3x room (an overrun
 * is logged). */
enum { RACE_TIRE_WORK_SIZE = 0x40000 };

/* file_req_alloc's kind 0: a file of the volume, unpacked (T504). */
enum { FILE_REQ_PLAIN = 0 };

#define SPARK_LIFT          0.05f           /* the sparks start this far above the road */

/* The reflections of the lamps of list B (drawLampReflections). */
#define LAMP_B_REFLECTION   0.3f

/* CarRenderCtx::setup: the runway lights looked at, from 4 before the one
 * the car is at; the GL lights of the car; the glows. */
enum {
    CAR_LIGHT_LOOK_BACK  = 4,
    CAR_LIGHT_CANDIDATES = 9,
    CAR_LIGHT_MAX        = 4,
    CAR_GLOW_MAX         = 6,
    CAR_LIGHT_UNITS      = 8                /* PGL_CAP_LIGHT0 .. + 7 */
};
#define SPOT_NO_CONE        180.0f          /* CourseEnvLightA::cutoff: not a spot light */

/* car_glow_draw: a fan of 10 segments, 36 degrees each. */
enum { GLOW_SEGMENTS = 10 };
#define GLOW_COS            0.809017f       /* cos 36 */
#define GLOW_SIN            (-0.58778524f)  /* -sin 36 */

/* drawGlows: the state saved around it (PGL_ATTR_*: 0x8C7) and the
 * orthographic projection of the sphere map. */
#define GLOW_ATTRIBS        (PGL_ATTR_SCISSOR | PGL_ATTR_VIEWPORT | PGL_ATTR_FOG | PGL_ATTR_TARGET_B | \
                             PGL_ATTR_DEPTH | PGL_ATTR_DRAW_BUFFER)
#define GLOW_ORTHO          0.9f

/* gfx_set_frame_write_mask: the bits written (G001: FBMSK = ~mask). */
#define FRAME_WRITE_RGBA    0xFFFFFFFFu
#define FRAME_WRITE_RGB     0x00FFFFFFu
#define FRAME_WRITE_ALPHA   0xFF000000u

/* ======================================================================== */
/* T180 data without a canonical declaration                                */
/* ======================================================================== */

/* drawTires: RaceTireModel::draw's blurPasses by the blur mode: {0 none,
 * 16 rendered passes (the camera's target), -1 the cheap blur (the others),
 * 0}.  T180 .rodata (defined in T180: its .rodata starts with it). */
extern const int g_00330B48[4];             /* 0x00330B48 */

/* ======================================================================== */
/* VU0 helpers                                                              */
/* ======================================================================== */

/* EE rsqrt.s fd, fs, ft: fs / sqrt(ft).  PC: with the EE's result for ft =
 * 0 (+-max float, not infinity: vu0g::eeRsqrt). */
static inline f32 ee_rsqrt(f32 num, f32 sq)
{
    return vu0g::eeRsqrt(num, sq);
}

#define vu0_read_translation vu0_get_translation  /* (with the vnop: same values) */
/* out.xyz = vf31 (the translation of the current matrix): the original
 * moves it through a GPR (qmfc2.ni + pcpyud + sdl/sdr/swl/swr, unaligned,
 * 12 bytes); this goes through an aligned temporary. */
static inline void vu0_get_translation(f32 *out)
{
    /* PC: vnop; sqc2 $vf31 -> the port's vf31 (pgl::g_vu0) */
    const vu0g::Vec4 &v = vu0g::vf(31);
    out[0] = v.x; out[1] = v.y; out[2] = v.z;
}

/* out.xyz = vf29 (the y column of the current matrix: the up vector). */
static inline void vu0_get_y_axis(f32 *out)
{
    /* PC: vnop; sqc2 $vf29 -> the port's vf29 */
    const vu0g::Vec4 &v = vu0g::vf(29);
    out[0] = v.x; out[1] = v.y; out[2] = v.z;
}

/* out = the screen position of in (x, y, z, w): vf16 = vf24..vf27 (the
 * combined matrix) x (in.xyz, 1); Q = 1 / vf16.w; vf16 = min(vf16, vf21) x
 * Q (vf21: the limits vu0_load_screen_regs loads). */
static inline void vu0_project(f32 *out, const f32 *in)
{
    /* PC: the VU0 asm (lqc2 vf16; vmulax / vmadday / vmaddaz / vmaddw vf0w
     * through vf24..vf27; vdiv Q, vf0w, vf16w; vmini vf21; vmulq) on the
     * port's registers; vf16 was scratch (vu0g::projectCombined). */
    vu0g::Vec4 v = vu0g::projectCombined(vu0g::Vec4{in[0], in[1], in[2], in[3]});
    out[0] = v.x; out[1] = v.y; out[2] = v.z; out[3] = v.w;
}

#endif /* U060_H */
