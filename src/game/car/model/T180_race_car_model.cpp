/* T180_race_car_model.cpp - RaceCarModel and T180's copies of the
 * UnitArena<64> members (0x001A9B90..0x001AB7D0); the class is declared in
 * include/game/car/model/T180_race_car_model.h.
 *
 * RaceCarModel is the drawn model of a race car (RaceEntryCar +0x11F8).  Its
 * car file is read asynchronously (file_req_alloc) into one 0xA8000-byte
 * block of the car model arena g_002C2F50 (7 blocks, set up by race_begin),
 * relocated in place, and its body handed to the CarModel (T446).  The tyre
 * file ("tire/n%d", the number from the car's tyre parts) and the wheel file
 * ("wheel/%s") are loaded the same way into buffers inside the car file.
 * The drawing methods place the VU0 matrix from the car state at time t
 * (the body frame, or the ground frame of T302's car_ground_* accessors)
 * and draw the body, its passes, the four tyres (with a motion blur from
 * their screen speed and the glow of a hot brake), the lamps (as lights of
 * the FlareClass) and the sparks thrown where the points under the car go
 * below the road.
 * PC (ROADMAP 18): the car file, the replacement tyre / wheel files and the
 * tyres' work space each take a buffer of their own size (memalign, freed
 * by release()), not the arena's fixed blocks: a transplanted car may be
 * bigger than retail's.
 */
/* Retail T180's .rodata (0x00330B38): <new>'s "bad_alloc" (unreferenced:
 * DATA_START; T179's is at 0x00330A98, u059's data pass), g_00330B48, the
 * two vtables, then the type names RaceCarModel, UnitArena<64>; T121's
 * RaceEntryCar is not seen. */
#include <new>                              /* (unused: "bad_alloc") */
// DATA_START 0x00330B38
#define GAME_T153_EMITS_TYPEINFO           /* egcs generates its weak __tft9UnitArena1Ui64 here too */
#include "game/race/core/T153_race_course.h"          /* UnitArena<64> (its __tf: the first deferred function
                                               egcs saves here, the last it writes) */
class CarModel;
CarModel *F_00179460(CarModel *m);          /* 0x00179460 (T121_race_entry.h: T121's copy) */
#include "game/car/model/race_car_model_internal.h"
#include "libc.h"                           /* malloc, free, memcpy, strcpy */
#include "game/car/model/T180_race_car_model.h"       /* RaceCarModel, g_002C2F50; + T446 CarModel, T449 RaceTireModel,
                                               T447 TireModelBase / the tyre and wheel files */
#include "game/race/core/T123_race_setup.h"           /* g_002B3BF0, the race object pool */
#include "game/effects/T159_flare.h"                /* FlareClass::addLight / drawReflection, CarLamp */
#include "game/effects/T165_particle_manager.h"     /* ParticleManager::emitSparks */
#include "game/effects/T166_particle_params.h"      /* g_002C03B0 (the spark thresholds) */
#include "game/course/T178_course_env.h"           /* CourseEnv (blurScale, blurExp) */
#include "game/course/T179_course_data.h"          /* CourseData (env) */
#include "game/car/model/T278_show_room_car.h"        /* ShowRoomCar's wheel accessors */
#include "game/car/physics/T289_car_mode.h"             /* car_display_level, CAR_SHOW_* */
#include "game/car/physics/T295_tyre_heat.h"            /* car_brake_heat_sq */
#include "game/car/physics/T302_car_state.h"            /* RaceCarState and its car_* accessors */
#include "game/car/model/T455_car_model_info.h"       /* CarModelInfo, CarAxleSpec, carmodelinfo_tire */
#include "game/car/model/T456_car_model_file.h"       /* CarModelFile, carmodelfile_relocate / _work_area */
#include "engine/io/T504_file_queue.h"           /* file_req_alloc */
#include "engine/io/T502_file_resolve.h"         /* PC: file_size_retry (the files' own sizes) */
#include "engine/io/T503_vol_thread.h"           /* PC: vol_device_object */
#include "engine/pgl/G001_flare.h"                /* the pgl_* / gfx_* / vu0_* calls, PGL_* */
#include "engine/media/G002_mpeg_frameq.h"          /* expf */
#include "engine/gfx/T451_model_set.h"            /* PC: modelset_pose_anims */
#include "game/enhanced/rendering.h"            /* PC: ENH_RENDER_CAR_ANIMS (pcPoseBegin) */
#include "game/app/pc_frame.h"                  /* PC: pc_anim_stepped / pc_anim_offset (the dynamic frame rate) */

/* The brake is on (st.brake > 0) as 1 / 0: ? an inline of the car state in
 * the original; addLamps / drawLampReflections keep it as a bool (retail:
 * the 1 / 0 then `sltu s4, $0, v1'). */
static inline int car_braking(RaceCarState *car)
{
    return car->st.brake > 0.0f ? 1 : 0;
}

/* drawTires' blurPasses by the blur mode (T180's .rodata starts with it). */
const int g_00330B48[4] = { 0, 16, -1, 0 };    /* 0x00330B48 */

/* PC (ROADMAP 18, no original): a 64-byte aligned buffer (memalign; the
 * owner frees it) for the file at path: its unpacked size rounded up to 64,
 * at least room for the path (a queued read keeps its path in its buffer
 * until it runs, as the original's).  *size gets the bytes.  A file above
 * `room' (what retail read it into, named roomName; 0: not known) is
 * logged, a full heap too (NULL). */
static void *race_model_file_alloc(const char *what, const char *path, u32 room,
                                   const char *roomName, u32 *size)
{
    u32 bytes = (file_size_retry(path, 0, &vol_device_object) + 0x3F) & ~0x3Fu;
    u32 alloc = bytes;
    u32 pathBytes = ((u32)strlen(path) + 1 + 0x3F) & ~0x3Fu;
    if (alloc < pathBytes)
        alloc = pathBytes;
    if (room != 0 && bytes > room)
        printf("PC: %s %s: %u bytes, over %s (0x%x): a buffer of its own\n", what, path, bytes,
               roomName, room);
    void *buf = memalign(0x40, alloc);
    if (buf == NULL) {
        printf("PC: %s %s: no memory for %u bytes, not loaded\n", what, path, alloc);
        alloc = 0;
    }
    *size = alloc;
    return buf;
}

/* PC (no original): a copy of a loaded tyre / wheel file of `size' bytes
 * (copyFrom); NULL when there is none or no memory (logged: the car file's
 * own part is used then). */
static void *race_model_copy_part(const char *what, const void *src, u32 size)
{
    if (src == NULL)
        return NULL;
    void *buf = memalign(0x40, size);
    if (buf == NULL) {
        printf("PC: %s file copy: no memory for %u bytes, the car file's own kept\n", what, size);
        return NULL;
    }
    memcpy(buf, src, size);
    return buf;
}

/* PC (no original): release()'s part for the buffers kept apart from the
 * car file (the replacement tyre / wheel files, the tyres' work space). */
static void race_model_free_parts(RaceCarModel *m)
{
    if (m->pcTireFile != NULL)
        free(m->pcTireFile);
    if (m->pcWheelFile != NULL)
        free(m->pcWheelFile);
    if (m->pcTireWork != NULL)
        free(m->pcTireWork);
    m->pcTireFile = NULL;
    m->pcTireSize = 0;
    m->pcWheelFile = NULL;
    m->pcWheelSize = 0;
    m->pcTireWork = NULL;
}

// FUNC 0x001A9B90 RaceCarModel::onModelLoaded
//   The car file's file_req_alloc callback: setData(buf).
void RaceCarModel::onModelLoaded(void *self, void *buf)
{
    ((RaceCarModel *)self)->setData((CarModelFile *)buf);
}

// FUNC 0x001A9BB0 RaceCarModel::onTireLoaded
//   The tyre file's callback: setTireData(buf).
void RaceCarModel::onTireLoaded(void *self, void *buf)
{
    ((RaceCarModel *)self)->setTireData(buf);
}

