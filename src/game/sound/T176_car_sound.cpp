/* T176_car_sound.cpp - CarSound and EngineSound (T176), u059.
 *
 * A car has three streamed engine voices (CarSoundEngine: the voice and the
 * EngineSound bank it plays), 32 slots of looped sound effects (the engine
 * whines, the cabin and body noises, seven per wheel: skids, road noise,
 * rumble, rain) and four one-shot effects ("b_off", "clash".."clash3").
 * Every frame RaceEntryCar::updateSound fills a CarSoundFrame (T121) and
 * calls update: each voice is panned from its position relative to the
 * listener (constant-power pan, 32 / distance attenuation, clamped to 1) and
 * scaled by the two global volume factors g_003521E0 x g_003521E4.
 *
 * Data (retail): .sdata 0x003521E0..0x00352230 (the volume factors, the
 * background flag, the bank kind names), .data 0x002C2088 (the kind table,
 * then the frame info), .rodata 0x003308E8..0x00330A2F (load's two formats,
 * g_00330910, the vtable, then the type names: "11EngineSound" and the
 * GameStatus's garage names, so EngineSound is defined after T429's
 * classes; no "bad_alloc", no RaceCameraBase / UnitArena<64>).
 *
 * The enhanced engine (Lupo 2, cameras_sound.md S1): the Toyota Pod's voice,
 * as Lupo 2 builds it: a channel of its own (GtSoundChannel, T468) and a pan
 * in CarSound, started / stopped with the car's other voices, and every
 * frame panned, levelled, pitched (the Doppler factor) and prioritised like
 * them (updatePodVoice, from update).  A new emotion of the Pod releases the
 * voice and keys on its pod_* effect (T469: bank 3, sound/gt3pod.ins).  The
 * GT3Hooks plugin (PodSound.c) played it from a hook after the frame was
 * gone: no Doppler, no priority, no stop with the car's sounds.
 */
#include "types.h"
#include "libc.h"                           /* memcpy */
#include "cxxrt.h"                          /* type_info, __rtti_si (GCC 14's __tf11EngineSound) */
#include "game/save/T429_game_status.h"          /* getGameStatus, GameOptions (raceSe / replaySe): the garage names */
#include "game/sound/T176_car_sound.h"            /* this translation unit: EngineSound after the garage classes */
#include "game/race/core/T121_race_entry.h"           /* CarSoundFrame, CarSoundVoice(Ex) */
#include "game/sound/T468_gtsound.h"              /* GTSOUNDINSTRUMENT, GtSoundVoice, GtSoundHandle, SoundProgram,
                                               sound_set_spu_addr / header_buffer, sound_load_es_cb, g_00352B8C */
#include "game/sound/T469_sound_effects.h"        /* se_program, se_play, g_roadNoiseBank, SE_* */
#include "engine/media/G002_mpeg_frameq.h"          /* SndVoiceParam, engine_*, snd_play_voice, snd_note_to_pitch,
                                               sinf, cosf */
#include "engine/io/T504_file_queue.h"           /* file_req_alloc, FileDoneFn */
#include "engine/pdistd/T534_sprintf.h"              /* sprintf */
#include "game/sound/car_sound_internal.h"                           /* sound_voice_priority, spu_volume_clamp, SPU_VOLUME_* */
#include "game/app/pc_volume.h"                   /* PC: pc_race_se_volume */
#include "game/race/race_cars.h"                  /* PC: PC_RACE_CARS_MAX (car_sound_budget) */
#include "game/camera/T383_camera_manager.h"      /* PC: CameraManager (car_sound_budget: the views, the eye) */
#include "game/race/core/T125_race_organization.h"    /* PC: RaceOrganization::entries (car_sound_budget) */
#include "game/car/physics/T302_car_state.h"      /* PC: car_view_x / y / z (car_sound_budget) */
#include "engine/sound/spu_memory.h"              /* PC: snd_trace_level / snd_trace_line */

/* The enhanced engine (Lupo 2's dispatcher L2 0x001A5318, its jump table
 * L2 0x00342DD0): the effect each emotion of the Pod plays (index the
 * emotion, CarSoundPodVoice::emotion); 0, neutral, plays nothing, and no
 * emotion plays pod_kowai2. */
#define POD_EMOTION_COUNT   11
static const s8 g_podEmotionEffect[POD_EMOTION_COUNT] = {
    -1,                                     /*  0 neutral */
    SE_POD_WINK,                            /*  1 */
    SE_POD_GENKI,                           /*  2 */
    SE_POD_KYORO,                           /*  3 */
    SE_POD_OKORU,                           /*  4 */
    SE_POD_ODOROKU,                         /*  5 */
    SE_POD_IYA,                             /*  6 */
    SE_POD_SABISHII,                        /*  7 */
    SE_POD_KOWAI1,                          /*  8 */
    SE_POD_NAKU,                            /*  9 */
    SE_POD_NEMUI,                           /* 10 */
};

