/* game_side.cpp - sndgame: the game's side of the harness, compiled like
 * the game (the decomp's prelude and options).
 *
 * Drives the port's own engine/sound (sound_driver.cpp, sound_seq.cpp) and
 * engine/pdi (T520_pdispu2.cpp, T521_pdibgm.cpp) the way the game does:
 *   - a bank is loaded as file_read_sound_bank does (head, header part kept
 *     in memory, samples to SPU RAM through spu_memory_write) and set up
 *     with inst_relocate / engn_relocate (GTSOUNDINSTRUMENT::transfer);
 *   - music_init's set-up (snd_init, rates, seq_init, STUDIO_3 at 0xFFF,
 *     effect volume 0xFFF, effects on), music_play's (master volume,
 *     seq_start, seq_restart), music_thread's tick (seq_update, snd_flush,
 *     pdibgm_update);
 *   - ads_play / ads_set_volume (pdibgm_set_volume, pdibgm_start) and the
 *     status reads of RaceBGMPS2 (pdibgm_get_status);
 *   - CarSound's engine voice (engine_setup / engine_update).
 * Exposes plain C functions to the native harness (sndgame.cpp). */
#include "engine/system/system_drivers_internal.h"
#include "engine/pdi/T520_pdispu2.h"
#include "engine/pdi/T521_pdibgm.h"
#include "engine/sound/spu_memory.h"

extern "C" {

/* file_read_sound_bank (0x002418D0) over a file image: the header part
 * (the head's `samples' word = its size) into new memory, the samples to
 * SPU RAM at spuAddr.  Returns the header block, NULL for an unknown head. */
static void *gs_load_bank(const u8 *file, u32 fileSize, u32 spuAddr)
{
    const SoundBankHeader *head = (const SoundBankHeader *)file;
    if (head->magic != 0x4E474E45u && head->magic != 0x54534E49u)
        return NULL;
    u32 headerSize = (u32)(size_t)head->samples;
    u8 *buf = (u8 *)memalign(0x40, headerSize);
    memcpy(buf, file, headerSize);
    if (headerSize + (u32)head->spuSize <= fileSize)
        spu_memory_write(spuAddr, file + headerSize, (u32)head->spuSize);
    return buf;
}

static InstBank *s_musicBank;
static SeqgBank *s_musicSeq;
static Sequence s_sequence;
static EngnBank *s_engn;
/* raw storage: GtSoundVoice's constructor is T468's (game code, not here);
 * engine_setup initialises the voice */
alignas(8) static u8 s_engineStore[sizeof(GtSoundVoice)];
#define s_engine (*(GtSoundVoice *)s_engineStore)

/* music_init (0x0022B058): the sound system's set-up. */
void gs_music_init(void)
{
    snd_init();
    snd_set_rate(0x3C);
    seq_init();
    seq_set_frame_rate(0x3C);
    snd_set_reverb(4, 0xFFF, 0xFFF);        /* SD_EFFECT_MODE_STUDIO_3, depth L / R */
    snd_set_effect_volume(0xFFF, 0xFFF);
    F_00268978(1);
}

/* music_load (0x0022B338): music.ins at SPU 0xAC000, music.seq. */
int gs_music_load(const u8 *ins, u32 insSize, u8 *seq)
{
    void *hdr = gs_load_bank(ins, insSize, 0xAC000);
    if (hdr == NULL)
        return 0;
    s_musicBank = inst_relocate((InstBank *)hdr, 0xAC000);
    s_musicSeq = seqg_relocate((SeqgBank *)seq);
    return s_musicBank != NULL && s_musicSeq != NULL;
}

/* music_play (0x0022B460) of sequence `entry' at `volume'. */
void gs_music_play(int entry, float volume)
{
    seq_stop(&s_sequence);
    int vol = (int)(volume * 16384.0f);     /* music_set_volume */
    if (!(vol < 0x4000))
        vol = 0x3FFF;
    seq_set_master_volume(vol);
    seq_start(&s_sequence, &s_musicSeq->entries[entry], s_musicBank);
    seq_restart(&s_sequence);
}

void gs_music_stop(void)
{
    seq_stop(&s_sequence);
}

/* music_thread (0x0022AFF8): one frame (clock_update left out). */
void gs_tick(void)
{
    seq_update();
    snd_flush();
    pdibgm_update();
}

/* The voices the EE voice manager has playing. */
int gs_voices_playing(void)
{
    int n = 0;
    for (int i = 0; i < 46; i++)
        if (g_002FF168.voices[i].playing)
            n++;
    return n;
}

/* ads_play (0x0022AC98) after file_locate: the volume, then the stream. */
void gs_ads_play(int lsn, int sectors, int loop, float volume)
{
    pdibgm_set_volume(volume);
    pdibgm_start(lsn, sectors, loop, 0x40000, "");
}

void gs_ads_stop(void)
{
    pdibgm_stop();
}

void gs_ads_pause(int on)
{
    if (on)
        pdibgm_pause();
    else
        pdibgm_resume();
}

float gs_ads_status(int *pos, int *id, int *ended)
{
    return pdibgm_get_status(pos, id, ended);
}

/* CarSound::load / start: an engine bank and its voice. */
int gs_engine_load(const u8 *es, u32 esSize, u32 spuAddr)
{
    void *hdr = gs_load_bank(es, esSize, spuAddr);
    if (hdr == NULL)
        return 0;
    s_engn = engn_relocate((EngnBank *)hdr, spuAddr);
    if (s_engn == NULL)
        return 0;
    engine_setup(&s_engine, s_engn, 0, 0);
    return 1;
}

/* CarSound::updateEngine's last steps: rpm, priority, the voice update. */
void gs_engine_rpm(int rpm, int volume)
{
    s_engine.rpm = (u16)rpm;
    s_engine.volL = (s16)volume;
    s_engine.volR = (s16)volume;
    engine_set_priority(&s_engine, 0);
    engine_update(&s_engine);
}

void gs_engine_stop(void)
{
    engine_stop(&s_engine);
}

} /* extern "C" */
