/* T266 - gt2k_menu::SmartIndexDoubleRange.  0x001B29A8..0x001B2C20.
 *
 * A SmartIndexT<int> that moves inside an inner range [lo, hi) (wrapping
 * or clamping by `clamp'): the value of a gt2k_menu::Slider (T238). */
#include "game/menu/race_menu/race_menu_internal.h"
#include "game/common/T017_smart_index.h"          /* SmartIndexT<int>'s typeinfo (T017's copy) */
#include "game/common/T266_smart_index_double_range.h"

namespace gt2k_menu {

/* SmartIndexT<int>() inline (value 0, range [0, 1), wraps), inner range
 * [0, 1).  Caller: Slider::Slider. */
// FUNC 0x001B29A8 gt2k_menu::SmartIndexDoubleRange::SmartIndexDoubleRange
SmartIndexDoubleRange::SmartIndexDoubleRange()
    : lo(0), hi(1)
{
}

/* The range (SmartIndexT<int>::setRange) and the inner range both
 * [b, e). */
// FUNC 0x001B29E0 gt2k_menu::SmartIndexDoubleRange::setRange
void SmartIndexDoubleRange::setRange(int b, int e)
{
    SmartIndexT<int>::setRange(b, e);
    lo = b;
    hi = e;
}

/* The inner range [l, h) (ignored when empty: l == h); the value is
 * clamped into it. */
// FUNC 0x001B2A28 gt2k_menu::SmartIndexDoubleRange::setInnerRange
void SmartIndexDoubleRange::setInnerRange(int l, int h)
{
    if (l != h) {
        int v = value;
        lo = l;
        hi = h;
        if (v < l)
            value = l;
        else if (!(v < h))
            value = h - 1;
    }
}

/* value = v clamped into [lo, hi); = hi when the inner range is empty. */
// FUNC 0x001B2A68 gt2k_menu::SmartIndexDoubleRange::set
void SmartIndexDoubleRange::set(int v)
{
    int l = lo;
    int h = hi;
    value = v;
    if (l == h) {
        value = h;
        return;
    }
    if (v < l) {
        value = l;
        return;
    }
    if (!(v < h))
        value = h - 1;
}

/* Slot 3 (index: virtual_3).  value += delta inside [lo, hi): wrapped
 * around (wrapped = 1 when it went out) or clamped.  1 when the value
 * changed.  Callers: Slider::inputImmediate, Slider::inputStep. */
// FUNC 0x001B2AA8 gt2k_menu::SmartIndexDoubleRange::move
int SmartIndexDoubleRange::move(int delta)
{
    int old = value;
    value = old + delta;
    if (clamp == 0) {
        if (value < lo || !(value < hi))
            wrapped = 1;
        else
            wrapped = 0;
        while (value < lo)
            value = value + (hi - lo);
        while (!(value < hi))
            value = value - (hi - lo);
    } else {
        if (value < lo)
            value = lo;
        if (!(value < hi))
            value = hi - 1;
    }
    return old != value;
}

} /* namespace gt2k_menu */

