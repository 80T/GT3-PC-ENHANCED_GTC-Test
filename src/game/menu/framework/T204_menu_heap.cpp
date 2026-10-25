/* T204 - the menu heap stack.  0x001B3918..0x001B3B58.
 *
 * Four pdistd::Allocator objects (g_0049C7E0, 0x18 bytes each); the menus
 * push one over a memory block when they start (menu_heap_push) and pop it
 * when they end.  gt2k_menu::Object::operator new allocates from the
 * current one.
 *
 * PC (ROADMAP 18): a menu heap that cannot run out.  The menus allocate and
 * free through menu_heap_allocate / menu_heap_free (below): a request the
 * current heap cannot meet is taken from the main heap (memalign) and
 * listed under the heap's level; it goes back with free() when the menu
 * frees it or, at the latest, when its level is popped (as the heap's own
 * blocks die with it).  A free goes to the heap whose range holds the block
 * (some blocks are freed while another heap is current: T212's car buffer
 * on the loader thread, FreeAgent's), else to the main heap when it is a
 * listed block, else it is ignored (as Allocator::free ignores a block that
 * is not its own).  A push without memory (memalign failed) or past the
 * four heaps gives a level whose blocks all come from the main heap. */
#include "game/menu/framework/T204_menu_heap.h"
#include "game/menu/framework/menu_buttons_internal.h"
#include "engine/pdistd/T532_heap.h"                 /* PC: memalign, malloc, free (the fallback) */
#include "eekernel.h"                       /* PC: EI */
#include "libc.h"                           /* PC: printf, memcpy */

/* Data (retail): .sdata 0x00352510 (g_00352510, -1), .bss 0x0049C7E0 (the
 * heaps); no .rodata (the original saw no class egcs writes a type name
 * for: pdistd::Allocator's key function is T530's). */

/* The heaps (.bss 0x0049C7E0, 0x18 bytes each), only used here.  Built
 * by egcs's static initialiser (Allocator(): best fit), destroyed by its
 * static destructor.  `static`: egcs puts a public object that has a
 * constructor in .data (T039's g_0029AD40), a static one in .bss, where
 * retail has this array; no other unit refers to it. */
static pdistd::Allocator g_0049C7E0[MENU_HEAP_LEVELS];     /* 0x0049C7E0 */

/* Index of the current heap in g_0049C7E0, -1 while none is pushed
 * (.sdata, gp-0x54E0).  Initialised data before the first function: egcs
 * names the static initialiser / destructor and the frame info after it. */
int g_00352510 = -1;                        /* 0x00352510 */

/* PC: the ranges the pushed heaps manage (menu_heap_free's routing; [0, 0)
 * for a level without memory), whether the level's first fallback was
 * logged, the empty heap menu_heap_current gives without a heap (no heap
 * pushed, or past the four: allocate gives NULL, free ignores everything),
 * and the blocks the main heap gave each level (a growing array). */
static u8  *g_menuHeapBase[MENU_HEAP_LEVELS];
static u32  g_menuHeapSize[MENU_HEAP_LEVELS];
static int  g_menuHeapLogged[MENU_HEAP_LEVELS];
static int  g_menuHeapNoneLogged;
static pdistd::Allocator g_menuHeapNone;
struct MenuHeapExtra {
    void *block;                            /* memalign's block, given to the menu as is */
    u32   size;                             /* the bytes asked (the log) */
    int   level;                            /* the heap level it stands in for (-1: none pushed) */
};
static MenuHeapExtra *g_menuHeapExtra;
static int g_menuHeapExtraCount;
static int g_menuHeapExtraMax;

// FUNC 0x001B3918 menu_heap_current
pdistd::Allocator *menu_heap_current(void)
{
    /* PC: no heap pushed (retail read g_0049C7E0[-1]) or past the four */
    if (g_00352510 < 0 || g_00352510 >= MENU_HEAP_LEVELS)
        return &g_menuHeapNone;
    return &g_0049C7E0[g_00352510];
}

/* Next heap, emptied (the inline Allocator::reset), assigned the block. */
// FUNC 0x001B3938 menu_heap_push
void menu_heap_push(void *mem, size_t size)
{
    g_00352510++;
    /* PC: a fifth level has no Allocator: everything it asks for comes
     * from the main heap (retail wrote past the array) */
    if (g_00352510 >= MENU_HEAP_LEVELS) {
        printf("PC: menu heap %d pushed past the %d heaps: its blocks come from the main heap\n",
               g_00352510, MENU_HEAP_LEVELS);
        return;
    }
    g_0049C7E0[g_00352510].reset();
    g_menuHeapLogged[g_00352510] = 0;
    /* PC: no memory for it (memalign failed): an empty heap, every block
     * from the main heap (retail assigned address 0) */
    if (mem == NULL) {
        printf("PC: menu heap %d: no memory for its 0x%X bytes: its blocks come from the main heap\n",
               g_00352510, (u32)size);
        g_menuHeapBase[g_00352510] = NULL;
        g_menuHeapSize[g_00352510] = 0;
        return;
    }
    g_0049C7E0[g_00352510].assign(mem, size);
    g_menuHeapBase[g_00352510] = (u8 *)mem;
    g_menuHeapSize[g_00352510] = size;
}

/* PC: frees the main-heap blocks listed under `level' (its pop: they die
 * with the heap they stand in for). */
