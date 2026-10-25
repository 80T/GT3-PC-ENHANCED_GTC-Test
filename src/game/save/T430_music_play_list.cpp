/* T430 - the music play list (? name, u037 / u044) (0x0021AB08..0x0021AFA0).
 *
 * The songs are the tracks of BGM group 0x14 (bgm_get_tracks), identified
 * by the 64-bit code of their name (nameToCode).  A play list holds up to
 * 64 songs, each enabled or not, and a play order: entry i plays at
 * position entries[i].order.  next() walks the positions, skipping the
 * songs already played in this round (a disabled song counts as played); at
 * the end of the order a new one is made (straight, or shuffled with the
 * 16-bit seed, which is advanced each time).
 */
#include "game/save/game_status_internal.h"
#include "game/save/T430_music_play_list.h"
#include "game/sound/T464_bgm.h"                      /* bgm_get_tracks */
#include "game/sound/T466_bgm_ads.h"                  /* BgmTrackList, MusicInfo (= BgmTrack) */
#include "game/data/T481_car_data_base.h"            /* nameToCode */
#include "engine/pdistd/T531_random.h"                   /* Random::next01, random_int */
#include "pc_ee.h"                          /* PC: ee_f2u, the EE's float -> unsigned conversion */
#include "libc.h"                           /* PC: printf, memcpy */
#include "game/save/pc_save_ext.h"          /* PC: the entries past 64, the keyed list */
#include <vector>                           /* PC: MusicRebuild */

// FUNC 0x0021AB08 findMusicInfo
//   The song of BGM group 0x14 whose name has code *code, its number in
//   *index (unless NULL); the first song (index 0) if there is none.
//   Callers: gt2k_menu::PlayMusicBox::preview / setSong,
//   RaceBGMPS2::play.
MusicInfo *findMusicInfo(const u64 *code, u32 *index)
{
    BgmTrackList songs = bgm_get_tracks(MUSIC_BGM_GROUP);  /* sp+0x0; 0x0022B750 */
    for (u32 i = 0; i < songs.count; i++) {
        if (*code == nameToCode(songs.tracks[i].name)) {
            if (index != NULL)
                *index = i;
            return &songs.tracks[i];
        }
    }
    if (index != NULL)
        *index = 0;
    return songs.tracks;
}

// FUNC 0x0021ABB0 MusicPlayList::init
//   ? name.  A random seed (random_int); all 64 entries enabled, not
//   played, no code; then every song of BGM group 0x14 in table order, and
//   a shuffled order (setRandom(1)).  Not a constructor (it does not return
//   this; GameOptions() does not run it).  Callers: RaceBGMPS2::RaceBGMPS2
//   (both of its lists), GameOptions::setDefaults.
void MusicPlayList::init()
{
    /* One counter (s2) and one entry pointer for both loops: set twice, the
     * pointer has no known base for egcs's alias analysis, so the first
     * loop's stores keep the load of songs.count after it (with a pointer
     * per loop GCSE hoists the load above the first loop). */
    u32 i;
    MusicEntry *entry;
    BgmTrackList songs = bgm_get_tracks(MUSIC_BGM_GROUP);  /* sp+0x0; 0x0022B750 */
    seed = random_int();
    for (i = 0; i < MUSIC_ENTRIES; i++) {
        entry = &entries[i];
        entry->played = 0;
        entry->enabled = 1;
        entry->code = 0;
    }
    /* PC: the heap entries (past 64) and the kept ones dropped */
    pc_ext_release(this, PC_EXT_MUSIC);
    count = 0;
    for (i = 0; i < songs.count; i++) {
        entry = entryAt(count);                 /* PC: past 64 on the heap (was &entries[count]) */
        entry->code = nameToCode(songs.tracks[i].name);
        count++;
    }
    setRandom(1);
}

/* ---- PC: more than 64 songs ------------------------------------------- */

/* PC (ROADMAP 18): entries[] holds the first 64 songs (the PS2's layout,
 * GameOptions is copied with memcpy); a BGM list of more (transplanted
 * sound files) has the entries past them on the heap (pc_save_ext, keyed
 * by the list's address).  The copies of a list (the options screen's, the
 * race's two) take them along: the copy constructor / operator= below,
 * GameOptions::operator=; the destructor frees them.  Every access goes
 * through entryAt.  New heap entries: no code, enabled, not played, no
 * play position. */
