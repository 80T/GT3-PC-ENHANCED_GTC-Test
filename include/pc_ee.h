/* pc_ee.h - EE conversions that MSVC does differently (GT3 PC).
 *
 * egcs compiled a float -> unsigned conversion for the R5900 as a call of
 * libgcc's fptoui (0x0027AEA0, fp-bit's float_to_usi = __fixunssfsi,
 * decomp src/runtime/libgcc/fp_bit.c): 0 for zero, NaN, negative values and
 * anything below 1, 0xFFFFFFFF from 2^32 up (and for infinity), else the
 * value truncated.  MSVC's (unsigned)f is undefined for negative values
 * (x86 wraps them: (unsigned)-1.0f = 0xFFFFFFFF), so the game's fptoui sites
 * (docs/runtime.md 5.3) convert with ee_f2u.
 *
 * Plain C++ without dependencies, for game and engine files. */
#ifndef PC_EE_H
#define PC_EE_H

/* (unsigned)f as the EE's fptoui gives it. */
static inline unsigned int ee_f2u(float f)
{
    if (!(f >= 1.0f))                       /* zero, NaN, negative, below 1 */
        return 0u;
    if (f >= 4294967296.0f)                 /* 2^32 up, infinity */
        return 0xFFFFFFFFu;
    if (f >= 2147483648.0f)                 /* 2^31 .. 2^32: f - 2^31 is exact */
        return (unsigned int)(int)(f - 2147483648.0f) + 0x80000000u;
    return (unsigned int)(int)f;            /* truncation toward zero */
}

#endif /* PC_EE_H */
