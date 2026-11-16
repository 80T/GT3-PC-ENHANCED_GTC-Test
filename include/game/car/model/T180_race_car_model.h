/* T180_race_car_model.h - RaceCarModel, the drawn model of a race car, and
 * the car model arena (translation unit T180, unit u060), 0x001A9B90..
 * 0x001AB7D0.
 *
 * RaceCarModel (RaceEntryCar +0x11F8, T121): its car file is read
 * asynchronously (file_req_alloc) into one 0xA8000-byte block of the car
 * model arena g_002C2F50 (a UnitArena<64> over a 0x498080-byte malloc: 7
 * blocks, initArena / freeArena from race_begin / race_end), relocated in
 * place (T456) and its body handed to the CarModel (T446).  The tyre file
 * ("tire/n%d") and the wheel file ("wheel/%s") are loaded the same way into
 * buffers of the car file.  The drawing methods load the VU0 matrix of the
 * car state at time t (the body frame, or the "ground" frame of T302's
 * car_ground_* accessors) and draw the body, its shadow passes, the four
 * tyres (RaceTireModel, T449, with a motion blur from their screen speed
 * and the glow of a hot brake), the lamps (as lights of the FlareClass,
 * T159) and the sparks thrown where the points under the car go below the
 * road.
 *
 * T180 also has dead weak copies of the UnitArena<64> members (0x001AB570
 * buildFreeList, 0x001AB5E8 ~UnitArena, 0x001AB780 __tft9UnitArena1Ui64:
 * T153's are the ones used; the template is T153's) and of T121's
 * F_00179460 (0x001AB778, returns its argument).  The names marked "?" are
 * u060's guesses. */
#ifndef GAME_T180_RACE_CAR_MODEL_H
#define GAME_T180_RACE_CAR_MODEL_H

#include "game/game.h"
#include <stddef.h>                         /* size_t */
#include "game/car/model/T446_car_model.h"            /* CarModel (held by value) */
#include "game/car/model/T449_race_tire_model.h"      /* RaceTireModel (held by value) */
#include "game/race/core/T153_race_course.h"          /* UnitArena<64> (g_002C2F50) */

struct CarModelFile;                        /* T456_car_model_file.h (pointers only) */
struct CarModelInfo;                        /* T455_car_model_info.h */
class RaceCarState;                         /* T302_car_state.h */
struct MenuCarSpec;                         /* T232_menu_car_spec.h */
class ShowRoomCar;                          /* T278_show_room_car.h */
class ParticleManager;                      /* T165_particle_manager.h */
class FlareClass;                           /* T159_flare.h */
class type_info;                            /* T548 */


/* vtable 0x00330B58 (1 slot), typeinfo __tf12RaceCarModel 0x001AB738
 * (user).  The first polymorphic class of its hierarchy: vptr +0x6BC,
 * after the data.  Size 0x6C0 (RaceEntryCar +0x11F8; the race pool:
 * operator new). */
class RaceCarModel {
public:
    /* The file_req_alloc callbacks: done(this, the buffer; NULL: failed). */
    static void onModelLoaded(void *self, void *buf);   /* 0x001A9B90 setData(buf) */
    static void onTireLoaded(void *self, void *buf);    /* 0x001A9BB0 setTireData(buf) */
    static void onWheelLoaded(void *self, void *buf);   /* 0x001A9BD0 setWheelData(buf) */
    /* The car model arena: 0x498080 bytes (malloc) of 0xA8000-byte blocks
     * (race_begin / race_end, u001; RaceCourse's course arena has the same
     * pair). */
    static void initArena();                            /* 0x001A9BF0 */
    static void freeArena();                            /* 0x001A9C30 */
    /* The race pool g_002B3BF0 (pdistd::Allocator), 16-byte aligned. */
    static void *operator new(size_t size);             /* 0x001A9C58 */
    static void  operator delete(void *p, size_t size); /* 0x001A9C80 the size is ignored */

    /* CarModel(), the four RaceTireModel(), then the fields (the state is
     * left).  Caller: RaceEntryCar's constructor (u042). */
    RaceCarModel();                                     /* 0x001A9CA8 */
    /* release(), the tyres from the last, ~CarModel. */
    virtual ~RaceCarModel();                            /* [0] +0x0C 0x001A9E18 */