MusicEntry *MusicPlayList::entryAt(u32 i)
{
    u32 need, old, b;
    u8 *ext;

    if (i < MUSIC_ENTRIES)
        return &entries[i];
    need = (i + 1 - MUSIC_ENTRIES) * sizeof(MusicEntry);
    ext = pc_ext_get(this, PC_EXT_MUSIC, need, &old);
    for (b = old; b < need; b += sizeof(MusicEntry)) {
        MusicEntry *e = (MusicEntry *)(ext + b);
        e->enabled = 1;
        e->order = -1;
    }
    return (MusicEntry *)ext + (i - MUSIC_ENTRIES);
}

MusicPlayList::MusicPlayList()
{
    /* (nothing: as the PS2's, its owners fill it with init) */
}

MusicPlayList::MusicPlayList(const MusicPlayList &o)
{
    memcpy((void *)this, &o, sizeof(MusicPlayList));
    pc_ext_copy(this, &o, PC_EXT_MUSIC);
}

MusicPlayList &MusicPlayList::operator=(const MusicPlayList &o)
{
    if (this != &o) {
        memcpy((void *)this, &o, sizeof(MusicPlayList));    /* the PS2's struct copy */
        pc_ext_copy(this, &o, PC_EXT_MUSIC);
    }
    return *this;
}

MusicPlayList::~MusicPlayList()
{
    pc_ext_release(this, PC_EXT_MUSIC);
}

/* PC: count clamped to the 64 entries of this object (the part GameOptions'
 * memcpy loads; GameStatus::load then rebuilds the list, rebuildSaved /
 * loadKeyed, which also checks the position).  Returns 1 if it was over. */
int MusicPlayList::clampCount()
{
    if (count <= MUSIC_ENTRIES)
        return 0;
    count = MUSIC_ENTRIES;
    return 1;
}

/* PC: a list rebuilt on the current BGM list from saved entries, in their
 * saved order (the play list menu lets the player order the songs): each
 * entry whose song is in the BGM list is taken as it was; the others are
 * kept aside (written back by the next save); the BGM songs no entry had
 * are added after them (enabled).  With exactly the saved songs (stock
 * data) everything is as saved, play order and position too; else a new
 * play order (setRandom of the saved mode). */
struct MusicRebuild {
    MusicPlayList   *list;
    std::vector<u64> songs;                     /* the BGM list's codes */
    std::vector<u8>  used;
    u32              placed;
    int              changed;
};

static void music_rebuild_begin(MusicRebuild *r, MusicPlayList *list)
{
    BgmTrackList songs = bgm_get_tracks(MUSIC_BGM_GROUP);

    r->list = list;
    r->songs.resize(songs.count);
    r->used.assign(songs.count, 0);
    for (u32 i = 0; i < songs.count; i++)
        r->songs[i] = nameToCode(songs.tracks[i].name);
    r->placed = 0;
    r->changed = 0;
}

/* 1 if the saved entry (code, played, enabled, order) was placed. */
static int music_rebuild_entry(MusicRebuild *r, u64 code, s8 played, s8 enabled, s16 order)
{
    for (u32 i = 0; i < r->songs.size(); i++) {
        if (r->songs[i] == code && !r->used[i]) {
            MusicEntry *e = r->list->entryAt(r->placed++);
            r->used[i] = 1;
            e->code = code;
            e->played = played;
            e->enabled = enabled;
            e->order = order;
            return 1;
        }
    }
    r->changed = 1;
    return 0;
}

static void music_rebuild_end(MusicRebuild *r)
{
    MusicPlayList *list = r->list;

    for (u32 i = 0; i < r->songs.size(); i++) {
        if (!r->used[i]) {
            MusicEntry *e = list->entryAt(r->placed++);
            e->code = r->songs[i];
            e->played = 0;
            e->enabled = 1;
            e->order = -1;
            r->changed = 1;
        }
    }
    list->count = r->placed;
    pc_ext_resize(list, PC_EXT_MUSIC,
                  r->placed > MUSIC_ENTRIES ? (r->placed - MUSIC_ENTRIES) * sizeof(MusicEntry) : 0);
    if (r->changed) {
        static int logged = 0;
        if (logged == 0) {
            logged = 1;
            printf("PC: MusicPlayList: the saved play list differs from the BGM list (%u songs): rebuilt, a new play order\n",
                   list->count);
        }
        list->setRandom(list->random);
    } else if (list->position >= list->count) {
        list->position = 0;
    }
}