/* The SPU RAM CarSound::reserveSpu takes for the two engine banks. */
#define CAR_SOUND_SPU_SIZE      0x25800
/* The SPU allocation pointer after car_sound_reset_spu's reset. */
#define CAR_SOUND_SPU_START     0xAC000
/* file_req_alloc's kind of a sound bank (T504). */
#define FILE_KIND_SOUND_BANK    1
/* A pan closer than this to the listener is moved back along z. */
#define PAN_MIN_DIST            0.25f
#define PAN_REF_DIST            32.0f       /* attenuation = PAN_REF_DIST / distance */
#define QUARTER_PI              0.78539819f
#define HALF_PI                 1.5707964f

/* Updates a pan from a position relative to the listener (inlined in
 * update, updateEngine and updateSlot); returns PAN_REF_DIST / distance.
 * The sqrt.s is a volatile asm (boot_sqrtf_v, types.h), as the original's:
 * retail has no errno call and keeps the code around it in place. */
static inline f32 pan_update(CarSoundPan *pan, const f32 *pos)
{
    f32 x = pos[0];
    f32 y = pos[1];
    f32 z = pos[2];
    f32 dist = boot_sqrtf_v(x * x + z * z);
    if (dist < PAN_MIN_DIST) {
        z = z - PAN_MIN_DIST + dist;
        dist = boot_sqrtf_v(x * x + z * z);
    }
    f32 zn = z / dist;
    f32 xn = x / dist;
    f32 atten = PAN_REF_DIST / dist;
    pan->y = y;
    pan->dist = dist;
    pan->z = zn;
    pan->x = xn;
    int flipL = pan->flipL != 0;
    int flipR = pan->flipR != 0;
    /* which side the sound is on, with hysteresis across the axes */
    if (zn < 0.0f) {
        if (0.0f <= xn)
            flipL = flipR;
        else
            flipR = flipL;
    } else {
        if (0.0f <= xn)
            flipL = flipR ^ 1;
        else
            flipR = flipL ^ 1;
    }
    pan->flipL = flipL;
    pan->flipR = flipR;
    return atten;
}

/* The constant-power gains of a pan (sinf, cosf): gain[0] / gain[1] get the
 * left / right gains (cos / sin of the angle, negated when the sound is
 * behind).  The two outputs go through pointer locals: retail keeps &gain[0]
 * and &gain[1] in saved registers across the two calls (an argument would
 * be folded into sp+4). */
static inline void pan_gains(CarSoundPan *pan, f32 *gain)
{
    f32 *l = &gain[0];
    f32 *r = &gain[1];
    f32 angle = (pan->x + 1.0f) * QUARTER_PI;
    if (angle < 0.0f)
        angle = 0.0f;
    else if (HALF_PI <= angle)
        angle = HALF_PI;
    f32 s = sinf(angle);
    f32 c = cosf(angle);
    if (pan->flipL)
        c = -c;
    *l = c;
    if (pan->flipR)
        s = -s;
    *r = s;
}

/* max.s: a volatile asm statement, as every max.s / min.s of the image
 * behaves (u018, u032, u044).  GCC 14: the C ternary (the same but for -0.0
 * against 0.0). */
static inline f32 car_sound_maxf(f32 a, f32 b)
{
    return a > b ? a : b;
}

/* The header of a bank as the engine bank engine_setup plays (an inline
 * accessor: retail keeps &bank in its own register and loads header from
 * it, which a direct field access folds into one address). */
static inline EngnBank *engine_bank_header(GTSOUNDINSTRUMENT *b)
{
    return (EngnBank *)b->header;
}

/* ------------------------------------------------------------------------ */
/* Data                                                                     */
/* ------------------------------------------------------------------------ */

/* .sdata (-G8): written in definition order, the kind names (8 bytes each,
 * small) just before the table that points at them. */
f32 g_003521E0 = 1.0f;                      /* 0x003521E0 */
f32 g_003521E4 = 1.0f;                      /* 0x003521E4 */
int g_003521E8 = 0;                         /* 0x003521E8 (explicit: .sdata, not the end of the file) */
const char *g_002C2088[8] = {               /* 0x002C2088 */
    "normal0", "normal1", "normal2", "normal3",
    "turbo0", "turbo1", "turbo2", "turbo3",
};

/* ------------------------------------------------------------------------ */
/* EngineSound                                                              */
/* ------------------------------------------------------------------------ */

// FUNC 0x001A6488 EngineSound::EngineSound
//   GTSOUNDINSTRUMENT(); the vptr; the header buffer of its own (fixed = 1,
//   header = &buf, headerCapacity 0x100); link = NULL.
EngineSound::EngineSound()
{
    /* implicit: GTSOUNDINSTRUMENT (base constructor) */
    fixed = 1;
    header = (SoundBankHeader *)&buf;
    headerCapacity = sizeof(buf);
    link = 0;
}

// FUNC 0x001A64D8 EngineSound::~EngineSound
//   The vptr, then ~GTSOUNDINSTRUMENT with the flags (it deletes).
EngineSound::~EngineSound()
{
    /* implicit: ~GTSOUNDINSTRUMENT (base destructor) */
    if (header != (SoundBankHeader *)&buf)  /* PC: setHeaderRoom's buffer (or a file's transfer kept) */
        free(header);
}

/* PC: not the game's (T176_car_sound.h).  A new buffer is rounded up to
 * 0x100 bytes, so that a slightly larger header later fits it. */
