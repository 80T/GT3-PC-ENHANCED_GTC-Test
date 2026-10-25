/* pc_libc.cpp - the newlib functions the game relies on (libc.h maps the
 * game's names to them).
 *
 * The PS2 program linked newlib, and two of its functions behave in ways
 * the game's results depend on, so the port keeps newlib's (the decomp's
 * src/runtime/newlib/stdlib.c, 0x0027DBE8..0x0027E650):
 *   rand   one global LCG x = x * 1103515245 + 12345, result x & 0x7FFFFFFF,
 *          seed 1 (MSVC's has RAND_MAX 0x7FFF and per-thread state; the
 *          tyre packets want 31 bits);
 *   qsort  Bentley-McIlroy, unstable in its own way: equal keys (menu
 *          objects at the same z, garage sort keys) must come out in the
 *          PS2's order.
 * malloc and the printf family are the game's own (pdistd T532..T535).
 * The state of rand is 32 bits: the PS2's was a 64-bit `unsigned long',
 * whose low 31 bits (the result) evolve the same. */
#include <stdint.h>
#include <stddef.h>
#include <ctype.h>

namespace {

uint32_t g_randNext = 1;

typedef int64_t s64;

#define qmin(a, b) ((a) < (b) ? (a) : (b))

#define swapcode(TYPE, parmi, parmj, n) {           \
        s64 i = (uint32_t)(n) / sizeof (TYPE);      \
        TYPE *pi = (TYPE *)(parmi);                 \
        TYPE *pj = (TYPE *)(parmj);                 \
        do {                                        \
            TYPE t = *pi;                           \
            *pi++ = *pj;                            \
            *pj++ = t;                              \
        } while (--i > 0);                          \
    }

#define SWAPINIT(a, es) \
    swaptype = ((uint32_t)(uintptr_t)(char *)(a) % sizeof (s64) || (es) % sizeof (s64)) ? 2 : \
               (es) == sizeof (s64) ? 0 : 1;

inline void swapfunc(char *a, char *b, int n, int swaptype)
{
    if (swaptype <= 1)
        swapcode(s64, a, b, n)
    else
        swapcode(char, a, b, n)
}

#define swap(a, b)                                  \
    if (swaptype == 0) {                            \
        s64 t = *(s64 *)(a);                        \
        *(s64 *)(a) = *(s64 *)(b);                  \
        *(s64 *)(b) = t;                            \
    } else                                          \
        swapfunc(a, b, (int)es, swaptype)

#define vecswap(a, b, n) if ((n) > 0) swapfunc(a, b, (int)(n), swaptype)

inline char *med3(char *a, char *b, char *c, int (*cmp)(const void *, const void *))
{
    return cmp(a, b) < 0 ?
           (cmp(b, c) < 0 ? b : (cmp(a, c) < 0 ? c : a))
          : (cmp(b, c) > 0 ? b : (cmp(a, c) < 0 ? a : c));
}

} // namespace

extern "C" {

int pc_rand(void)
{
    g_randNext = g_randNext * 1103515245u + 12345u;
    return (int)(g_randNext & 0x7FFFFFFF);
}

void pc_srand(unsigned int seed)
{
    g_randNext = seed;
}

char *pc_strupr(char *s)
{
    for (char *p = s; *p; p++)
        *p = (char)toupper((unsigned char)*p);
    return s;
}

/* newlib's qsort (BSD 4.4), 0x0027DBE8. */
void pc_qsort(void *a, size_t n, size_t es, int (*cmp)(const void *, const void *))
{
    char *pa, *pb, *pc, *pd, *pl, *pm, *pn;
    int   d, r, swaptype, swap_cnt;

loop:
    SWAPINIT(a, es);
    swap_cnt = 0;
    if (n < 7) {
        for (pm = (char *)a + es; pm < (char *)a + n * es; pm += es)
            for (pl = pm; pl > (char *)a && cmp(pl - es, pl) > 0; pl -= es)
                swap(pl, pl - es);
        return;
    }
    pm = (char *)a + (n / 2) * es;
    if (n > 7) {
        pl = (char *)a;
        pn = (char *)a + (n - 1) * es;
        if (n > 40) {
            d = (int)((n / 8) * es);
            pl = med3(pl, pl + d, pl + 2 * d, cmp);
            pm = med3(pm - d, pm, pm + d, cmp);
            pn = med3(pn - 2 * d, pn - d, pn, cmp);
        }
        pm = med3(pl, pm, pn, cmp);
    }
    swap((char *)a, pm);
    pa = pb = (char *)a + es;

    pc = pd = (char *)a + (n - 1) * es;
    for (;;) {
        while (pb <= pc && (r = cmp(pb, a)) <= 0) {
            if (r == 0) {
                swap_cnt = 1;
                swap(pa, pb);
                pa += es;
            }
            pb += es;
        }
        while (pb <= pc && (r = cmp(pc, a)) >= 0) {
            if (r == 0) {
                swap_cnt = 1;
                swap(pc, pd);
                pd -= es;
            }
            pc -= es;
        }
        if (pb > pc)
            break;
        swap(pb, pc);
        swap_cnt = 1;
        pb += es;
        pc -= es;
    }
    if (swap_cnt == 0) {
        for (pm = (char *)a + es; pm < (char *)a + n * es; pm += es)
            for (pl = pm; pl > (char *)a && cmp(pl - es, pl) > 0; pl -= es)
                swap(pl, pl - es);
        return;
    }

    pn = (char *)a + n * es;
    r = (int)qmin(pa - (char *)a, pb - pa);
    vecswap((char *)a, pb - r, r);
    r = (int)qmin(pd - pc, pn - pd - (int)es);
    vecswap(pb, pn - r, r);
    if ((r = (int)(pb - pa)) > (int)es)
        pc_qsort(a, r / es, es, cmp);
    if ((r = (int)(pd - pc)) > (int)es) {
        a = pn - r;
        n = r / es;
        goto loop;
    }
}

} // extern "C"
