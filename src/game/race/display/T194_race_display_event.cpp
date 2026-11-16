/* T194_race_display_event.cpp - RaceDisplayEventBase and the race display
 * events (0x00190198..0x001909E0); the classes are declared in
 * include/game/race/display/T194_race_display_event.h.
 *
 * A display event is a small object that travels through the race event
 * queue (RaceOrganization::eventQueue) as (type, one 32-bit word): post()
 * queues getEventType() / pack() with a delay of 2 frames, take() removes
 * the first event of the type and unpacks it, peek() reads the nth one and,
 * when the object is bound to a car (car >= 0), accepts it only if it is
 * for that car.
 *
 * Word layouts (bits 0..3 always the car, clamped to 0..15):
 *   lap time    (type 0xB):  lap 8..17 (0..999)
 *   check point (type 0xC):  check point 4..7 (0..15), lap 8..17
 *   difference  (abstract):  a signed 28-bit value 4..31 (+-0x7FFFFFF);
 *                            setTime / getTime map times above 0xFFFFFFE
 *                            to 0xFFFFFFF / RACE_TIME_INVALID
 *   speed       (type 0x11): speed * 10 in 4..17 (0..9999)
 *   information (type 0x12): an information code 4..7
 * pack() sets the bit fields of an uninitialised word: the other bits are
 * whatever the register held (the original's quirk, kept).
 *
 * PC (ROADMAP 35): the races of up to 32 cars (game/race/race_cars.h): the
 * car is 5 bits, 0..4, clamped to 0..31 (EVENT_CAR_MAX), and the fields
 * after it one bit higher:
 *   lap time    (type 0xB):  lap 8..17 (bits 5..7 unused, as 4..7 were)
 *   check point (type 0xC):  check point 5..8, lap 9..18
 *   difference  (abstract):  a signed 27-bit value 5..31 (+-0x3FFFFFF);
 *                            setTime / getTime at 0x7FFFFFE / 0x7FFFFFF
 *                            (race_championship_internal.h: the HUD shows
 *                            every time as before)
 *   speed       (type 0x11): speed * 10 in 5..18
 *   information (type 0x12): an information code 5..8
 * Every poster goes through pack() (T154, T303, T315, T350 and the enhanced
 * engine's build the event objects; nobody packs a word itself) and the
 * only reader is unpack() (peek / take).
 *
 * Data (u050's data pass): .rodata (0x0032DDA0..0x0032DF89) is "bad_alloc"
 * (<new>), the six vtables, then the type names in reverse definition
 * order: the events of game/race/display/T194_race_display_event.h (T154's three
 * difference events included: the original header defined them) and
 * UnitArena<64> last (T153 read first, with its name).  No .sdata.
 */
// DATA_START 0x0032DDA0
#include "types.h"
#include <new>                              /* "bad_alloc" (retail's T194 has it first) */
#define GAME_T153_EMITS_TYPEINFO            /* UnitArena<64>'s type name, last (T153_race_course.h) */
#include "game/race/core/T153_race_course.h"          /* (UnitArena<64>: retail's type names) */
#define GAME_T194_EMITS_TYPEINFO            /* egcs generates __tf20RaceDisplayEventBase here */
#include "game/race/display/T194_race_display_event.h"   /* own */
#include "game/race/core/T122_race_event_queue.h"     /* RaceEventQueue::find / post / findNth */
#include "game/race/core/T125_race_organization.h"    /* RaceOrganization (eventQueue) */
#include "game/race/core/T128_race_lap_record.h"      /* RACE_TIME_INVALID */
#include "game/race/core/race_championship_internal.h"                           /* the unit's constants */
#include "game/race/race_cars.h"                /* PC: PC_RACE_CARS_MAX (the car field's width) */

