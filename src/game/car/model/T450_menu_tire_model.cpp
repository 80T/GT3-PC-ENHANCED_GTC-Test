/* T450_menu_tire_model.cpp - MenuTireModel (unit u101, 0x00224090..0x00224600).
 *
 * The tyre of a menu car (TheCar in the car lists, the menu car model): one
 * TirePacket of 30 segments, and the wheel drawn as part 2 (front) or 3
 * (rear) of the car's own model set.  Its view test also decides whether
 * the outer face is turned to the camera (faceVisible).
 *
 * Data (retail, all EXACT at these places): .rodata 0x003410C0..0x003410F8
 * (the origin, the vtable, the type name: no other class seen, no <new>),
 * .lit4 0x003509B4..0x003509BC, .data 0x002E70AC..0x002E721C (frame info).
 */
#include "game/car/model/tire_model_internal.h"
#include "game/car/model/T450_menu_tire_model.h"
#include "game/app/pc_menu_race_model.h"         /* PC (2026-11-12): a race wheel's face */
#include "game/car/model/pc_brake_fit.h"         /* PC (2026-11-13): the brake behind the wheel */

// FUNC 0x00224090 MenuTireModel::MenuTireModel
//   TireModelBase(), the vptr 0x003410D0, TirePacket() of the packet, no
//   wheel model (with the EH cleanup ~TireModelBase).  Callers:
//   gt2k_menu::CarsItem::CarsItem, gt2k_menu::Car::Car.
MenuTireModel::MenuTireModel()
{
    /* implicit: TireModelBase, ~TireModelBase, TirePacket (compiler-generated: bases, members,
     * deleting form) */
    wheelModel = NULL;
    pcBrakePush = 0.0f;                     /* PC (2026-11-13): no brake fit yet */
    pcBrakeFitted = 0;
}

// FUNC 0x00224100 MenuTireModel::~MenuTireModel
//   (slot 0) The vptr, ~TirePacket of the packet (flags 2), then
//   ~TireModelBase with the flags (the deleting form's operator delete is
//   TireModelBase's).
MenuTireModel::~MenuTireModel()
{
    /* implicit: ~TireModelBase, ~TirePacket (compiler-generated: bases, members, deleting form) */
}

// FUNC 0x00224150 MenuTireModel::build
//   Sets up the packet (segs segments, bevel, inside faces and shadow strip,
//   x0 0.001: TirePacket::setup) and builds it at work (TirePacket::
//   virtual_1, called directly); returns the end.  Callers:
//   gt2k_menu::TheCar::setup, gt2k_menu::Car::setupTires (with 30).
void *MenuTireModel::build(void *work, int segs)
{
    TirePacket *p = &packet;
    p->setup(this, segs, 1, 1, 1, 0.001f);
    pcBrakeFitted = 0;                      /* PC (2026-11-13): its files and size are new: draw fits the brake again */
    return p->TirePacket::virtual_1(work);
}

/* PC (2026-11-13, no original): see T450_menu_tire_model.h.  The wheel: its
 * file's model, the part drawn for this position; the brake: the car's
 * model set's part (none in a race model's, [Debug] MenuRaceModel: nothing
 * to move).  (The rings are the game thread's, as the draws.)
 * PC (2026-11-14; user: "A crash in the wheel shop.  Was scrolling through
 * the wheel list at the maximum speed possible with the dpad"): not while
 * the wheel file is not in (filesIn 0).  A menu car changes its wheel by
 * reading the new file over the old one in the model's buffer (gt2k_menu::
 * Car::loadStep, state 2: file->wheel), over several frames, with tiresReady
 * 0 until Car::wheelLoaded has relocated it and built the tyres again;
 * meanwhile draw still runs (withTire 0: no tyre, no rim, the brake part of
 * the body alone) and the wheel file holds the new file's raw bytes - its
 * `model' the offset 0x20.  A wheel chosen before the last one's first draw
 * (its build left pcBrakeFitted 0) had the fit read that: the crash in
 * stamp_of, reading 0x30.  The push stays the last wheel's until the new
 * one's fit. */
