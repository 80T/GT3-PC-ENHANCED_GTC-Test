/* T468 - sound banks (GTSOUNDINSTRUMENT), the SPU / header allocators, and
 * the program voices (GtSoundHandle, GtSoundVoice).  0x0022BBF8..0x0022C3A0.
 *
 * A bank file (.ins, or .es with es = 1) is read into a temporary buffer:
 * its samples go to SPU RAM at the instrument's SPU address, its header is
 * copied into the header buffer (sound_set_header_buffer) and set up again
 * there; the file buffer is freed.  Without setFixed, the header buffer and
 * the SPU address are taken from the two global allocators, which then
 * advance. */
#include "game/sound/gtsound_internal.h"
#include "game/sound/T468_gtsound.h"
#include "engine/sound/spu_memory.h"            /* PC: the sound trace, the memory's size */
#include "engine/io/file_missing.h"             /* PC: file_missing_wait (sound_bank_sizes) */

/* .sdata, in retail's order (all initialised: written where defined). */
f32 g_seVolume = 1.0f;                          /* 0x00352B88 the effect volume */
int g_00352B8C = 0x5040;                        /* 0x00352B8C the SPU allocation pointer */
SoundBankHeader *g_soundHeaderNext = NULL;      /* 0x00352B90 */
int g_soundHeaderLeft = 0;                      /* 0x00352B94 */

/* PC: the sound trace's context (T468_gtsound.h; not the game's). */
SndTraceContext g_sndTraceContext = { -1, -1 };

/* PC: the sound trace (GT3PC_SND_TRACE=2, docs/sound.md 3.6): a handle keyed
 * a voice on (or found none): the car and slot it plays for, why it had no
 * voice (stolen by snd_voice_alloc, ended by snd_flush_voices, stop(0) at
 * zero volume, stop(1)), its volumes and pitch factor (thousandths) and,
 * for a wheel slot, the wheel's surface, squeal volume and load. */
static void gtsound_trace_keyon(const GtSoundHandle *h, int prio, f32 l, f32 r, f32 pitch)
{
    static const char *const kCarSlot[4] = { "gear whine", "turbo", "road", "silent" };
    static const char *const kWheelSlot[7] = {
        "none", "squeal 0/7/8/9", "squeal 5", "kerb 1/13", "squeal 2/3/4", "roll 0/7/8/9", "squeal 6/12/14"
    };
    const SndTraceContext *c = &g_sndTraceContext;
    char who[160], was[128], got[48];
    int lv = 0, lp = 0, lb = 0, ago = 0;
    int why = snd_trace_lost_reason(&h->voice, &lv, &lp, &lb, &ago);

    if (c->slot >= 4 && c->slot < 32) {
        int w = (c->slot - 4) / 7;
        sprintf(who, "car %d slot %d (wheel %d %s: surface %d, squeal %d, load %d)", c->car, c->slot, w,
                kWheelSlot[(c->slot - 4) % 7], c->surface[w], (int)(c->squeal[w] * 1000.0f), (int)c->load[w]);
    } else if (c->slot >= 0 && c->slot < 4) {
        sprintf(who, "car %d slot %d (%s)", c->car, c->slot, kCarSlot[c->slot]);
    } else {
        sprintf(who, "not a car slot");
    }
    switch (why) {
    case SND_LOST_STOLEN:
        sprintf(was, "stolen %d frames ago (voice %d at prio %d, by a prio %d request)", ago, lv, lp, lb);
        break;
    case SND_LOST_ENDED:
        sprintf(was, "ended %d frames ago (voice %d: the status said silent)", ago, lv);
        break;
    case SND_LOST_STOP:
        sprintf(was, "stop(0) %d frames ago (zero volume; voice %d)", ago, lv);
        break;
    case SND_LOST_STOP_FADE:
        sprintf(was, "stop(1) %d frames ago (voice %d)", ago, lv);
        break;
    default:
        sprintf(was, "no note (first key-on, or noted too long ago)");
        break;
    }
    if (h->voice >= 0)
        sprintf(got, "voice %d", h->voice);
    else
        sprintf(got, "NO VOICE (none free or weaker)");
    snd_trace_line("EE key-on %s -> %s, prio %d, l %d r %d pitch %d, SPU 0x%07X; was off: %s", who, got, prio,
                   (int)(l * 1000.0f), (int)(r * 1000.0f), (int)(pitch * 1000.0f), h->program->addr << 3, was);
}

