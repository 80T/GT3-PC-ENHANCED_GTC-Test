/* T212 - gt2k_menu::Car, the 3D car of the menu screens,
 * 0x001B8330..0x001B9318.
 *
 * setCar(car) picks the model file of the car (MenuCarSpec's body model
 * code) and restarts the loading; loadStep (each frame, from update) waits
 * until no other Car loads (g_00352560: one flag per player), loads the
 * model file into the player's 0xA8000-byte buffer (shared by the Cars of a
 * player, reference counted, freed by a deferred call when the last one
 * goes), then the wheel file named by setWheel over the model's own wheel
 * file.  When the model is in, the body (CarModel) and the four tyres are
 * set up from it; update fades the car in (slower in the presets 0..3 and
 * 9) and runs the camera springs; draw renders the car with CarRender, the
 * mirror pass first (drawMirror), blending from the previous colour after
 * setColor.
 * PC (ROADMAP 18): a model whose file, wheel file and tyres do not fit the
 * 0xA8000 bytes goes into a buffer of the Car's own (bigBuffer). */
#define U064_T212                           /* page_manager_internal.h: this unit's declarations */
#include "game/menu/framework/page_manager_internal.h"
#include "game/menu/framework/T212_menu_car.h"
/* Not used here: retail T212's .rodata starts with the format string of
 * SimpleCameraBase::print's inline body, "(%f, %f, %f) %f %f %f\n"
 * (0x00333BC0, before the jump tables), so the original unit saw that
 * header (? through its camera header).  Unreferenced: the anchor below
 * starts T212's .rodata range there (it was counted to T211). */
#include "game/camera/T412_simple_camera_base.h"
#include "engine/io/T502_file_resolve.h"         /* PC: file_resolve (the buffer sizes) */
#include "engine/io/T503_vol_thread.h"           /* PC: vol_device_object */
// DATA_START 0x00333BC0

using namespace gt2k_menu;

/* max.s: egcs's >? operator (as the bootstrap's ee_maxf); GCC 14 has none. */
#define CAR_MAXF(a, b) ((a) > (b) ? (a) : (b))

/* The model buffers of the two players, their reference counts and the
 * "loading" flags (page_manager_internal.h).  Retail has them as zeros in T212's .sdata
 * (0x00352550..0x00352568, between T211's "menu" and u065's data): public
 * globals without an initialiser, which egcs writes as zeros into .sdata
 * (-G8, 8 bytes each) at the end of the file's output, in the order of
 * their first declaration (page_manager_internal.h's externs). */
void *g_00352550[2];                        /* 0x00352550 gp-0x54A0 */
int   g_00352558[2];                        /* 0x00352558 gp-0x5498 */
int   g_00352560[2];                        /* 0x00352560 gp-0x5490 */

// FUNC 0x001B8330 menu_car_alloc_buffer
//   The model buffer of `player' (allocated on first use), one reference
//   more; interrupts suspended.
static void *menu_car_alloc_buffer(int player)
{
    int intr = intr_suspend();
    void *buf = g_00352550[player];
    if (buf == 0)       /* PC: menu_heap_allocate: never full (ROADMAP 18) */
        buf = g_00352550[player] = menu_heap_allocate(MENU_CAR_BUFFER_SIZE, 0x40);
    g_00352558[player]++;
    if (intr)
        EI();
    return buf;
}

// FUNC 0x001B83C0 menu_car_free_buffer
//   The deferred free of menu_car_release_buffer (a LoaderFn: the second
//   argument is not used).
static void menu_car_free_buffer(void *buf, int unused)
{
    menu_heap_free(buf);    /* PC: by the block's address (retail: whichever heap was current) */
}

/* PC (ROADMAP 18): the deferred free of a Car's own buffer (main heap). */
static void menu_car_free_big(void *buf, int unused)
{
    free(buf);
}

/* PC: the (unpacked) size of a file, 0 when it is not there (file_resolve
 * once: file_size_retry waits for a missing file). */
