/* pdi1394_rpc.cpp - the i.LINK (IEEE 1394) driver of the EE side
 * (0x0026E3E0..0x0026F030 in the PS2 program).
 *
 * On the PS2 this was the client of the pdi1394 IOP module ('PFIM' /
 * 'PFIS' RPC servers, an EE server thread receiving the link status and
 * the node messages), under which pdi1394 (T541) and the link battle
 * session (T473) ran 2 to 8 consoles connected by i.LINK cables.
 *
 * PC: there is no i.LINK.  The driver reports a bus with no node on it, so
 * pdi1394::updateTopology finds no link and the i.LINK battle menus say no
 * console is connected, as a PS2 with no cable did.  Every send fails
 * quietly and nothing is ever received. */
#include "engine/pdi/G003_pdi1394_rpc.h"
#include <string.h>

static Pdi1394Callback s_callback;
static void *s_callbackArg;

// FUNC 0x0026E3E0 pdi1394_init
void pdi1394_init(int priority)
{
    (void)priority;
}

// FUNC 0x0026E530 pdi1394_bind
void pdi1394_bind(void)
{
}

// FUNC 0x0026E6B0 pdi1394_msg_find_locked
void pdi1394_msg_find_locked(int chan, Pdi1394Msg *out, int type)
{
    (void)chan;
    (void)type;
    memset(out, 0, sizeof(*out));
}

// FUNC 0x0026E718 pdi1394_msg_take_locked
void pdi1394_msg_take_locked(int chan, Pdi1394Msg *out, int type)
{
    (void)chan;
    (void)type;
    memset(out, 0, sizeof(*out));
}

// FUNC 0x0026E888 pdi1394_send_msg
int pdi1394_send_msg(const Pdi1394Msg *msg)
{
    (void)msg;
    return 0;
}

// FUNC 0x0026E938 pdi1394_exchange
/* 3 = a bad reply: no frames came back. */
int pdi1394_exchange(void *send, void *recv, int size, int mode, int seq)
{
    (void)send;
    (void)size;
    (void)mode;
    (void)seq;
    if (recv != NULL)
        memset(recv, 0, 8 * 0x7C);
    return 3;
}

// FUNC 0x0026EB90 F_0026EB90
int F_0026EB90(void)
{
    return 0;
}

// FUNC 0x0026EC20 F_0026EC20
void F_0026EC20(int node)
{
    (void)node;
}

// FUNC 0x0026ECB8 pdi1394_set_callback
Pdi1394Callback pdi1394_set_callback(Pdi1394Callback func, void *arg)
{
    s_callbackArg = arg;
    s_callback = func;
    return func;
}

// FUNC 0x0026ED90 pdi1394_wait_event
/* 1: the vblank timeout (nothing arrives). */
int pdi1394_wait_event(void)
{
    return 1;
}

// FUNC 0x0026EE38 pdi1394_get_status
/* No node on the bus. */
void pdi1394_get_status(void *out)
{
    memset(out, 0, sizeof(Pdi1394Status));
}

// FUNC 0x0026EFA8 pdi1394_msg_init
void pdi1394_msg_init(void)
{
}
