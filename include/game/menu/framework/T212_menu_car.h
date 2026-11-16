/* T212_menu_car.h - gt2k_menu::Car, the 3D car of the menu screens
 * (translation unit T212, unit u064), 0x001B8330..0x001B9318.
 *
 * The car of the garage, the shops, the car select screens: a MenuCarCamera
 * (T411), a CarRender (T245), the body CarModel (T446) and four
 * MenuTireModels (T450).  The model file of the car and its wheel file are
 * loaded asynchronously (T507) into a 0xA8000-byte buffer per player (on the
 * menu heap, shared and reference counted: g_00352550 / g_00352558, the
 * loading flags g_00352560, and the file-local menu_car_alloc_buffer
 * 0x001B8330 / menu_car_free_buffer 0x001B83C0 / menu_car_release_buffer
 * 0x001B83F0 / menu_car_any_loading 0x001B8498).  The car fades in once the
 * model (and the tyres) are in; colour changes blend from the colour shown.
 */
#ifndef GAME_T212_MENU_CAR_H
#define GAME_T212_MENU_CAR_H

#include "game/game.h"
#include "game/menu/framework/T216_menu_transitive.h"      /* base gt2k_menu::Transitive */
#include "game/camera/T411_menu_car_camera.h"      /* MenuCarCamera (camera) */
#include "game/car/model/T245_car_render.h"           /* CarRender (render) */
#include "game/car/model/T446_car_model.h"            /* CarModel (model) */
#include "game/car/model/T450_menu_tire_model.h"      /* MenuTireModel (tires) */
#include "game/menu/framework/T260_motion_value.h"         /* MotionValue (fade, blend) */

struct MenuCarSpec;                         /* T232 (pointers only) */
struct CarModelFile;                        /* T456: a loaded car model file */
struct Tex1Image;                           /* G001 */

/* Declared inline in CarRender (T245); defined here, where MenuCarCamera
 * (T411) is complete and before Car's inline members use it. */
inline void CarRender::setView(f32 focal, f32 tiltX, f32 tiltY)
{
    camera->setProjection(-320.0f, 320.0f, -224.0f, 224.0f, 0.1f, 1000.0f, focal);
    camera->setTilt(tiltX, tiltY);
}

namespace gt2k_menu {

/* vtable 0x00333C40 (8 slots: [3] Object's, [4..7] Transitive's).  typeinfo
 * __tfQ29gt2k_menu3Car 0x001B92C8 (__si_type_info 0x004B24D0, base
 * Transitive).  Size 0x720. */
class Car : public Transitive {
public:
    /* buffer: the model buffer to use (kept by the caller), NULL: the
     * player's shared one.  envImage goes to render.envImage. */
    Car(Tex1Image *envImage, MenuCarSpec *spec, int player, void *buffer);  /* 0x001B84C8 */
    virtual ~Car();                         /* [0] +0x0C 0x001B8868 releases the shared buffer */
    /* Update: the scripts, the colour blend, loadStep(), then the camera and
     * the fade in once the model (and, except in presets 6 / 7, the tyres)
     * are in.  The enhanced engine (menus_a.md 33, the rendering area's
     * port): the shown car's model animations step and apply every frame;
     * while it is not shown its animation id goes to 1 then 0. */
    virtual void virtual_1();               /* [1] +0x14 0x001B8EC8 */
    virtual void virtual_2();               /* [2] +0x1C 0x001B9068 draw: the mirror pass (drawMirror),
                                               then the car */
    void setCar(const char *car);           /* 0x001B8998 the car to show (NULL: none); restarts the
                                               loading, interrupts off */
    void setWheel(const char *name);        /* 0x001B8A58 the wheel ("menu/wheel/<name>"; NULL: the car's
                                               own) */
    void setColor(int color);               /* 0x001B8B20 blends from the colour shown to color */
    void setupTires(CarModelFile *file);    /* 0x001B8B50 the four tyres, their data after the file */
    static void modelLoadedCallback(void *ctx, void *data); /* 0x001B8C30 the loader's (ctx: the Car) */
    void modelLoaded(CarModelFile *file);   /* 0x001B8C70 */
    static void wheelLoadedCallback(void *ctx, void *data); /* 0x001B8CD0 */
    void wheelLoaded(void *wheelFile);      /* 0x001B8CF0 */
    void loadStep();                        /* 0x001B8D28 a frame of the loading (loadState) */
    /* Inline in the original: no callers, and 0x001B91E8..0x001B92C0 are
     * the out-of-line copies egcs emits before the typeinfo function; T036's
     * make_car_render (u010) expands setDirt, setCameraMode and
     * updateViewport, T010 0x0010616C setCameraMode.  Declared in the
     * reverse of their addresses: egcs writes a class's in-class inline
     * members in reverse declaration order (T212's frame info 0x002C77B0:
     * getCamera's FDE first, setCameraMode's last, then the __tf's). */
    void setCameraMode(int mode) { camera.type = mode; } /* 0x001B92C0 (MenuCarCamera::setType's store) */
    void setDrawMirror(int on) { drawMirror = on; }     /* 0x001B92B8 (F_001B92B8) */
    /* The render's viewport from the rectangle (CarRender::setViewport).
     * x / y through MotionValue::get(): u010's make_car_render reads them
     * through the pointers its Drawable::setRect left in $s4 / $s2 (GCSE
     * reuses the inline `this'); the copy is the same either way. */
    void updateViewport()                               /* 0x001B9280 */
    {
        render.setViewport(x.get(), y.get(), w, h);
    }
    void F_001B9278(int v) { render.unk0C = v; }        /* 0x001B9278 */
    /* render.camera's projection and tilt (CarRender::setView; its only
     * caller). */
    void setView(f32 focal, f32 tiltX, f32 tiltY)       /* 0x001B91F8 */
    {
        render.setView(focal, tiltX, tiltY);
    }
    void setDirt(f32 v) { dirt = v; }                   /* 0x001B91F0 (F_001B91F0) */
    MenuCarCamera *getCamera() { return &camera; }      /* 0x001B91E8 */