    /* ? name: it returns *this (probably the original's operator=).
     * Releases the model, takes src's load flags, state and colour, copies
     * src's car file into a new block, relocates it (setData) and sets up
     * the tyres. */
    RaceCarModel &copyFrom(const RaceCarModel &src);    /* 0x001A9ED8 */
    /* Gives the car file's block back; the load flags cleared. */
    void release();                                     /* 0x001A9F78 */
    /* Unless a model is there: a block, the file read over it. */
    void load(const char *path);                        /* 0x001A9FC8 */
    /* The loaded car file (NULL: the read failed) relocated in place, its
     * body given to the CarModel. */
    void setData(CarModelFile *f);                      /* 0x001AA048 */
    /* The tyre file of the car's tyre part parts[0] (flag: the second
     * table of carmodelinfo_tire), unless it is the car's default. */
    void loadTire(const int *parts, int flag);          /* 0x001AA0A8 */
    void setTireData(void *buf);                        /* 0x001AA178 relocates it; tireLoaded */
    /* "wheel/<name>" into the car file's wheel buffer. */
    void loadWheel(const char *name);                   /* 0x001AA1B0 */
    void setWheelData(void *buf);                       /* 0x001AA218 relocates it; wheelLoaded */
    /* Each tyre: its position, colour, files, its axle's brake (CarAxleSpec)
     * and size (the state's params.gear tyreWidth / tyreRadius /
     * tyreSidewall); their work spaces from the end of the car file. */
    void setupTires();                                  /* 0x001AA250 */
    /* The current matrix times the body frame at t. */
    void loadBodyMatrix(f32 t);                         /* 0x001AA3A0 */
    /* The mirror matrix of the ground plane at t (mat_reflect). */
    void setReflectionPlane(f32 t);                     /* 0x001AA460 */
    void F_001AA568();                                  /* 0x001AA568 empty */
    /* loadBodyMatrix with the ground frame (car_ground_*). */
    void loadGroundMatrix(f32 t);                       /* 0x001AA570 */
    void drawShadow(f32 t);                             /* 0x001AA630 the body's drawShadow in the ground frame (one per
                                                           CarRenderCtx pass, T181: the shadow of each light) */
    void F_001AA680(f32 t);                             /* 0x001AA680 the body's F_0021F2E0 in the ground frame */
    void F_001AA6D0(f32 t);                             /* 0x001AA6D0 the body's F_0021F258 in the ground frame */
    /* The body in the body frame at t (withMatrix != 0), else in the
     * current matrix.  The float first (the same registers either way):
     * CarIconMaker::drawIcon (u061) sets $f12 before $a1, as egcs does for
     * the arguments in this order. */
    void drawBody(f32 t, int withMatrix);               /* 0x001AA720 */
    /* The four tyres at the CarModel LOD lod (RaceTireModel::draw's).  blur:
     * 0 none, 1 rendered (16 passes), 2 the cheap one (-1): g_00330B48; the
     * blur angle from the tyres' screen speed (prevScreen).  scale x the
     * brake heat (car_brake_heat_sq) is the glow of a heat brake.  lod last
     * (as RaceEntryCar::drawTires, u042; the same registers either way):
     * the parameters' order decides retail's $s6 / $s7 (u060 notes). */
    void drawTires(int blur, f32 scale, f32 t, int lod);    /* 0x001AA7A0 */
    /* The four tyres as plain cylinders (RaceTireModel::drawCylinder, the
     * lowest level; RacePS2Base::drawCars with lod > 0), not the wheel
     * models.  The first argument is not used.  (u060 drawWheels.) */
    void drawTireCylinders(f32 unused, f32 t);          /* 0x001AAA90 */
    /* The tyres of a showroom car at its wheel positions (its only caller is
     * CarIconMaker::drawIcon, T184, with its stack ShowRoomCar). */
    void drawMenuTires(ShowRoomCar *car);               /* 0x001AABE8 */
    void F_001AAD70(f32 t);                             /* 0x001AAD70 empty */
    void F_001AAD78(f32 t);                             /* 0x001AAD78 the body's F_0021F010 in the body frame */
    /* The sparks of the car file's spark points that go below the road
     * (g_002C03B0, T166: the thresholds). */
    void emitSparks(ParticleManager *pm, f32 t);        /* 0x001AADD0 */
    /* The state to the body's model switches: the brake (brakeLamps), the
     * four wheels' shake (setWheelShake), the dirt (setDirt) and the
     * steering (setSteer).  (u060 updateLamps.)  The enhanced engine (Lupo 2
     * L2 0x001A8938; rendering.md 1-5): also the moving wing, the Pod's
     * tail / colour / state / face, the model's animations (CarModel::
     * animate) and the lamps' blink phases. */
    void updateBodyState();                             /* 0x001AB0F8 */
    /* The car file's lamps as lights of the flare (lists A, C, B; list C
     * are the brake lamps).  The enhanced engine: a list A lamp with a blink
     * pattern at its pattern's brightness (rendering.md 4). */
    void addLamps(FlareClass *flare, f32 t);            /* 0x001AB1B8 */
    /* Their reflections on the road (FlareClass::drawReflection). */
    void drawLampReflections(FlareClass *flare, f32 t); /* 0x001AB370 */
    /* The enhanced engine (new, Lupo 2's RaceCarModel slot 3, L2 0x001A8B00;
     * rendering.md 1): the body's dirty CLUT records written into its
     * palettes (CarModel::applyAnims).  Once per frame for every entry, from
     * RacePS2Base::virtual_3 (race/core, after course_apply_clut_anims). */
    void applyAnims();
    /* PC (no original): the dynamic frame rate (game/app/pc_frame.h).  For
     * a race view drawn between two ticks (RacePS2Base, T131), the body at
     * the shown time: the switches (wheel shake, steering) from the
     * interpolating accessors, the lamp blinks and the model's animation
     * tracks 1/60 s x the lag back, the wing and the Pod's tail and colour
     * fade blended; nothing unless updateBodyState ran in the last tick.
     * pcPoseEnd puts the tick's state back.  pcPosePalettes: the body's
     * CLUT animations at the shown time (pose != 0) or at their state (0),
     * at the end of a race frame (the renderer reads the palettes when the
     * frame's lists execute). */
    struct PcPose {
        int posed;
        f32 wheelShake[4];
        f32 steer;
        f32 blinkPhase[2];
    };
    void pcPoseBegin(PcPose *saved);
    void pcPoseEnd(const PcPose *saved);
    void pcPosePalettes(int pose);
    /* The accessors below: inline in the original, but defined in T180's
     * source, not here (the other units call getCarModel out of line);
     * egcs writes them after the static constructor (the data pass,
     * ref/data/u060/notes.md).  No callers: the arena g_002C2F50.
     * allocBlock / freeBlock are also defined below the class for T139 /
     * T146, which expand them (GAME_T180_INLINE_BLOCK_ACCESSORS). */
    static void *releaseArena();                        /* 0x001AB610 forgets its buffer, returns it */
    static void *setArena(void *buf, u32 size);         /* 0x001AB630 0xA8000-byte blocks; the old buffer */
    static void  freeBlock(void *block);                /* 0x001AB668 inline (below) */
    static void *allocBlock();                          /* 0x001AB690 inline (below) */
    CarModelInfo  *getInfo();                           /* 0x001AB6B8 no callers: data->info */
    RaceTireModel *getTire(int i);                      /* 0x001AB6C8 no callers */
    CarModel      *getCarModel();                       /* 0x001AB6E0 &carModel */
    /* No callers: it sets the body's fixed LOD (CarModel::Callback::lod, as
     * RaceEntryCar::setModelLod does; u060 setDrawMode). */
    void setModelLod(int lod);                          /* 0x001AB6E8 */
    int  getColorIndex();                               /* 0x001AB718 no callers */
    void setColorIndex(int c);                          /* 0x001AB720 no callers */
    RaceCarState *getState();                           /* 0x001AB728 no callers */
    void setState(RaceCarState *s);                     /* 0x001AB730 no callers */

