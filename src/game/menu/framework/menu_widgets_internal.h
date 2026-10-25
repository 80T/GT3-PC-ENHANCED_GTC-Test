/* menu_widgets_internal.h - unit u067 of the GT3 A-Spec US main executable (SCUS-97102),
 * 0x001BF390..0x001C2318, second pass.
 *
 * Translation units (link order):
 *   T220  gt2k_menu::Object (the node of the menu object tree: links, the
 *         class allocator on the menu heap, the dying states) and
 *         gt2k_menu::RootObject (the root of a tree: updates, draws (also
 *         z-sorted), deletes, counts the nodes under it)
 *   T221  gt2k_menu::CallbackItem (shows one of N items, the index given by
 *         a callback)
 *   T222  gt2k_menu::DecideEffect (the flash over a decided button)
 *   T223  gt2k_menu::Pointer (the map pointer: eases to a target, drawn
 *         through a zoomed view)
 *   T224  gt2k_menu::WheelButtonList / WheelButton (the wheel list of the
 *         garage: a "mark" on the fitted wheel, the focused wheel shown on
 *         the 3D car)
 *   T225  MenuItemArray (the per-entry data of a list built at run time)
 *   T226  MenuImageHolder (a layout item's texture, unpacked when gzipped)
 *   T227  run_text_input (the text input page loop)
 *   T267  gt2k_menu::ConcurrentButton (a button carrying a sub ButtonList)
 *   T228  gt2k_menu::SliderItem (a slider bar with a "pinch" knob)
 *
 * Every class is defined in its canonical header (include/game/T220..T228,
 * T267).  No class headers here: each source includes the headers it uses
 * itself, in the order that gives retail's .rodata (egcs writes the type
 * names of the classes a file sees in the reverse order of their
 * definition; each source's comment lists retail's names).  This header
 * holds only what GCC 14 needs besides: the typeinfo storage and base lists
 * of the unit's own classes (egcs generates them itself).  T222 defines the
 * decide effect's script (internal linkage, so no declaration here).
 */
#ifndef U067_H
#define U067_H

#include "types.h"
#include <stddef.h>

/* The menus' virtual screen: T259's MENU_SCREEN_* (640 x 448, its centre
 * (320, 224)). */

/* ======================================================================== */
/* RTTI of the unit's own classes                                           */
/* ======================================================================== */

#endif /* U067_H */
