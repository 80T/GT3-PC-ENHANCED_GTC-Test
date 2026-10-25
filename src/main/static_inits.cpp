/* static_inits.cpp - runs the hand-written static initialisers of the PS2
 * build in the PS2's order (include/pc_static_init.h). */
#include "pc_static_init.h"
#include "platform/log.h"

#include <algorithm>
#include <vector>

namespace {

PcStaticInit *g_head = nullptr;     /* constant-initialised: safe before any constructor */

/* The PS2 __CTOR_LIST__ entries in list order (tools/gen_static_inits.py). */
const unsigned int kCtorList[] = {
#include "static_init_order.inc"
};

int list_index(unsigned int addr)
{
    for (int i = 0; i < (int)(sizeof(kCtorList) / sizeof(kCtorList[0])); i++)
        if (kCtorList[i] == addr)
            return i;
    return -1;
}

} // namespace

PcStaticInit::PcStaticInit(void (*f)(void), unsigned int a, const char *n)
    : fn(f), addr(a), name(n), next(g_head)
{
    g_head = this;
}

void pc_run_static_inits(void)
{
    std::vector<PcStaticInit *> all;
    for (PcStaticInit *p = g_head; p; p = p->next)
        all.push_back(p);
    /* __do_global_ctors walked the list from its end to its start. */
    std::sort(all.begin(), all.end(), [](PcStaticInit *a, PcStaticInit *b) {
        return list_index(a->addr) > list_index(b->addr);
    });
    for (PcStaticInit *p : all) {
        if (list_index(p->addr) < 0)
            pclog::write("static init %s (0x%08X) is not in the PS2 list", p->name, p->addr);
        p->fn();
    }
    pclog::write("static init: %d initialisers run", (int)all.size());
}