    MenuCarCamera camera;                   /* 0x0C4 */
    CarRender     render;                   /* 0x210 */
    MenuCarSpec  *spec;                     /* 0x2B4 */
    /* dirt: u010 sets it from CarCondition::dirt (0..0.8), u013's TumbleCar
     * to 0 when the car wash ends (modelFade before the sweep). */
    f32           dirt;                     /* 0x2B8 CarModel::setDirt when drawn */
    const char   *car;                      /* 0x2BC the car on show, NULL: none */
    void         *buffer;                   /* 0x2C0 0xA8000 bytes for the model file (PC: or bigBuffer) */
    int           ownBuffer;                /* 0x2C4 1: given by the caller */
    CarModelFile *file;                     /* 0x2C8 the loaded model file */
    CarModel      model;                    /* 0x2CC */
    MenuTireModel tires[4];                 /* 0x304 */
    int           tiresSet;                 /* 0x624 */
    /* PC (ROADMAP 18): the paths 0x100 bytes (retail 0x40: "menu/wheel/"
     * and a long wheel name ran over), wheelName's copy bounded; the
     * members after them sit 0x180 later than shown */
    char          modelPath[0x100];         /* 0x628 */
    char          wheelPath[0x100];         /* 0x668 */
    char          wheelName[0x40];          /* 0x6A8 */
    u8            unk_6E8[4];               /* 0x6E8 */
    int           loadState;                /* 0x6EC 0 wait, 1 model, 2 wheel, 3 done */
    int           ready;                    /* 0x6F0 the model is in */
    int           tiresReady;               /* 0x6F4 */
    int           drawMirror;               /* 0x6F8 (1) the mirror pass (CarRender::draw mirror 1) too */
    MotionValue   fade;                     /* 0x6FC the car's alpha */
    int           color;                    /* 0x708 */
    int           prevColor;                /* 0x70C */
    MotionValue   blend;                    /* 0x710 prevColor -> color */
    int           player;                   /* 0x71C 0 / 1: the shared buffer used */
    /* PC (ROADMAP 18): a buffer of the Car's own (main heap) for a model
     * file that, with its wheel file and the tyres, does not fit `buffer'
     * (NULL: none; freed as the shared one, deferred), and the bytes
     * `buffer' holds (MENU_CAR_BUFFER_SIZE, or the own buffer's). */
    void         *bigBuffer;                /* 0x720 */
    u32           bufferSize;               /* 0x724 */
    /* PC (ROADMAP 45): ticks left of the right stick's turn (pc_menu_car.h) */
    int           pcStickHold;
    /* PC (2026-11-12): modelPath / wheelPath are the race model's (cars/day,
     * wheel/: [Debug] MenuRaceModel, game/app/pc_menu_race_model.h); the
     * setting changed: the car loaded again (update) */
    int           pcRaceModel;
    void pcCheckRaceModel();
};

/* Clears the two "loading" flags (g_00352560).  Called by T010
 * ArcadeControl::carSelect2P (0x00108710). */
void menu_car_clear_loading(void);          /* 0x001B8468 */

} /* namespace gt2k_menu */

/* The model buffers: the players' shared ones (T212) and the arcade's two
 * (ArcadeControl::load, T010); a Car takes a caller's buffer for this size. */
#define MENU_CAR_BUFFER_SIZE  0xA8000       /* the largest model file (car_model_load) */

/* PC (ROADMAP 18): the bytes a menu car buffer needs for model file
 * modelPath with wheel file wheelPath loaded over its own (NULL: none):
 * both files (64-aligned) and the four tyre packets built after them; 0 when
 * the model file is not there.  The callers keep their retail size as the
 * least (T212's buffers, T061's prize cars). */
u32 menu_car_buffer_need(const char *modelPath, const char *wheelPath);

/* The enhanced engine: CarModel 0x1C longer: the members from tires on sit
 * 0x1C later than shown (GT3 0x720).  PC: the longer paths, bigBuffer /
 * bufferSize after them (polymorphic: MSVC's own layout, not checked). */
EGCS_SIZE(gt2k_menu::Car, 0x8C4);


#endif /* GAME_T212_MENU_CAR_H */
