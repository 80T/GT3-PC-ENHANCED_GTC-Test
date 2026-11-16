/* race_championship_internal.h - Gran Turismo 3 A-Spec US (SCUS-97102) main executable, unit u050,
 * 0x0018DD80..0x001909E0, second pass: translation units T151, T152, T153,
 * T194.
 *
 *   T151  RaceChampionshipInformation (a GT mode championship: the global
 *         and current race codes, the session counter, the 8 cars and their
 *         points) and RaceChampionship (the race mode of a championship
 *         race) (include/game/race/modes/T151_race_championship.h)
 *   T152  RaceGTmodeInformation (a GT mode race: the player's car, the
 *         vibration / transmission options)
 *         (include/game/race/core/T152_race_gtmode_information.h)
 *   T153  RaceCourse (the course data of a race in the course arena
 *         UnitArena<64> g_002BB5B8, the road queries, the start / attach
 *         camera files) and the road query records
 *         (include/game/race/core/T153_race_course.h)
 *   T194  RaceDisplayEventBase and the display events (lap time, check
 *         point, difference, speed, information): the payload of the race
 *         event queue's display events, one 32-bit word
 *         (include/game/race/display/T194_race_display_event.h)
 *
 * Every shared class is defined in its canonical header (include/game/);
 * this header adds what only this unit uses: named constants and the
 * externs of RaceChampionship's race class record and node (T151 defines
 * them).
 *
 * No game header (u050's data pass): each source includes the canonical
 * headers it needs itself, in the order that gives retail's type names
 * (egcs writes the type names of the classes an object sees with inline
 * virtual functions; each source's comment lists retail's), and this header
 * last.
 */
#ifndef U050_H
#define U050_H

#include "types.h"

struct RaceClassInfo;                       /* T129 */
struct RaceClassNode;                       /* T129 */

/* ======================================================================== */
/* Constants (only used here)                                               */
/* ======================================================================== */

/* RaceBase::replayMode (virtual_8's argument): RACE_REPLAY (T129). */

/* RaceEntryInformation::inputId of the player (0x100 + player, T121). */
enum { INPUT_ID_PLAYER = 0x100 };

/* The cars of a GT mode race (RaceGTmodeInformation's entryCount; the
 * championship orders its entries 0..5) and the entries a championship
 * keeps (the save data has as many). */
enum {
    GTMODE_RACE_CARS     = 6,
    CHAMPIONSHIP_ENTRIES = 8
};

/* gt2k_game::MenuStatus::raceMode set by RaceChampionshipInformation::
 * resume: the next race of a series (T427). */
enum { MENU_RACE_MODE_SERIES = 2 };

/* ShowRoomCar::power's unit tag: 'ps' (the callers' multi-character
 * constant; the callee does not read it). */
enum { UNIT_TAG_PS = 0x7073 };

/* The course arena: one malloc of COURSE_ARENA_SIZE bytes cut into units of
 * COURSE_UNIT_SIZE (one course, CourseData), rounded up to 64 bytes.  PC:
 * the unit's least size; RaceCourse::load grows it to a bigger course's
 * file (race_course_fit_arena). */
enum {
    COURSE_ARENA_SIZE = 0xD00080,
    COURSE_UNIT_SIZE  = 0xD00000
};

/* The start / attach camera files: read whole into 64-byte aligned
 * buffers of this size (file_read_blocking, unpack 1).  PC: retail's size,
 * for the log; the buffers take the files' sizes. */
enum {
    CAMERA_FILE_SIZE  = 0x4000,
    CAMERA_FILE_ALIGN = 0x40
};

/* Race event types: RACE_EVENT_* of T122_race_event_queue.h. */
/* The queue timer of a posted display event: it expires after 2 frames. */
enum { DISPLAY_EVENT_DELAY = 2 };

/* The ranges of the display event word's fields (pack() clamps to them).
 * PC (ROADMAP 35): the car field is 5 bits, cars 0..31 (game/race/
 * race_cars.h PC_RACE_CARS_MAX; GT3's 4 bits, 0..15), and every field after
 * it one bit higher (T194_race_display_event.cpp): the difference keeps 27
 * signed bits.  The PS2's values in brackets. */
enum {
    EVENT_CAR_MAX         = 31,             /* bits 0..4 [15: 0..3] */
    EVENT_CHECK_POINT_MAX = 15,             /* bits 5..8 [4..7] */
    EVENT_LAP_MAX         = 999,            /* bits 8..17 (lap time), 9..18 (check point) [8..17] */
    EVENT_SPEED_MAX       = 9999,           /* bits 5..18, tenths [4..17] */
    EVENT_DIFF_MAX        = 0x3FFFFFF       /* bits 5..31, signed [0x7FFFFFF: 4..31] */
};
/* The largest time a difference event carries (RaceDisplayDiffEvent::
 * setTime / getTime: above it EVENT_TIME_SATURATED in the word,
 * RACE_TIME_INVALID out of it).  PC: both halved with the field (twice
 * EVENT_DIFF_MAX, as GT3's 0xFFFFFFE / 0xFFFFFFF were): pack clamps the
 * saturated value to EVENT_DIFF_MAX as before, so getTime still never gives
 * RACE_TIME_INVALID, and EVENT_DIFF_MAX (67108863 ms) stays above the
 * longest time the HUD shows (race_format_time, T158: 59999999, 999'59.999),
 * which every larger time is shown as, as before. */
enum {
    EVENT_TIME_MAX       = 0x07FFFFFE,      /* [0x0FFFFFFE] */
    EVENT_TIME_SATURATED = 0x07FFFFFF       /* [0x0FFFFFFF] */
};

/* ======================================================================== */
/* Data of the unit                                                         */
/* ======================================================================== */

/* The RaceClassInfo record {"RaceChampionship", 0x34600,
 * RaceChampionship_create, RaceChampionship_construct} and its registry
 * node (race_championship_static_ctor links them); defined in T151. */
extern RaceClassInfo g_002BAD10;            /* 0x002BAD10 */
extern RaceClassNode g_00351F88;            /* 0x00351F88 gp-0x5A68 */

#endif /* U050_H */