/* The SPU volumes are s16 in -0x4000..0x3FFF. */
static inline int u104_clamp_volume(int vol)
{
    if (vol >= SPU_VOLUME_UNITY)
        return SPU_VOLUME_UNITY - 1;
    if (vol < -SPU_VOLUME_UNITY)
        return -SPU_VOLUME_UNITY;
    return vol;
}

/* Keys program p on on any free voice (fire and forget); volume 0 plays
 * nothing. */
// FUNC 0x0022BBF8 se_play_program
void se_play_program(SoundProgram *p, int pan, f32 volume, int prio)
{
    int vol = (int)(volume * g_seVolume * 16384.0f);    /* scaled, then the voice volume */

    if (volume != 0.0f) {
        SndVoiceParam vp;                   /* sp+0x00 */

        vol = (vol * p->volume) >> 14;
        if (vol >= SPU_VOLUME_UNITY)
            vol = SPU_VOLUME_UNITY - 1;
        else if (vol < -SPU_VOLUME_UNITY)
            vol = -SPU_VOLUME_UNITY;
        snd_pan_volume(vp.vol.lr, vol, pan);    /* 0x00269750 */
        vp.pitch    = snd_note_to_pitch(p->note << 8, p->basePitch);    /* 0x002687B0 */
        vp.unk_10   = p->unk_0A;
        vp.adsr     = SND_HALF_PAIR(p->adsr);   /* as GtSoundHandle::play */
        vp.addr     = p->addr;
        vp.priority = (prio < 0) ? p->prio : (u8)prio;
        vp.flags    = p->flags;
        snd_play_voice(NULL, &vp);          /* 0x002685A8 */
    }
}

// FUNC 0x0022BCE0 GtSoundHandle::GtSoundHandle
GtSoundHandle::GtSoundHandle()
{
    voice = -1;
}

// FUNC 0x0022BCF0 GtSoundHandle::set
void GtSoundHandle::set(SoundProgram *p)
{
    program = p;
    voice = -1;
}

/* Plays the program with the volumes l / r (times the effect volume) and
 * the pitch factor (1.0 = the program's); updates the voice if it already
 * plays.  Both volumes 0: stop(0). */
// FUNC 0x0022BD00 GtSoundHandle::play
void GtSoundHandle::play(int prio, f32 l, f32 r, f32 pitch)
{
    SndVoiceParam vp;                       /* sp+0x00 */
    s16 *lr = vp.vol.lr;
    int volL = (int)(l * g_seVolume * 16384.0f);    /* scaled, then the voice volume */
    int volR = (int)(r * g_seVolume * 16384.0f);
    int factor, notePitch;

    volR = (program->volume * volR) >> 14;
    volL = (program->volume * volL) >> 14;
    if ((volL | volR) == 0) {
        stop(0);
        return;
    }
    lr[0] = u104_clamp_volume(volL);
    lr[1] = u104_clamp_volume(volR);
    factor = (int)(pitch * 4096.0f);        /* 4.12 fixed point */
    notePitch = (snd_note_to_pitch(program->note << 8, program->basePitch) * factor) >> 12;
    if (voice >= 0) {
        if (prio >= 0)
            snd_voice_set_priority(&voice, prio);   /* 0x00267C18 */
        snd_voice_set_pitch(&voice, notePitch);     /* 0x00267BA8 */
        snd_voice_set_volume(&voice, &vp.vol.word);      /* 0x00267C70 */
    } else {
        vp.pitch    = notePitch;
        vp.unk_10   = program->unk_0A;
        /* adsr1 / adsr2 read as one word (a union access: retail keeps the
         * load after the unk_10 store) */
        vp.adsr     = SND_HALF_PAIR(program->adsr);
        vp.addr     = program->addr;
        vp.priority = (prio < 0) ? program->prio : (u8)prio;
        vp.flags    = program->flags;
        snd_play_voice(&voice, &vp);        /* 0x002685A8 */
        if (snd_trace_level() >= 2)         /* PC: the sound trace */
            gtsound_trace_keyon(this, vp.priority, l, r, pitch);
    }
}

