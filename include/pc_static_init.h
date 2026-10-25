/* pc_static_init.h - the static initialisers of the PS2 build (GT3 PC).
 *
 * The PS2 program ran its static initialisers from collect2's
 * __CTOR_LIST__: the compiler's own (objects with constructors, which the
 * C++ runtime handles in the port too) and functions that the decomp
 * writes out by hand (T019_static_init filling a hook table,
 * race_basic_static_init registering a race class...).  Each of the latter
 * is followed by PC_STATIC_INIT(name, address) in its file; the start-up
 * code runs them before the game's main, in the order __do_global_ctors
 * ran them on the PS2 (src/main/static_inits.cpp holds the PS2 list). */
#ifndef PC_STATIC_INIT_H
#define PC_STATIC_INIT_H

#ifdef __cplusplus

struct PcStaticInit {
    void (*fn)(void);
    unsigned int addr;          /* the function's PS2 address (its place in the list) */
    const char *name;
    PcStaticInit *next;
    PcStaticInit(void (*f)(void), unsigned int a, const char *n);
};

#define PC_STATIC_INIT_CAT_(a, b) a##b
#define PC_STATIC_INIT_CAT(a, b)  PC_STATIC_INIT_CAT_(a, b)
#define PC_STATIC_INIT(fn, addr) \
    static PcStaticInit PC_STATIC_INIT_CAT(pc_static_init_, __LINE__)(fn, addr, #fn)

/* Runs every registered initialiser once, in the PS2 order. */
void pc_run_static_inits(void);

#endif /* __cplusplus */

#endif /* PC_STATIC_INIT_H */