int EngineSound::setHeaderRoom(int size)
{
    void *p;

    if (size <= headerCapacity)
        return 1;
    size = (size + 0xFF) & ~0xFF;
    p = malloc(size);
    if (p == NULL)
        return 0;
    if (header != (SoundBankHeader *)&buf)
        free(header);
    header = (SoundBankHeader *)p;
    headerCapacity = size;
    return 1;
}

// FUNC 0x001A6500 EngineSound::onTransfer
//   At the end of GTSOUNDINSTRUMENT::transfer: the linked bank goes right
//   after this one in SPU RAM and has that much less room.
void EngineSound::onTransfer()
{
    GTSOUNDINSTRUMENT *next = link;
    if (next != 0) {
        next->spuAddr = spuAddr + spuSize;
        next->spuRoom = next->spuRoom - spuSize;
    }
}

// FUNC 0x001A6538 EngineSound::copyHeader
//   memcpy(buf, src->header, src->headerSize); returns this.
EngineSound *EngineSound::copyHeader(const GTSOUNDINSTRUMENT *src)
{
    /* PC: into header (buf, or one of the header's size: a larger rn_01.es
     * from another build overran buf); no source header or no memory: not
     * copied */
    if (src->header != NULL && setHeaderRoom(src->headerSize))
        memcpy(header, src->header, src->headerSize);
    return this;
}

// FUNC 0x001A6570 EngineSound::operator=
//   The copy assignment: only the 0x100-byte buffer (a struct copy: a
//   doubleword loop, or an unaligned doubleword one); the base's words and
//   link stay the target's.  Caller: CarSound's compiler-generated
//   operator= (T121, 0x00179230), for each engine.
EngineSound &EngineSound::operator=(const EngineSound &o)
{
    buf = o.buf;
    /* PC: when either header is not in its buf (setHeaderRoom), the
     * source's whole header into this one's, as the buf copy did */
    if (this != &o && (header != (SoundBankHeader *)&buf || o.header != (const SoundBankHeader *)&o.buf)) {
        int n = o.header == (const SoundBankHeader *)&o.buf ? (int)sizeof(o.buf) : o.headerCapacity;
        if (setHeaderRoom(n))
            memcpy(header, o.header, n);
    }
    return *this;
}

/* ------------------------------------------------------------------------ */
/* CarSound                                                                 */
/* ------------------------------------------------------------------------ */

// FUNC 0x001A6628 car_sound_reset_spu
//   RaceBase::virtual_8: resets the sound allocators (no header buffer, the
//   SPU allocation pointer at 0xAC000).
void car_sound_reset_spu(void)
{
    sound_set_header_buffer(0, 0);
    sound_set_spu_addr(CAR_SOUND_SPU_START);
}

// FUNC 0x001A6658 CarSoundPan::reset
void CarSoundPan::reset()
{
    flipL = 0;
    flipR = 0;
    x = y = z = dist = 0.0f;
}

// FUNC 0x001A6678 car_sound_update_se_switch
//   The sound effects switch: g_003521E0 = 1 when the option for races
//   (replay == 0) / replays is on, else 0 (RaceBase::virtual_29,
//   RacePS2Base::virtual_8).  One conditional expression: retail jumps from
//   each option's test straight to the 1.0 / 0.0 arms.
//   PC: and the player's volume of them (pc_race_se_volume) into
//   g_pcRaceSeLevel, read each frame (RaceBase::virtual_29): a change is
//   heard at once.  (PC 2026-11-11: the switch is 1 / 0 again, the level
//   applied after the volumes' clamp, pc_volume.h; it scaled them ahead of
//   it, and the clashes stayed at full volume.)
void car_sound_update_se_switch(int replay)
{
    g_003521E0 = (replay ? getGameStatus()->getGameOptions()->replaySe
                         : getGameStatus()->getGameOptions()->raceSe) ? 1.0f : 0.0f;
    g_pcRaceSeLevel = pc_race_se_volume(replay);
}

// FUNC 0x001A66F0 CarSound::CarSound
//   Members: engines[3] (GtSoundVoice(), EngineSound()), slots[32]
//   (GtSoundHandle()).  Then the effects of the slots (program A): 0..3 the
//   car's, then the four wheels' groups of seven (slots 4..31).  The EH
//   landing pads destroy the engines built so far (~EngineSound, flags 2).
CarSound::CarSound()
{
    /* implicit: GtSoundVoice, EngineSound, GtSoundHandle (member constructors), ~EngineSound (exception cleanup) */
    slots[1].handle.set(se_program(SE_TURBINE, 0));
    slots[0].handle.set(se_program(SE_TURBINE2, 0));
    slots[2].handle.set(se_program(SE_IN_CAR, 0));
    slots[3].handle.set(se_program(SE_BODY_NOIZ, 0));
    int first = 4;
    for (int wheel = 3; wheel >= 0; wheel--) {
        slots[first + 0].handle.set(se_program(SE_OFF_SKILL, 0));
        slots[first + 1].handle.set(se_program(SE_ON_SKILL, 0));
        slots[first + 2].handle.set(se_program(SE_OFF_ROADNOIZ, 0));
        slots[first + 3].handle.set(se_program(SE_RUMBLE, 0));
        slots[first + 4].handle.set(se_program(SE_OFF_SKILL, 0));
        slots[first + 5].handle.set(se_program(SE_ON_ROAD, 0));
        slots[first + 6].handle.set(se_program(SE_RAIN_NOIZ, 0));
        first += 7;
    }
    playing = 0;
}