// FUNC 0x001A9BD0 RaceCarModel::onWheelLoaded
//   The wheel file's callback: setWheelData(buf).
void RaceCarModel::onWheelLoaded(void *self, void *buf)
{
    ((RaceCarModel *)self)->setWheelData(buf);
}

// FUNC 0x001A9BF0 RaceCarModel::initArena
//   The car model arena: a 0x498080-byte buffer (malloc) of 0xA8000-byte
//   blocks, 7 car files (_UnitArenaBase::init).  Caller: race_begin (u001).
void RaceCarModel::initArena()
{
    void *buf = malloc(CAR_MODEL_ARENA_SIZE);
    g_002C2F50.init(buf, CAR_MODEL_ARENA_SIZE, CAR_MODEL_BLOCK_SIZE);
}

// FUNC 0x001A9C30 RaceCarModel::freeArena
//   The arena forgets its buffer (_UnitArenaBase::reset), which is freed.
//   Caller: race_end (u001).
void RaceCarModel::freeArena()
{
    free(g_002C2F50.reset());
}

// FUNC 0x001A9C58 RaceCarModel::operator new
//   The race pool, 16-byte aligned (pdistd::Allocator::allocate).
void *RaceCarModel::operator new(size_t size)
{
    return g_002B3BF0.allocate(size, 0x10);
}

// FUNC 0x001A9C80 RaceCarModel::operator delete
//   The size (0x6C0) is ignored (pdistd::Allocator::free).
void RaceCarModel::operator delete(void *p, size_t size)
{
    g_002B3BF0.free(p);
}

// FUNC 0x001A9CA8 RaceCarModel::RaceCarModel
//   vptr, CarModel(), the four RaceTireModel() (with the EH cleanup of the
//   constructed ones and ~CarModel), then the fields.  Caller: RaceEntryCar's
//   constructor (u042).  The state is not initialised.
RaceCarModel::RaceCarModel()
{
    /* implicit: CarModel (base constructor), RaceTireModel (constructor of each tyre), ~CarModel (exception cleanup) */
    data = NULL;
    colorIndex = 0;
    tireLoaded = 0;
    wheelLoaded = 0;
    for (int tire = 0; tire < 4; tire++)
        for (int k = 3; k >= 0; k--)
            prevScreen[tire][k] = 0.0f;
    /* The enhanced engine: the lamps' blink phases, cleared (no reader
     * yet; rendering.md 4). */
    blinkPhase[0] = 0.0f;
    blinkPhase[1] = 0.0f;
    /* PC: no buffer of its own yet (ROADMAP 18) */
    pcFileSize = 0;
    pcTireFile = NULL;
    pcTireSize = 0;
    pcWheelFile = NULL;
    pcWheelSize = 0;
    pcTireWork = NULL;
}

// FUNC 0x001A9E18 RaceCarModel::~RaceCarModel
//   (RaceCarModel::virtual_0) vptr, release(), the tyres destroyed from the
//   last (~RaceTireModel through their vtables, flags 0), ~CarModel (flags
//   2); deleting form: operator delete(this, 0x6C0).
RaceCarModel::~RaceCarModel()
{
    release();
    /* implicit: ~CarModel (base destructor) */
}

// FUNC 0x001A9ED8 RaceCarModel::copyFrom
//   /* ? */ name: it returns *this (probably the original's operator=).
//   Releases the model, takes src's load flags, state and colour, copies
//   src's whole car file block into a new block (_UnitArenaBase::alloc),
//   relocates it (setData) and sets up the tyres.  Callers:
//   RaceEntryCar::copyFrom / applyPendingModel (u042).
RaceCarModel &RaceCarModel::copyFrom(const RaceCarModel &src)
{
    release();
    tireLoaded = src.tireLoaded;
    state = src.state;
    colorIndex = src.colorIndex;
    wheelLoaded = src.wheelLoaded;
    /* PC: a block of src's size (was a 0xA8000-byte block of g_002C2F50,
     * all of it copied) */
    u32 size = src.pcFileSize;              /* in a variable: retail calls memcpy */
    CarModelFile *file = (CarModelFile *)memalign(0x40, size);
    data = file;
    if (file == NULL) {
        printf("PC: car model copy: no memory for %u bytes\n", size);
        tireLoaded = 0;
        wheelLoaded = 0;
        return *this;
    }
    pcFileSize = size;
    memcpy(file, src.data, size);
    setData(file);
    /* PC: src's loaded tyre / wheel files are its own buffers (the car file
     * keeps its parts): copied, each relocated by itself (they hold where
     * they were) */
    if (src.tireLoaded != 0) {
        pcTireFile = race_model_copy_part("tyre", src.pcTireFile, src.pcTireSize);
        if (pcTireFile != NULL) {
            pcTireSize = src.pcTireSize;
            tire_file_relocate((TireFile2 *)pcTireFile);
        } else {
            tireLoaded = 0;
        }
    }
    if (src.wheelLoaded != 0) {
        pcWheelFile = race_model_copy_part("wheel", src.pcWheelFile, src.pcWheelSize);
        if (pcWheelFile != NULL) {
            pcWheelSize = src.pcWheelSize;
            wheel_file_relocate((WheelFile2 *)pcWheelFile);
        } else {
            wheelLoaded = 0;
        }
    }
    setupTires();
    return *this;
}

// FUNC 0x001A9F78 RaceCarModel::release
//   Gives the car file block back (the body forgets it: CarModel::
//   clearModel; _UnitArenaBase::release) and clears the load flags.
//   Callers: the destructor, copyFrom, RaceEntryCar::unloadModel (u042).
void RaceCarModel::release()
{
    if (data != NULL) {
        carModel.clearModel();
        free(data);                         /* PC: its own block (was g_002C2F50.release) */
    }
    data = NULL;
    tireLoaded = 0;
    wheelLoaded = 0;
    pcFileSize = 0;                         /* PC: and the buffers kept apart from it */
    race_model_free_parts(this);
}

// FUNC 0x001A9FC8 RaceCarModel::load
//   Unless a model is there: takes a block (_UnitArenaBase::alloc), copies
//   the path into it (strcpy) and queues the file's read over it
//   (file_req_alloc, done: onModelLoaded).  Caller: RaceEntryCar::loadModel
//   (u042).
void RaceCarModel::load(const char *path)
{
    if (data == NULL) {
        /* PC: a block of the file's own size (was a 0xA8000-byte block of
         * g_002C2F50: a bigger file ran into the next car's block) */
        char *block = (char *)race_model_file_alloc("car model", path, CAR_MODEL_BLOCK_SIZE,
                                                    "retail's block", &pcFileSize);
        if (block == NULL)
            return;
        strcpy(block, path);
        file_req_alloc(block, block, 0, onModelLoaded, this, FILE_REQ_PLAIN);
    }
}

// FUNC 0x001AA048 RaceCarModel::setData
//   The loaded car file (NULL: the read failed): relocated in place
//   (carmodelfile_relocate), its body given to the CarModel (setModel, not
//   relocated again).
void RaceCarModel::setData(CarModelFile *file)
{
    data = file;
    if (file != NULL) {
        carmodelfile_relocate(file);
        carModel.setModel(data->body, 0);
    } else {
        /* quirk: retail gives back a NULL block (g_002C2F50.release(NULL)).
         * PC: the block is not the arena's; a read into a given buffer
         * never returns NULL */
    }
}