static u32 menu_car_file_size(const char *path)
{
    FileLoc loc;            /* sp+0x00 */
    if (path == 0 || path[0] == '\0' || file_resolve(path, &loc, &vol_device_object) != 0)
        return 0;
    return loc.size;
}

/* PC: see T212_menu_car.h. */
u32 menu_car_buffer_need(const char *modelPath, const char *wheelPath)
{
    u32 model = menu_car_file_size(modelPath);
    if (model == 0)
        return 0;
    u32 wheel = menu_car_file_size(wheelPath);
    return ((model + 0x3F) & ~0x3Fu) + ((wheel + 0x3F) & ~0x3Fu) + 0x40 + MENU_CAR_TIRE_ROOM;
}

/* PC: makes the Car's buffer hold `need' bytes: a buffer of its own from
 * the main heap when it does not (the old own one freed as the shared one
 * is, deferred: the loader may still write into it).  Without memory the
 * load goes on into the buffer it has, as retail. */
static void menu_car_fit_buffer(gt2k_menu::Car *car, u32 need)
{
    if (need <= car->bufferSize)
        return;
    void *big = memalign(0x40, need);
    if (big == 0) {
        printf("PC: menu car %s: no 0x%X bytes for a buffer of its own\n", car->modelPath, need);
        return;
    }
    printf("PC: menu car %s: 0x%X bytes (model, wheel, tyres) > the 0x%X-byte buffer: one of its own\n",
           car->modelPath, need, car->bufferSize);
    if (car->bigBuffer != 0)
        loader_defer(menu_car_free_big, car->bigBuffer, 0);
    car->bigBuffer = big;
    car->buffer = big;
    car->bufferSize = need;
}

// FUNC 0x001B83F0 menu_car_release_buffer
//   One reference less; the last one frees the buffer on the loader thread
//   (deferred: the loader may still be writing into it).
static void menu_car_release_buffer(int player)
{
    int intr = intr_suspend();
    if (--g_00352558[player] == 0) {
        loader_defer(menu_car_free_buffer, g_00352550[player], 0);
        g_00352550[player] = 0;
    }
    if (intr)
        EI();
}

namespace gt2k_menu {

// FUNC 0x001B8468 menu_car_clear_loading
//   (Name not the original's: no symbol.)  Both players' "loading" flags
//   off.  Called by T010 ArcadeControl::carSelect2P (0x00108710).
void menu_car_clear_loading(void)
{
    for (int player = 1; player >= 0; player--)
        g_00352560[player] = 0;
}

} /* namespace gt2k_menu */

// FUNC 0x001B8498 menu_car_any_loading
//   1 if a Car of either player is loading.  The caller passes the Car
//   (loadStep: a0 = this), which is not used.
static int menu_car_any_loading(const Car *car)
{
    for (int player = 0; player < 2; player++)
        if (g_00352560[player] != 0)
            return 1;
    return 0;
}