// FUNC 0x001A69A8 CarSound::~CarSound
//   Destroys engines[2..0].bank (~EngineSound, flags 2); egcs's
//   operator delete when asked.
CarSound::~CarSound()
{
    /* implicit: ~EngineSound (member destructors), operator delete (deleting destructor) */
    sound_spu_window_free(SOUND_SPU_WINDOW_CARS, this);     /* PC: load's area (car_sound_place_banks) */
}

// FUNC 0x001A6A30 CarSound::reserveSpu
//   spuBase = the SPU allocation pointer; reserves 0x25800 bytes after it.
//   PC (ROADMAP 35): only while the slot ends below the core 0 reverb
//   (SOUND_SPU_MUSIC_END): the 8 slots from CAR_SOUND_SPU_START.  A car past
//   them (the 9th of a race of up to 32) keeps the pointer where it is (no
//   slot: car_sound_place_banks puts its banks in the cars' window), so the
//   pointer no longer walks over the reverb, gt3sys and the windows above
//   2 MB.  The first 8 cars reserve as retail.
void CarSound::reserveSpu()
{
    int addr = g_00352B8C;
    spuBase = addr;
    if (addr + CAR_SOUND_SPU_SIZE <= SOUND_SPU_MUSIC_END)   /* PC: a slot that fits */
        sound_set_spu_addr(addr + CAR_SOUND_SPU_SIZE);
}

/* PC: not the game's.  An engine voice whose bank's header buffer moved
 * (setHeaderRoom) forgets the segments it had from the old one (a voice
 * with no set, as engine_setup leaves no layer playing) until
 * CarSound::start sets it up again. */
static void car_sound_voice_detach(GtSoundVoice *v)
{
    engine_stop(v);
    v->segs = NULL;
    v->nsegs = 0;
    for (int i = 0; i < 2; i++) {
        v->layer[i].voice = -1;
        v->layer[i].seg = -1;
        v->layer[i].prev = -1;
    }
}

/* PC: not the game's.  What CarSound::load queues and where it goes, from
 * the two engine banks' files (sound_bank_sizes, not waited for: a missing
 * or broken file is not queued and its bank gets an empty set, silent,
 * where the file layer would wait for it forever).  Each header buffer
 * holds its header (setHeaderRoom; the voice detached when the buffer
 * moved).  The samples go to the PS2's slot (spuBase, CAR_SOUND_SPU_SIZE)
 * when the two fit and the slot ends below the core 0 reverb
 * (SOUND_SPU_MUSIC_END: 8 cars), else to an area of the cars' window above
 * 2 MB, the car's own (logged).  Bank 1's address is set here (bank 0's
 * onTransfer sets the same once it is in), so the order the file queue
 * loads them in does not matter.  Returns bit i: queue bank i.
 * PC (ROADMAP 35): with the window full, no bank goes over another area
 * (retail's place, the fallback before, ran into the next car's slot, or
 * for the 9th car on into the reverb, gt3sys and the windows): the banks
 * that fit the car's PS2 slot go there, in order; a bank that does not
 * fit, and both of a car with no slot (reserveSpu), are silent (logged). */