// FUNC 0x001AA0A8 RaceCarModel::loadTire
//   Unless the tyre file is there: the tyre file number of the car's tyre
//   part parts[0] (carmodelinfo_tire; flag: its second table); unless it is
//   the car's default, its name (tire_file_path, "tire/n%d") goes into the
//   car file's tyre buffer and the file is read over it (file_req_alloc,
//   done: onTireLoaded).  The loop runs once.  Caller:
//   RaceEntryCar::loadTires (u042).
void RaceCarModel::loadTire(const int *parts, int flag)
{
    if (tireLoaded == 0 && data != NULL) {  /* PC: data: load may have found no memory (logged) */
        int i = 0;
        do {
            CarModelInfo *info = data->info;
            char *buf = (char *)data->tire;
            int number = carmodelinfo_tire(info, parts[i], flag);
            if (number == info->defaultTire)
                break;
            /* PC: into a buffer of the file's own size (was over the car
             * file's tyre part: a bigger file ran into the wheel part and
             * the tyres' work space); its room in the car file, up to the
             * wheel part, is logged when outgrown.  One read at a time: a
             * buffer already queued is kept. */
            if (pcTireFile != NULL)
                break;
            char path[0x40];
            u32 room = 0;
            if ((u8 *)data->wheel > (u8 *)data->tire)
                room = (u32)((u8 *)data->wheel - (u8 *)data->tire);
            tire_file_path(path, number, 0);
            buf = (char *)race_model_file_alloc("tyre", path, room, "the car file's tyre part",
                                                &pcTireSize);
            if (buf == NULL)
                break;
            pcTireFile = buf;
            strcpy(buf, path);
            file_req_alloc(buf, buf, 0, onTireLoaded, this, FILE_REQ_PLAIN);
            i++;
        } while (i <= 0);
    }
}

// FUNC 0x001AA178 RaceCarModel::setTireData
//   The loaded tyre file (NULL: failed) is relocated (tire_file_relocate).
//   PC: buf is pcTireFile (setupTires takes it while tireLoaded).
void RaceCarModel::setTireData(void *buf)
{
    if (buf != NULL) {
        tire_file_relocate((TireFile2 *)buf);
        tireLoaded = 1;
    }
}

// FUNC 0x001AA1B0 RaceCarModel::loadWheel
//   Unless the wheel file is there: "wheel/<name>" (wheel_file_path) into
//   the car file's wheel buffer, read over it (file_req_alloc, done:
//   onWheelLoaded).  Caller: RaceEntryCar::loadWheel (u042).
void RaceCarModel::loadWheel(const char *name)
{
    if (wheelLoaded == 0 && data != NULL) { /* PC: data: as loadTire */
        /* PC: into a buffer of the file's own size (was over the car
         * file's wheel part, the last one: a bigger file ran over the
         * tyres' work space and out of the block); the car file's own
         * wheel size is logged when outgrown.  A buffer already queued is
         * kept (one read at a time). */
        if (pcWheelFile != NULL)
            return;
        char path[0x100];
        wheel_file_path(path, name, 0);
        char *buf = (char *)race_model_file_alloc("wheel", path, (u32)data->wheel->size,
                                                  "the car file's wheel part", &pcWheelSize);
        if (buf == NULL)
            return;
        pcWheelFile = buf;
        strcpy(buf, path);
        file_req_alloc(buf, buf, 0, onWheelLoaded, this, FILE_REQ_PLAIN);
    }
}

// FUNC 0x001AA218 RaceCarModel::setWheelData
//   The loaded wheel file (NULL: failed) is relocated (wheel_file_relocate).
//   PC: buf is pcWheelFile (setupTires takes it while wheelLoaded).
void RaceCarModel::setWheelData(void *buf)
{
    if (buf != NULL) {
        wheel_file_relocate((WheelFile2 *)buf);
        wheelLoaded = 1;
    }
}

/* The enhanced engine (ref/enhanced/ported/rendering.md): one log line per
 * car model at the race's set-up when it carries something of Lupo 2's
 * (a special kind, animated textures or blinking lamps), so that a
 * transplanted model can be checked:
 *   [enhanced] car model: kind K, animated textures T, tracks yes|no, blinking lamps L
 * GT3's own cars log nothing (kind 0, none of the three). */
static void race_model_log(RaceCarModel *rcm)
{
    CarModelFile *file = rcm->data;
    ModelSet *m;
    int kind, textures, blinking, i;

    if (file == NULL || rcm->state == NULL)
        return;
    m = file->body;
    kind = rcm->state->params.specialKind;
    textures = 0;
    if (m != NULL && m->tex != NULL) {
        for (i = 0; i < m->nTex; i++) {
            if (m->tex[i] != NULL && m->tex[i]->clutAnim != NULL)
                textures++;
        }
    }
    blinking = 0;
    for (i = 0; i < file->info->lampCountA; i++) {
        if (file->info->lampsA[i].blink != LAMP_BLINK_STEADY)
            blinking++;
    }
    if (kind == CAR_KIND_NONE && textures == 0 && blinking == 0)
        return;
    printf("[enhanced] car model: kind %d, animated textures %d, tracks %s, blinking lamps %d\n",
           kind, textures, (m != NULL && m->anim != NULL) ? "yes" : "no", blinking);
}

// FUNC 0x001AA250 RaceCarModel::setupTires
//   Each tyre: its position, colour, the tyre and wheel files
//   (TireModelBase's inline setters), its
//   axle's brake from the car file (RaceTireModel::setBrake: disc, caliper,
//   disc radius / offset, caliper angle; wheels 0, 1 front, 2, 3 rear) and
//   its size from the state's drivetrain (TireModelBase::setSizeProfile:
//   tyreWidth, tyreRadius, tyreSidewall of the axle); the tyres take their
//   work spaces one after the other from the end of the car file's data
//   (carmodelfile_work_area, RaceTireModel::build).  (The original indexes
//   the car file's records by wheel / 2 and the state's by wheel >> 1.)
//   Callers: copyFrom, RaceEntryCar::setupTires (u042).
//   The enhanced engine: first the model's log line (race_model_log).
void RaceCarModel::setupTires()
{
    race_model_log(this);
    if (data == NULL)                       /* PC: no car file (load logged a full heap) */
        return;
    /* PC: the work space is a buffer of its own (was the room after the car
     * file's wheel part, carmodelfile_work_area(data), inside the 0xA8000-
     * byte block), kept until release(); the loaded tyre / wheel files are
     * buffers of their own too (loadTire / loadWheel) */
    if (pcTireWork == NULL) {
        pcTireWork = memalign(0x40, RACE_TIRE_WORK_SIZE);
        if (pcTireWork == NULL) {
            printf("PC: car model tyres: no memory for their work space (0x%x bytes)\n",
                   (u32)RACE_TIRE_WORK_SIZE);
            return;
        }
    }
    void *work = pcTireWork;
    WheelFile2 *wheelFile = (wheelLoaded != 0 && pcWheelFile != NULL) ? (WheelFile2 *)pcWheelFile
                                                                        : data->wheel;
    TireFile2 *tireFile = (tireLoaded != 0 && pcTireFile != NULL) ? (TireFile2 *)pcTireFile
                                                                   : data->tire;
    RaceCarState *car = state;
    int color = colorIndex;
    CarModelInfo *info = data->info;
    for (int wheel = 0; wheel < 4; wheel++) {
        tires[wheel].setPosition(wheel);
        tires[wheel].setColor(color);
        tires[wheel].setWheelFile(wheelFile);   /* PC: was data->wheel */
        tires[wheel].setTireFile(tireFile);     /* PC: was data->tire */
        RaceTireModel *tire = &tires[wheel];
        CarAxleSpec *axle = &info->axles[wheel / 2];
        /* CarAxleSpec (T455): b the disc, a the caliper, c / d the disc's
         * radius and offset, e the caliper's angle in degrees */
        tire->setBrake(axle->c, axle->d, axle->e, axle->b, axle->a);
        GearCalc *gear = &car->params.gear;
        tire->setSizeProfile(gear->tyreWidth[wheel >> 1], gear->tyreRadius[wheel >> 1],
                             gear->tyreSidewall[wheel >> 1]);
        work = tire->build(work);
    }
    /* PC: RACE_TIRE_WORK_SIZE is meant to be ample; say so if it was not */
    u32 used = (u32)((u8 *)work - (u8 *)pcTireWork);
    if (used > (u32)RACE_TIRE_WORK_SIZE)
        printf("PC: car model tyres: work space overrun, %u bytes of 0x%x\n", used,
               (u32)RACE_TIRE_WORK_SIZE);
}

