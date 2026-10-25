/* T520 - pdispu2.cxx.  0x0025BF38..0x0025C830.
 *
 * Client of the pdispu2 IOP module: two objects of the anonymous-namespace
 * class RPC (_GLOBAL_$N$.._.._pdispu2.cxx9KEP7e::RPC):
 *   @0x004A6F50, server 'SPUP' (0x53505550): the SPU2 state (SpuState, fno 4),
 *        a status request (fno 3), fno 5 with three words; answers land in
 *        the 0x40-byte buffer g_002FCA00;
 *   @0x004A6BC0, server 'SPUV' (0x53505556): transfers (fno 1 / 2) of an EE
 *        buffer (? to / from SPU2 memory: {spuAddr, buf, size}).
 * The SpuState helpers fill a state block the caller owns (the G002 sound
 * code's 0x00302698).  engine/pdi/T520_pdispu2.h is a C header: the free functions
 * have C linkage.
 *
 * PC: there is no IOP.  What pdispu2.irx did with each call (apply the
 * state block to the SPU2 through libsd, answer the voice-active mask) is
 * done by the native SPU2 of engine/sound/native (sound_native.h,
 * docs/sound.md): the RPC objects are gone, the functions the game calls
 * keep their semantics.  The 'SPUP' answer is still mirrored in
 * g_002FCA00. */
#include "engine/pdi/T520_pdispu2.h"          /* C linkage */
#include "engine/pdi/pdi_internal.h"
#include "engine/sound/native/sound_native.h"
#include "game/app/pc_sound.h"                  /* PC: pc_sound_block_time */

/* PC: the native block is the same 0x350 bytes (sound_native.h). */
static_assert(sizeof(SpuState) == sizeof(SndNativeState), "SpuState / SndNativeState");
static_assert(sizeof(SpuVoiceState) == sizeof(SndNativeVoiceState), "SpuVoiceState / SndNativeVoiceState");

/* Answer buffer of the 'SPUP' calls (SIF DMA: 64-byte aligned); doubleword
 * 0 is the voice-active mask.  A public C++ global without an initialiser:
 * egcs puts it in .data, at the end of the file's variables. */
alignas(64) u64 g_002FCA00[8];    /* 0x002FCA00 */

/* Caller: RaceIOPLoadModules::cmdBindClients.
 * PC: nothing to bind; the native SPU2 is started by the platform
 * (snd_native_init, before the game runs). */
// FUNC 0x0025C048 pdispu2_init
void pdispu2_init(void)
{
}

/* No callers.
 * PC: 'SPUV' fno 1, read as EE buffer -> SPU RAM (the direction is not
 * confirmed; nothing calls it). */
// FUNC 0x0025C088 F_0025C088
void F_0025C088(void *buf, u32 spuAddr, int size)
{
    if (size > 0)
        snd_native_write_spu(spuAddr, buf, (u32)size);
}

/* No callers.
 * PC: 'SPUV' fno 2, read as SPU RAM -> EE buffer (idem). */
// FUNC 0x0025C0C0 F_0025C0C0
void F_0025C0C0(void *buf, u32 spuAddr, int size)
{
    if (size > 0)
        snd_native_read_spu(spuAddr, buf, (u32)size);
}

/* No callers.
 * PC: 'SPUP' fno 3: the mask recomputed now. */
// FUNC 0x0025C0F8 pdispu2_request_status
void pdispu2_request_status(void)
{
    snd_native_request_status();
    g_002FCA00[0] = snd_native_voice_active_mask();
}

/* Caller: snd_flush_voices (0x00269550), a tail jump (not in index.txt's
 * callers).  RPC::sendState (0x0025C608, inline): copies the state with
 * interrupts off and clears its dirty flag, then sends it (NOWAIT, the
 * answer goes to g_002FCA00).
 * PC: the copy goes to the native SPU2, which applies it as pdispu2's
 * handler did and computes the answer at once. */
// FUNC 0x0025C140 pdispu2_send_state
void pdispu2_send_state(SpuState *st)
{
    pdispu2_send_state_ext(st, NULL, 0);
}

/* PC: the voice pool's (T520_pdispu2.h): the extra voices' states copied
 * in the same interrupts-off section as the block (one frame's), sent with
 * the time of the game tick the flush belongs to; the answer's two words
 * mirrored in g_002FCA00. */
