/* usb_pc.cpp - the USB device manager and the PJOY wheel client
 * (usbd.c 0x00265530.., pjoy.c 0x00265D80.. in the PS2 program).
 *
 * On the PS2 these talked to Polyphony's pdiusb IOP module, which drove
 * the USB steering wheels (Logitech GT Force and friends): their state
 * came over RPC and force-feedback commands went back.  Here PJOY ports 0
 * and 1 are the PC's wheels (platform/wheel.h: Wheel 1 / 2, each a Logitech
 * Driving Force, 0x46D:0xC294, mapped from an SDL joystick), connected while
 * their device is; with none the manager reports no device, as a PS2 with
 * nothing in its USB sockets did, and the wheel ports (T479) stay
 * disconnected.  No force feedback yet: a connected wheel takes the
 * commands (so that T479's set-up goes on) and drops them. */
#include "engine/media/G002_mpeg_frameq.h"
#include "engine/system/system_drivers_internal.h"
#include "platform/wheel.h"

#include <string.h>

/* The Driving Force's USB ids (engine/input/control_internal.h). */
enum { PC_WHEEL_VENDOR = 0x046D, PC_WHEEL_PRODUCT = 0xC294 };

/* A wheel on logical `port'. */
static int pc_wheel_connected(int port)
{
    return wheel::connected(port);
}

// FUNC 0x00265610 usbd_init
int usbd_init(int priority)
{
    (void)priority;
    return 0;
}

// FUNC 0x00265770 usbjoy_start
void usbjoy_start(void)
{
}

// FUNC 0x00265E50 pjoy_get_event_count
/* Events seen for the device on logical `port', -1 if none is mapped. */
int pjoy_get_event_count(int port)
{
    (void)port;
    return -1;
}

// FUNC 0x00265EA0 pjoy_read_state
/* The wheel as the Driving Force reports it: the 10-bit wheel, the pedals
 * at 0xFF released (0 pressed fully), 12 buttons, the hat (8 none). */
int pjoy_read_state(int port, PjoyState *out)
{
    wheel::State w;
    if (!wheel::read(port, &w))
        return PJOY_NO_DEVICE;
    memset(out, 0, sizeof(*out));
    out->x = w.x;
    out->slider[0] = 0xFF - w.gas;
    out->slider[1] = 0xFF - w.brake;
    out->pov[0] = w.hat;
    for (int i = 0; i < 12; i++)
        out->buttons[i] = w.buttons[i];
    return PJOY_OK;
}

// FUNC 0x002660E0 pjoy_get_info
int pjoy_get_info(int port, PjoyInfo *out)
{
    if (!pc_wheel_connected(port))
        return PJOY_NO_DEVICE;
    memset(out, 0, sizeof(*out));
    out->vendor = PC_WHEEL_VENDOR;
    out->product = PC_WHEEL_PRODUCT;
    return PJOY_OK;
}

/* Force feedback: taken by a connected wheel (and dropped). */
// FUNC 0x00266830 ff_send_download_play
int ff_send_download_play(int port, FfCmd *cmd)
{
    (void)cmd;
    return pc_wheel_connected(port) ? PJOY_OK : PJOY_NO_DEVICE;
}

// FUNC 0x00266880 ff_set_default_spring
int ff_set_default_spring(int port, int slots, int k1, int k2, int clip)
{
    (void)slots; (void)k1; (void)k2; (void)clip;
    return pc_wheel_connected(port) ? PJOY_OK : PJOY_NO_DEVICE;
}

// FUNC 0x002668E8 ff_default_spring
int ff_default_spring(int port, int slots, int on)
{
    (void)slots; (void)on;
    return pc_wheel_connected(port) ? PJOY_OK : PJOY_NO_DEVICE;
}

/* The command builders only fill the 8-byte command; nothing reads it here. */
// FUNC 0x00266998 ff_build_constant
FfCmd *ff_build_constant(FfCmd *c, int f0, int f1, int f2, int f3)
{
    (void)f0; (void)f1; (void)f2; (void)f3;
    return c;
}

// FUNC 0x00266A00 ff_build_damper
FfCmd *ff_build_damper(FfCmd *c, int slots, int k1, int k2, int s1, int s2)
{
    (void)slots; (void)k1; (void)k2; (void)s1; (void)s2;
    return c;
}