/* fade: the volumes to 0 first; then key off and release. */
// FUNC 0x0022BEA0 GtSoundHandle::stop
void GtSoundHandle::stop(int fade)
{
    if (snd_trace_level() >= 2 && voice >= 0)  /* PC: the trace's note (the manager sets -1 on every loss) */
        snd_trace_lost(&voice, fade ? SND_LOST_STOP_FADE : SND_LOST_STOP, voice, 0, 0);
    if (fade) {
        SndVoiceParam vp;                   /* sp+0x00 (only vol is set) */
        s16 *lr = vp.vol.lr;

        lr[0] = 0;
        lr[1] = 0;
        snd_voice_set_volume(&voice, &vp.vol.word);  /* 0x00267C70 */
    }
    snd_voice_keyoff(&voice);               /* 0x00267B48 */
    snd_voice_release(&voice);              /* 0x00267AF0 */
}

/* The copy assignment: the old voice stopped, the program taken, no voice
 * yet (CarSound's compiler-generated operator=, T121, for each slot). */
// FUNC 0x0022BEF8 GtSoundHandle::operator=
GtSoundHandle &GtSoundHandle::operator=(const GtSoundHandle &o)
{
    stop(0);
    program = o.program;
    voice = -1;
    return *this;
}

// FUNC 0x0022BF40 GTSOUNDINSTRUMENT::GTSOUNDINSTRUMENT
GTSOUNDINSTRUMENT::GTSOUNDINSTRUMENT()
    : fixed(0), header(NULL), headerSize(0), headerCapacity(0),
      spuAddr(0), spuSize(0), spuRoom(0)
{
}

// FUNC 0x0022BF70 GTSOUNDINSTRUMENT::~GTSOUNDINSTRUMENT
GTSOUNDINSTRUMENT::~GTSOUNDINSTRUMENT()
{
    /* implicit: operator delete (compiler-generated: bases, members, deleting form) */
    /* the deleting form: operator delete (0x0027C0C0) for flags & 1 */
}

// FUNC 0x0022BFA0 GTSOUNDINSTRUMENT::setFixed
void GTSOUNDINSTRUMENT::setFixed(SoundBankHeader *hdr, int capacity, int spu, int room)
{
    fixed = 1;
    header = hdr;
    headerCapacity = capacity;
    spuAddr = spu;
    spuRoom = room;
}

/* Where the samples of the bank go. */
// FUNC 0x0022BFC0 GTSOUNDINSTRUMENT::nextSpuAddr
int GTSOUNDINSTRUMENT::nextSpuAddr()
{
    if (fixed)
        return spuAddr;
    return g_00352B8C;
}

/* Sets the loaded file up at spuAddr, copies its header into `header' and
 * sets the copy up too, frees the file, onTransfer(). */
// FUNC 0x0022BFE0 GTSOUNDINSTRUMENT::transfer
void GTSOUNDINSTRUMENT::transfer(SoundBankHeader *file, int es)
{
    int size, samples, part;

    if (file == NULL) {                     /* PC: not a bank, damaged, no memory (file_read_sound_bank's NULL) */
        printf("snd: the sound bank for SPU 0x%07X is not an ENGN / INST bank: not loaded\n", spuAddr);
        headerSize = 0;
        spuSize = 0;
        return;
    }
    /* PC: the file's block holds the header part: `samples' bytes, its
     * offset in the file before the setup relocates it (file_read_sound_bank
     * reads it into a block of that size, its own when the caller's buffer
     * was smaller) */
    part = (int)(intptr_t)file->samples;
    if (es == 0)
        inst_relocate((InstBank *)file, spuAddr);   /* 0x00268630 */
    else
        engn_relocate((EngnBank *)file, spuAddr);   /* 0x00268340 */
    size    = file->size;
    samples = file->spuSize;
    if (size > part) {
        /* PC: a header longer than the part read: only that part is there */
        printf("snd: a bank header says %d bytes, its file has %d: %d copied\n", size, part, part);
        size = part;
    }
    if (header == NULL || size > headerCapacity) {
        /* PC: a header larger than its buffer (the PS2 copied it over what
         * followed the buffer) stays in the file's buffer, set up already:
         * not freed (an EngineSound frees it with its own; the callers size
         * the buffers from the file first, sound_bank_sizes) */
        printf("snd: a bank header of %d bytes does not fit its %d-byte buffer: kept in the file's buffer\n",
               size, headerCapacity);
        header = file;
        headerCapacity = size;
    } else {
        memcpy(header, file, size);
        if (es == 0)
            inst_relocate((InstBank *)header, spuAddr);
        else
            engn_relocate((EngnBank *)header, spuAddr);
        free(file);
    }
    headerSize = size;
    spuSize = samples;
    /* PC: the samples are in SPU memory already (file_read_sound_bank):
     * logged when they ran past the room the owner gave (a fixed bank) or
     * past the memory's end (they wrapped to its start) */
    if (fixed && samples > spuRoom)
        printf("snd: a bank's %d bytes of samples at SPU 0x%07X overran their %d bytes of room\n", samples,
               spuAddr, spuRoom);
    if ((u32)spuAddr + (u32)samples > spu_memory_size())
        printf("snd: a bank's samples at SPU 0x%07X..0x%07X run past the sound memory's end 0x%07X\n", spuAddr,
               spuAddr + samples, spu_memory_size());
    onTransfer();
}