// FUNC 0x001AA3A0 RaceCarModel::loadBodyMatrix
//   Multiplies the current matrix by the body frame at t: translate to the
//   view position (car_view_x / y / z), then rotate y (car_yaw), x
//   (car_pitch), -z (car_roll), in degrees.
void RaceCarModel::loadBodyMatrix(f32 t)
{
    RaceCarState *car = state;
    f32 x = car_view_x(car, t);
    f32 y = car_view_y(car, t);
    pgl_translate(x, y, car_view_z(car, t));
    vu0_rotate_y(car_yaw(car, t) * RAD_TO_DEG);
    vu0_rotate_x(car_pitch(car, t) * RAD_TO_DEG);
    vu0_rotate_z(-(car_roll(car, t) * RAD_TO_DEG));
}

// FUNC 0x001AA460 RaceCarModel::setReflectionPlane
//   The rotation of the ground frame at t (car_yaw2, car_ground_pitch,
//   car_ground_roll) gives its up vector (vf29, read between
//   pgl_push_matrix / pgl_pop_matrix); mat_reflect builds the mirror matrix
//   of the plane through the ground position (car_ground_view_x / y / z)
//   with that normal.  Caller: RaceEntryCar::setReflectionPlane (u042).
void RaceCarModel::setReflectionPlane(f32 t)
{
    RaceCarState *car = state;
    f32 up[3];                              /* sp+0x00 */
    f32 x = car_ground_view_x(car, t);
    f32 y = car_ground_view_y(car, t);
    f32 z = car_ground_view_z(car, t);
    pgl_push_matrix();
    vu0_load_identity_regs();
    vu0_rotate_y(car_yaw2(car, t) * RAD_TO_DEG);
    vu0_rotate_x(car_ground_pitch(car, t) * RAD_TO_DEG);
    vu0_rotate_z(-(car_ground_roll(car, t) * RAD_TO_DEG));
    vu0_get_y_axis(up);
    pgl_pop_matrix();
    mat_reflect(x, y, z, up[0], up[1], up[2]);
}

// FUNC 0x001AA568 RaceCarModel::F_001AA568
//   Empty.  Caller: RaceEntryCar::F_00177878 (u042).
void RaceCarModel::F_001AA568()
{
}

// FUNC 0x001AA570 RaceCarModel::loadGroundMatrix
//   loadBodyMatrix with the ground frame: car_ground_view_x / y / z,
//   car_yaw2, car_ground_pitch, car_ground_roll.
void RaceCarModel::loadGroundMatrix(f32 t)
{
    RaceCarState *car = state;
    f32 x = car_ground_view_x(car, t);
    f32 y = car_ground_view_y(car, t);
    pgl_translate(x, y, car_ground_view_z(car, t));
    vu0_rotate_y(car_yaw2(car, t) * RAD_TO_DEG);
    vu0_rotate_x(car_ground_pitch(car, t) * RAD_TO_DEG);
    vu0_rotate_z(-(car_ground_roll(car, t) * RAD_TO_DEG));
}

// FUNC 0x001AA630 RaceCarModel::drawShadow
//   The body's shadow (CarModel::drawShadow) in the ground frame.  Caller:
//   RaceEntryCar::drawShadow (u042; once per CarRenderCtx pass, T181: the
//   shadow of each light).
void RaceCarModel::drawShadow(f32 t)
{
    pgl_push_matrix();
    loadGroundMatrix(t);
    carModel.drawShadow();
    pgl_pop_matrix();
}

// FUNC 0x001AA680 RaceCarModel::F_001AA680
//   The body's CarModel::F_0021F2E0 (part 5, a ground pass) in the ground
//   frame.  Caller: RaceEntryCar::F_001778C0 (u042).
void RaceCarModel::F_001AA680(f32 t)
{
    pgl_push_matrix();
    loadGroundMatrix(t);
    carModel.F_0021F2E0();
    pgl_pop_matrix();
}

// FUNC 0x001AA6D0 RaceCarModel::F_001AA6D0
//   The body's CarModel::F_0021F258 (part 6 in the shadow colour, a ground
//   pass) in the ground frame.  Caller: RaceEntryCar::F_001778E8 (u042).
void RaceCarModel::F_001AA6D0(f32 t)
{
    pgl_push_matrix();
    loadGroundMatrix(t);
    carModel.F_0021F258();
    pgl_pop_matrix();
}

// FUNC 0x001AA720 RaceCarModel::drawBody
//   The body (CarModel::draw) with the model's colour between beginFade /
//   endFade, in the body frame at t when withMatrix != 0 (else in the
//   current matrix).  Callers: RaceEntryCar::drawBody (u042, withMatrix 1),
//   CarIconMaker::drawIcon (u061, 0).
void RaceCarModel::drawBody(f32 t, int withMatrix)
{
    CarModel *body = &carModel;
    body->color = colorIndex;
    pgl_push_matrix();
    body->beginFade();
    if (withMatrix != 0)
        loadBodyMatrix(t);
    body->draw();
    body->endFade();
    pgl_pop_matrix();
}

// FUNC 0x001AA7A0 RaceCarModel::drawTires
//   The four tyres in the body frame at t, each at its wheel position
//   (car_wheel_lateral_pos, car_wheel_spring, car_wheel_axle_x) turned by
//   its steer / camber / rotation (degrees), with RaceTireModel::draw at
//   the CarModel LOD lod and blurPasses g_00330B48[blur] ({0, 16, -1, 0}).
//   With blur != 0 each tyre's screen position (VU0: vf24..vf27, clamped by
//   vf21 after vu0_load_screen_regs, divided by w) is compared with the
//   last frame's (prevScreen); its screen speed v gives the blur angle:
//   the wheel's rotation at blurScale * expf(blurExp * v) less its rotation
//   at t (radians).  The glow of a heat brake: scale (CourseEnv::brakeGlowScale,
//   T131) x car_brake_heat_sq.  Caller: RaceEntryCar::drawTires (u042).
//   Matching (98.4%, the same with 991111-01): only the three scratch
//   registers of the prevScreen addresses differ (retail: wheel * 16 in $v0,
//   ours $a0; a local-alloc order).  Tried: a prev pointer (hoists this +
//   0x66C, 70%), the statement macro around the deltas / the whole block,
//   all 90 orders of the delta / prevScreen statements (the natural one is
//   best).
void RaceCarModel::drawTires(int blur, f32 scale, f32 t, int lod)
{
    CarModel *body = &carModel;
    RaceCarState *car = state;
    int blurPasses = g_00330B48[blur];
    CourseEnv *env = car->course->data->env;
    f32 blurAngle = 0.0f;
    pgl_push_matrix();
    body->beginFade();
    loadBodyMatrix(t);
    for (int wheel = 0; wheel < 4; wheel++) {
        f32 pos[4];                         /* sp+0x00 */
        pos[0] = car_wheel_lateral_pos(car, wheel, t);
        pos[1] = car_wheel_spring(car, wheel, t);
        pos[2] = car_wheel_axle_x(car, wheel, t);     /* t: not read */
        pos[3] = 1.0f;
        f32 rotation = car_wheel_rotation(car, wheel, t);
        if (blur != 0) {
            f32 delta[3];                   /* sp+0x10 (stored, not read again) */
            f32 screen[4];                  /* sp+0x20 */
            /* PC: vnop (a VU0 wait): nothing */
            vu0_load_screen_regs();
            do { vu0_project(screen, pos); } while (0);    /* (a statement macro: retail
                                                               aligns the label after it) */
            delta[0] = screen[0] - prevScreen[wheel][0];
            delta[1] = screen[1] - prevScreen[wheel][1];
            delta[2] = screen[2] - prevScreen[wheel][2];
            prevScreen[wheel][0] = screen[0];
            prevScreen[wheel][1] = screen[1];
            prevScreen[wheel][2] = screen[2];
            f32 timeScale = env->blurScale;
            f32 speed = boot_sqrtf_v(delta[0] * delta[0] + delta[1] * delta[1]);
            blurAngle = car_wheel_rotation(car, wheel, timeScale * expf(env->blurExp * speed)) - rotation;
        }
        pgl_push_matrix();
        f32 spin = rotation * RAD_TO_DEG;
        pgl_translate(pos[0], pos[1], pos[2]);
        RaceTireModel *tire = &tires[wheel];
        f32 steer = car_wheel_steer(car, wheel, t) * RAD_TO_DEG;
        f32 camber = car_wheel_camber(car, wheel, t) * RAD_TO_DEG;
        f32 heat = car_brake_heat_sq(car, wheel, t);
        tire->draw(lod, steer, camber, spin, heat * scale, blurPasses, blurAngle);
        pgl_pop_matrix();
    }
    body->endFade();
    pgl_pop_matrix();
}

