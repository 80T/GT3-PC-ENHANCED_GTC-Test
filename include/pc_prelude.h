/* pc_prelude.h - force-included (/FI) into every game and engine translation
 * unit of GT3 PC.
 *
 * The decompiled code is GNU C++ (egcs / GCC 14).  MSVC reads it with these
 * few spellings mapped; everything with a meaning (alignment, packing, VU0 /
 * MMI assembly) is rewritten in the source itself, with a "PC:" comment. */
#ifndef PC_PRELUDE_H
#define PC_PRELUDE_H

#ifdef _MSC_VER
#include <malloc.h>                 /* _alloca */
#ifdef __cplusplus
#include <type_traits>
#endif

/* Remaining GNU attributes are hints only (noreturn on abort, may_alias):
 * MSVC does no type-based alias analysis. */
#define __attribute__(x)
#define __inline__          inline
#define __inline            inline
#define __restrict__        __restrict
#define __extension__
#define __signed__          signed
#define __const__           const

#ifdef __cplusplus
#define __typeof__(x)       std::remove_reference_t<decltype(x)>
#endif

#define __builtin_alloca(n)         _alloca(n)
#define __builtin_expect(x, y)      (x)
#define __builtin_memcpy            memcpy
#define __builtin_memset            memset
#define __builtin_memcmp            memcmp
#define __builtin_strlen            strlen
#define __builtin_strcmp            strcmp
#define __builtin_sqrtf(x)          sqrtf(x)
#define __builtin_fabsf(x)          fabsf(x)
#define __builtin_fabs(x)           fabs(x)
#define __builtin_abs(x)            abs(x)
#define __builtin_prefetch(...)     ((void)0)
#endif /* _MSC_VER */

/* The C library names (malloc, free, printf, rand...) are mapped to the
 * port's versions from the first line of every decompiled file, so that a
 * member that shares such a name (pdistd::Allocator::free) is renamed the
 * same way everywhere.  Native code of the port does not include this. */
#include "libc.h"
#include "pc_static_init.h"

#endif /* PC_PRELUDE_H */