    /* The offsets are GT3's; in the enhanced engine CarModel is 0x1C longer,
     * so every member from tires on sits 0x1C later (blinkPhase and the vptr
     * show this build's). */
    CarModelFile  *data;                    /* 0x000 a block of g_002C2F50 (u042 RaceCarModelData) */
    CarModel       carModel;                /* 0x004 the body */
    RaceTireModel  tires[4];                /* 0x03C 0, 1 front, 2, 3 rear */
    f32            prevScreen[4][4];        /* 0x66C the tyres' last screen positions (the blur) */
    RaceCarState  *state;                   /* 0x6AC */
    int            colorIndex;              /* 0x6B0 */
    /* The load flags: bools (egcs's are 4 bytes, with their own alias set:
     * retail's copyFrom loads them ahead of the int stores).  GCC 14 keeps
     * ints (the values are 0 / 1), as T151's highlight. */
    int            tireLoaded;
    int            wheelLoaded;
    /* ---- The enhanced engine (ref/enhanced/INTERFACES.md): the two blink
     * phase accumulators of the lamp patterns (rendering.md 4; 0 .. the
     * pattern's period 0.4 / 0.9 s, advanced by 1/60 a frame in the update,
     * L2 0x001A8A84..0x001A8ADC; Lupo 2 RaceCarModel +0x6DC / +0x6E0).
     * Rendering only; added here so that RaceEntryCar grows once.
     * [0] pattern 1 (LAMP_BLINK_PULSE), [1] pattern 2 (LAMP_BLINK_STROBE). */
    f32            blinkPhase[2];           /* 0x6D8 */
    /* ---- PC (ROADMAP 18, no original): the buffers the car model takes
     * apart from the arena g_002C2F50, each sized by its file and freed by
     * release(): the car file's block (data, memalign; was a 0xA8000-byte
     * block of the arena), the replacement tyre / wheel file (loadTire /
     * loadWheel; was read over the car file's own part, which a bigger file
     * overran; the car file keeps its own, setupTires picks the loaded one)
     * and the tyres' work space (setupTires; was the room after the car
     * file's wheel part). */
    u32            pcFileSize;              /* data's block, bytes (copyFrom copies them) */
    void          *pcTireFile;              /* the tyre file loadTire read (NULL: the car file's) */
    u32            pcTireSize;
    void          *pcWheelFile;             /* the wheel file loadWheel read (NULL: the car file's) */
    u32            pcWheelSize;
    void          *pcTireWork;              /* RACE_TIRE_WORK_SIZE bytes (T180) */
    /* ---- PC (2026-11-11, no original): the cockpit camera's menu model
     * (PC 2026-11-15: per car model, [CockpitMenuModels], the calibrator's;
     * game/app/pc_headcam.h): the car's
     * menu/cars/ file (the showroom's body: the race body's frame, ~40 %
     * more triangles, no driver), read by pcLoadMenu for the cars the views
     * follow while the setting is on (RacePS2Base::virtual_3) and kept until
     * release().  drawBody draws its body instead of the race body while
     * pcMenuDraw (the cockpit view's own car, RacePS2Base::drawCars): the
     * CarModel's state - colour, lamps, dirt, switches - stays the car's. */
    CarModelFile  *pcMenuFile;
    int            pcMenuColors;            /* its body's colour count (CarModel::setModel's) */
    u8             pcMenuState;             /* 0 not read yet, 1 read, 2 none (missing, no memory) */
    u8             pcMenuDraw;
    /* Reads the menu model of car `code' (blocking, once). */
    void pcLoadMenu(u64 code);
    /* ---- PC (2026-11-12, no original): the menu wheel ([Graphics]
     * MenuWheels, game/app/pc_menu_wheels.h): the showroom's wheel file of
     * the car's wheel - an aftermarket wheel's "menu/wheel/<name>" (loadWheel
     * keeps the name), else the wheel part of the car's menu file - in a
     * buffer of its own, relocated, read by pcLoadMenuWheel (blocking, once:
     * RaceEntryCar::setupTires while the setting is on, RacePS2Base::
     * virtual_3 when it is turned on in a race), kept until release().
     * drawTires gives it to the tyres while the wheel is slow
     * (pcMenuWheelFast). */
    void          *pcMenuWheel;             /* a WheelFile2; NULL: none */
    u32            pcMenuWheelSize;
    u8             pcMenuWheelState;        /* 0 not read yet, 1 read, 2 none (missing, no memory) */
    u8             pcMenuWheelFast[4];      /* per wheel: its race wheel has taken over (pc_menu_wheel_fast) */
    char           pcWheelName[0x40];       /* loadWheel's name, "" for the car's own wheel */
    void pcLoadMenuWheel(u64 code);
    /* ---- PC (2026-11-13, no original): the menu tyre ([Graphics] MenuTires,
     * game/app/pc_menu_tires.h): the menu file of the car's tyre,
     * "menu/tire/n<k>" (k: loadTire's number, else the car file's default),
     * shared with the other cars on that tyre (pc_menu_tire_acquire), taken
     * by pcLoadMenuTire (a blocking read the first time; once a car:
     * RaceEntryCar::setupTires while the setting is on, RacePS2Base::
     * virtual_3 when it is turned on in a race); pcBuildMenuTires builds the
     * four tyres' menu packets with it (RaceTireModel::pcBuildMenu) into
     * pcMenuTireWork - at the end of setupTires, or after a read in a race.
     * Kept until release(). */
    void          *pcMenuTire;              /* a TireFile2 (shared: pc_menu_tire_release); NULL: none */
    u32            pcMenuTireSize;
    u8             pcMenuTireState;         /* 0 not read yet, 1 read, 2 none (missing, no memory) */
    int            pcTireNumber;            /* loadTire's tyre file number (-1: not asked: the default) */
    void          *pcMenuTireWork;          /* PC_MENU_TIRE_WORK_SIZE bytes (T180) */
    void pcLoadMenuTire();
    void pcBuildMenuTires();
    /*             vptr                        (after the PC members) */
};
EGCS_SIZE(RaceCarModel, 0x6E4);