void MenuTireModel::pcFitBrake(int part, int filesIn)
{
    static PcRadial brake;
    if (filesIn == 0)
        return;
    pcBrakePush = 0.0f;
    pcBrakeFitted = 1;
    if (!pc_brake_fit_on() || wheelFile == NULL || wheelFile->model == NULL || wheelModel == NULL || !(rim > 0.0f))
        return;
    pc_radial_clear(&brake, kPcRadialMax);
    pc_radial_model(&brake, wheelModel, part, 0);
    if (brake.triangles == 0)
        return;
    const PcRadial *wheel = pc_radial_wheel(wheelFile->model, wheel_file_lod(wheelFile, position), 0);
    PcBrakeFit found;
    pcBrakePush = pc_brake_fit(wheel, &brake, rim, width, &found);
    /* the log: a brake moved (every fit with GT3PC_BRAKE_FIT_LOG=1), an
     * axle's first tyre's line only */
    if ((position & 1) == 0 && (pcBrakePush > 0.0f || pc_brake_fit_log()))
        printf("PC: brake fit: a menu car's %s brake model (%d triangles): the wheel's back %+.1f mm from it, "
               "moved back %.1f mm%s%s\n",
               position / 2 ? "rear" : "front", brake.triangles, found.gap * 1000.0f, pcBrakePush * 1000.0f,
               found.limited ? " (limited)" : "", found.own ? " (the wheel has a brake of its own there)" : "");
}

/* The origin (0, 0, 0, 1): T450's first .rodata item (0x003410C0, before its
 * vtable and type name), so external linkage (tire_model_internal.h's extern): egcs writes
 * it where it is defined.  Defined after the file's first function, so the
 * frame info keeps its name. */
const f32 g_003410C0[4] = { 0.0f, 0.0f, 0.0f, 1.0f };     /* 0x003410C0 */

// FUNC 0x002241A8 MenuTireModel::virtual_1
//   (slot 1) The view test: 0 if the bounding box is out of the view
//   (clipTest).  Else the eye in the tyre's space (vu0_invert_rigid of the
//   pushed matrix applied to the origin), seen from the face plane x =
//   -width / 2: faceVisible = its direction cosine along x (rsqrt.s) <
//   0.05; returns 1.
int MenuTireModel::virtual_1()
{
    if (clipTest() == 0)
        return 0;
    f32 eye[4];                             /* sp+0x00 */
    pgl_push_matrix();
    vu0_invert_rigid();
    vu0_apply_matrix(eye, g_003410C0);
    pgl_pop_matrix();
    f32 halfW = width * 0.5f;
    f32 yy = eye[1] * eye[1];
    halfW = -halfW;
    f32 zz = eye[2] * eye[2];
    f32 x = eye[0] - halfW;
    eye[0] = x;                             /* a dead store, as in the code */
    f32 cosine = ee_rsqrt(x, x * x + yy + zz);
    faceVisible = cosine < 0.05f ? 1 : 0;
    return 1;
}

/* PC (2026-11-12, no original): a race wheel in a menu ([Debug]
 * MenuRaceModel, game/app/pc_menu_race_model.h).  Its model draws the face
 * from the second texture table's TEX2_WHEEL_FACE slot (texture 0x1FF at
 * base 1), which RaceTirePacket::drawWheelTexture fills in a race: here the
 * still face of the wheel's texture set for `level' (set level + the
 * wheel's part x 3, as there), its variation, made resident in `set' (alive
 * through the draw) into table[TEX2_WHEEL_FACE], the table's environment entry copied to
 * table[0], and table set as the second table.  Returns the table it
 * replaced (texsel_set_table2 it back after the draw), NULL when nothing was
 * done (no face, no table). */