// FUNC 0x0022C0B0 GTSOUNDINSTRUMENT::onTransfer
void GTSOUNDINSTRUMENT::onTransfer()
{
}

// FUNC 0x0022C0B8 GTSOUNDINSTRUMENT::load
void GTSOUNDINSTRUMENT::load(SoundBankHeader *file, int es)
{
    if (fixed) {
        transfer(file, es);
        return;
    }
    spuRoom = 0;
    header = g_soundHeaderNext;
    headerCapacity = g_soundHeaderLeft;
    spuAddr = g_00352B8C;
    transfer(file, es);
    /* PC: only a header the buffer took advances it (transfer keeps a larger
     * one in its file's buffer): g_soundHeaderLeft stays >= 0 */
    if (header == g_soundHeaderNext) {
        g_soundHeaderNext = (SoundBankHeader *)((u8 *)header + headerSize);
        g_soundHeaderLeft -= headerSize;
    }
    g_00352B8C += spuSize;
}

// FUNC 0x0022C140 GTSOUNDINSTRUMENT::loadFile
void GTSOUNDINSTRUMENT::loadFile(const char *path, int es)
{
    load((SoundBankHeader *)file_read_sound_bank(path, nextSpuAddr(), NULL, 0), es);    /* 0x002418D0 */
}

// FUNC 0x0022C1A0 GTSOUNDINSTRUMENT::program
SoundProgram *GTSOUNDINSTRUMENT::program(int i)
{
    return &header->programs[i];
}

// FUNC 0x0022C1B8 GTSOUNDINSTRUMENT::play
void GTSOUNDINSTRUMENT::play(int i, int pan, f32 volume, int prio)
{
    se_play_program(program(i), pan, volume, prio);
}

/* Sets the SPU allocation pointer; returns the old one. */
// FUNC 0x0022C208 sound_set_spu_addr
int sound_set_spu_addr(int addr)
{
    int old = g_00352B8C;

    g_00352B8C = addr;
    return old;
}

// FUNC 0x0022C218 sound_set_header_buffer
void sound_set_header_buffer(void *buf, int size)
{
    g_soundHeaderNext = (SoundBankHeader *)buf;
    g_soundHeaderLeft = size;
}

// FUNC 0x0022C228 sound_load_ins_cb
void sound_load_ins_cb(GTSOUNDINSTRUMENT *inst, SoundBankHeader *data)
{
    inst->load(data, 0);
}

// FUNC 0x0022C248 sound_load_es_cb
void sound_load_es_cb(GTSOUNDINSTRUMENT *inst, SoundBankHeader *data)
{
    inst->load(data, 1);
}

// FUNC 0x0022C268 GtSoundVoice::GtSoundVoice
GtSoundVoice::GtSoundVoice()
{
    layer[0].voice = -1;
    layer[1].voice = -1;
}

/* The copy assignment: engine_stop on the old voice, then a byte copy of
 * all 0x30 bytes, then no voices (CarSound's compiler-generated operator=,
 * T121, for each engine). */
// FUNC 0x0022C280 GtSoundVoice::operator=
GtSoundVoice &GtSoundVoice::operator=(const GtSoundVoice &o)
{
    struct Raw { u8 b[0x30]; };

    engine_stop(this);                      /* 0x00268278 */
    *(Raw *)this = *(const Raw *)&o;
    layer[0].voice = -1;
    layer[1].voice = -1;
    return *this;
}

// FUNC 0x0022C320 sound_get_spu_addr
int sound_get_spu_addr(void)
{
    return g_00352B8C;
}

// FUNC 0x0022C328 GTSOUNDINSTRUMENT::getHeaderSize
int GTSOUNDINSTRUMENT::getHeaderSize()
{
    return headerSize;
}

