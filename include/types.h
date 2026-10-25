/* types.h - the common types of the game and the engine (GT3 PC).
 *
 * The decomp's version described the PS2 EE (MIPS R5900: 32-bit pointers,
 * 64-bit GPRs, 128-bit lq / sq) for two compilers, the period egcs and GCC
 * 14.  The port compiles with MSVC for 32-bit x86: pointers stay 32 bits, so
 * the game's in-place relocated data keeps its layout; `long long` is the
 * 64-bit type as before (u64 / s64).  The 128-bit GPR type is a plain
 * 16-byte aligned pair of words (only packet code used it as a number). */
#ifndef BOOT_TYPES_H
#define BOOT_TYPES_H

#define BOOT_CAT_(a, b) a##b
#define BOOT_CAT(a, b)  BOOT_CAT_(a, b)

/* Attributes of the GCC builds that have no meaning here. */
#define BOOT_USED
#define BOOT_MAY_ALIAS

#include <math.h>

/* EE sqrt.s / abs.s (the decomp kept them as asm for matching). */
#define boot_sqrtf(x)    sqrtf(x)
#define boot_sqrtf_v(x)  sqrtf(x)
#define boot_fabsf(x)    fabsf((float)(x))

typedef unsigned char       u8;
typedef unsigned short      u16;
typedef unsigned int        u32;
typedef unsigned long long  u64;
typedef signed char         s8;
typedef signed short        s16;
typedef signed int          s32;
typedef signed long long    s64;
typedef float               f32;
typedef double              f64;

/* An EE 128-bit register value (a quadword in memory: lq / sq). */
#ifdef __cplusplus
struct alignas(16) u128 { u64 lo, hi; };
struct alignas(16) s128 { u64 lo, hi; };
#else
typedef struct { _Alignas(16) u64 lo; u64 hi; } u128;
typedef struct { _Alignas(16) u64 lo; u64 hi; } s128;
#endif

#ifndef NULL
#ifdef __cplusplus
#define NULL 0
#else
#define NULL ((void *)0)
#endif
#endif

#ifdef __cplusplus
#define EXTERN_C extern "C"
#else
#define EXTERN_C extern
#endif

#endif /* BOOT_TYPES_H */