static GsTexRegs *pc_race_wheel_face(MenuTireModel *m, int level, TexSet *set, GsTexRegs *table)
{
    WheelFile2 *w = m->wheelFile;
    GsTexRegs *env = texsel_get_table2();
    if (w == NULL || w->model == NULL || env == NULL)
        return NULL;
    ModelSet *model = w->model;
    Tex1Image *face = NULL;
    if (model->texSets != NULL && model->nTexSets != 0) {
        int texSet = level + wheel_file_lod(w, m->position) * RACE_TIRE_LEVELS;
        if (texSet >= model->nTexSets)
            texSet = model->nTexSets - 1;
        face = model->texSets[texSet][WHEEL_TEX_FACE];
    } else if (model->tex != NULL && model->nTex > WHEEL_TEX_FACE) {
        face = model->tex[WHEEL_TEX_FACE];
    }
    if (face == NULL)
        return NULL;
    const int variation = wheel_file_variation(w, m->color, m->position);
    if (variation >= 0)
        tex1_select_variation(face, variation);
    set->load(face);
    table[0] = env[0];
    memset(&table[TEX2_WHEEL_FACE], 0, sizeof(table[TEX2_WHEEL_FACE]));
    table[TEX2_WHEEL_FACE].tex0 = face->tex->tex0;
    table[TEX2_WHEEL_FACE].tex1 = face->tex->tex1;
    table[TEX2_WHEEL_FACE].miptbp1 = face->tex->miptbp1;
    table[TEX2_WHEEL_FACE].miptbp2 = face->tex->miptbp2;
    table[TEX2_WHEEL_FACE].clamp = face->tex->clamp;
    texsel_set_table2(table);
    return env;
}