// FUNC 0x001AAA90 RaceCarModel::drawTireCylinders
//   (u060 drawWheels.)  The four tyres as plain cylinders
//   (RaceTireModel::drawCylinder, the lowest level; RacePS2Base::drawCars
//   with lod > 0), not the wheel models.  In the body frame at t, at the
//   wheel positions, steer / camber / rotation in degrees; the first float
//   argument (u045: CourseEnv::brakeGlowScale) is not used.  Caller:
//   RaceEntryCar::drawTireCylinders (u042).
void RaceCarModel::drawTireCylinders(f32 unused, f32 t)
{
    CarModel *body = &carModel;
    RaceTireModel *tire = tires;
    RaceCarState *car = state;
    pgl_push_matrix();
    body->beginFade();
    loadBodyMatrix(t);
    for (int wheel = 0; wheel < 4; wheel++) {
        pgl_push_matrix();
        f32 x = car_wheel_lateral_pos(car, wheel, t);
        f32 y = car_wheel_spring(car, wheel, t);
        pgl_translate(x, y, car_wheel_axle_x(car, wheel, t));
        f32 steer = car_wheel_steer(car, wheel, t) * RAD_TO_DEG;
        f32 camber = car_wheel_camber(car, wheel, t) * RAD_TO_DEG;
        f32 rotation = car_wheel_rotation(car, wheel, t);
        tire->drawCylinder(steer, camber, rotation * RAD_TO_DEG);
        tire++;
        pgl_pop_matrix();
    }
    body->endFade();
    pgl_pop_matrix();
}

// FUNC 0x001AABE8 RaceCarModel::drawMenuTires
//   The four tyres of a showroom car (the ShowRoomCar record of
//   CarIconMaker::drawIcon, u061) at its wheel positions (wheelLateralPos,
//   wheelHeight, wheelAxleX; steer and stroke 0): first
//   RaceTireModel::drawCylinder into the alpha only (no depth writes,
//   gfx_set_frame_write_mask 0xFF000000), then RaceTireModel::draw at lod 0
//   (no view test) with all writes, no blur and no glow.
void RaceCarModel::drawMenuTires(ShowRoomCar *car)
{
    pgl_push_matrix();
    for (int wheel = 0; wheel < 4; wheel++) {
        pgl_push_matrix();
        f32 x = car->wheelLateralPos(wheel, 0.0f, 0.0f);
        f32 y = car->wheelHeight(wheel, 0.0f, 0.0f);
        pgl_translate(x, y, car->wheelAxleX(wheel, 0.0f, 0.0f));
        f32 steer = car->wheelSteerAngle(wheel, 0.0f, 0.0f) * RAD_TO_DEG;
        f32 camber = car->wheelCamber(wheel, 0.0f, 0.0f) * RAD_TO_DEG;
        gfx_depth_mask(0);
        gfx_set_frame_write_mask(FRAME_WRITE_ALPHA);
        RaceTireModel *tire = &tires[wheel];
        tire->drawCylinder(steer, camber, 0.0f);
        gfx_depth_mask(1);
        gfx_set_frame_write_mask(FRAME_WRITE_RGBA);
        tire->draw(0, steer, camber, 0.0f, 0.0f, 0, 0.0f);
        pgl_pop_matrix();
    }
    pgl_pop_matrix();
}

// FUNC 0x001AAD70 RaceCarModel::F_001AAD70
//   Empty.  Caller: RaceEntryCar::F_00177988 (u042).
void RaceCarModel::F_001AAD70(f32 t)
{
}

// FUNC 0x001AAD78 RaceCarModel::F_001AAD78
//   The body's CarModel::F_0021F010 (part 4) in the body frame.  Caller:
//   RaceEntryCar::F_001779B0 (u042, itself uncalled).
void RaceCarModel::F_001AAD78(f32 t)
{
    CarModel *body = &carModel;
    pgl_push_matrix();
    loadBodyMatrix(t);
    body->F_0021F010();
    pgl_pop_matrix();
}

// FUNC 0x001AADD0 RaceCarModel::emitSparks
//   When the car is drawn (car_display_level >= CAR_SHOW_GHOST), the car file has
//   spark points and the car is faster than g_002C03B0.sparkSpeed km/h
//   (|v|^2 against (speed / 3.6)^2): the road plane under the body's origin
//   (RaceCourse::getPlane from the state's sector hint), for the surfaces
//   0, 1, 7 and 9 only (the ones ground_surface_type, T347, turns into wet
//   codes: ? the paved ones); each spark point of the car, in the body
//   frame, that is below the road (or always, sparkAlways) throws
//   clamp((road - y) * sparkPerDepth, sparkMin, sparkMax) sparks
//   (ParticleManager::emitSparks) from 0.05 above the road.  The direction
//   is the velocity in view axes (v.x, v.z, -v.y).  Caller:
//   RaceEntryCar::emitSparks (u042).
//   Matching (98.5%; 991111-01 93.6%): after the first translation read,
//   retail sets $a0 before loading RAD_TO_DEG (ours: the load first: a
//   sched2 tie behind the volatile asm); in the loop, retail loads pos[1]
//   late and reuses $f3 for it (ours: sched1 issues that load first, $f5),
//   with plane[0] / pos[0] the other way round.  Tried: the products'
//   operand orders (24 forms), inline plane-height helpers, pos[1] in a
//   local, the comparison reversed or negated, a flag variable, the asm
//   non-volatile (worse) or with a memory clobber (no change).
void RaceCarModel::emitSparks(ParticleManager *pm, f32 t)
{
    RaceCarState *car = state;
    RaceCourse *course = car->course;
    CarModelInfo *info = data->info;
    f32 pos[3];                             /* sp+0x00 */
    f32 plane[4];                           /* sp+0x10 */
    f32 dir[3];                             /* sp+0x20 */
    int sector;                             /* sp+0x30 */
    int attr;                               /* sp+0x34 */

    if (car_display_level(car) < CAR_SHOW_GHOST)
        return;
    if (info->sparkCount == 0)
        return;
    f32 *vel = car->st.vel;
    ParticleParams *pp = &g_002C03B0;
    dir[0] = vel[0];
    dir[1] = vel[2];
    dir[2] = -vel[1];
    if (dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]
        < pp->sparkSpeed * pp->sparkSpeed * KMH_TO_MS_SQ)
        return;

    pgl_push_matrix();
    vu0_load_identity_regs();
    f32 x = car_view_x(car, t);
    f32 y = car_view_y(car, t);
    pgl_translate(x, y, car_view_z(car, t));
    vu0_get_translation(pos);
    vu0_rotate_y(car_yaw(car, t) * RAD_TO_DEG);
    vu0_rotate_x(car_pitch(car, t) * RAD_TO_DEG);
    vu0_rotate_z(-(car_roll(car, t) * RAD_TO_DEG));
    attr = 0;
    sector = car->st.sector;
    course->getPlane(pos, &sector, &attr, plane);
    if (sector < 0) {
        pgl_pop_matrix();
        return;
    }
    do {                                    /* (a statement macro in the original:
                                               retail aligns the case tree's label
                                               as the first one in a loop) */
        switch (attr) {
        case 0:
        case 1:
        case 7:
        case 9:
            break;
        default:
            pgl_pop_matrix();
            return;
        }
    } while (0);
    for (int i = 0; i < info->sparkCount; i++) {
        pgl_push_matrix();
        f32 *point = info->sparks[i];
        pgl_translate(point[0], point[1], point[2]);
        vu0_read_translation(pos);          /* (no vnop here in the original) */
        f32 road = -(pos[0] * plane[0] + pos[2] * plane[2] + plane[3]) / plane[1];
        if (pos[1] < road || pp->sparkAlways != 0) {
            f32 depth = road - pos[1];
            pos[1] = road + SPARK_LIFT;
            int count = (int)(depth * pp->sparkPerDepth);
            if (count < pp->sparkMin)
                count = pp->sparkMin;
            if (pp->sparkMax < count)
                count = pp->sparkMax;
            pm->emitSparks(pos, dir, plane, count);
        }
        pgl_pop_matrix();
    }
    pgl_pop_matrix();
}