static int car_sound_place_banks(CarSound *cs)
{
    const char *path[2] = { cs->path0, cs->path1 };
    SoundBankSizes sz[2];
    int base = cs->spuBase;
    int room = CAR_SOUND_SPU_SIZE;
    int need = 0, queue = 0;
    int i;

    sound_spu_window_free(SOUND_SPU_WINDOW_CARS, cs);       /* a reload: the old area */
    for (i = 0; i < 2; i++) {
        EngineSound *bank = &cs->engines[i].bank;
        SoundBankHeader *old = bank->header;
        int err = sound_bank_sizes(path[i], &sz[i], 0);

        if (err != SOUND_BANK_OK) {
            printf("snd: %s %s: that engine sound is silent\n", path[i],
                   err == SOUND_BANK_MISSING ? "not found" : "is not an ENGN sound bank");
            sz[i].header = sz[i].samples = 0;
            memset(bank->header, 0, sizeof(EngnBank));      /* no set: engine_setup gives no segments */
            bank->headerSize = 0;
            bank->spuSize = 0;
            continue;
        }
        if (!bank->setHeaderRoom(sz[i].header))
            printf("snd: %s: no memory for its %d-byte header\n", path[i], sz[i].header);
        if (bank->header != old)
            car_sound_voice_detach(&cs->engines[i].voice);
        need += sz[i].samples;
        queue |= 1 << i;
    }
    if (queue != 0 && (need > CAR_SOUND_SPU_SIZE || base + CAR_SOUND_SPU_SIZE > SOUND_SPU_MUSIC_END)) {
        int high = sound_spu_window_alloc(SOUND_SPU_WINDOW_CARS, need, cs);

        if (high >= 0) {
            printf("snd: %s + %s: %d bytes of samples, %s: at SPU 0x%07X (the cars' window)\n", cs->path0,
                   cs->path1, need, need > CAR_SOUND_SPU_SIZE ? "more than the PS2's 0x25800" :
                   "the PS2 slot runs into the reverb (the 9th car on)", high);
            base = high;
            room = need;
            cs->spuBase = high;
        } else {
            /* PC (ROADMAP 35): the slot's room only (0: no slot), the banks
             * that do not fit it silent, as a missing file's */
            int slot = (base + CAR_SOUND_SPU_SIZE <= SOUND_SPU_MUSIC_END) ? CAR_SOUND_SPU_SIZE : 0;
            int used = 0;

            printf("snd: %s + %s: %d bytes of samples, the cars' window is full: %s\n", cs->path0, cs->path1,
                   need, slot != 0 ? "what fits the car's PS2 slot" : "the car has no PS2 slot (the 9th car on)");
            for (i = 0; i < 2; i++) {
                EngineSound *bank = &cs->engines[i].bank;

                if (!(queue & (1 << i)))
                    continue;
                if (used + sz[i].samples <= slot) {
                    used += sz[i].samples;
                    continue;
                }
                printf("snd: %s: no room for its %d bytes of samples: that engine sound is silent\n", path[i],
                       sz[i].samples);
                car_sound_voice_detach(&cs->engines[i].voice);
                sz[i].header = sz[i].samples = 0;
                memset(bank->header, 0, sizeof(EngnBank));  /* no set: engine_setup gives no segments */
                bank->headerSize = 0;
                bank->spuSize = 0;
                queue &= ~(1 << i);
            }
            room = slot;
        }
    }
    cs->engines[0].bank.spuAddr = base;
    cs->engines[0].bank.spuRoom = room;
    cs->engines[1].bank.spuAddr = base + sz[0].samples;
    cs->engines[1].bank.spuRoom = room;
    return queue;
}

// FUNC 0x001A6A60 CarSound::load
//   The two engine banks share the reserved SPU area (bank 0 links bank 1
//   after itself): "sound/engine/%05d" and "sound/<kind>/%05d" are loaded
//   asynchronously; bank 2 gets the header of the road noise bank.
//   PC: both paths first, then the banks placed and sized from their files
//   before they are queued (car_sound_place_banks).
void CarSound::load(int code, int kind)
{
    EngineSound *bank0 = &engines[0].bank;
    EngineSound *bank1 = &engines[1].bank;
    bank0->link = bank1;
    int base = spuBase;
    bank0->spuRoom = CAR_SOUND_SPU_SIZE;
    bank0->spuAddr = base;
    bank1->spuAddr = base;
    bank1->spuRoom = CAR_SOUND_SPU_SIZE;
    sprintf(path0, "sound/engine/%05d", code);
    sprintf(path1, "sound/%s/%05d", g_002C2088[kind], code);
    int queue = car_sound_place_banks(this);
    if (queue & 1)
        file_req_alloc(path0, 0, 0, (FileDoneFn)sound_load_es_cb, bank0, FILE_KIND_SOUND_BANK);
    if (queue & 2)
        file_req_alloc(path1, 0, 0, (FileDoneFn)sound_load_es_cb, bank1, FILE_KIND_SOUND_BANK);
    engines[2].bank.copyHeader(&g_roadNoiseBank[0]);
}

/* The effects of the four one-shot sounds (CarSoundFrame::oneShot[i]):
 * "b_off", "clash", "clash2", "clash3".  .rodata after load's formats and
 * before the vtable: an extern const (header) is written where it is
 * defined. */
const int g_00330910[4] = { SE_B_OFF, SE_CLASH, SE_CLASH2, SE_CLASH3 };   /* 0x00330910 */

// FUNC 0x001A6B80 CarSound::start
//   Unless playing: the pans reset, the engine voices started on their
//   banks (engine_setup, set 0, priority 0), field_7CC = 0.  One counter for
//   both loops (retail's s1; the first loop is reversed into a count-down).
//   The enhanced engine: the Pod voice's pan reset and its channel without a
//   program (Lupo 2 L2 0x001A4844..0x001A4854).
void CarSound::start()
{
    int i;
    if (playing == 0) {
        playing = 1;
        for (i = 0; i < 3; i++) {
            engines[i].pan.reset();
            engine_setup(&engines[i].voice, engine_bank_header(&engines[i].bank), 0, 0);
        }
        for (i = 0; i < 32; i++)
            slots[i].pan.reset();
        podPan.reset();
        podVoice.set(NULL);
        field_7CC = 0;
    }
}