/* The enhanced engine (rendering.md 4): CarLamp.blink (T159_flare.h), a
 * list A lamp's blink pattern (Lupo 2's table L2 0x001A9048, 2 x 16 bytes at
 * 0x004A8F28; T180's g_lampBlink). */
enum {
    LAMP_BLINK_STEADY = 0,                  /* lit at 1.0, as every GT3 lamp */
    LAMP_BLINK_PULSE  = 1,                  /* one 0.25 s pulse every 0.4 s */
    LAMP_BLINK_STROBE = 2,                  /* five back-to-back 0.05 s flashes every 0.9 s */
    LAMP_BLINK_ALWAYS = 0x80                /* steady (Lupo 2: lit even with its lamps off) */
};

/* The car model arena: 7 blocks of 0xA8000 bytes (RaceCarModel::initArena).
 * The replay memory takes blocks of it too (u047 RaceSolitaire: three, u049
 * LicenseExaminationMessages: one; their BlockPool), through allocBlock /
 * freeBlock.  PC: none of them uses it any more (the car models, the replay
 * memory and the licence trophies take buffers sized by their files: ROADMAP
 * 18); initArena / freeArena keep it as retail's. */
extern UnitArena<64> g_002C2F50;            /* 0x002C2F50 */

/* A block of the arena and back: inline (retail T139's RaceSolitaire::
 * virtual_57 / ~RaceSolitaire and T146's LicenseExaminationMessages
 * constructor / destructor expand them; Lupo 2 calls them out of line
 * there).  egcs: only for the files that define
 * GAME_T180_INLINE_BLOCK_ACCESSORS (T139, T146) before including this
 * header: the bodies would put an undefined g_002C2F50 (egcs's MIPS
 * `.extern') into every other includer's object (ref/data/u050/notes.md).
 * T180's source defines them itself under egcs, among its other accessors:
 * its out-of-line copies come out in the reverse of the order egcs read the
 * bodies, and retail's order (the allocBlock copy before getInfo's) needs
 * them read after getInfo's body, which the other includers must not see.
 * GCC 14: every includer, T180 too.  An exact form, not necessarily the
 * original's. */
inline void *RaceCarModel::allocBlock()
{
    return g_002C2F50.alloc(0);
}

inline void RaceCarModel::freeBlock(void *block)
{
    g_002C2F50.release(block);
}

/* The .ctors / .dtors entries of the car model arena are egcs's
 * _GLOBAL_$D$g_00330B48 0x001AB508 (.dtors entry 0x0028F220: ~UnitArena<64>)
 * and _GLOBAL_$I$g_00330B48 0x001AB538 (.ctors entry 0x0028F140:
 * UnitArena<64>()), generated for g_002C2F50, which T180 defines (u060's
 * race_car_model_static_dtor / _ctor; the vptr 0x0032DD60, T153's vtable). */

#endif /* GAME_T180_RACE_CAR_MODEL_H */