/* The 8-byte code + the 4-byte record of a keyed list entry, as kept. */
static void music_keep(MusicPlayList *list, u64 code, s8 played, s8 enabled, s16 order)
{
    u8 raw[2 + PC_KEY_CODE_BYTES + PC_REC_MUSIC];
    u16 len = PC_KEY_CODE_BYTES;

    memcpy(raw, &len, 2);
    memcpy(raw + 2, &code, 8);
    raw[10] = (u8)played;
    raw[11] = (u8)enabled;
    memcpy(raw + 12, &order, 2);
    pc_kept_add(list, PC_EXT_MUSIC, raw, sizeof(raw));
}

/* PC: a 7.x save's list (GameOptions' 64 entries, just loaded) rebuilt on
 * the current BGM list.  Caller: GameStatus::load. */
void MusicPlayList::rebuildSaved()
{
    MusicEntry saved[MUSIC_ENTRIES];
    u32 n = count < MUSIC_ENTRIES ? count : MUSIC_ENTRIES;
    MusicRebuild r;

    memcpy(saved, entries, sizeof(saved));
    pc_kept_clear(this, PC_EXT_MUSIC);
    music_rebuild_begin(&r, this);
    for (u32 i = 0; i < n; i++) {
        if (!music_rebuild_entry(&r, saved[i].code, saved[i].played, saved[i].enabled, saved[i].order))
            music_keep(this, saved[i].code, saved[i].played, saved[i].enabled, saved[i].order);
    }
    music_rebuild_end(&r);
}

/* PC: the 8.0 keyed list: (code; played, enabled, order) of every entry in
 * the list's order, then the entries kept from the last load. */
void MusicPlayList::saveKeyed(PcSaveWriter *w)
{
    PcKeyedList list;

    pc_keyed_begin(&list, w);
    for (u32 i = 0; i < count; i++) {
        MusicEntry *e = entryAt(i);
        u8 rec[PC_REC_MUSIC];
        rec[0] = (u8)e->played;
        rec[1] = (u8)e->enabled;
        memcpy(rec + 2, &e->order, 2);
        pc_keyed_put(&list, &e->code, PC_KEY_CODE_BYTES, rec, PC_REC_MUSIC);
    }
    pc_keyed_end(&list, this, PC_EXT_MUSIC);
}

static int music_place(void *ctx, const char *key, u32 keyLen, const u8 *rec)
{
    u64 code;
    s16 order;

    if (keyLen != PC_KEY_CODE_BYTES)
        return 0;
    memcpy(&code, key, 8);
    memcpy(&order, rec + 2, 2);
    return music_rebuild_entry((MusicRebuild *)ctx, code, (s8)rec[0], (s8)rec[1], order);
}

/* PC: the 8.0 keyed list read and the list rebuilt on the current BGM list
 * (after GameOptions::load, whose 64 entries it replaces).  Caller:
 * GameStatus::load. */
const u8 *MusicPlayList::loadKeyed(const u8 *src, const u8 *end)
{
    MusicRebuild r;
    const u8 *after;

    music_rebuild_begin(&r, this);
    after = pc_keyed_load(src, end, PC_REC_MUSIC, this, PC_EXT_MUSIC, music_place, &r, "play list songs");
    music_rebuild_end(&r);
    return after;
}

// FUNC 0x0021AC98 MusicPlayList::resetPlayed
//   A new round: an entry counts as played only when it is disabled;
//   allPlayed if every entry is.
void MusicPlayList::resetPlayed()
{
    s8 all = 1;
    for (u32 i = 0; i < count; i++) {
        MusicEntry *e = entryAt(i);             /* PC: was &entries[i] */
        e->played = e->enabled == 0;
        if (e->played == 0)
            all = 0;
    }
    allPlayed = all;
}

