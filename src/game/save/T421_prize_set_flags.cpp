/* T421 (0x00215D00..0x00215EF8): the prize sets won (GameStatus+0x2EE60), one
 * byte per set of the product database (product_config, by prizeSetIndex).
 * Only the single-car sets are flagged (PrizeCarList skips a won one). */
#include "game/save/prize_records_internal.h"
#include "game/save/T421_prize_set_flags.h"
#include "game/save/pc_save_ext.h"              /* PC: the flags past 0xC8, the keyed list */

/* PC (ROADMAP 18): won[] holds the first 0xC8 flags (the PS2's layout);
 * product.gcf's prize sets past them have theirs on the heap (pc_save_ext,
 * sized from the set count at first use).  Every access goes through
 * prize_set_flag. */
#define PRIZE_SETS_SAVED 0xC8

/* PC: the flags held: product.gcf's prize sets, at least 0xC8. */
static u32 prize_sets_capacity(void)
{
    u32 n = product_config() != NULL ? (u32)product_config()->prizeSetCount() : 0;

    return n > PRIZE_SETS_SAVED ? n : PRIZE_SETS_SAVED;
}

/* PC: flag `index' (prizeSetIndex: (u32)-1 for a set the database lacks,
 * was garbage); NULL outside the capacity (logged once): never flagged, it
 * reads as not won. */
static s8 *prize_set_flag(PrizeSetFlags *flags, u32 index, const char *set)
{
    if (index >= prize_sets_capacity()) {
        static int logged = 0;
        if (logged == 0) {
            logged = 1;
            printf("PC: PrizeSetFlags: prize set %s is not in product.gcf: not recorded\n",
                   set != NULL ? set : "?");
        }
        return NULL;
    }
    if (index < PRIZE_SETS_SAVED)
        return &flags->won[index];
    return (s8 *)pc_ext_get(flags, PC_EXT_PRIZE_SETS, prize_sets_capacity() - PRIZE_SETS_SAVED, NULL)
           + (index - PRIZE_SETS_SAVED);
}

// FUNC 0x00215D00 PrizeSetFlags::PrizeSetFlags
PrizeSetFlags::PrizeSetFlags()
{
}

// FUNC 0x00215D08 PrizeSetFlags::clear
void PrizeSetFlags::clear()
{
    for (int i = 0; i < (int)sizeof(won); i++)   /* loop.c reverses it (bgez) */
        won[i] = 0;
    /* PC: the heap flags too, and the kept entries of the keyed list */
    for (u32 k = PRIZE_SETS_SAVED; k < prize_sets_capacity(); k++)
        *prize_set_flag(this, k, NULL) = 0;
    pc_kept_clear(this, PC_EXT_PRIZE_SETS);
}

// FUNC 0x00215D38 PrizeSetFlags::set
void PrizeSetFlags::set(const char *set)
{
    s8 *flag = prize_set_flag(this, product_config()->prizeSetIndex(set), set);   /* PC: range-checked */

    if (flag != NULL)
        *flag = 1;
}

// FUNC 0x00215D80 PrizeSetFlags::isSet
int PrizeSetFlags::isSet(const char *set)
{
    s8 *flag = prize_set_flag(this, product_config()->prizeSetIndex(set), set);   /* PC: range-checked */

    if (flag == NULL)
        return 0;
    return *flag != 0;
}

/* No caller. */
// FUNC 0x00215DC8 PrizeSetFlags::carCode
const char *PrizeSetFlags::carCode(const char *set, int i)
{
    return product_config()->prizeCarCode(set, i, NULL);
}

/* No caller.  The flags won (of the first 0xC5 only) over the number of
 * single-car prize sets (ProductConfig::singleCarPrizeSetCount). */
// FUNC 0x00215E10 PrizeSetFlags::ratio
f32 PrizeSetFlags::ratio()
{
    int wonCount = 0;
    for (int i = 0; i < 0xC5; i++) {
        if (won[i] != 0)
            wonCount++;
    }
    return (f32)wonCount / (f32)product_config()->singleCarPrizeSetCount();
}

// FUNC 0x00215E88 PrizeSetFlags::getSize
int PrizeSetFlags::getSize()
{
    return sizeof(PrizeSetFlags);               /* 0xC8 */
}

// FUNC 0x00215E90 PrizeSetFlags::load
//   (PC: a 7.x save's, by position; the heap flags are cleared first.)
const u8 *PrizeSetFlags::load(const u8 *src, int arg)
{
    u32 size = 0xC8;                            /* a variable: retail calls memcpy */

    clear();                                    /* PC: the heap flags, the kept entries */
    memcpy(this, src, size);
    return src + size;
}

/* PC: the 8.0 keyed list: (prize set name, its flag) of the sets won, then
 * the entries kept from the last load. */
void PrizeSetFlags::saveKeyed(PcSaveWriter *w)
{
    PcKeyedList list;
    int sets = product_config()->prizeSetCount();

    pc_keyed_begin(&list, w);
    for (int i = 0; i < sets; i++) {
        s8 *flag = prize_set_flag(this, (u32)i, NULL);
        if (*flag != 0) {
            const char *name = product_config()->prizeSetName(i);
            pc_keyed_put(&list, name, (u32)strlen(name), flag, PC_REC_PRIZE_SET);
        }
    }
    pc_keyed_end(&list, this, PC_EXT_PRIZE_SETS);
}

static int prize_set_place(void *ctx, const char *key, u32 keyLen, const u8 *rec)
{
    u32 index = product_config()->prizeSetIndex(key);

    if (index >= prize_sets_capacity())
        return 0;
    *prize_set_flag((PrizeSetFlags *)ctx, index, key) = (s8)rec[0];
    return 1;
}

const u8 *PrizeSetFlags::loadKeyed(const u8 *src, const u8 *end)
{
    clear();
    return pc_keyed_load(src, end, PC_REC_PRIZE_SET, this, PC_EXT_PRIZE_SETS, prize_set_place, this,
                         "prize set flags");
}

// FUNC 0x00215EC0 PrizeSetFlags::save
u8 *PrizeSetFlags::save(u8 *dst)
{
    u32 size = 0xC8;

    memcpy(dst, this, size);
    return dst + size;
}
