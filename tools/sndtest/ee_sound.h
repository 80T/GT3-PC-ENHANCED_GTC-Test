/* ee_sound.h - sndtest: a TEST COPY of GT3's EE-side sound driver.
 *
 * The voice manager (snd_play_voice, snd_voice_alloc / start / keyoff /
 * release / set_*, snd_flush_voices), the sequencer (seq_start,
 * seq_run_tracks, seq_note_on, seq_update, seq_update_channels) and the
 * engine-sound voices (engine_setup / update / stop) of
 * decomp/core_gt3enhanced src/engine/sound (sound_driver.c, sound_seq.c),
 * rewritten over the plain structures of banks.h so the tool can drive the
 * native back end exactly as the game will: snd_flush_voices builds the
 * SPU2 state block and hands it to snd_native_send_state, reading the
 * voice-active mask back from snd_native_voice_active_mask.  This copy is
 * for testing only; the game uses its own engine/sound code. */
#ifndef SNDTEST_EE_SOUND_H
#define SNDTEST_EE_SOUND_H

#include <cstdint>

#include "banks.h"

namespace ee {

constexpr int NUM_VOICES = 46;          /* SND_NUM_VOICES */
constexpr int NUM_CHANNELS = 47;        /* SEQ_NUM_CHANNELS */
constexpr int NUM_TRACKS = 16;

/* SndVoiceParam */
struct VoiceParam {
    int16_t  volL = 0, volR = 0;
    uint16_t pitch = 0;
    uint32_t addr8 = 0;                 /* SPU address / 8 */
    uint32_t adsr = 0;
    uint16_t unk10 = 0;                 /* zone unk_0A: non-zero = one-shot */
    uint8_t  priority = 0;
    uint8_t  flags = 0;                 /* bit 0: reverb send */
};

/* sound_driver.c */
void     snd_init();                                /* + spu_state_init, rate 60 */
void     snd_set_reverb_studio3(int depthL, int depthR);   /* snd_set_reverb(4, l, r) */
void     snd_set_effect_volume(int l, int r);
void     snd_set_effect_enable(int on);             /* F_00268978 */
void     snd_play_voice(int8_t *handle, const VoiceParam &p);
void     snd_voice_keyoff(int8_t *handle);
void     snd_voice_release(int8_t *handle);
void     snd_voice_set_pitch(int8_t *handle, int pitch);
void     snd_voice_set_volume(int8_t *handle, int16_t l, int16_t r);
void     snd_stop_all_voices();
uint16_t snd_note_to_pitch(int note, int base);
void     snd_pan_volume(int16_t *out, int vol, int pan);
void     snd_flush();                               /* snd_flush_voices */
extern const uint16_t *g_velocity;                  /* g_00345F10: 0x4000 * i / 127 */

/* the sequencer */
struct Sequence;
void seq_init();
Sequence *seq_play(const SeqBank &seqs, int entry, const InstBank &bank, uint16_t masterVolume);
void seq_stop(Sequence *s);
void seq_update();                                  /* once a frame */
bool seq_finished(const Sequence *s);

/* the engine sound (GtSoundVoice) */
struct EngineVoice {
    const std::vector<EngnSeg> *segs = nullptr;
    uint8_t prio = 0;
    uint16_t rpm = 0;
    int16_t pitch = 0x1000, volL = 0x3FFF, volR = 0x3FFF;
    struct Layer { int8_t voice = -1, seg = -1, prev = -1; int16_t volL = 0, volR = 0; uint16_t pitch = 0; uint32_t addr = 0; } layer[2];
};
void engine_setup(EngineVoice &es, const EngnBank &bank, int set, int prio);
void engine_update(EngineVoice &es);
void engine_stop(EngineVoice &es);

/* Voices playing (EE view) for reports. */
int voices_in_use();

} /* namespace ee */

#endif /* SNDTEST_EE_SOUND_H */