// FUNC 0x0021ACE0 MusicPlayList::setRandom
//   Straight (0) or shuffled (1) play, and a new order.  Callers:
//   gt2k_menu::PlayList::save, init.
void MusicPlayList::setRandom(s8 r)
{
    random = r;
    reorder();
}

// FUNC 0x0021AD00 MusicPlayList::reorder
//   A new round and play order: straight or shuffled.
void MusicPlayList::reorder()
{
    if (random == 0)
        orderStraight();
    else
        orderShuffle();
}

// FUNC 0x0021AD38 MusicPlayList::orderStraight
//   A new round; entry i plays at position i.
void MusicPlayList::orderStraight()
{
    resetPlayed();
    u32 songs = count;
    position = 0;
    for (u32 i = 0; i < songs; i++)
        entryAt(i)->order = i;                  /* PC: was entries[i] */
}

// FUNC 0x0021AD90 MusicPlayList::orderShuffle
//   A new round; positions 0, 1, ... go to random entries (a Random on the
//   stack, from the seed): a random entry, or when it already has a
//   position the next free one after it (wrapping; after count tries the
//   last one looked at is taken anyway).  The seed is then advanced:
//   (int)(next01() * 32768).  The (u32) conversion of the random float is
//   a call of fptoui (__fixunssfsi).
//   98.3%: tries and the i + 1 that GCSE hoists (retail $a2 / $a3) have
//   their registers swapped; everything else is exact.  global-alloc takes
//   the temporary first (priority 3*9/20 against tries' 3*11/34); declared
//   inside the if, tries wins $a2 but its `= 0' then misses the beqz delay
//   slot (97.7%).  Tried: tries / pick / n declaration orders and types,
//   the for / while / goto forms of both loops, the condition at the
//   bottom, i++ in the store; the reference compiler gives the same.
void MusicPlayList::orderShuffle()
{
    u32 i;                                      /* s1: one counter for both loops */
    resetPlayed();
    u32 songs = count;
    for (i = 0; i < songs; i++)
        entryAt(i)->order = -1;                 /* PC: was entries[i] (and entryAt below) */
    Random rng;                                 /* sp+0x0 (egcs drops the dead 0x87654321 store) */
    rng.seed = seed;
    position = 0;
    for (i = 0; i < count; i++) {
        f32 r = rng.next01();
        u32 n = count;
        u32 pick = ee_f2u(r * (f32)n);          /* implicit: fptoui; PC: ee_f2u */
        u32 tries = 0;
        /* (entries + pick): retail adds the index to this (`addu v0, a1, s2'),
         * entries[pick] gives this + index */
        if (n != 0 && entryAt(pick)->order >= 0) {
            /* a do-while (a for (;;) gets its head copied in front of it);
             * reorg steals the pick++ into the bgez's delay slot */
            do {
                pick++;
                tries++;
                if (!(pick < n))
                    pick = 0;
                if (!(tries < n))
                    break;
            } while (entryAt(pick)->order >= 0);
        }
        entryAt(pick)->order = i;
    }
    seed = (int)(rng.next01() * MUSIC_SEED_SCALE);
}

// FUNC 0x0021AEE8 MusicPlayList::next
//   The next song to play, NULL when every song is played (or disabled):
//   the entry at the current position, unless it was played in this round;
//   the position moves on either way, a new order is made at the end of
//   the order.  (Loops for ever when no entry has the position.)  Caller:
//   RaceBGMPS2::play.
MusicEntry *MusicPlayList::next()
{
    MusicEntry *found = NULL;
    u32 pos = position;
    for (;;) {
        if (allPlayed != 0)
            return NULL;
        /* count, not a local: GCSE keeps it in a reaching register, which
         * splits the loop's back edge (loop.c leaves this loop alone) */
        for (u32 i = 0; i < count; i++) {
            MusicEntry *entry = entryAt(i);     /* PC: was &entries[i] */
            if ((s16)pos != entry->order)
                continue;
            pos++;
            if (entry->played == 0) {
                entry->played = 1;
                found = entry;
            }
            if (!(pos < count)) {
                reorder();
                pos = 0;
            }
            break;
        }
        if (found != NULL)
            break;
    }
    position = pos;
    return found;
}
