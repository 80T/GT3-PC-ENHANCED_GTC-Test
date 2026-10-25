/* T236 - gt2k_menu::FreeAgent (0x001C5D40..0x001C5E50).
 *
 * An Object that owns one block of the menu heap (a list factory's entry
 * array, MenuItemArray::buffer) and frees it when the object tree releases
 * it.  The list factories hang one under each list they build.
 *
 * Data (retail .rodata 0x003365B0..0x003365F6): FreeAgent's vtable and its
 * type name only (the original saw no class with only inline virtuals).
 */
#include "game/menu/framework/T236_free_agent.h"
#include "game/menu/framework/T204_menu_heap.h"            /* menu_heap_current */
#include "game/menu/framework/menu_links_internal.h"

namespace gt2k_menu {

// FUNC 0x001C5D40 gt2k_menu::FreeAgent::~FreeAgent
FreeAgent::~FreeAgent()
{
    /* The vptr store, the block back to the current menu heap, then
     * Object::~Object(0) and Object::operator delete if flags & 1. */
    if (mem != 0) {
        menu_heap_free(mem);                /* PC: by the block's address (the heap of the block) */
        mem = 0;
    }
    /* implicit: ~Object (base destructor), operator delete (deleting destructor) */
}

} /* namespace gt2k_menu */

// FUNC 0x001C5DB8 gt2k_menu::FreeAgent::FreeAgent
/* inline in game/menu/framework/T236_free_agent.h; egcs emits this copy (no callers):
 * Object::Object(), mem, the vptr */
/* implicit: Object (base constructor) */