static void menu_heap_release_level(int level)
{
    int count = 0;
    u32 bytes = 0;
    int intr = intr_suspend();
    for (int i = 0; i < g_menuHeapExtraCount; ) {
        if (g_menuHeapExtra[i].level == level) {
            free(g_menuHeapExtra[i].block);
            count++;
            bytes += g_menuHeapExtra[i].size;
            g_menuHeapExtra[i] = g_menuHeapExtra[--g_menuHeapExtraCount];
        } else {
            i++;
        }
    }
    if (intr)
        EI();
    if (count != 0)
        printf("PC: menu heap %d popped: %d blocks (0x%X bytes) of the main heap freed with it\n",
               level, count, bytes);
}

/* Unassigns the current heap, steps back, then empties the heap it popped. */
// FUNC 0x001B3998 menu_heap_pop
void menu_heap_pop(void)
{
    /* PC: nothing pushed (retail stepped to -2) */
    if (g_00352510 < 0) {
        printf("PC: menu heap popped with none pushed\n");
        return;
    }
    menu_heap_release_level(g_00352510);    /* PC */
    if (g_00352510 >= MENU_HEAP_LEVELS) {   /* PC: a level without a heap */
        g_00352510--;
        return;
    }
    g_0049C7E0[g_00352510].unassign();
    int popped = g_00352510;
    g_00352510 = popped - 1;
    g_0049C7E0[popped].reset();
    g_menuHeapBase[popped] = NULL;          /* PC */
    g_menuHeapSize[popped] = 0;
}

// FUNC 0x001B3A00 menu_heap_depth
int menu_heap_depth(void)
{
    return g_00352510;
}

/* 1 while a heap is pushed (depth >= 0: the sign bit of ~depth).  No
 * callers. */
// FUNC 0x001B3A08 menu_heap_active
int menu_heap_active(void)
{
    return (u32)~menu_heap_depth() >> 31;
}

/* PC: the current heap's block, or one of the main heap's when the heap is
 * full (listed under the current level; the first one of a push logged). */
void *menu_heap_allocate(u32 size, u32 align)
{
    void *p = menu_heap_current()->allocate(size, align);
    if (p != NULL)
        return p;

    int level = g_00352510;
    p = memalign(align, size);
    if (p == NULL) {
        printf("PC: menu heap %d: the main heap has no 0x%X bytes either\n", level, size);
        return NULL;
    }
    int intr = intr_suspend();
    if (g_menuHeapExtraCount == g_menuHeapExtraMax) {
        int max = g_menuHeapExtraMax != 0 ? g_menuHeapExtraMax * 2 : 64;
        MenuHeapExtra *list = (MenuHeapExtra *)malloc(max * sizeof(MenuHeapExtra));
        if (list == NULL) {
            if (intr)
                EI();
            free(p);
            printf("PC: menu heap %d: no memory to list a main-heap block\n", level);
            return NULL;
        }
        if (g_menuHeapExtra != NULL) {
            memcpy(list, g_menuHeapExtra, g_menuHeapExtraCount * sizeof(MenuHeapExtra));
            free(g_menuHeapExtra);
        }
        g_menuHeapExtra = list;
        g_menuHeapExtraMax = max;
    }
    g_menuHeapExtra[g_menuHeapExtraCount].block = p;
    g_menuHeapExtra[g_menuHeapExtraCount].size = size;
    g_menuHeapExtra[g_menuHeapExtraCount].level = level;
    g_menuHeapExtraCount++;
    int first = 0;
    if (level < 0) {
        first = g_menuHeapNoneLogged == 0;
        g_menuHeapNoneLogged = 1;
    } else if (level < MENU_HEAP_LEVELS && g_menuHeapLogged[level] == 0) {
        g_menuHeapLogged[level] = 1;        /* (past the heaps: logged at the push) */
        first = 1;
    }
    if (intr)
        EI();
    if (first && level < 0)
        printf("PC: menu heap: a block of 0x%X bytes with no heap pushed: from the main heap\n",
               size);
    else if (first)
        printf("PC: menu heap %d (0x%X bytes) full: 0x%X bytes from the main heap (and so on)\n",
               level, g_menuHeapSize[level], size);
    return p;
}

/* PC: a block back to the heap whose range holds it, or to the main heap if
 * it is one of the listed blocks; anything else is ignored (as the heap's
 * own free ignores a block that is not its own).  NULL: nothing. */
void menu_heap_free(void *p)
{
    if (p == NULL)
        return;
    int intr = intr_suspend();
    int top = g_00352510 < MENU_HEAP_LEVELS ? g_00352510 : MENU_HEAP_LEVELS - 1;
    for (int level = top; level >= 0; level--) {
        if ((u8 *)p >= g_menuHeapBase[level] &&
            (u8 *)p < g_menuHeapBase[level] + g_menuHeapSize[level]) {
            g_0049C7E0[level].free(p);
            if (intr)
                EI();
            return;
        }
    }
    for (int i = g_menuHeapExtraCount - 1; i >= 0; i--) {
        if (g_menuHeapExtra[i].block == p) {
            g_menuHeapExtra[i] = g_menuHeapExtra[--g_menuHeapExtraCount];
            free(p);
            break;
        }
    }
    if (intr)
        EI();
}

/* Compiler-generated: egcs's static destructor and initialiser of
 * g_0049C7E0 (.dtors entry 0x0028F230, .ctors entry 0x0028F154), emitted at
 * the end of the unit, the destructor first.  The destructor runs the four
 * virtual destructors (flags 0), last first, behind a never-taken NULL
 * test of the array; the initialiser builds Allocator(0) on each, its
 * landing pad destroys the ones already built. */
// FUNC 0x001B3A30 T204_static_dtor (_GLOBAL_$D$g_00352510)
/* implicit: ~Allocator (the static destructor of g_0049C7E0) */

// FUNC 0x001B3A98 T204_static_init (_GLOBAL_$I$g_00352510)
/* implicit: Allocator (the static initialiser of g_0049C7E0) */
