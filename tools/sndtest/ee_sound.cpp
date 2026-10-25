/* ee_sound.cpp - sndtest: TEST COPY of GT3's EE sound driver (see
 * ee_sound.h).  Function names and addresses are those of
 * decomp/core_gt3enhanced src/engine/sound/sound_driver.c and sound_seq.c;
 * the logic is theirs, rewritten over banks.h's structures. */
#include "ee_sound.h"

#include <cmath>
#include <cstring>

#include "engine/sound/native/sound_native.h"

namespace ee {

/* ------------------------------------------------------------------------ */
/* Tables (the image's, generated from the expressions the decomp gives)   */
/* ------------------------------------------------------------------------ */

static uint16_t s_pitchOctave[384];     /* 0x00345AE8: 0x4000 * 2^(i / 384), rounded */
static uint16_t s_panLaw[128];          /* 0x00345E10: 0x4000 * sin(pi/2 * i / 127), rounded */
static uint16_t s_velocity[128];        /* 0x00345F10: 0x4000 * i / 127, rounded */
static int16_t  s_sine[0x1400];         /* 0x002FFE98: (int)(4096 * sin(2 pi i / 4096) + 0.5) */
const uint16_t *g_velocity = s_velocity;

static void build_tables()
{
    static bool done = false;
    if (done)
        return;
    const double pi = 3.14159265358979323846;
    for (int i = 0; i < 384; i++)
        s_pitchOctave[i] = (uint16_t)std::lround(16384.0 * std::pow(2.0, i / 384.0));
    for (int i = 0; i < 128; i++) {
        s_panLaw[i] = (uint16_t)std::lround(16384.0 * std::sin(pi / 2 * i / 127.0));
        s_velocity[i] = (uint16_t)std::lround(16384.0 * i / 127.0);
    }
    for (int i = 0; i < 0x1400; i++)
        s_sine[i] = (int16_t)(int)(4096.0 * std::sin(2 * pi * i / 4096.0) + 0.5);
    done = true;
}

/* ------------------------------------------------------------------------ */
/* Voices (sound_driver.c / sound_seq.c)                                    */
/* ------------------------------------------------------------------------ */

struct SndVoice {
    uint8_t  state = 0, playing = 0, keyedOn = 0, priority = 0;
    uint8_t  dirty = 0, keyOnReq = 0, keyOffReq = 0;
    uint16_t volL = 0, volR = 0, pitch = 0;
    uint32_t addr = 0;
    uint16_t adsr1 = 0, adsr2 = 0;
    int8_t  *owner = nullptr;
    uint32_t age = 0;
    int32_t  unk20 = 0;
    uint8_t  unk28 = 0;
};

static SndVoice s_voices[NUM_VOICES];   /* g_002FF168.voices */
static uint16_t s_rateScale = 256;      /* g_002FF168.unk_06 = rate * 256 / 60 */
static SndNativeState s_spu;            /* g_00302698 */

/* spu_state_init (0x0025C2F8) */
static void spu_state_init()
{
    std::memset(&s_spu, 0, sizeof(s_spu));
    for (auto &v : s_spu.voice)
        v.flags = 0x50;
    for (int i = 0; i < 2; i++) {
        SndNativeCoreState &c = s_spu.core[i];
        c.effect.core = i;
        c.mmix = i ? 0xFCC : 0xFC0;
        c.unk_00 = c.unk_02 = 0x3FFF;
        c.evolL = c.evolR = 0x7FFF;
        c.effectEnable = 0;
        c.effect.mode = 0;
    }
    s_spu.unk_348 = s_spu.unk_34A = 0x3FFF;
}

void snd_init()
{
    build_tables();
    spu_state_init();
    for (auto &v : s_voices)
        v = SndVoice();
    s_rateScale = (60 << 8) / 60;
}

/* snd_set_reverb (0x00268990) with mode 4 (STUDIO_3): both cores. */
void snd_set_reverb_studio3(int depthL, int depthR)
{
    for (int c = 0; c < 2; c++) {
        SndNativeEffectAttr &e = s_spu.core[c].effect;
        std::memset(&e, 0, sizeof(e));
        e.core = c;
        e.mode = 4;
        e.depthL = (int16_t)depthL;
        e.depthR = (int16_t)depthR;
    }
    s_spu.dirty = 1;
}

/* snd_set_effect_volume (0x00268A48 -> spu_state_set_unk04) */
void snd_set_effect_volume(int l, int r)
{
    for (int c = 0; c < 2; c++) {
        s_spu.core[c].evolL = (uint16_t)l;
        s_spu.core[c].evolR = (uint16_t)r;
    }
}

/* F_00268978 -> spu_state_set_unk08 */
void snd_set_effect_enable(int on)
{
    s_spu.core[0].effectEnable = s_spu.core[1].effectEnable = (uint8_t)on;
}

static SndVoice *snd_voice_lookup(int8_t *handle)     /* 0x00267CE0 */
{
    int idx = *handle;
    if (idx >= 0 && s_voices[idx].owner == handle)
        return &s_voices[idx];
    return nullptr;
}

void snd_voice_release(int8_t *handle)                 /* 0x00267AF0 */
{
    SndVoice *v = snd_voice_lookup(handle);
    if (v) {
        v->owner = nullptr;
        *handle = -1;
    }
}

void snd_voice_keyoff(int8_t *handle)                  /* 0x00267B48 */
{
    SndVoice *v = snd_voice_lookup(handle);
    if (v) {
        v->keyOnReq = 0;
        v->keyOffReq = 1;
        v->playing = 1;
    }
}

void snd_voice_set_pitch(int8_t *handle, int pitch)    /* 0x00267BA8 */
{
    SndVoice *v = snd_voice_lookup(handle);
    if (v) {
        v->pitch = (uint16_t)pitch;
        v->dirty |= 1;
    }
}

void snd_voice_set_volume(int8_t *handle, int16_t l, int16_t r)    /* 0x00267C70 */
{
    SndVoice *v = snd_voice_lookup(handle);
    if (v) {
        v->dirty |= 2;
        v->volL = (uint16_t)l;
        v->volR = (uint16_t)r;
    }
}

static int snd_voice_alloc(int priority)                /* 0x00269798 */
{
    SndVoice *best = nullptr;
    int bestIdx = -1, bestPrio = priority;
    uint32_t bestAge = 0;
    for (int i = 0; i < NUM_VOICES; i++) {
        SndVoice *v = &s_voices[i];
        if (v->state == 0) {
            best = v;
            bestIdx = i;
            best->priority = (uint8_t)priority;
            return bestIdx;
        }
        if (v->state == 1) {
            int prio = v->priority;
            if (prio < bestPrio)
                continue;
            if (bestPrio == prio && v->age < bestAge)
                continue;
            best = v;
            bestPrio = prio;
            bestAge = v->age;
            bestIdx = i;
        }
    }
    if (best) {
        if (best->owner) {
            *best->owner = -1;
            best->owner = nullptr;
        }
        best->dirty = 0;
        best->keyOffReq = 1;
        best->keyOnReq = 0;
        best->priority = (uint8_t)priority;
    }
    return bestIdx;
}

static void snd_voice_start(int idx, const VoiceParam &p)   /* 0x00269890 */
{
    SndVoice &v = s_voices[idx];
    v.playing = 0;
    v.priority = p.priority;
    v.dirty = 7;
    v.keyOnReq = 1;
    v.keyOffReq = 0;
    v.volL = (uint16_t)p.volL;
    v.volR = (uint16_t)p.volR;
    v.addr = p.addr8;
    v.pitch = p.pitch;
    v.adsr1 = (uint16_t)p.adsr;
    v.adsr2 = (uint16_t)(p.adsr >> 16);
    v.age = 0;
    v.unk20 = s_rateScale * p.unk10;
    v.unk28 = p.flags & 1;
}

void snd_play_voice(int8_t *handle, const VoiceParam &p)    /* 0x002685A8 */
{
    int idx = snd_voice_alloc(p.priority);
    if (handle)
        *handle = (int8_t)idx;
    if (idx >= 0) {
        s_voices[idx].state = 1;
        s_voices[idx].owner = handle;
        snd_voice_start(idx, p);
    }
}

void snd_stop_all_voices()                              /* 0x002688F8 */
{
    for (auto &v : s_voices) {
        v.dirty = 2;
        v.volL = v.volR = 0;
        v.keyOffReq = 1;
        v.keyOnReq = 0;
        v.state = 0;
        if (v.owner) {
            *v.owner = -1;
            v.owner = nullptr;
        }
    }
    snd_flush();
}

uint16_t snd_note_to_pitch(int note, int base)          /* 0x002687B0 */
{
    int n = note - base;
    int octave = -2;
    if (n < 0) {
        do {
            n += 0xC00;
            octave--;
        } while (n < 0);
    } else {
        while (n >= 0xC00) {
            n -= 0xC00;
            octave++;
        }
    }
    uint16_t pitch = s_pitchOctave[(uint32_t)n >> 3];
    if (octave > 0)
        return (uint16_t)(pitch << octave);
    return (uint16_t)((int)pitch >> -octave);
}

void snd_pan_volume(int16_t *out, int vol, int pan)     /* 0x00269750 */
{
    out[0] = (int16_t)((vol * s_panLaw[0x7F - pan]) >> 14);
    out[1] = (int16_t)((vol * s_panLaw[pan]) >> 14);
}

/* snd_flush_voices (0x00269550): the voices into the state block, sent. */
void snd_flush()
{
    uint64_t active = snd_native_voice_active_mask();
    for (int n = 0; n < NUM_VOICES; n++, active >>= 1) {
        SndVoice &v = s_voices[n];
        SndNativeVoiceState &cmd = s_spu.voice[n];
        int flags = (uint16_t)cmd.flags & ~0xFA3;

        if (v.playing && v.keyOnReq == 0 && !(active & 1)) {
            v.playing = 0;
            v.keyOffReq = 1;
            if (v.state == 1) {
                if (v.owner)
                    *v.owner = -1;
                v.owner = nullptr;
                v.state = 0;
            }
        }
        if (v.keyOffReq) {
            if (v.keyedOn) {
                v.keyedOn = 0;
                flags |= SND_NATIVE_V_KEY_OFF;
            }
            v.keyOffReq = 0;
        }
        if (v.keyOnReq) {
            if (v.keyedOn)
                flags |= SND_NATIVE_V_KEY_OFF;
            v.keyedOn = 1;
            v.playing = 1;
            v.keyOnReq = 0;
            flags |= SND_NATIVE_V_KEY_ON;
        }
        if (v.playing)
            v.age++;
        if (v.unk28)
            flags |= 0xA0;
        if (v.unk20)
            flags |= SND_NATIVE_V_ONE_SHOT;
        if (v.dirty & 1)
            flags |= SND_NATIVE_V_PITCH;
        if (v.dirty & 2)
            flags |= SND_NATIVE_V_VOLUME;
        if (v.dirty & 4)
            flags |= SND_NATIVE_V_ADSR;
        cmd.flags = (int16_t)flags;
        cmd.pitch = (int16_t)((int)(v.pitch * 0xEB3) / 4096);
        cmd.volL = v.volL & 0x7FFF;
        cmd.volR = v.volR & 0x7FFF;
        cmd.addr = (int32_t)(v.addr << 3);
        cmd.adsr1 = v.adsr1;
        cmd.adsr2 = v.adsr2;
        v.dirty = 0;
    }
    /* pdispu2_send_state: the block is copied, its dirty flag cleared */
    SndNativeState copy = s_spu;
    s_spu.dirty = 0;
    snd_native_send_state(&copy);
}

int voices_in_use()
{
    int n = 0;
    for (auto &v : s_voices)
        if (v.playing)
            n++;
    return n;
}

/* ------------------------------------------------------------------------ */
/* Sequencer                                                                */
/* ------------------------------------------------------------------------ */

struct SeqTrack {
    const uint8_t *pos = nullptr;
    int32_t  wait = 0;
    uint16_t volume = 0, volScaled = 0;
    int16_t  bend = 0;
    uint8_t  pan = 0, program = 0;
    int8_t   loopDepth = -1;
    const uint8_t *loopPos[4] = {};
    uint8_t  loopCount[4] = {};
};

struct Sequence {
    Sequence *next = nullptr;
    uint16_t volume = 0, volEff = 0;
    int32_t  tempo = 0, tickAcc = 0, frameStep = 0, ticks = 0;
    const InstBank *bank = nullptr;
    SeqTrack tracks[NUM_TRACKS];
};

struct SeqChannel {
    Sequence *seq = nullptr;
    SeqTrack *track = nullptr;
    const Program *prog = nullptr;
    const Zone *zone = nullptr;
    int32_t  remain = 0;
    uint16_t lastVol = 0;
    int16_t  lastBend = 0;
    uint8_t  note = 0, velocity = 0, lastPan = 0;
    int8_t   voice = -1;
};

static struct {
    Sequence *head = nullptr;
    uint16_t frameRate = 60;
    uint16_t volume = 0x3FFF;
    uint8_t  nextChannel = 0;
    SeqChannel ch[NUM_CHANNELS];
} s_seq;

static Sequence s_sequence;             /* music_play's g_musicSequence */

void seq_init()                                         /* 0x00268508 */
{
    s_seq.head = nullptr;
    s_seq.volume = 0x3FFF;
    s_seq.nextChannel = 0;
    for (auto &c : s_seq.ch)
        c = SeqChannel();
    s_seq.frameRate = 60;
}

static void seq_unlink(Sequence *seq)                   /* 0x00268A60 */
{
    Sequence *prev = nullptr;
    for (Sequence *n = s_seq.head; n; prev = n, n = n->next) {
        if (n == seq) {
            if (prev)
                prev->next = n->next;
            else
                s_seq.head = n->next;
            break;
        }
    }
}

static void seq_update_volume(Sequence *seq)            /* 0x00268860 */
{
    int vol = (s_seq.volume * seq->volume) >> 14;
    seq->volEff = (uint16_t)vol;
    for (auto &t : seq->tracks)
        t.volScaled = (uint16_t)((uint32_t)(vol * t.volume) >> 14);
}

/* seq_note_on (0x00268C40) */
static void seq_note_on(Sequence *seq, SeqTrack *t, int note, int velocity, int duration)
{
    const InstBank *bank = seq->bank;
    if (t->program >= bank->programs.size())
        return;
    const Program *prog = &bank->programs[t->program];
    int vol = (((t->volScaled * prog->vol) >> 14) * s_velocity[velocity & 0x7F]) >> 14;
    int progPan = t->pan + prog->pan;

    for (size_t j = 0; j < prog->zones.size(); j++) {
        if (prog->zones[j] >= bank->zones.size())
            continue;
        const Zone *zone = &bank->zones[prog->zones[j]];
        if (note < zone->lo || zone->hi < note)
            continue;
        int idx = s_seq.nextChannel;
        for (int tries = 0; tries < NUM_CHANNELS; tries++) {
            idx++;
            if (idx == NUM_CHANNELS)
                idx = 0;
            SeqChannel *ch = &s_seq.ch[idx];
            if (ch->voice < 0) {
                int pitch = note * 0x100;
                s_seq.nextChannel = (uint8_t)idx;
                int zoneVol = (vol * zone->vol) >> 14;
                int pan = progPan + zone->pan - 0x80;
                pan = pan < 0 ? 0 : (pan > 0x7F ? 0x7F : pan);
                int16_t bend = t->bend;
                ch->note = (uint8_t)note;
                ch->velocity = (uint8_t)velocity;
                ch->lastBend = bend;
                ch->seq = seq;
                ch->remain = duration + seq->ticks;
                ch->lastVol = t->volScaled;
                ch->track = t;
                ch->prog = prog;
                ch->zone = zone;
                ch->lastPan = t->pan;
                VoiceParam p;
                p.adsr = zone->adsr;
                p.addr8 = zone->addr8;
                p.priority = zone->prio;
                p.unk10 = zone->unk0A;
                p.flags = zone->flags;
                if (bend != 0)
                    pitch += (bend * ((bend > 0) ? zone->bendUp : zone->bendDown)) >> 5;
                p.pitch = snd_note_to_pitch(pitch, zone->baseNote);
                int16_t lr[2];
                snd_pan_volume(lr, (zoneVol * zoneVol) >> 14, pan);
                p.volL = lr[0];
                p.volR = lr[1];
                snd_play_voice(&ch->voice, p);
                break;
            }
        }
    }
}

/* seq_run_tracks (0x00268ED8) */
static void seq_run_tracks(Sequence *seq)
{
    for (auto &tr : seq->tracks) {
        SeqTrack *track = &tr;
        const uint8_t *pos = track->pos;
        if (!pos)
            continue;
        track->wait -= seq->ticks;
        if (track->wait <= 0) {
            do {
                uint32_t c = *pos++;
                if (c & 0x80) {
                    uint32_t velocity = *pos++;
                    uint32_t length;
                    c &= 0x7F;
                    length = *pos++;
                    if (length & 0x80) {
                        uint32_t byte;
                        length &= 0x7F;
                        do {
                            byte = *pos++;
                            length = (length << 7) + (byte & 0x7F);
                        } while (byte & 0x80);
                    }
                    seq_note_on(seq, track, (int)c, (int)velocity, (int)(length + seq->ticks + track->wait));
                } else if (c & 0x40) {
                    track->bend = (int16_t)((*pos++ | ((c & 0x3F) << 8)) - 0x2000);
                } else if (c & 0x20) {
                    int depth = track->loopDepth + 1;
                    if (depth > 3)
                        depth = 3;
                    track->loopCount[depth] = (uint8_t)((c & 0x1F) + 1);
                    track->loopPos[depth] = pos;
                    track->loopDepth = (int8_t)depth;
                } else {
                    switch (c) {
                    case 0:
                        pos = nullptr;
                        goto store;
                    case 1: {
                        int depth = track->loopDepth + 1;
                        if (depth > 3)
                            depth = 3;
                        track->loopCount[depth] = *pos++;
                        track->loopPos[depth] = pos;
                        track->loopDepth = (int8_t)depth;
                        break;
                    }
                    case 2:
                        if (track->loopDepth < 0)
                            break;
                        if (track->loopCount[track->loopDepth] != 0) {
                            if (track->loopCount[track->loopDepth] != 0xFF)
                                track->loopCount[track->loopDepth]--;
                            pos = track->loopPos[track->loopDepth];
                        } else {
                            track->loopDepth--;
                        }
                        break;
                    case 3:
                        track->program = *pos++;
                        break;
                    case 4:
                        track->volume = s_velocity[*pos++ & 0x7F];
                        track->volScaled = (uint16_t)((seq->volEff * track->volume) >> 14);
                        break;
                    case 5:
                        track->pan = *pos++;
                        break;
                    case 6:
                        seq->tempo = (pos[0] << 16) | (pos[1] << 8) | pos[2];
                        pos += 3;
                        break;
                    default:
                        break;
                    }
                }
                c = *pos++;
                if (c & 0x80) {
                    uint32_t byte;
                    c &= 0x7F;
                    do {
                        byte = *pos++;
                        c = (c << 7) + (byte & 0x7F);
                    } while (byte & 0x80);
                }
                track->wait += (int32_t)c;
            } while (track->wait <= 0);
        }
    store:
        track->pos = pos;
    }
}

/* seq_update_channels (0x00269328) */
static void seq_update_channels()
{
    for (auto &chr : s_seq.ch) {
        SeqChannel *ch = &chr;
        if (ch->voice < 0)
            continue;
        if (ch->seq == nullptr) {
            snd_voice_keyoff(&ch->voice);
            snd_voice_release(&ch->voice);
            continue;
        }
        if (ch->remain > 0) {
            ch->remain -= ch->seq->ticks;
            if (ch->remain <= 0)
                snd_voice_keyoff(&ch->voice);
        }
        if (ch->track->bend != ch->lastBend) {
            const Zone *zone = ch->zone;
            int range = (ch->track->bend > 0) ? zone->bendUp : zone->bendDown;
            snd_voice_set_pitch(&ch->voice,
                                snd_note_to_pitch((ch->note << 8) + ((range * ch->track->bend) >> 5), zone->baseNote));
            ch->lastBend = ch->track->bend;
        }
        if (ch->track->volScaled != ch->lastVol || ch->track->pan != ch->lastPan) {
            const Program *prog = ch->prog;
            const Zone *zone = ch->zone;
            int gain = (((((ch->track->volScaled * prog->vol) >> 14) * s_velocity[ch->velocity & 0x7F]) >> 14)
                        * zone->vol) >> 14;
            int pan = zone->pan + ch->track->pan + prog->pan - 0x80;
            pan = pan < 0 ? 0 : (pan > 0x7F ? 0x7F : pan);
            int16_t lr[2];
            snd_pan_volume(lr, (gain * gain) >> 14, pan);
            snd_voice_set_volume(&ch->voice, lr[0], lr[1]);
            ch->lastVol = ch->track->volScaled;
            ch->lastPan = ch->track->pan;
        }
    }
}

/* seq_start (0x00268AB0) + seq_restart (0x002688A8), as music_play uses
 * them, with the master volume set first (music_set_volume). */
Sequence *seq_play(const SeqBank &seqs, int entry, const InstBank &bank, uint16_t masterVolume)
{
    Sequence *seq = &s_sequence;
    seq_stop(seq);
    s_seq.volume = masterVolume;
    const SeqEntry &e = seqs.entries[entry];
    seq_unlink(seq);
    seq->volume = e.volume;
    seq->bank = &bank;
    seq->next = nullptr;
    seq->volEff = (uint16_t)((s_seq.volume * e.volume) >> 14);
    int step = 0x1C9C3800 / (int)s_seq.frameRate;
    seq->frameStep = step;
    seq->tickAcc = step;
    seq->tempo = e.tempo;
    for (int k = 0; k < NUM_TRACKS; k++) {
        SeqTrack &t = seq->tracks[k];
        t = SeqTrack();
        if (e.track[k] < 0) {
            t.pos = nullptr;
            continue;
        }
        const uint8_t *pos = seqs.data.data() + e.track[k];
        t.wait = *pos++;
        if (t.wait & 0x80) {
            uint32_t byte;
            t.wait &= 0x7F;
            do {
                byte = *pos++;
                t.wait = (t.wait << 7) + (byte & 0x7F);
            } while (byte & 0x80);
        }
        t.pos = pos;
        t.volume = 0x4000;
        t.volScaled = seq->volEff;
        t.bend = 0;
        t.pan = 0;
        t.program = 0;
        t.loopDepth = -1;
    }
    seq_unlink(seq);
    seq_update_volume(seq);
    seq->next = s_seq.head;
    s_seq.head = seq;
    return seq;
}

void seq_stop(Sequence *s)                              /* 0x002694E8 */
{
    seq_unlink(s);
    for (auto &c : s_seq.ch)
        if (c.seq == s)
            c.seq = nullptr;
}

/* seq_update (0x002691F0), fades left out (unused in GT3) */
void seq_update()
{
    for (Sequence *seq = s_seq.head; seq; seq = seq->next) {
        seq->ticks = 0;
        while (seq->tickAcc > 0) {
            seq->tickAcc -= seq->tempo;
            seq->ticks++;
        }
        seq->tickAcc += seq->frameStep;
        seq_run_tracks(seq);
    }
    seq_update_channels();
}

bool seq_finished(const Sequence *s)
{
    for (auto &t : s->tracks)
        if (t.pos)
            return false;
    for (auto &c : s_seq.ch)
        if (c.voice >= 0)
            return false;
    return true;
}

/* ------------------------------------------------------------------------ */
/* Engine sound (GtSoundVoice)                                              */
/* ------------------------------------------------------------------------ */

static void engine_layer_pitch(EngineVoice &es, int i, int rpm, int smooth)    /* 0x00267D18 */
{
    const EngnSeg &seg = (*es.segs)[es.layer[i].seg];
    uint32_t pitch = (uint32_t)(seg.pitch * rpm) / (uint32_t)(int)seg.baseRpm;
    pitch = (pitch * es.pitch) >> 12;
    if (pitch == 0)
        pitch = 1;
    if (smooth) {
        uint32_t old = es.layer[i].pitch;
        if (old * 2 < pitch)
            pitch = old * 2;
        else if (pitch < (old >> 1))
            pitch = old >> 1;
    }
    es.layer[i].pitch = (uint16_t)pitch;
}

static void engine_layer_volume(EngineVoice &es, int i, int rpm, int smooth)   /* 0x00267D98 */
{
    const EngnSeg &seg = (*es.segs)[es.layer[i].seg];
    int fade = 0x4000;
    int o = es.layer[(i + 1) & 1].seg;
    if (o >= 0) {
        const EngnSeg &oseg = (*es.segs)[o];
        if (rpm < seg.lo)
            fade = ((rpm - oseg.hi) << 14) / (seg.lo - oseg.hi);
        else if (seg.hi < rpm)
            fade = ((oseg.lo - rpm) << 14) / (oseg.lo - seg.hi);
    }
    if (!(fade > -1))
        fade = 0;
    fade = (s_sine[((uint32_t)fade >> 4) & 0xFFF] * 0x3FFF) >> 12;
    fade = (fade * seg.vol) >> 14;
    int volL = (es.volL * fade) >> 14;
    int volR = (es.volR * fade) >> 14;
    if (smooth) {
        auto slew = [](int v, int old) {
            int neg = v < 0;
            if (neg)
                v = -v;
            old = old > -1 ? old : -old;
            if (old + 0x1000 < v)
                v = old + 0x1000;
            else if (v < old - 0x1000)
                v = old - 0x1000;
            return neg ? -v : v;
        };
        volL = slew(volL, es.layer[i].volL);
        volR = slew(volR, es.layer[i].volR);
    }
    es.layer[i].volL = (int16_t)volL;
    es.layer[i].volR = (int16_t)volR;
}

void engine_setup(EngineVoice &es, const EngnBank &bank, int set, int prio)    /* 0x00268208 */
{
    build_tables();
    es.prio = (uint8_t)prio;
    es.segs = &bank.sets[set];
    es.pitch = 0x1000;
    es.volL = es.volR = 0x3FFF;
    for (auto &l : es.layer) {
        l.voice = -1;
        l.seg = -1;
        l.prev = -1;
    }
}

void engine_update(EngineVoice &es)                     /* 0x00267F30 */
{
    const std::vector<EngnSeg> &segs = *es.segs;
    int rpm = es.rpm < 200 ? 200 : es.rpm;
    const EngnSeg *prev = nullptr;
    for (size_t i = 0; i < segs.size(); i++) {
        const EngnSeg *seg = &segs[i];
        if (rpm >= seg->lo && seg->hi >= rpm) {
            es.layer[0].seg = (int8_t)i;
            es.layer[1].seg = -1;
            break;
        }
        if (prev && prev->hi < rpm && rpm < seg->lo) {
            es.layer[1].seg = (int8_t)i;
            es.layer[0].seg = (int8_t)(i - 1);
            break;
        }
        prev = seg;
    }
    int8_t seg0 = es.layer[0].seg;
    if (seg0 == es.layer[1].prev || es.layer[1].seg == es.layer[0].prev) {
        es.layer[0].seg = es.layer[1].seg;
        es.layer[1].seg = seg0;
    }
    for (int i = 0; i < 2; i++) {
        auto &L = es.layer[i];
        if (L.seg == L.prev) {
            if (L.voice >= 0) {
                if (L.seg >= 0) {
                    engine_layer_pitch(es, i, rpm, 1);
                    engine_layer_volume(es, i, rpm, 1);
                    snd_voice_set_pitch(&L.voice, L.pitch);
                    snd_voice_set_volume(&L.voice, L.volL, L.volR);
                }
                continue;
            }
        } else if (L.voice >= 0) {
            snd_voice_keyoff(&L.voice);
            snd_voice_release(&L.voice);
        }
        if (L.seg >= 0) {
            engine_layer_pitch(es, i, rpm, 0);
            engine_layer_volume(es, i, rpm, 0);
            L.addr = segs[L.seg].addr >> 3;
            VoiceParam p;
            p.volL = L.volL;
            p.volR = L.volR;
            p.pitch = L.pitch;
            p.addr8 = L.addr;
            p.adsr = 0x3000F;           /* ENGINE_ADSR */
            p.priority = es.prio;
            p.flags = 1;
            p.unk10 = 0;
            snd_play_voice(&L.voice, p);
        }
        L.prev = L.seg;
    }
}

void engine_stop(EngineVoice &es)                       /* 0x00268278 */
{
    for (auto &L : es.layer)
        if (L.voice >= 0) {
            snd_voice_keyoff(&L.voice);
            snd_voice_release(&L.voice);
        }
}

} /* namespace ee */