// FUNC 0x001A6C38 CarSound::update
//   The engines, the slots, then the car's own pan (pan_update inlined:
//   sinf, cosf) and its four one-shot effects (g_00330910: "b_off",
//   "clash".."clash3"; only the first one is panned, the others play at the
//   louder gain on both sides) when loud enough.  The clashes are a switch
//   (cases 1..3): retail tests i < 4 and i >= 1 (as pointer compares), the
//   range test of a switch; `if (i >= 1)' is one test.
//   The enhanced engine: the Pod's voice after the slots (Lupo 2 L2
//   0x001A48F4), when the caller hands its part of the frame (pod).
void CarSound::update(const CarSoundFrame *f, const CarSoundPodVoice *pod)
{
    PcSeLevelScope pcLevel(g_pcRaceSeLevel);  /* PC: the player's level, after the clamps (pc_volume.h) */
    for (int i = 0; i < 3; i++)
        updateEngine(i, f);
    for (int i = 0; i < 32; i++)
        updateSlot(i, f);
    if (pod != NULL)
        updatePodVoice(f, pod);

    f32 gain[2];
    f32 atten = pan_update(&pan, f->pos);
    pan_gains(&pan, gain);  /* implicit: sinf, cosf (inline pan_gains) */

    for (int i = 0; i < 4; i++) {
        f32 volume = f->oneShot[i];
        int loud = (SOUND_MIN_VOLUME <= volume) ? 1 : 0;
        f32 l = gain[0];
        f32 r = gain[1];
        switch (i) {                        /* the clashes: centred, at the louder gain */
        case 1:
        case 2:
        case 3:
            l = boot_fabsf(gain[0]);
            r = boot_fabsf(gain[1]);
            r = car_sound_maxf(l, r);
            l = r;
            break;
        }
        if (loud)
            playOneShot(g_00330910[i], volume, atten, l, r);
    }
}

// FUNC 0x001A6ED8 CarSound::playOneShot
//   Keys program A of effect `id' on a free voice, with no handle
//   (snd_play_voice(NULL, ...); `this' is unused): the volumes times the
//   program's (1 << 14 = unity), its pitch, ADSR, address and flags.
void CarSound::playOneShot(int id, f32 vol, f32 atten, f32 l, f32 r)
{
    f32 v = vol * atten * (g_003521E0 * g_003521E4);
    if (v == 0.0f || pc_se_silent())        /* PC: or the player's level 0 (pc_volume.h) */
        return;
    SoundProgram *prog = se_program(id, 0);
    int prio = sound_voice_priority(v);
    int priority;
    if (prio >= 0x100)
        priority = 0xFF;
    else
        priority = prio;
    /* The parameter block with its word view: a union, whose members egcs
     * gives alias set 0, so every program load stays after the previous
     * store, as in retail (a plain SndVoiceParam lets the loads move up). */
    union {
        SndVoiceParam p;
        u32           w[5];
    } u;
    s16 *volume = u.p.vol.lr;
    /* PC: the player's level after the clamps (pc_volume.h): v is the
     * clash's level x 32 / distance, far beyond the SPU's full scale from the
     * car's own view, so a level applied ahead of the clamp was not heard */
    volume[0] = pc_se_out(spu_volume_clamp((spu_volume_clamp((int)(v * l * (f32)SPU_VOLUME_UNITY)) * prog->volume) >> 14));
    volume[1] = pc_se_out(spu_volume_clamp((spu_volume_clamp((int)(v * r * (f32)SPU_VOLUME_UNITY)) * prog->volume) >> 14));
    u.p.pitch = snd_note_to_pitch(prog->note << 8, prog->basePitch);
    u.p.unk_10 = prog->unk_0A;
    u.p.adsr = prog->adsr;
    u.p.addr = prog->addr;
    u.p.priority = priority;
    u.p.flags = prog->flags;
    snd_play_voice(0, &u.p);
}

// FUNC 0x001A70C0 CarSound::stop
//   When playing: the engine voices stopped (engine_stop), the slots too
//   (GtSoundHandle::stop with the fade).  The enhanced engine: the Pod's
//   voice too, with the fade (Lupo 2 L2 0x001A4D10): a pause, the race's
//   end, a camera without Doppler stop it with the car's other sounds.
void CarSound::stop()
{
    if (playing != 0) {
        playing = 0;
        for (int i = 0; i < 3; i++)
            engine_stop(&engines[i].voice);
        for (int i = 0; i < 32; i++)
            slots[i].handle.stop(1);
        podVoice.stop(1);
    }
}

// FUNC 0x001A7140 CarSound::updateEngine
//   Volume, pan (pan_update inlined: sinf, cosf), pitch (pitch x 4096,
//   0..0x3FFF) and rpm (param) of engine voice i, then its priority
//   (engine_set_priority) and the voice's update (engine_update).
void CarSound::updateEngine(int i, const CarSoundFrame *f)
{
    const CarSoundVoiceEx *src = &f->engine[i];
    CarSoundEngine *eng = &engines[i];
    GtSoundVoice *voice = &eng->voice;
    f32 gain[2];
    f32 atten = pan_update(&eng->pan, src->pos);
    pan_gains(&eng->pan, gain);  /* implicit: sinf, cosf (inline pan_gains) */

    f32 a = atten * f->scale;
    f32 k;
    if (a < -1.0f)
        k = -1.0f;
    else if (1.0f < a)
        k = 1.0f;
    else
        k = a;
    f32 master = g_003521E0 * g_003521E4;
    f32 volume = k * src->volume * master;
    int prio = sound_voice_priority(volume);
    int priority;
    if (prio >= 0x100)
        priority = 0xFF;
    else
        priority = prio;
    /* The volumes go through the halfword-pair view (SndWordView, G002): a
     * union access, which egcs gives alias set 0 and keeps in place, as
     * retail stores each volume right after its clamp (plain s16 stores
     * sink below the next computation).  The original's GtSoundVoice most
     * likely held them as a SndVolPair; the canonical header keeps two s16
     * for u123's engine code. */
    /* PC: the player's level after the clamp (pc_volume.h) */
    ((SndWordView *)&voice->volL)->lr[0] = pc_se_out(spu_volume_clamp((int)(gain[0] * volume * (f32)SPU_VOLUME_UNITY)));
    ((SndWordView *)&voice->volL)->lr[1] = pc_se_out(spu_volume_clamp((int)(gain[1] * volume * (f32)SPU_VOLUME_UNITY)));
    int t = (int)(src->pitch * 4096.0f);
    int pitch;
    if (t < 0)
        pitch = 0;
    else if (t >= 0x4000)
        pitch = 0x3FFF;
    else
        pitch = t;
    voice->pitch = pitch;
    voice->rpm = (int)src->param;
    engine_set_priority(voice, priority & 0xFF);
    engine_update(voice);
}