/* ------------------------------------------------------------------------ */
/* The enhanced engine: the car state's render inputs (rendering.md 3, 5)   */
/* ------------------------------------------------------------------------ */

/* Lupo 2 computes these in its car code (L2 0x001E1288..0x001E2110) from
 * its car+0x5F0..+0x5F5; here they come from the RaceCarState fields the
 * physics writes (ref/enhanced/INTERFACES.md: wingPosition, podEmotion,
 * podEmotionHold, podAnimFrame, podLean) and the special kind
 * (CarDynamics.specialKind).  With GT3's cars (kind 0) the wing reads 0 and
 * the Pod block never runs. */

/* isPod (L2 0x001E1288): the special kind 1. */
static inline int race_model_is_pod(RaceCarState *car)
{
    return car->params.specialKind == CAR_KIND_POD;
}

/* The moving wing's position, SWITCH 6 (L2 0x001E2110): wingPosition for
 * the kinds the moving-part updater runs (L2 0x001E1E00: 2, 8 and 10 by
 * hysteresis, 9 linear in the speed), else 0. */
static inline f32 race_model_wing(RaceCarState *car)
{
    switch (car->params.specialKind) {
    case CAR_KIND_WING_GTR:
    case CAR_KIND_WING_XF:
    case CAR_KIND_WING_CIEN:
    case CAR_KIND_WING_W12:
        return car->wingPosition;
    }
    return 0.0f;
}

/* PC: a car of a moving-wing kind (race_model_wing's). */
static inline int race_model_has_wing(RaceCarState *car)
{
    switch (car->params.specialKind) {
    case CAR_KIND_WING_GTR:
    case CAR_KIND_WING_XF:
    case CAR_KIND_WING_CIEN:
    case CAR_KIND_WING_W12:
        return 1;
    }
    return 0;
}

/* The Pod's colour category of each emotion, SWITCH 8's branch (L2 .rodata
 * 0x002DE940; indexed by a nibble: 11..15 are 0 there too). */
static const u8 g_podColorCategory[16] = {
    0, 4, 4, 4, 1, 1, 1, 2, 2, 2, 3, 0, 0, 0, 0, 0
};

/* The Pod's colour category and its cross-fade, SWITCH 8 (L2 0x001E17D8):
 * while the emotion's hold count (podEmotionHold, signed) is at most 24 the
 * current emotion's category, faded in by (24 - hold) / 24; above 24 the
 * previous emotion's, faded out by (hold - 24) / 12. */
static inline int race_model_pod_color(RaceCarState *car, f32 *blend)
{
    int hold = car->podEmotionHold;

    if (hold < 25) {
        *blend = (f32)(24 - hold) / 24.0f;
        return g_podColorCategory[car->podEmotion & 0xF];
    }
    *blend = (f32)(hold - 24) / 12.0f;
    return g_podColorCategory[car->podEmotion >> 4];
}

/* The bob's triangle wave over the Pod's animation frame 0..47 (L2
 * 0x001E1470): 0..12 rising, 13..36 falling to -12, 37..47 back to 0. */
static inline int pod_bob_wave(int frame)
{
    if (frame < 37) {
        if (frame < 13)
            return frame;
        return 24 - frame;
    }
    return frame - 48;
}

/* The Pod's tail, SWITCH 7's two morphs, 0.5 at rest (L2 0x001E18A0): the
 * look 0.5 - podLean / 60 (podLean -30..30), the bob 0.5 + wave / 24. */
static inline void race_model_pod_tail(RaceCarState *car, f32 *lean, f32 *bob)
{
    *lean = 0.5f - (f32)car->podLean / 60.0f;
    *bob = 0.5f + (f32)pod_bob_wave(car->podAnimFrame) / 24.0f;
}

/* The Pod's state (SWITCH 9, L2 0x001E1950) and its face (the CLUT animation
 * id, SWITCH 10, L2 0x001E1990): both the current emotion, the low nibble
 * (L2 0x001E12E8). */
static inline int race_model_pod_emotion(RaceCarState *car)
{
    return car->podEmotion & 0xF;
}

/* ------------------------------------------------------------------------ */
/* The enhanced engine: the lamps' blink patterns (rendering.md 4)          */
/* ------------------------------------------------------------------------ */

/* A blink pattern (Lupo 2's 16-byte record, built once by L2 0x001A9048 into
 * its .bss 0x004A8F28 and fetched by L2 0x001A9090; constant data here). */
struct LampBlinkPattern {
    f32 period;                             /* 0x0 s */
    int pulses;                             /* 0x4 */
    f32 spacing;                            /* 0x8 s from one pulse's start to the next */
    f32 on;                                 /* 0xC s a pulse lasts */
};

/* LAMP_BLINK_PULSE, LAMP_BLINK_STROBE (T180_race_car_model.h) */
static const LampBlinkPattern g_lampBlink[2] = {
    { 0.4f, 1, 0.5f, 0.25f },               /* one 0.25 s pulse every 0.4 s */
    { 0.9f, 5, 0.05f, 0.05f },              /* five back-to-back 0.05 s flashes every 0.9 s */
};

/* The blink phases' step: 1/60 s a frame (L2 gp-0x79D0). */
#define LAMP_BLINK_STEP (1.0f / 60.0f)

/* The brightness of a pattern at the car's phase + the lamp's offset (L2
 * 0x001A8FB0): within one of its pulses 1 - (2 t / on - 1)^2 (0 at the
 * pulse's ends, 1 in its middle), else 0. */
static f32 lamp_blink_level(const LampBlinkPattern *p, f32 phase, f32 offset)
{
    f32 t = phase + offset;

    if (p->period < t)
        t -= p->period;
    int pulse = (int)(t / p->spacing);
    if (pulse < p->pulses) {
        t -= (f32)pulse * p->spacing;
        if (!(p->on < t)) {
            t = t / (p->on * 0.5f) - 1.0f;
            return 1.0f - t * t;
        }
    }
    return 0.0f;
}

/* A list A lamp's brightness (L2 0x001A8BF4..0x001A8C34 / 0x001A8E44..
 * 0x001A8E90): its blink pattern's, 1.0 when steady (0, or bit 0x80).
 * Not in Lupo 2: a pattern past 2 is steady here (Lupo 2 would read past
 * its two records). */
static inline f32 lamp_level(const f32 *phases, const CarLamp *lamp)
{
    u32 blink = lamp->blink;

    if (blink == LAMP_BLINK_STEADY || (blink & LAMP_BLINK_ALWAYS) != 0 || blink > LAMP_BLINK_STROBE)
        return 1.0f;
    return lamp_blink_level(&g_lampBlink[blink - 1], phases[blink - 1], lamp->blinkOffset);
}