// FUNC 0x00224260 MenuTireModel::draw
//   In the current matrix: rotated by steer (+ 180 on the odd side) about
//   y and camber about z (degrees; the third float is not read).  Nothing
//   if the view test (virtual_1, a virtual call) fails.  Moved by -width /
//   2, scaled by the rim radius (pgl_set_scale), mirrored if the wheel file
//   says so (and the other cull face); the tyre (withTire, once built:
//   TirePacket::draw, texture alpha off around it); with the face seen and
//   a wheel model, the model's part 3 (rear: position / 2 != 0) or 2
//   (modelset_draw_part), mirrored once more when the side and the
//   mirroring differ (so it is mirrored on the odd side only).  That flip
//   sets the other cull face again, not the one found: on the even side
//   of a mirrored wheel file the model is drawn unmirrored with the
//   mirrored cull face (? a bug).  Then, with a tyre file that is not an F1 tyre,
//   withTire, withShadow and a built packet: the shadow strip
//   (TirePacket::drawShadow) without the steer, colour only.  Callers:
//   gt2k_menu::TheCar::drawTires, CarRender::drawModel.
void MenuTireModel::draw(f32 steer, f32 camber, f32 unused, int withTire, int withShadow)
{
    pgl_push_matrix();
    int side = position & 1;
    vu0_rotate_y(steer + (side ? 180.0f : 0.0f));
    vu0_rotate_z(camber);
    if (virtual_1() == 0) {
        pgl_pop_matrix();
        return;
    }
    if (clipped == 0)
        gfx_disable(PGL_CAP_VU1_PROG_33E);
    gfx_disable(PGL_CAP_ALPHA_TEST);
    int otherMode = 1;
    gfx_disable(PGL_CAP_DEST_ALPHA);
    gfx_enable(PGL_CAP_DEPTH_TEST);
    gfx_set_texture_function(PGL_TFX_MODULATE);
    gfx_depth_mask(1);
    gfx_set_frame_write_mask(GS_WRITE_RGBA);
    pgl_push_matrix();
    gfx_enable(PGL_CAP_FLAG24);
    gfx_enable(PGL_CAP_SCALE);
    pgl_translate(-width * 0.5f, 0.0f, 0.0f);
    int mode = gfx_get_cull_face();
    if ((mode ^ 1) == 0)                    /* gfx_other_cull_mode(mode) */
        otherMode = 2;
    int mirror = wheel_file_mirrored(wheelFile, position);
    f32 r = rim;
    pgl_set_scale(r, r, r);                 /* F_00257F40 */
    if (mirror != 0) {
        vu0_scale(1.0f, 1.0f, -1.0f);
        gfx_cull_face(otherMode);
    }
    /* PC (2026-11-12): a race wheel's face (pc_race_wheel_face) for the
     * wheel TirePacket::draw draws */
    alignas(16) GsTexRegs pcTable[1 + TEX2_WHEEL_FACE];
    TexSet pcFace;
    GsTexRegs *pcEnv = NULL;
    const int pcLevel = pc_menu_race_wheel_level();
    if (pcLevel >= 0 && withTire != 0 && faceVisible != 0)
        pcEnv = pc_race_wheel_face(this, pcLevel, &pcFace, pcTable);
    if (withTire != 0) {
        TirePacket *p = &packet;
        if (p->built != 0) {
            gfx_tex_param(PGL_TEXPARAM_TCC, PGL_TCC_RGB);
            p->draw();
            gfx_tex_param(PGL_TEXPARAM_TCC, PGL_TCC_RGBA);
        }
    }
    if (pcEnv != NULL)
        texsel_set_table2(pcEnv);           /* PC (2026-11-12): the menu's table back */
    if (faceVisible != 0 && wheelModel != NULL) {
        /* flipped again when the side and the mirroring differ: a ?: as
         * the condition (a jump from each arm, retail's sltu / xori) */
        bool mirrored = mirror;
        if ((side ? mirrored ^ 1 : mirrored) != 0) {
            vu0_scale(1.0f, 1.0f, -1.0f);
            gfx_cull_face(otherMode);
        }
        /* PC (2026-11-13): the brake model moved back along the axle until
         * the wheel clears it (pcFitBrake; metres: PGL_CAP_SCALE scales the
         * model, not this) */
        /* (PC 2026-11-14: fitted once the wheel file is in - withTire, a
         * menu car's tiresReady: not while the next wheel is read over it) */
        const int pcPart = position / 2 != 0 ? 3 : 2;
        if (pcBrakeFitted == 0)
            pcFitBrake(pcPart, withTire);
        if (pcBrakePush > 0.0f) {
            pgl_push_matrix();
            pgl_translate(pcBrakePush, 0.0f, 0.0f);
        }
        if (position / 2 != 0)
            modelset_draw_part(wheelModel, 3);
        else
            modelset_draw_part(wheelModel, 2);
        if (pcBrakePush > 0.0f)
            pgl_pop_matrix();
    }
    gfx_cull_face(mode);
    gfx_disable(PGL_CAP_SCALE);
    gfx_disable(PGL_CAP_FLAG24);
    pgl_pop_matrix();
    gfx_enable(PGL_CAP_VU1_PROG_33E);
    gfx_depth_mask(1);
    gfx_set_frame_write_mask(GS_WRITE_RGBA);
    pgl_pop_matrix();
    if (tireFile == NULL)
        return;
    if (tireFile->isF1())
        return;
    if (withTire == 0)
        return;
    if (withShadow == 0)
        return;
    TirePacket *p = &packet;
    if (p->built == 0)
        return;
    pgl_push_matrix();
    vu0_rotate_y(side ? 180.0f : 0.0f);
    vu0_rotate_z(camber);
    gfx_set_frame_write_mask(GS_WRITE_RGB);
    p->drawShadow();
    gfx_set_frame_write_mask(GS_WRITE_RGBA);
    pgl_pop_matrix();
}

// FUNC 0x002245A8 MenuTireModel::setWheelModel
/* inline in game/car/model/T450_menu_tire_model.h; egcs emits this copy */