// FUNC 0x001A74A0 CarSound::updateSlot
//   Volume and pan of slot i (pan_update inlined: sinf, cosf);
//   GtSoundHandle::play plays / updates its effect.
void CarSound::updateSlot(int i, const CarSoundFrame *f)
{
    const CarSoundVoice *src = &f->voice[i];
    CarSoundSlot *slot = &slots[i];
    GtSoundHandle *h = &slot->handle;
    f32 gain[2];
    f32 atten = pan_update(&slot->pan, src->pos);
    pan_gains(&slot->pan, gain);  /* implicit: sinf, cosf (inline pan_gains) */

    f32 a = atten * f->scale;
    f32 k;
    if (a < -1.0f)
        k = -1.0f;
    else if (1.0f < a)
        k = 1.0f;
    else
        k = a;
    f32 master = g_003521E0 * g_003521E4;
    f32 volume = k * src->volume * master;
    int prio = sound_voice_priority(volume);
    gain[0] = gain[0] * volume;
    gain[1] = gain[1] * volume;
    g_sndTraceContext.slot = i;             /* PC: the sound trace's context (T468) */
    h->play(prio < 0x100 ? prio : 0xFF, gain[0], gain[1], src->pitch);
    g_sndTraceContext.slot = -1;
}

/* The enhanced engine (Lupo 2 L2 0x001A5318, cameras_sound.md S1; ? name):
 * the Pod's voice for this frame, from CarSound::update.  Its own pan from
 * the car's position (the frame's, even where the engine voices sound at
 * the listener: in-car views) and the level of the engine voices (32 /
 * distance x the frame's scale, within -1..1, x pod->volume x the effect
 * factors), with the priority of that level.  A new emotion releases the
 * voice still sounding and binds its pod_* effect: no program when the Pod
 * bank or the se.inf entry is missing (se_find_program), so the voice stays
 * silent.  Then the channel plays: keyed on once when just bound, else its
 * priority, pitch (pod->pitch, the Doppler factor, Lupo 2's +0x380) and
 * volumes follow the car; at level 0 it stops. */
void CarSound::updatePodVoice(const CarSoundFrame *f, const CarSoundPodVoice *pod)
{
    f32 gain[2];
    f32 atten = pan_update(&podPan, f->pos);
    pan_gains(&podPan, gain);  /* implicit: sinf, cosf (inline pan_gains) */

    f32 a = atten * f->scale;
    f32 k;
    if (a < -1.0f)
        k = -1.0f;
    else if (1.0f < a)
        k = 1.0f;
    else
        k = a;
    f32 volume = k * pod->volume * (g_003521E0 * g_003521E4);
    int prio = sound_voice_priority(volume);
    int priority;
    if (prio >= 0x100)
        priority = 0xFF;
    else
        priority = prio;
    gain[0] = gain[0] * volume;
    gain[1] = gain[1] * volume;
    if ((u32)pod->emotion < POD_EMOTION_COUNT) {
        int se = g_podEmotionEffect[pod->emotion];
        if (se >= 0)
            podVoice.bind(se_find_program(se, SE_BANK_POD));
    }
    podVoice.play(priority, gain[0], gain[1], pod->pitch);
}

// FUNC 0x001A7750 race_se_play
//   se_play(se, centre, the program's priority, the volume factors) unless
//   g_003521E8 (the background mode) is set.
void race_se_play(int se)
{
    PcSeLevelScope pcLevel(g_pcRaceSeLevel);  /* PC: the player's level, after the clamp (pc_volume.h) */
    if (g_003521E8 == 0)
        se_play(se, 0x40, -1, g_003521E0 * g_003521E4);
}

// FUNC 0x001A7788 EngineSound::setLink
void EngineSound::setLink(GTSOUNDINSTRUMENT *next)
{
    link = next;
}

/* ------------------------------------------------------------------------ */
/* PC (ROADMAP 35, not the game's): the car sound budget                     */
/* ------------------------------------------------------------------------ */