// FUNC 0x001AB0F8 RaceCarModel::updateBodyState
//   (u060 updateLamps.)  The state to the body's model switches, not only
//   the lamps: CarModel::pushLamps(the brake is on: st.brake > 0, the brake
//   lamps), setWheelShake(wheel, car_wheel_shake_at) for the four wheels,
//   setDirt(car_dirt) and setSteer(car_steer_view).  Callers:
//   RaceEntryCar::updateBodyState (u042), RacePS2Base::virtual_29 (u044).
//   The enhanced engine (Lupo 2's RaceCarModel::update, L2 0x001A8938):
//   then the moving wing (every car), the Pod's tail, colour, state and face
//   (a Pod only), the model's animations (every car) and the blink phases.
void RaceCarModel::updateBodyState()
{
    RaceCarState *car = state;
    CarModel *body = &carModel;
    body->pushLamps(car->st.brake > 0.0f ? 1 : 0);
    for (int wheel = 0; wheel < 4; wheel++)
        body->setWheelShake(wheel, car_wheel_shake_at(car, wheel, 0.0f));
    body->setDirt(car_dirt(car));
    body->setSteer(car_steer_view(car, 0.0f));
    /* Lupo 2 (L2 0x001A89E0..0x001A89F4, rendering.md 3): the moving wing,
     * 0 but for the wing cars */
    body->setWing(race_model_wing(car));
    if (race_model_has_wing(car))           /* PC: drawn between the ticks at the shown time */
        pc_tween(body, &body->wing, PC_TWEEN_ANY);
    /* Lupo 2 (L2 0x001A89F8..0x001A8A6C, rendering.md 5): the Pod */
    if (race_model_is_pod(car)) {
        f32 blend, lean, bob;
        int color = race_model_pod_color(car, &blend);
        int emotion = race_model_pod_emotion(car);
        body->setPodColor(color, blend);
        race_model_pod_tail(car, &lean, &bob);
        body->setPodTail(lean, bob);
        /* PC: the tail and the colour fade drawn between the ticks at the
         * shown time (pcPoseBegin; a jump - a new colour category - is not
         * blended) */
        pc_tween(body, &body->podLean, PC_TWEEN_FADE);
        pc_tween(body, &body->podBob, PC_TWEEN_FADE);
        pc_tween(body, &body->podColorBlend, PC_TWEEN_FADE);
        body->setPodState(emotion);
        if (body->animSelect != emotion)    /* (the port's log line, not Lupo 2's) */
            printf("[enhanced] pod: face %d -> %d\n", body->animSelect, emotion);
        body->setAnimSelect(emotion);
    }
    /* Lupo 2 (L2 0x001A8A7C, rendering.md 1): the model's animations, every
     * car (applied once a frame by applyAnims) */
    body->animate();
    /* Lupo 2 (L2 0x001A8A84..0x001A8ADC, rendering.md 4): the blink phases,
     * wrapped at their pattern's period */
    for (int i = 0; i < 2; i++) {
        blinkPhase[i] += LAMP_BLINK_STEP;
        if (g_lampBlink[i].period < blinkPhase[i])
            blinkPhase[i] -= g_lampBlink[i].period;
    }
    /* PC: the body stepped this tick (pcPoseBegin; the tracks' step, CarModel::
     * animate's, is 1/60 s too) */
    pc_anim_stepped(this, LAMP_BLINK_STEP);
}

// RaceCarModel::pcPoseBegin (PC, no original; the header)
//   The switches as updateBodyState sets them, from the accessors at the
//   shown time (view_t lags them while drawing); the blink phases and the
//   tracks back by the offset (a phase below 0 wraps as the step wraps);
//   the wing and the Pod's tail and colour fade blended between the last
//   two ticks (pc_tween).  The Pod's state and the lamps' history stay the
//   tick's; the palettes: pcPosePalettes.
void RaceCarModel::pcPoseBegin(PcPose *saved)
{
    CarModel *body = &carModel;
    RaceCarState *car = state;
    f32 ofs = pc_anim_offset(this);

    pc_tween_begin(body);                   /* the wing, the Pod's tail and colour fade */
    saved->posed = ofs != 0.0f && car != NULL;
    if (!saved->posed)
        return;
    for (int wheel = 0; wheel < 4; wheel++) {
        saved->wheelShake[wheel] = body->callback.wheelShake[wheel];
        body->setWheelShake(wheel, car_wheel_shake_at(car, wheel, 0.0f));
    }
    saved->steer = body->callback.steer;
    body->setSteer(car_steer_view(car, 0.0f));
    for (int i = 0; i < 2; i++) {
        saved->blinkPhase[i] = blinkPhase[i];
        f32 phase = blinkPhase[i] + ofs;
        if (phase < 0.0f)
            phase += g_lampBlink[i].period;
        blinkPhase[i] = phase;
    }
#if ENH_RENDER_CAR_ANIMS
    if (body->animSelect < 0)               /* the tracks step only with -1 (CarModel::animate) */
        modelset_pose_anims(body->model, ofs);
#endif
}

// RaceCarModel::pcPoseEnd (PC, no original)
void RaceCarModel::pcPoseEnd(const PcPose *saved)
{
    CarModel *body = &carModel;

    pc_tween_end(body);
    if (!saved->posed)
        return;
    for (int wheel = 0; wheel < 4; wheel++)
        body->setWheelShake(wheel, saved->wheelShake[wheel]);
    body->setSteer(saved->steer);
    for (int i = 0; i < 2; i++)
        blinkPhase[i] = saved->blinkPhase[i];
#if ENH_RENDER_CAR_ANIMS
    if (body->animSelect < 0)
        modelset_pose_anims(body->model, 0.0f);
#endif
}

// RaceCarModel::pcPosePalettes (PC, no original; the header)
//   The body's CLUT animations of animSelect (every one with -1), as
//   CarModel::animate steps them, written into the palettes at the shown
//   time (pose != 0: when updateBodyState ran in the last tick) or at their
//   state (pose 0).  At the end of a race frame (T131).
void RaceCarModel::pcPosePalettes(int pose)
{
    f32 ofs = pose ? pc_anim_offset(this) : 0.0f;

    if (pose && ofs == 0.0f)
        return;
    modelset_pose_clut_anims(carModel.model, carModel.animSelect, ofs);
}

// FUNC 0x001AB1B8 RaceCarModel::addLamps
//   When the car is drawn (car_display_level >= CAR_SHOW_GHOST), in the body frame
//   at t, the car file's lamps go to the flare (FlareClass::addLight): list
//   A (directional, 1.0); list C, the brake lamps, when the body's brake
//   lamp history is partly set (CarModelLamps::fraction > 0): the flagged
//   (bit 0) lamps at that fraction, the others at 1.0 while braking now;
//   list B (not directional, 1.0).  Caller: RaceEntryCar::addLamps (u042).
//   The enhanced engine (Lupo 2's setLampFlare, L2 0x001A8B20; rendering.md
//   4): a list A lamp with a blink pattern at the pattern's brightness.
//   Lupo 2's third argument, the lights on, is always 1 (its addCarLamps L2
//   0x0017EB20), so its "off" branches (list A's steady lamps without bit
//   0x80, lists C and B skipped) never run and are left out.
void RaceCarModel::addLamps(FlareClass *flare, f32 t)
{
    bool braking = car_braking(state);
    CarModelInfo *info = data->info;
    if (car_display_level(state) < CAR_SHOW_GHOST)
        return;
    pgl_push_matrix();
    loadBodyMatrix(t);
    for (int i = 0; i < info->lampCountA; i++) {
        CarLamp *lamp = &info->lampsA[i];
        flare->addLight(lamp, lamp_level(blinkPhase, lamp), 1);     /* Lupo 2: blinking (GT3: 1.0) */
    }
    f32 lit = carModel.lamps.fraction();
    if (lit > 0.0f) {
        for (int i = 0; i < info->lampCountC; i++) {
            CarLamp *lamp = &info->lampsC[i];
            if (lamp->flags & 1)
                flare->addLight(lamp, lit, 1);
            else if (braking)
                flare->addLight(lamp, 1.0f, 1);
        }
    }
    for (int i = 0; i < info->lampCountB; i++)
        flare->addLight(&info->lampsB[i], 1.0f, 0);
    pgl_pop_matrix();
}