/* lo <= v <= hi (the original's clamp: < lo first, then > hi) */
static inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* The event words, as bit fields (bit 0 first).  PC (ROADMAP 35): the car
 * field is EVENT_CAR_BITS wide (GT3's 4), the fields after it move up. */
enum { EVENT_CAR_BITS = 5 };                /* PC: cars 0..31 (was 4: 0..15) */
static_assert((1 << EVENT_CAR_BITS) - 1 == EVENT_CAR_MAX, "PC: the car field holds EVENT_CAR_MAX");
static_assert(EVENT_CAR_MAX + 1 >= PC_RACE_CARS_MAX, "PC: the car field holds every car of a race");
union LapTimeBits {                         /* RaceDisplayLapTimeEvent */
    int word;
    struct {
        u32 car : EVENT_CAR_BITS;           /* PC: was 4 */
        u32     : 8 - EVENT_CAR_BITS;       /* PC: was 4 (lap stays at bit 8) */
        u32 lap : 10;
    } f;
};
union CheckPointBits {                      /* RaceDisplayCheckPointEvent */
    int word;
    struct {
        u32 car : EVENT_CAR_BITS;           /* PC: was 4 */
        u32 checkPoint : 4;
        u32 lap : 10;
    } f;
};
union DiffBits {                            /* RaceDisplayDiffEvent */
    int word;
    struct {
        u32 car : EVENT_CAR_BITS;           /* PC: was 4 */
        s32 diff : 32 - EVENT_CAR_BITS;     /* PC: 27 bits, was 28 (EVENT_DIFF_MAX) */
    } f;
};
union SpeedBits {                           /* RaceDisplaySpeedEvent */
    int word;
    struct {
        u32 car : EVENT_CAR_BITS;           /* PC: was 4 */
        u32 speed : 14;                     /* tenths */
    } f;
};
union InformationBits {                     /* RaceDisplayInformationEvent */
    int word;
    struct {
        u32 car : EVENT_CAR_BITS;           /* PC: was 4 */
        u32 information : 4;
    } f;
};
/* PC: the words still fit 32 bits, the difference's 27 signed bits hold
 * EVENT_DIFF_MAX */
static_assert(sizeof(CheckPointBits) == 4 && sizeof(DiffBits) == 4 && sizeof(SpeedBits) == 4,
              "PC: one word per event");
static_assert(EVENT_DIFF_MAX == (1 << (31 - EVENT_CAR_BITS)) - 1, "PC: EVENT_DIFF_MAX fills the difference");
static_assert(EVENT_TIME_SATURATED == 2 * EVENT_DIFF_MAX + 1 && EVENT_TIME_MAX == EVENT_TIME_SATURATED - 1,
              "PC: the time limits follow the difference field");

/* ======================================================================== */
/* RaceDisplayEventBase                                                     */
/* ======================================================================== */

// FUNC 0x00190198 RaceDisplayEventBase::RaceDisplayEventBase
//   car = -1 (any), the vptr.  Callers: the 4 event constructors.
RaceDisplayEventBase::RaceDisplayEventBase()
{
    car = -1;
}

// FUNC 0x001901B8 RaceDisplayEventBase::post
//   Queues (getEventType(), pack()) with a delay of 2 frames
//   (RaceEventQueue::post).  14 callers (RaceDisplay::virtual_0, the T303 /
//   T315 / T350 event posters, ...).
void RaceDisplayEventBase::post(RaceOrganization *org)
{
    int type = getEventType();
    int bits = pack();
    org->eventQueue.post(type, bits, DISPLAY_EVENT_DELAY);
}

// FUNC 0x00190230 RaceDisplayEventBase::take
//   No callers.  Removes the first event of the type (RaceEventQueue::find
//   with take = 1) and unpacks it: 1, or 0 if there is none.
int RaceDisplayEventBase::take(RaceOrganization *org)
{
    int bits;                               /* sp+0x00 */
    int type = getEventType();
    if (org->eventQueue.find(type, &bits, 1) != 0) {
        unpack(bits);
        return 1;
    }
    return 0;
}

// FUNC 0x001902B0 RaceDisplayEventBase::peek
//   The nth event of the type (RaceEventQueue::findNth), unpacked; when
//   this event was bound to a car (car >= 0) and the event is another
//   car's, the car is restored and 0 returned.  Callers: the RaceDisplay
//   pollers (T154: pollLapTimeEvents, pollCheckPointEvents,
//   pollLapDiffEvents, pollTimeDiffEvents, pollSpeedEvents,
//   pollInformationEvents).
int RaceDisplayEventBase::peek(RaceOrganization *org, int nth)
{
    int bits;                               /* sp+0x00 */
    int type = getEventType();
    if (org->eventQueue.findNth(type, &bits, nth) != 0) {
        int boundCar = car;
        unpack(bits);
        if (boundCar < 0 || car == boundCar)
            return 1;
        car = boundCar;
        return 0;
    }
    return 0;
}

/* ======================================================================== */
/* RaceDisplayLapTimeEvent                                                  */
/* ======================================================================== */

// FUNC 0x00190350 RaceDisplayLapTimeEvent::RaceDisplayLapTimeEvent
//   RaceDisplayEventBase::RaceDisplayEventBase, the vptr, lap 0.  Callers:
//   RaceDisplayCheckPointEvent's constructor, RaceDisplay::
//   pollLapTimeEvents (T154), dc_post_lap_time (T303).
RaceDisplayLapTimeEvent::RaceDisplayLapTimeEvent()
    : RaceDisplayEventBase()
{
    lap = 0;
}

// FUNC 0x00190388 RaceDisplayLapTimeEvent::pack
//   (index: virtual_1.)  car in 0..3, lap in 8..17; bits 4..7 and 18..31
//   uninitialised.  PC: car in 0..4 (0..31), bits 5..7 uninitialised.
int RaceDisplayLapTimeEvent::pack() const
{
    LapTimeBits bits;                       /* not initialised (quirk) */
    bits.f.car = clampi(car, 0, EVENT_CAR_MAX);
    bits.f.lap = clampi(lap, 0, EVENT_LAP_MAX);
    return bits.word;
}

// FUNC 0x001903F0 RaceDisplayLapTimeEvent::unpack
//   (index: virtual_2.)
void RaceDisplayLapTimeEvent::unpack(int word)
{
    LapTimeBits bits;
    bits.word = word;
    car = bits.f.car;
    lap = bits.f.lap;
}

/* ======================================================================== */
/* RaceDisplayCheckPointEvent                                               */
/* ======================================================================== */

// FUNC 0x00190408 RaceDisplayCheckPointEvent::RaceDisplayCheckPointEvent
//   RaceDisplayLapTimeEvent::RaceDisplayLapTimeEvent, the vptr, check
//   point 0.  Callers: RaceDisplay::pollCheckPointEvents (T154),
//   dc_post_section_time (T303).
RaceDisplayCheckPointEvent::RaceDisplayCheckPointEvent()
    : RaceDisplayLapTimeEvent()
{
    checkPoint = 0;
}

// FUNC 0x00190440 RaceDisplayCheckPointEvent::pack
//   (index: virtual_1.)  car in 0..3, the check point in 4..7, lap in
//   8..17; bits 18..31 uninitialised.  PC: car in 0..4 (0..31), the check
//   point in 5..8, lap in 9..18; bits 19..31 uninitialised.
int RaceDisplayCheckPointEvent::pack() const
{
    CheckPointBits bits;                    /* not initialised (quirk) */
    bits.f.car = clampi(car, 0, EVENT_CAR_MAX);
    bits.f.checkPoint = clampi(checkPoint, 0, EVENT_CHECK_POINT_MAX);
    bits.f.lap = clampi(lap, 0, EVENT_LAP_MAX);
    return bits.word;
}

// FUNC 0x001904D8 RaceDisplayCheckPointEvent::unpack
//   (index: virtual_2.)
void RaceDisplayCheckPointEvent::unpack(int word)
{
    CheckPointBits bits;
    bits.word = word;
    car = bits.f.car;
    checkPoint = bits.f.checkPoint;
    lap = bits.f.lap;
}

/* ======================================================================== */
/* RaceDisplayDiffEvent                                                     */
/* ======================================================================== */

// FUNC 0x00190500 RaceDisplayDiffEvent::RaceDisplayDiffEvent
//   RaceDisplayEventBase::RaceDisplayEventBase, the vptr, diff 0.  13
//   callers (the T154 difference events and their users, T303 / T315).
RaceDisplayDiffEvent::RaceDisplayDiffEvent()
    : RaceDisplayEventBase()
{
    diff = 0;
}

// FUNC 0x00190538 RaceDisplayDiffEvent::pack
//   (index: virtual_1; also the slot of RaceDisplayGeneralTimeEvent /
//   LapDiffEvent / TimeDiffEvent.)  car in 0..3, the value clamped to
//   +-0x7FFFFFF in 4..31 (here the whole word is set).  PC: car in 0..4
//   (0..31), the value clamped to +-0x3FFFFFF (EVENT_DIFF_MAX) in 5..31.
int RaceDisplayDiffEvent::pack() const
{
    DiffBits bits;                          /* not initialised (all bits set below) */
    bits.f.car = clampi(car, 0, EVENT_CAR_MAX);
    bits.f.diff = clampi(diff, -EVENT_DIFF_MAX, EVENT_DIFF_MAX);
    return bits.word;
}

// FUNC 0x001905B0 RaceDisplayDiffEvent::unpack
//   (index: virtual_2.)  The value sign-extended (sra 4; PC: sra 5).
void RaceDisplayDiffEvent::unpack(int word)
{
    DiffBits bits;
    bits.word = word;
    car = bits.f.car;
    diff = bits.f.diff;
}

// FUNC 0x001905C8 RaceDisplayDiffEvent::setTime
//   A time (unsigned), 0xFFFFFFF if above 0xFFFFFFE (the largest the word
//   can carry).  Callers: dc_post_section_time / dc_post_lap_time (T303).
//   PC: 0x7FFFFFF above 0x7FFFFFE (EVENT_TIME_*, the 27-bit field).
void RaceDisplayDiffEvent::setTime(u32 t)
{
    if (t > EVENT_TIME_MAX)
        t = EVENT_TIME_SATURATED;
    diff = t;
}

// FUNC 0x001905E8 RaceDisplayDiffEvent::getTime
//   The time; RACE_TIME_INVALID for a value above 0xFFFFFFE (unsigned; PC:
//   0x7FFFFFE).
//   Callers: RaceDisplay::pollLapTimeEvents / pollCheckPointEvents (T154).
u32 RaceDisplayDiffEvent::getTime()
{
    u32 t = diff;
    return (t > EVENT_TIME_MAX) ? RACE_TIME_INVALID : t;
}

/* ======================================================================== */
/* RaceDisplaySpeedEvent                                                    */
/* ======================================================================== */

// FUNC 0x00190608 RaceDisplaySpeedEvent::RaceDisplaySpeedEvent
//   RaceDisplayEventBase::RaceDisplayEventBase, the vptr, speed 0.
//   Callers: RaceDisplay::pollSpeedEvents (T154), dc_post_speed (T303).
RaceDisplaySpeedEvent::RaceDisplaySpeedEvent()
    : RaceDisplayEventBase()
{
    speed = 0.0f;
}

// FUNC 0x00190640 RaceDisplaySpeedEvent::pack
//   (index: virtual_1.)  car in 0..3, (int)(speed * 10) (truncated)
//   clamped to 0..9999 in 4..17; bits 18..31 uninitialised.  PC: car in
//   0..4 (0..31), the speed in 5..18, bits 19..31 uninitialised.
int RaceDisplaySpeedEvent::pack() const
{
    SpeedBits bits;                         /* not initialised (quirk) */
    bits.f.car = clampi(car, 0, EVENT_CAR_MAX);
    bits.f.speed = clampi((int)(speed * 10.0f), 0, EVENT_SPEED_MAX);
    return bits.word;
}

// FUNC 0x001906C0 RaceDisplaySpeedEvent::unpack
//   (index: virtual_2.)  The field converted as an unsigned value, times
//   0.1.
void RaceDisplaySpeedEvent::unpack(int word)
{
    SpeedBits bits;
    bits.word = word;
    car = bits.f.car;
    speed = (f32)(u32)bits.f.speed * 0.1f;
}

/* ======================================================================== */
/* RaceDisplayInformationEvent                                              */
/* ======================================================================== */

// FUNC 0x00190710 RaceDisplayInformationEvent::RaceDisplayInformationEvent
//   RaceDisplayEventBase::RaceDisplayEventBase, the vptr, information 0.
//   Callers: RaceDisplay::virtual_0 / pollInformationEvents (T154),
//   dc_post_pit_in / dc_post_pit_out (T350), DynamicsConductorFreePractice::
//   virtual_16 / virtual_17 (T315).
RaceDisplayInformationEvent::RaceDisplayInformationEvent()
    : RaceDisplayEventBase()
{
    information = 0;
}

// FUNC 0x00190748 RaceDisplayInformationEvent::pack
//   (index: virtual_1.)  car in 0..3, the low 4 bits of the information in
//   4..7 (not clamped); bits 8..31 uninitialised.  PC: car in 0..4
//   (0..31), the information in 5..8, bits 9..31 uninitialised.
int RaceDisplayInformationEvent::pack() const
{
    InformationBits bits;                   /* not initialised (quirk) */
    bits.f.car = clampi(car, 0, EVENT_CAR_MAX);
    bits.f.information = information;
    return bits.word;
}

// FUNC 0x00190798 RaceDisplayInformationEvent::unpack
//   (index: virtual_2.)
void RaceDisplayInformationEvent::unpack(int word)
{
    InformationBits bits;
    bits.word = word;
    car = bits.f.car;
    information = bits.f.information;
}

/* Inline in game/race/display/T194_race_display_event.h (declared in the reverse of
 * this order); egcs emits these copies at the end of the unit, before
 * the class's typeinfo function (this unit defines the class's key
 * function, pack). */
// FUNC 0x001907B0 RaceDisplayInformationEvent::getEventType
// FUNC 0x001907B8 RaceDisplayInformationEvent::getInformation
//   No callers.
// FUNC 0x001907C0 RaceDisplayInformationEvent::setInformation
//   No callers.


/* ======================================================================== */
/* RaceDisplaySpeedEvent: accessors                                         */
/* ======================================================================== */

/* Inline in game/race/display/T194_race_display_event.h (declared in the reverse of
 * this order); egcs emits these copies at the end of the unit, before
 * the class's typeinfo function (this unit defines the class's key
 * function, pack). */
// FUNC 0x00190818 RaceDisplaySpeedEvent::getEventType
// FUNC 0x00190820 RaceDisplaySpeedEvent::getSpeed
// FUNC 0x00190828 RaceDisplaySpeedEvent::setSpeed


/* ======================================================================== */
/* RaceDisplayCheckPointEvent: accessors                                    */
/* ======================================================================== */

/* Inline in game/race/display/T194_race_display_event.h (declared in the reverse of
 * this order); egcs emits these copies at the end of the unit, before
 * the class's typeinfo function (this unit defines the class's key
 * function, pack). */
// FUNC 0x001908D0 RaceDisplayCheckPointEvent::getEventType
// FUNC 0x001908D8 RaceDisplayCheckPointEvent::getCheckPoint
// FUNC 0x001908E0 RaceDisplayCheckPointEvent::setCheckPoint


/* ======================================================================== */
/* RaceDisplayLapTimeEvent: accessors                                       */
/* ======================================================================== */

/* Inline in game/race/display/T194_race_display_event.h (declared in the reverse of
 * this order); egcs emits these copies at the end of the unit, before
 * the class's typeinfo function (this unit defines the class's key
 * function, pack). */
// FUNC 0x00190938 RaceDisplayLapTimeEvent::getEventType
// FUNC 0x00190940 RaceDisplayLapTimeEvent::getLap
//   No callers.
// FUNC 0x00190948 RaceDisplayLapTimeEvent::setLap
//   No callers.