// FUNC 0x0022C330 GTSOUNDINSTRUMENT::F_0022C330
SoundBankHeader *GTSOUNDINSTRUMENT::F_0022C330()
{
    return header;
}

// FUNC 0x0022C338 GTSOUNDINSTRUMENT::F_0022C338
SoundBankHeader *GTSOUNDINSTRUMENT::F_0022C338()
{
    return header;
}

// FUNC 0x0022C340 GTSOUNDINSTRUMENT::F_0022C340
SoundBankHeader *GTSOUNDINSTRUMENT::F_0022C340()
{
    return header;
}

// FUNC 0x0022C348 GTSOUNDINSTRUMENT::setSpuRoom
void GTSOUNDINSTRUMENT::setSpuRoom(int v)
{
    spuRoom = v;
}

// FUNC 0x0022C350 GTSOUNDINSTRUMENT::getSpuAddr
int GTSOUNDINSTRUMENT::getSpuAddr()
{
    return spuAddr;
}

// FUNC 0x0022C358 GTSOUNDINSTRUMENT::setSpuAddr
void GTSOUNDINSTRUMENT::setSpuAddr(int v)
{
    spuAddr = v;
}

/* ------------------------------------------------------------------------ */
/* PC: not the game's.  The sound memory above the PS2's 2 MB (the SPU      */
/* windows) and the sizes of a bank file (T468_gtsound.h).                  */
/* ------------------------------------------------------------------------ */

/* The windows' bounds (the last one ends at the memory's end) and their
 * areas, sorted by address.  A window holds a few dozen owners at most (the
 * effect banks, the music, a race's cars). */
#define SOUND_SPU_AREAS_MAX     128
#define SOUND_SPU_ALIGN         0x40

struct SoundSpuArea {
    const void *owner;
    int         start, end;
};
struct SoundSpuWindow {
    int          count;
    SoundSpuArea area[SOUND_SPU_AREAS_MAX];
};
static const int g_spuWindowStart[SOUND_SPU_WINDOW_COUNT + 1] = {
    SPU_MEMORY_PS2_BYTES, 0xA00000, 0x1200000, 0x7FFFFFFF
};
static SoundSpuWindow g_spuWindows[SOUND_SPU_WINDOW_COUNT];

int sound_spu_window_alloc(int w, int size, const void *owner)
{
    int lo, hi, pos, at = -1, i, intr;
    SoundSpuWindow *win;

    if ((u32)w >= SOUND_SPU_WINDOW_COUNT || size < 0)
        return -1;
    size = (size + SOUND_SPU_ALIGN - 1) & ~(SOUND_SPU_ALIGN - 1);
    if (size == 0)
        size = SOUND_SPU_ALIGN;
    lo = g_spuWindowStart[w];
    hi = g_spuWindowStart[w + 1];
    if ((u32)hi > spu_memory_size())
        hi = (int)spu_memory_size();
    win = &g_spuWindows[w];
    intr = intr_suspend();
    if (win->count < SOUND_SPU_AREAS_MAX) {
        /* first fit: the gap before area i (i == count: the window's end) */
        pos = lo;
        for (i = 0; i <= win->count; i++) {
            int next = i < win->count ? win->area[i].start : hi;
            if (next - pos >= size) {
                at = pos;
                break;
            }
            if (i < win->count)
                pos = win->area[i].end;
        }
        if (at >= 0) {
            memmove(&win->area[i + 1], &win->area[i], (win->count - i) * sizeof(SoundSpuArea));
            win->area[i].owner = owner;
            win->area[i].start = at;
            win->area[i].end = at + size;
            win->count++;
        }
    }
    intr_resume(intr);
    return at;
}

void sound_spu_window_free(int w, const void *owner)
{
    SoundSpuWindow *win;
    int i, j, intr;

    if ((u32)w >= SOUND_SPU_WINDOW_COUNT)
        return;
    win = &g_spuWindows[w];
    intr = intr_suspend();
    for (i = j = 0; i < win->count; i++)
        if (win->area[i].owner != owner)
            win->area[j++] = win->area[i];
    win->count = j;
    intr_resume(intr);
}

/* The head of the file (its first 0x20 bytes: SoundBankHeader without the
 * programs) through the file layer, before file_read_sound_bank reads the
 * bank (which sends the samples to SPU memory as it reads them: their
 * address must be chosen first). */