// FUNC 0x001AB370 RaceCarModel::drawLampReflections
//   addLamps with FlareClass::drawReflection, no display level test, list
//   B at 0.3.  (The original leaves list C's count in $a2 for the list C
//   calls; drawReflection takes no such argument.)  Caller:
//   RaceEntryCar::drawLampReflections (u042).
//   The enhanced engine (Lupo 2's setReflectionLampFlare, L2 0x001A8DA0;
//   rendering.md 4): a blinking list A lamp's reflection follows it.
void RaceCarModel::drawLampReflections(FlareClass *flare, f32 t)
{
    bool braking = car_braking(state);
    CarModelInfo *info = data->info;
    pgl_push_matrix();
    loadBodyMatrix(t);
    for (int i = 0; i < info->lampCountA; i++) {
        CarLamp *lamp = &info->lampsA[i];
        flare->drawReflection(lamp, lamp_level(blinkPhase, lamp));  /* Lupo 2: blinking (GT3: 1.0) */
    }
    f32 lit = carModel.lamps.fraction();
    if (lit > 0.0f) {
        for (int i = 0; i < info->lampCountC; i++) {
            CarLamp *lamp = &info->lampsC[i];
            if (lamp->flags & 1)
                flare->drawReflection(lamp, lit);
            else if (braking)
                flare->drawReflection(lamp, 1.0f);
        }
    }
    for (int i = 0; i < info->lampCountB; i++)
        flare->drawReflection(&info->lampsB[i], LAMP_B_REFLECTION);
    pgl_pop_matrix();
}

/* The enhanced engine (new, Lupo 2's RaceCarModel slot 3, L2 0x001A8B00;
 * rendering.md 1): the body's dirty CLUT records written into its palettes
 * (CarModel::applyAnims), the step updateBodyState's animate left.  Lupo 2
 * calls it for every entry from RacePS2Base::virtual_3 (L2 0x0017BA98..
 * 0x0017BADC, after course_apply_clut_anims); that caller is race/core's
 * (ref/enhanced/ported/rendering.md, "For other areas"). */
void RaceCarModel::applyAnims()
{
    carModel.applyAnims();
}

/* The car model arena (.data 0x002C2F50: egcs puts a public object that
 * has a constructor in .data, zero-filled). */
UnitArena<64> g_002C2F50;                   /* 0x002C2F50 */

/* Compiler-generated: egcs's static destructor and initialiser of
 * g_002C2F50, emitted at the end of the unit, the destructor first. */
// FUNC 0x001AB508 race_car_model_static_dtor (_GLOBAL_$D$g_00330B48)
//   .dtors entry (0x0028F220): UnitArena<64>::~UnitArena of the car model
//   arena, inlined (the vptr 0x0032DD60, then _UnitArenaBase::
//   ~_UnitArenaBase with flags 2).
/* implicit: ~_UnitArenaBase (base destructor, inlined ~UnitArena) */

// FUNC 0x001AB538 race_car_model_static_ctor (_GLOBAL_$I$g_00330B48)
//   .ctors entry (0x0028F140): the car model arena (UnitArena<64>'s inline
//   constructor: _UnitArenaBase::_UnitArenaBase, then the vptr 0x0032DD60).
/* implicit: _UnitArenaBase (base constructor, inlined UnitArena constructor) */

// FUNC 0x001AB570 UnitArena<64>::buildFreeList
/* template code in game/race/core/T153_race_course.h (UnitArena<N>::buildFreeList,
 * index: virtual_1); egcs emits this dead weak copy in T180 (T153's
 * 0x0018FE98 is the one linked; the vtable copy 0x00330B68 points to T153's
 * members too) */

// FUNC 0x001AB5E8 UnitArena<64>::~UnitArena
/* inline in game/race/core/T153_race_course.h (index: virtual_0: the vptr, then
 * _UnitArenaBase::~_UnitArenaBase with the caller's flags); egcs emits this
 * dead weak copy in T180 (T153's is 0x0018FF10) */
/* implicit: ~_UnitArenaBase (base destructor) */

/* The accessors: inline in the original, defined here (out of the class:
 * the other units call getCarModel out of line, T121 / T131 / T184, so
 * they did not see the bodies).  The class's key function is here, so egcs
 * writes every one of them, after the static destructor / constructor and
 * UnitArena<64>'s members, in the reverse of their definition order
 * (retail's frame info): defined from setState (0x001AB730) back to
 * releaseArena (0x001AB610).  setModelLod is defined before getCarModel,
 * so its call to getCarModel stays (retail calls 0x001AB6E0).  GCC 14:
 * plain functions (the other units call them), except allocBlock /
 * freeBlock (the header's inline bodies, below). */
#define T180_INLINE

// FUNC 0x001AB730 RaceCarModel::setState
//   No callers.
T180_INLINE void RaceCarModel::setState(RaceCarState *s)
{
    state = s;
}

// FUNC 0x001AB728 RaceCarModel::getState
//   No callers.
T180_INLINE RaceCarState *RaceCarModel::getState()
{
    return state;
}

// FUNC 0x001AB720 RaceCarModel::setColorIndex
//   No callers.
T180_INLINE void RaceCarModel::setColorIndex(int c)
{
    colorIndex = c;
}

// FUNC 0x001AB718 RaceCarModel::getColorIndex
//   No callers.
T180_INLINE int RaceCarModel::getColorIndex()
{
    return colorIndex;
}

// FUNC 0x001AB6E8 RaceCarModel::setModelLod
//   (u060 setDrawMode.)  No callers: it sets the body's fixed LOD
//   (CarModel::Callback::lod, +0x08; u045 / u061 CarDrawParam::mode), as
//   RaceEntryCar::setModelLod does, through getCarModel and T121's
//   F_00179460 (egcs: through the helper above, see there).
T180_INLINE void RaceCarModel::setModelLod(int lod)
{
    F_00179460(getCarModel())->callback.lod = lod;
}

// FUNC 0x001AB6E0 RaceCarModel::getCarModel
//   Callers: RaceEntryCar::setModelLod (u042), RacePS2Base::drawCars /
//   virtual_51 (u045), setDrawMode, CarIconMaker::drawIcon (u061).
T180_INLINE CarModel *RaceCarModel::getCarModel()
{
    return &carModel;
}

// FUNC 0x001AB6C8 RaceCarModel::getTire
//   No callers.
T180_INLINE RaceTireModel *RaceCarModel::getTire(int i)
{
    return &tires[i];
}

// FUNC 0x001AB6B8 RaceCarModel::getInfo
//   No callers (RaceEntryCar::getModelInfo has the same code).
T180_INLINE CarModelInfo *RaceCarModel::getInfo()
{
    return data->info;
}

/* allocBlock / freeBlock: inline in the header too, for T139 (RaceSolitaire's
 * virtual_57 / destructor) and T146 (LicenseExaminationMessages' constructor
 * / destructor), which expand them (GAME_T180_INLINE_BLOCK_ACCESSORS).
 * egcs: defined here, not from the header (T180 does not define the macro):
 * the header's bodies would be read before getInfo .. setState, and their
 * copies would come out after setState's.  GCC 14: the header's. */

// FUNC 0x001AB630 RaceCarModel::setArena
//   No callers.  The arena over buf (0xA8000-byte blocks,
//   _UnitArenaBase::init); returns the previous buffer.
T180_INLINE void *RaceCarModel::setArena(void *buf, u32 size)
{
    return g_002C2F50.init(buf, size, CAR_MODEL_BLOCK_SIZE);
}

// FUNC 0x001AB610 RaceCarModel::releaseArena
//   No callers.  The arena forgets its buffer (_UnitArenaBase::reset) and
//   returns it.
T180_INLINE void *RaceCarModel::releaseArena()
{
    return g_002C2F50.reset();
}


/* (0x001AB778: T180's weak copy of F_00179460, its marker at the top.) */