void pdispu2_send_state_ext(SpuState *st, SpuVoiceState *ext, int n)
{
    static SpuVoiceState extCopy[SND_NATIVE_EXTRA_VOICES];  /* (the music thread's alone) */
    SpuState copy;                      /* PC: the RPC object's send area */
    int wasEnabled = intr_suspend();

    copy = *st;
    st->dirty = 0;
    if (ext != NULL && n >= SND_NATIVE_EXTRA_VOICES)
        memcpy(extCopy, ext, sizeof(extCopy));
    intr_resume(wasEnabled);
    snd_native_send_state_ext((const SndNativeState *)&copy,
                              ext != NULL && n >= SND_NATIVE_EXTRA_VOICES ? (const SndNativeVoiceState *)extCopy
                                                                         : NULL,
                              SND_NATIVE_EXTRA_VOICES, pc_sound_block_time());
    u64 mask[2];
    snd_native_voice_active_mask128(mask);
    g_002FCA00[0] = mask[0];
    g_002FCA00[1] = mask[1];
}

void pdispu2_status128(u64 out[2])
{
    snd_native_voice_active_mask128((uint64_t *)out);
}

/* Caller: sound_init (0x0022C900), with (1, 1, 1).
 * PC: 'SPUP' fno 5 set the S/PDIF output mode (sceSdSetCoreAttr); nothing
 * to do natively. */
// FUNC 0x0025C280 F_0025C280
void F_0025C280(int a, int b, int c)
{
    snd_native_set_output_mode(a, b, c);
}

#define SPU_VOICES 48                   /* 2 cores x 24 */

/* Caller: snd_init (0x00268470). */
// FUNC 0x0025C2F8 spu_state_init
void spu_state_init(SpuState *st)
{
    int i;
    SpuVoiceState *v;
    SpuCoreState *core;

    memset(st, 0, sizeof(SpuState));
    /* a walking pointer: the counter alone counts down (indexing let loop.c
     * run the stores backwards) */
    v = st->voice;
    for (i = 0; i < SPU_VOICES; i++) {
        v->flags = 0x50;
        v++;
    }
    core = st->core;
    for (i = 0; i < 2; i++) {
        core->effect.core = i;
        core->unk_0C = i ? 0xFCC : 0xFC0;
        core->unk_00 = 0x3FFF;
        core->unk_02 = 0x3FFF;
        core->unk_04 = 0x7FFF;
        core->unk_06 = 0x7FFF;
        core->unk_08 = 0;
        core->effect.mode = 0;
        core++;
    }
    st->unk_348 = 0x3FFF;
    st->unk_34A = 0x3FFF;
}

/* No callers.  Does not set the dirty flag. */
// FUNC 0x0025C3B0 spu_state_set_unk08
void spu_state_set_unk08(SpuState *st, int core0, int core1)
{
    int wasEnabled = intr_suspend();

    st->core[0].unk_08 = core0;
    st->core[1].unk_08 = core1;
    intr_resume(wasEnabled);
}

/* Caller: snd_set_reverb (0x00268990).  The core fields of the copies are
 * forced to 0 / 1. */
// FUNC 0x0025C400 spu_state_set_effect
void spu_state_set_effect(SpuState *st, const SpuEffectAttr *core0, const SpuEffectAttr *core1)
{
    int wasEnabled = intr_suspend();

    st->dirty = 1;
    st->core[0].effect = *core0;
    st->core[0].effect.core = 0;
    st->core[1].effect = *core1;
    st->core[1].effect.core = 1;
    intr_resume(wasEnabled);
}

/* No callers.  Sets the same pair in both cores; does not set the dirty flag. */
// FUNC 0x0025C4A8 spu_state_set_unk04
void spu_state_set_unk04(SpuState *st, int l, int r)
{
    int wasEnabled = intr_suspend();

    st->core[0].unk_04 = l;
    st->core[0].unk_06 = r;
    st->core[1].unk_04 = l;
    st->core[1].unk_06 = r;
    intr_resume(wasEnabled);
}

/* Caller: snd_flush_voices (0x00269550).  First doubleword of the last
 * 'SPUP' answer: the voice-active mask.
 * PC: the native SPU2's answer (as of the last send). */
// FUNC 0x0025C500 pdispu2_status
u64 pdispu2_status(void)
{
    return snd_native_voice_active_mask();
}