/* GT3's races hold 6 cars.  Each keeps three engine voices of one or two
 * layers, its slots (the whines, the cabin, seven per wheel) and the Pod's
 * voice: a race of up to PC_RACE_CARS_MAX cars asks for more voices than the
 * mixer's 126 (engine/sound/native), and snd_voice_alloc steals by priority
 * every frame - the engines of the cars would take each other's voices.  So
 * in a race of more than CAR_SOUND_BUDGET cars only that many sound:
 *   - the car each view follows (CameraManager::views[v].targetInfo.index[0],
 *     v < viewCount: the camera's target) and the player's, car 0;
 *   - then the cars nearest the listener: the current view's eye
 *     (CameraManager::getPosition*, the debug camera's under it), measured to
 *     each car's position as updateSound places it (car_view_*, t 0); a car
 *     sounding now counts 0.8 x its distance (CAR_SOUND_KEEP_SQ, a
 *     hysteresis: two cars at about the same distance do not swap every
 *     frame, which would restart their engines); equal ones by number.
 * The set is chosen once a frame, at car 0's call (RacePS2Base::virtual_29
 * updates the cars' sounds in order, car 0 first).  A car out of it is
 * stopped (CarSound::stop: its engine voices keyed off and released, its
 * slots and the Pod's voice faded out) and its frame is not built, so it
 * takes no voice; one back in is started again (CarSound::start: the engine
 * voices set up, they key on in the frame's update).  With CAR_SOUND_BUDGET
 * cars or fewer nothing is done: as retail. */
enum { CAR_SOUND_BUDGET = 8 };
#define CAR_SOUND_KEEP_SQ   0.64f           /* a sounding car's squared distance x 0.8^2 */
#define CAR_SOUND_FAR       3.0e38f         /* a car without a state: last */

static u32 s_carSoundSet;                   /* bit n: car n sounds this frame */
static const void *s_carSoundLogged;        /* the entries the budget was logged for */

static_assert(PC_RACE_CARS_MAX <= 32, "PC: the budget's set is a 32-bit mask");

/* The set of the cars that sound this frame (bit n: car n). */
static u32 car_sound_pick(RaceEntryBase *entries, CameraManager *cm)
{
    f32 dist[PC_RACE_CARS_MAX];
    int count = entries->count;
    f32 ex = cm->getPositionX();
    f32 ey = cm->getPositionY();
    f32 ez = cm->getPositionZ();
    u32 set = 0;
    int left = CAR_SOUND_BUDGET;
    int i;

    if (count > PC_RACE_CARS_MAX)
        count = PC_RACE_CARS_MAX;
    /* the cars the views follow, then the player's */
    for (i = 0; i < cm->viewCount && i < 4; i++) {
        int c = cm->views[i].targetInfo.index[0];
        if (c >= 0 && c < count && (set & (1u << c)) == 0) {
            set |= 1u << c;
            left--;
        }
    }
    if ((set & 1u) == 0) {
        set |= 1u;
        left--;
    }
    /* the others by their distance to the listener */
    for (i = 0; i < count; i++) {
        RaceEntryCar *car = entries->cars[i];
        RaceCarState *st = car != NULL ? car->state : NULL;
        f32 d = CAR_SOUND_FAR;

        if (st != NULL) {
            f32 dx = car_view_x(st, 0.0f) - ex;
            f32 dy = car_view_y(st, 0.0f) - ey;
            f32 dz = car_view_z(st, 0.0f) - ez;
            d = dx * dx + dy * dy + dz * dz;
            if (car->sound.playing != 0)
                d = d * CAR_SOUND_KEEP_SQ;
        }
        dist[i] = d;
    }
    while (left > 0) {
        int best = -1;
        for (i = 0; i < count; i++) {
            if ((set & (1u << i)) == 0 && (best < 0 || dist[i] < dist[best]))
                best = i;
        }
        if (best < 0)
            break;
        set |= 1u << best;
        left--;
    }
    return set;
}

/* From RaceEntryCar::updateSound (T121), before the car's frame is built: 1
 * when the car sounds this frame (started again if the budget had stopped
 * it), 0 when it is silent (stopped; the caller builds no frame). */
int car_sound_budget(RaceEntryCar *car, CameraManager *cm)
{
    RaceOrganization *org = cm->views[0].targetInfo.org;
    RaceEntryBase *entries = org != NULL ? org->entries : NULL;
    int n = car->number;

    if (entries == NULL || entries->count <= CAR_SOUND_BUDGET)
        return 1;                           /* up to 8 cars: as retail */
    if (n < 0 || n >= PC_RACE_CARS_MAX)
        return 1;
    if (n == 0) {
        s_carSoundSet = car_sound_pick(entries, cm);
        if (s_carSoundLogged != entries) {
            s_carSoundLogged = entries;
            printf("snd: %d cars: the car sound budget - %d of them sound (the followed and the player's, then "
                   "the nearest)\n", entries->count, CAR_SOUND_BUDGET);
        }
    }
    if (s_carSoundSet & (1u << n)) {
        if (car->sound.playing == 0 && snd_trace_level() >= 2)
            snd_trace_line("EE car %d: in the car sound budget again (started)", n);
        car->sound.start();                 /* (nothing when it plays) */
        return 1;
    }
    if (car->sound.playing != 0 && snd_trace_level() >= 2)
        snd_trace_line("EE car %d: out of the car sound budget (stopped)", n);
    car->sound.stop();                      /* (nothing when it is stopped) */
    return 0;
}

