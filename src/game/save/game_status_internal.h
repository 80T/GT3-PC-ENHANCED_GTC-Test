/* game_status_internal.h - Gran Turismo 3 A-Spec US (SCUS-97102) main executable, unit u098,
 * 0x002185D8..0x0021B790.
 *
 * Translation units (address order):
 *   T429  gt2k_game::GameStatus: the game's save data (0x35210 bytes, one
 *         instance, gp-0x4F5C): day, money, statistics, the garage (200
 *         GarageCar slots in a GarageCarList, 200 GarageWheel slots in a
 *         GarageWheelList), the arcade progress, and the record blocks of
 *         T415..T434 (licences, event results, arcade records, ...).  A
 *         Serialize::Base: version 7.1, its registration "GameStatus".
 *         Also out-of-line copies of inline / template code (the
 *         List<GarageCar / GarageWheel> destructors, GarageCar's inline
 *         destructor and typeinfo functions).
 *   T430  MusicPlayList (? name, u037 / u044): the songs of BGM group 0x14,
 *         the play order (straight or shuffled with a 16-bit seed) and the
 *         next song to play.
 *   T431  the ranking code (u015: RankingRecord): a race result packed into
 *         80 bits, 4 bits of a hash built on SAFER (T433), a 12-bit CRC,
 *         everything SAFER-encrypted and written as digits with a check
 *         digit.
 *
 * The classes and functions are declared in the canonical headers
 * include/game/T429..T431 and in the headers of the units they use; each
 * source includes what it needs itself, in the order that gives the
 * original's type names (ref/data/u098/notes.md).  This header keeps only
 * what is local to the unit: named constants and, for the GCC 14 build, the
 * type_info storage of T429's classes.  The unit's data is defined in its
 * files: the registration g_002E34B8, the instance g_00352A94 and the
 * factory g_00352A98 in T429, the cipher key g_00340A28 in T431.
 *
 * Offsets in comments are the original (egcs) ones: egcs puts the vptr after
 * the data of the first polymorphic class (pCell, pList: +0x08), GCC 14 at
 * +0.
 */
#ifndef U098_H
#define U098_H

#include "types.h"

/* ======================================================================== */
/* T429 constants                                                           */
/* ======================================================================== */
#define GARAGE_CARS             200             /* GameStatus::cars[] */
#define GARAGE_WHEELS           200             /* GameStatus::wheels[] */
/* PC: the garage's heap slots come in blocks of GARAGE_BLOCK; at most
 * GARAGE_LIMIT slots of each kind (a save with more is taken for damaged). */
#define GARAGE_BLOCK            200
#define GARAGE_LIMIT            100000
/* The scalars saved as one block: day (+0x08) up to cars (+0x138). */
#define GAME_STATUS_BLOCK_SIZE  0x130
#define GAME_STATUS_VERSION     7               /* the PS2's saves (7.0, 7.1): positional records */
#define GAME_STATUS_SUBVERSION  1
/* PC: this build's saves, 8.0: the records indexed by product.gcf / the BGM
 * list go out as name-keyed lists (T429 pcWriteBody).  A new version, not a
 * sub-version: the PS2's code and older PC builds load any 7.x sub-version
 * (their checkHeader gives 1) but refuse another version (2). */
#define GAME_STATUS_PC_VERSION    8
#define GAME_STATUS_PC_SUBVERSION 0

/* Serialize::Base::checkHeader's result for a save of another version. */
#define CHECK_HEADER_BAD_VERSION 2

/* The arcade single races: the product database's course list, the race
 * codes of the three difficulties GameStatus checks (the second letter:
 * E easy, N normal, H hard), and the levels (arcadeClear[level] for N,
 * arcadeClear[ARCADE_LEVELS + level] for H). */
#define ARCADE_COURSE_LIST      "SINGLE_RACE"
#define ARCADE_RACE_EASY        "AECT0001"
#define ARCADE_RACE_NORMAL      "ANCT0001"
#define ARCADE_RACE_HARD        "AHCT0001"
#define ARCADE_LEVELS           6
#define PLACE_WON               0               /* ArcadeRecords::getPlace: first place */

/* The product database's service prices (ProductConfig::servicePrice). */
#define SERVICE_INITIAL_MONEY   "initial"
#define SERVICE_WHEEL           "wheel"


/* GameThumbnail::type of a game data save. */
#define THUMBNAIL_GAME_DATA     0

/* ======================================================================== */
/* T430 constants                                                           */
/* ======================================================================== */
#define MUSIC_BGM_GROUP         0x14            /* the BGM group of the play lists (ADS group 2) */
#define MUSIC_ENTRIES           64              /* MusicPlayList::entries[] */
#define MUSIC_SEED_SCALE        32768.0f        /* the next seed: next01() * 32768 */

/* ======================================================================== */
/* T431 constants                                                           */
/* ======================================================================== */
#define RANKING_BITS_BYTES      10              /* the 80 packed bits */
#define RANKING_CODE_BYTES      12              /* 16 bits of hash and CRC, then the 80 bits */
#define RANKING_HASH_ROUNDS     10              /* SAFER rounds of RankingHash */
#define RANKING_CODE_ROUNDS     13              /* SAFER rounds of the code */
#define RANKING_CHECK_SIZE      30              /* out[0] becomes a check digit from this size on */
#define SAFER_BLOCK             8               /* bytes of a SAFER block / RankingHash block */

/* ======================================================================== */
/* RTTI (modern compiler only; egcs generates these itself)                 */
/* ======================================================================== */

#endif /* U098_H */