int sound_bank_sizes(const char *path, SoundBankSizes *out, int wait)
{
    alignas(64) u8 head[0x40];
    const SoundBankHeader *h = (const SoundBankHeader *)head;
    FileLoc loc;

    while (file_resolve(path, &loc, &vol_device_object) == -1) {
        if (!wait)
            return SOUND_BANK_MISSING;
        file_missing_wait(path);
    }
    if (!(loc.flags & 1) || loc.size < sizeof(SoundBankHeader))
        return SOUND_BANK_INVALID;
    memset(head, 0, sizeof(head));
    if (file_read_blocking(path, head, sizeof(SoundBankHeader), 0, &vol_device_object) == NULL)
        return SOUND_BANK_INVALID;
    if (h->magic != ENGN_MAGIC && h->magic != INST_MAGIC)
        return SOUND_BANK_INVALID;
    if (h->size < (int)sizeof(SoundBankHeader) || (u32)h->size > loc.size || h->spuSize < 0 ||
        (u32)h->spuSize > loc.size)
        return SOUND_BANK_INVALID;
    out->header = h->size;
    out->samples = h->spuSize;
    return SOUND_BANK_OK;
}

/* ------------------------------------------------------------------------ */
/* The enhanced engine: GtSoundChannel (Lupo 2, cameras_sound.md S1; the     */
/* Pod's voice, T176), after GT3's functions.                               */
/* ------------------------------------------------------------------------ */

/* Lupo 2 L2 0x0023B0A0. */
GtSoundChannel::GtSoundChannel()
{
    pending = 0;
    voice = -1;
}

/* Lupo 2 L2 0x0023B0B0: no stop (the voice is CarSound::stop's). */
void GtSoundChannel::set(SoundProgram *p)
{
    pending = p != NULL;
    voice = -1;
    program = p;
}

/* Lupo 2 L2 0x0023B0C8: the voice still sounding released first. */
void GtSoundChannel::bind(SoundProgram *p)
{
    stop(0);
    set(p);
}

/* Lupo 2 L2 0x0023B110 (? name: the survey's "position"): GtSoundHandle::
 * play's volumes and pitch; while the voice sounds the priority, pitch and
 * volumes are set again; else the program is keyed on once, when bound. */
void GtSoundChannel::play(int prio, f32 l, f32 r, f32 pitch)
{
    SndVoiceParam vp;
    s16 *lr = vp.vol.lr;
    int volL, volR, factor, notePitch;

    if (program == NULL)
        return;
    volL = (int)(l * g_seVolume * 16384.0f);
    volR = (int)(r * g_seVolume * 16384.0f);
    volL = (program->volume * volL) >> 14;
    volR = (program->volume * volR) >> 14;
    if ((volL | volR) == 0) {
        stop(0);
        return;
    }
    lr[0] = u104_clamp_volume(volL);
    lr[1] = u104_clamp_volume(volR);
    factor = (int)(pitch * 4096.0f);        /* 4.12 fixed point */
    notePitch = (snd_note_to_pitch(program->note << 8, program->basePitch) * factor) >> 12;
    if (voice >= 0) {
        if (prio >= 0)
            snd_voice_set_priority(&voice, prio);
        snd_voice_set_pitch(&voice, notePitch);
        snd_voice_set_volume(&voice, &vp.vol.word);
    } else if (pending) {
        pending = 0;
        vp.pitch    = notePitch;
        vp.unk_10   = program->unk_0A;
        vp.adsr     = SND_HALF_PAIR(program->adsr);
        vp.addr     = program->addr;
        vp.priority = (prio < 0) ? program->prio : (u8)prio;
        vp.flags    = program->flags;
        snd_play_voice(&voice, &vp);
    }
}

/* Lupo 2 L2 0x0023B2B8: as GtSoundHandle::stop. */
void GtSoundChannel::stop(int fade)
{
    if (fade) {
        SndVoiceParam vp;                   /* (only vol is set) */
        s16 *lr = vp.vol.lr;

        lr[0] = 0;
        lr[1] = 0;
        snd_voice_set_volume(&voice, &vp.vol.word);
    }
    snd_voice_keyoff(&voice);
    snd_voice_release(&voice);
}

/* Lupo 2 L2 0x0023B310: the old voice stopped, the program taken, no voice
 * yet; pending is not copied. */
GtSoundChannel &GtSoundChannel::operator=(const GtSoundChannel &o)
{
    stop(0);
    program = o.program;
    voice = -1;
    return *this;
}