namespace gt2k_menu {

// FUNC 0x001B84C8 gt2k_menu::Car::Car
//   buffer: the model buffer to use (kept by the caller), NULL: the
//   player's shared one.  The members' constructors: MenuCarCamera()
//   (inline: mode and its springs 0), CarRender(spec, &camera), CarModel(),
//   the four MenuTireModel(), MotionValue() for fade and blend.  Ends with
//   the loader forgetting its file name (the next request loads again).
//   The landing pads undo the members built so far (the tyres through their
//   vtable, ~CarModel, GSBuffer::~GSBuffer of the render's two buffers,
//   CameraBase::~CameraBase for the camera, ~Transitive).
Car::Car(Tex1Image *envImage, MenuCarSpec *carSpec, int carPlayer, void *carBuffer)
    : Transitive(),
      camera(),
      render(carSpec, &camera),
      spec(carSpec), dirt(0), car(0), buffer(0), ownBuffer(0), file(0),
      model()
{
    /* implicit: CarRender, CarModel, MenuTireModel (member constructors), ~CarModel, ~GSBuffer, ~CameraBase (exception cleanup) */
    tiresSet = 0;
    loadState = 0;
    ready = 0;
    tiresReady = 0;
    drawMirror = 1;
    color = 0;
    prevColor = 0;
    player = carPlayer;
    if (carBuffer == 0) {
        buffer = menu_car_alloc_buffer(carPlayer);
    } else {
        ownBuffer = 1;
        buffer = carBuffer;
    }
    /* PC: a caller's buffer is MENU_CAR_BUFFER_SIZE too (T010's) */
    bigBuffer = 0;
    bufferSize = buffer != 0 ? MENU_CAR_BUFFER_SIZE : 0;
    render.setEnvImage(envImage);
    modelPath[0] = '\0';
    wheelName[0] = '\0';
    async_loader_get()->forget();
}

// FUNC 0x001B8868 gt2k_menu::Car::~Car
//   Slot 0.  Then the members (the tyres from the last one down, through
//   their vtable; ~CarModel; the render's two ~GSBuffer; the camera's implicit
//   destructor: CameraBase::~CameraBase) and ~Transitive; the deleting form
//   then frees it with Object::operator delete.
//   Matching: exact with the reference compiler 991111-01; 991111b gives
//   the four member addresses (model, the render's buffers, camera) the
//   callee-saved registers in another order (s6/s5/s4/s2 for retail's
//   s5/s4/s2/s6): GCSE creates their registers in hash order, and the hash
//   includes the register number of `this' (116 vs 84, MATCHING.md).
Car::~Car()
{
    if (ownBuffer == 0)
        menu_car_release_buffer(player);
    if (bigBuffer != 0)                     /* PC: its own buffer, deferred as the shared one */
        loader_defer(menu_car_free_big, bigBuffer, 0);
    /* implicit: ~CarModel, ~GSBuffer, ~CameraBase (member destructors), ~Transitive (base destructor), operator delete (deleting destructor) */
}

// FUNC 0x001B8998 gt2k_menu::Car::setCar
//   (Name not the original's: no symbol.)  The car to show (NULL: none).
//   The pointer only identifies the car (its callers pass the name of the
//   car MenuCarSpec holds): the model file is the spec's body model code's
//   (none if it has no code).  Restarts the loading (state 0, flag off),
//   the colours and the camera preset; interrupts suspended.
void Car::setCar(const char *newCar)
{
    if (car == newCar)
        return;
    int intr = intr_suspend();
    ready = 0;
    tiresReady = 0;
    car = newCar;
    if (newCar != 0) {
        s64 code = spec->getCode();
        if (code == 0) {
            modelPath[0] = '\0';
            car = 0;
        } else {
            car_model_path(code, modelPath, CAR_MODEL_MENU);
            file = 0;
            tiresSet = 0;
        }
    }
    color = 0;
    prevColor = 0;
    camera.reset();
    wheelName[0] = '\0';
    loadState = 0;
    g_00352560[player] = 0;
    if (intr)
        EI();
}

// FUNC 0x001B8A58 gt2k_menu::Car::setWheel
//   (Name not the original's: no symbol.)  The wheel to fit
//   ("menu/wheel/<name>"; NULL: the car's own).  A change makes the tyres be
//   set up again; after a complete load the wheel file is loaded next
//   (state 2).
void Car::setWheel(const char *name)
{
    if (name != 0) {
        if (str_equal(wheelName, name) != 0)
            return;
        strncpy(wheelName, name, sizeof(wheelName) - 1);    /* PC: bounded (was strcpy) */
        wheelName[sizeof(wheelName) - 1] = '\0';
    } else {
        if (wheelName[0] == '\0')
            return;
        wheelName[0] = '\0';
    }
    int intr = intr_suspend();
    if (wheelName[0] != '\0')
        wheel_file_path(wheelPath, wheelName, 1);
    tiresReady = 0;
    tiresSet = 0;
    if (intr)
        EI();
    if (loadState == 3) {
        loadState = 2;
        gfx_vsync(1);
    }
}

// FUNC 0x001B8B20 gt2k_menu::Car::setColor
//   (Name not the original's: no symbol.)  Starts a blend (1 -> 0, see
//   virtual_1) from the colour shown to newColor.
void Car::setColor(int newColor)
{
    int oldColor = color;
    if (oldColor != newColor) {
        prevColor = oldColor;
        color = newColor;
        blend.set(1.0f);
    }
}

// FUNC 0x001B8B50 gt2k_menu::Car::setupTires
//   (Name not the original's: no symbol.)  Once per model / wheel: the four
//   tyres (0 / 1 front, 2 / 3 rear) with the file's tyre and wheel files
//   and the body's model set (the wheels are its parts), their sizes from
//   the spec (width, outer radius, profile per axle), their packets built
//   one after the other in the free space after the file.
void Car::setupTires(CarModelFile *modelFile)
{
    if (tiresSet != 0)
        return;
    void *work = carmodelfile_work_area(modelFile);
    for (int i = 0; i < 4; i++) {
        MenuTireModel *tire = &tires[i];
        int axle = i >> 1;
        tire->setPosition(i);
        tire->setWheelFile(modelFile->wheel);
        tire->setTireFile(modelFile->tire);
        tire->setWheelModel(modelFile->body);
        tire->setSizeProfile(spec->car.dyn.gear.tyreWidth[axle], spec->car.dyn.gear.tyreRadius[axle],
                             spec->car.dyn.gear.tyreSidewall[axle]);
        work = tire->build(work, MENU_TIRE_SEGMENTS);
    }
    tiresSet = 1;
    tiresReady = 1;
}

// FUNC 0x001B8C30 gt2k_menu::Car::modelLoadedCallback
//   The loader's callback for the model file (ctx = the Car): relocates it.
void Car::modelLoadedCallback(void *ctx, void *data)
{
    carmodelfile_relocate((CarModelFile *)data);
    ((Car *)ctx)->modelLoaded((CarModelFile *)data);
}

// FUNC 0x001B8C70 gt2k_menu::Car::modelLoaded
//   The body from the file; the tyres wait for the wheel file when a wheel
//   is set.
void Car::modelLoaded(CarModelFile *modelFile)
{
    file = modelFile;
    model.setModel(modelFile->body, 0);
    if (wheelName[0] == '\0')
        setupTires(modelFile);
    ready = 1;
}

// FUNC 0x001B8CD0 gt2k_menu::Car::wheelLoadedCallback
//   The loader's callback for the wheel file (ctx = the Car).
void Car::wheelLoadedCallback(void *ctx, void *data)
{
    ((Car *)ctx)->wheelLoaded(data);
}

// FUNC 0x001B8CF0 gt2k_menu::Car::wheelLoaded
void Car::wheelLoaded(void *wheelFile)
{
    wheel_file_relocate((WheelFile2 *)wheelFile);
    setupTires(file);
}

// FUNC 0x001B8D28 gt2k_menu::Car::loadStep
//   (Name not the original's: no symbol.)  One frame of the loading
//   (loadState 0: wait until no Car loads, then take the flag; 1: the model
//   file into the buffer; 2: the wheel file over the model file's own
//   wheel file; 3: done, the flag given back).  Without a car: no file.
void Car::loadStep()
{
    if (car == 0) {
        file = 0;
        return;
    }
    switch (loadState) {
    case 0: {
        int intr = intr_suspend();
        if (menu_car_any_loading(this) == 0) {
            loadState = 1;
            g_00352560[player] = 1;
        }
        if (intr)
            EI();
        /* PC (ROADMAP 18): a buffer that holds the model file and the tyres
         * (the loader reads a file whole; a wheel file over the model's own
         * is sized in state 2, where its place is known) */
        if (loadState == 1)
            menu_car_fit_buffer(this, menu_car_buffer_need(modelPath, 0));
        break;
    }
    case 1:
        /* PC: with the buffer's capacity (nothing read past it) */
        file = (CarModelFile *)async_loader_get()->poll(modelPath, buffer, (int)bufferSize,
                                                        modelLoadedCallback, this);
        if (file == 0)
            break;
        if (ready != 0) {
            if (wheelName[0] != '\0' && tiresReady == 0)
                loadState = 2;
            else
                loadState = 3;
        } else {
            /* The loader had the file before this Car asked (its callback
             * did not run for it): forget the name, so that it loads (and
             * calls back) again; the state stays 1. */
            async_loader_get()->forget();
        }
        break;
        break;
    case 2:
        /* PC (ROADMAP 18): a wheel file (set after the model was in) that,
         * with the tyres, does not fit after the model: a bigger buffer and
         * the load again from state 0 (without one: on, as retail) */
        if (file != 0 &&
            (u32)((u8 *)file->wheel - (u8 *)buffer) +
            ((menu_car_file_size(wheelPath) + 0x3F) & ~0x3Fu) + 0x40 + MENU_CAR_TIRE_ROOM > bufferSize) {
            menu_car_fit_buffer(this, menu_car_buffer_need(modelPath, wheelPath));
            if (buffer != (void *)file) {   /* (the file is at the buffer's start) */
                int intr = intr_suspend();
                ready = 0;
                tiresReady = 0;
                tiresSet = 0;
                file = 0;
                loadState = 0;
                g_00352560[player] = 0;
                async_loader_get()->forget();
                if (intr)
                    EI();
                break;
            }
        }
        if (file != 0) {
            /* PC: with the room left after the wheel part's start */
            if (async_loader_get()->poll(wheelPath, file->wheel,
                                         (int)(bufferSize - (u32)((u8 *)file->wheel - (u8 *)buffer)),
                                         wheelLoadedCallback, this) != 0)
                loadState = 3;
        } else {
            loadState = 0;
        }
        break;
    case 3:
        if (g_00352560[player] != 0)
            g_00352560[player] = 0;
        break;
    }
}

// FUNC 0x001B8EC8 gt2k_menu::Car::virtual_1
//   Update (slot 1): the scripts, the colour blend (pulled down from 1 at
//   10 / s; held at 0 until the car has faded in), the loading, then once
//   the model (and, except in the presets 6 / 7, the tyres) are in the
//   camera springs and the fade in (force 2 in the presets 0..3 and 9, 8 in
//   the others, clamped at 1); before that the camera stays on its preset
//   and the car transparent.
//   The enhanced engine (menus_a.md 33, rendering.md 1; Lupo 2's twin L2
//   0x001BE3B0; owned by the rendering area): the shown car's model
//   animations step and apply every frame (CarModel::animate / applyAnims,
//   L2 0x001BE448 / 0x001BE450), and while it is not shown its animation id
//   goes to 1 then 0 (L2 0x001BE52C / 0x001BE538): the records of ids 1
//   and 0 rewind, and the menu plays id 0 (no animation tracks: they step
//   only with -1).  GT3's menu models (menu/cars) have no CLUT records and no
//   tracks: nothing changes for them.
void Car::virtual_1()
{
    Transitive::virtual_1();
    blend.setVelocity(-10.0f);
    blend.update(0.0f);
    loadStep();
    int shown;
    if (camera.getType() == 6 || camera.getType() == 7)
        shown = ready;
    else
        shown = ready != 0 && tiresReady != 0;
    if (shown != 0) {
        camera.virtual_8(&g_002EB170);
        /* Lupo 2 (L2 0x001BE448 / 0x001BE450): the model's animations */
        model.animate();
        model.applyAnims();
        switch (camera.getType()) {
        case 0: case 1: case 2: case 3: case 9:
            fade.pull(1.0f, 2.0f);
            break;
        case 4: case 5: case 6: case 7: case 8: case 10: case 11:
            fade.pull(1.0f, 8.0f);
            break;
        }
        fade.update(1.0f);
        fade.limit(1.0f);
    } else {
        camera.reset();
        fade.set(0.0f);
        /* Lupo 2 (L2 0x001BE52C / 0x001BE538): animation id 1, then 0 (each
         * change rewinds that id's CLUT records).  Not in Lupo 2: only once
         * the model is in (ready).  While a new car loads, model.model still
         * points into the buffer the loader is filling (setCar does not
         * clear it), and the rewind would walk and write a half-read model;
         * the relocation at the load rewinds every record anyway, so only
         * the id is set then. */
        if (ready != 0) {
            model.setAnimSelect(1);
            model.setAnimSelect(0);
        } else {
            model.animSelect = 0;
        }
    }
    if (fade.get() < 1.0f)
        blend.set(0.0f);
}

// FUNC 0x001B9068 gt2k_menu::Car::virtual_2
//   Draw (slot 2), when visible with a car whose model is in: the body's
//   fade, then CarRender's passes (the jittered copies it averages): 4 for
//   the car and 2 for the mirror pass, 2 / 1 in the presets 1 and 2, the
//   mirror 1 while the car is under 0.8 alpha.  The colour blend is drawn
//   with blend.value clamped at 0.  The switch lists every preset 0..11
//   (retail's jump table has 12 entries).
//   Matching (83%): the shape is retail's; left are the callee-saved
//   registers of &model / &render and &blend / &tires (each pair swapped)
//   and the order of the draw arguments' loads.  Tried: local pointers for
//   fade / blend / model / render, the fade and the blend value in locals,
//   &tires[0], an inline draw wrapper, a MotionValue / inline max helper
//   instead of >? (the reference compiler gives the same code).
void Car::virtual_2()
{
    if (visible == 0)
        return;
    if (car == 0)
        return;
    if (ready == 0)
        return;
    int passes = 4;
    int mirrorPasses = 2;
    switch (camera.getType()) {
    case 0: case 3: case 4: case 5: case 6: case 7: case 8: case 9: case 10: case 11:
        break;
    case 1: case 2:
        passes = 2;
        mirrorPasses = 1;
        break;
    }
    if (fade.get() < 0.8f)
        mirrorPasses = 1;
    model.setDirt(dirt);
    render.beginFrame();
    if (drawMirror != 0)
        render.draw(&model, tires, color, prevColor, 1, 0, tiresReady, fade.get(),
                    CAR_MAXF(blend.get(), 0.0f), mirrorPasses);
    render.draw(&model, tires, color, prevColor, 0, 0, tiresReady, fade.get(),
                CAR_MAXF(blend.get(), 0.0f), passes);
    render.endFrame();
}

// FUNC 0x001B91E8 gt2k_menu::Car::getCamera
/* inline in game/menu/framework/T212_menu_car.h; egcs emits this copy */

// FUNC 0x001B91F0 gt2k_menu::Car::setDirt
/* inline in game/menu/framework/T212_menu_car.h; egcs emits this copy (dirt = v) */

// FUNC 0x001B91F8 gt2k_menu::Car::setView
/* inline in game/menu/framework/T212_menu_car.h; egcs emits this copy (calls
 * MenuCarCamera::setProjection and setTilt) */
/* implicit: setProjection, setTilt (the inline body) */

// FUNC 0x001B9278 gt2k_menu::Car::F_001B9278
/* inline in game/menu/framework/T212_menu_car.h; egcs emits this copy (render.unk0C = v) */

// FUNC 0x001B9280 gt2k_menu::Car::updateViewport
/* inline in game/menu/framework/T212_menu_car.h; egcs emits this copy */

// FUNC 0x001B92B8 gt2k_menu::Car::setDrawMirror
/* inline in game/menu/framework/T212_menu_car.h; egcs emits this copy (drawMirror = v) */

// FUNC 0x001B92C0 gt2k_menu::Car::setCameraMode
/* inline in game/menu/framework/T212_menu_car.h; egcs emits this copy (MenuCarCamera::setType's store) */

} /* namespace gt2k_menu */

