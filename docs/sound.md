# Sound

How GT3 makes sound on the PS2, the data formats, and the native back end
that replaces the IOP side (`src/engine/sound/native/`, test tool
`tools/sndtest/`). Addresses: EE functions are GT3 US retail addresses (the
`// FUNC` markers of `decomp/core_gt3enhanced`); IOP addresses are `.text`
offsets of the modules in `gt3_disc/files/` (an IRX is linked at 0).

## 1. The PS2 side

### 1.1 Overview

```
 game/sound            T176 CarSound, T177 RaceAmbientSound, T288 / T333 car and tyre
   |                   parameters, T458 course sounds, T463 music, T464 BGM front end,
   |                   T466 ADS, T468 banks / handles, T469 effects, T470 init,
   |                   T130 RaceBGMPS2, T529 mpegif (movies)
 engine/sound          the voice manager, sequencer and engine-sound voices (EE code)
   |  once a frame: snd_flush_voices -> the SPU2 state block
 engine/pdi            RPC clients: T520 pdispu2 ('SPUP', 'SPUV'), T521 pdibgm ('PBGM'),
   |                   T522 pdimpeg ('MPG1' / 'MPG2'), T523 pdipipe ('PDIP')
 ===== SIF RPC =====
 pdispu2.irx           "PDI_SPU2_Manager": applies the state block, voice status,
   |                   stream voices / ADMA, transfer queue
 pdistr.irx            "PDI_Streaming_service": PBGM, MPG1 / MPG2, PDIP
 libsd.irx             Sony's SPU2 library (registers, effect presets)
 SPU2                  2 cores x 24 voices, 2 MB sound RAM, reverb per core
```

All synthesis decisions (which sample, pitch, volume, pan, envelope,
priority, voice stealing, the music sequencer, engine crossfades) are EE
code. The IOP only writes registers. The native port keeps the EE code and
replaces the IOP and the chip.

### 1.2 The EE voice manager (engine/sound)

`sound_driver.c` / `sound_seq.c`. 46 voices (`SndVoiceBank g_002FF168`):
EE voice *i* is SPU2 core *i / 24*, voice *i % 24* (core 1 voices 22 and 23
are left to the BGM stream). An owner holds an `s8` voice handle; the voice
points back at it and writes -1 into it when stolen or ended.

| Function | Address | What it does |
|---|---|---|
| `snd_init` | 0x00268470 | state block init, 46 voices cleared, rate 60 |
| `snd_play_voice` | 0x002685A8 | `snd_voice_alloc` (free voice, else steal the highest priority value >= the request, oldest first) + `snd_voice_start` |
| `snd_voice_start` | 0x00269890 | voice <- `SndVoiceParam` (vol pair, pitch, addr / 8, ADSR pair, `unk_10` -> one-shot, `flags` bit 0 -> reverb send), key-on request, all dirty bits |
| `snd_voice_keyoff` / `_release` / `_set_pitch` / `_set_volume` / `_set_priority` | 0x00267B48 / 0x00267AF0 / 0x00267BA8 / 0x00267C70 / 0x00267C18 | through the handle |
| `snd_note_to_pitch` | 0x002687B0 | 8.8 semitone note vs zone base note -> pitch, 0x1000 = 44.1 kHz |
| `snd_pan_volume` | 0x00269750 | equal-power pan law (`g_snd_pan_law`) |
| `snd_flush` / `snd_flush_voices` | 0x00269530 / 0x00269550 | once a frame (music thread): voice-active mask from the last answer, voices that ended are freed, key flags, dirty bits; pitch x 0xEB3 / 4096 (44.1 -> 48 kHz); sends the block |
| `snd_set_reverb` | 0x00268990 | effect attributes of both cores (music_init: mode 4 STUDIO_3, depth 0xFFF) |
| `snd_set_effect_volume` | 0x00268A48 | EVOL of both cores (music_init 0xFFF; RaceAmbientSound: tunnel level x 32767; stop: 0xFFF) |
| `F_00268978` | 0x00268978 | effect enable of both cores (music_init: 1) |
| `snd_stop_all_voices` | 0x002688F8 | all voices to key-off, volume 0, flushed |
| sequencer | 0x00268508..0x00269A68 | `seq_start`, `seq_run_tracks` (byte code), `seq_note_on` (47 channels), `seq_update` (tempo ticks per frame), `seq_update_channels` (durations, bends, volumes) |
| engine sound | 0x00267D18..0x002682D0 | `engine_update`: the rpm segment(s) of an ENGN set on two layers, equal-power crossfade, pitch = seg.pitch x rpm / base rpm, slew-limited; ADSR 0x000F / 0x0003, reverb send on |

The music thread (`music_thread` 0x0022AFF8) wakes on every VBLANK_E
(`music_vblank_handler` 0x0022AFC8, a semaphore) and runs `seq_update`,
`snd_flush`, `pdibgm_update`, `clock_update`.

### 1.3 The RPC protocol

| Server | fno | EE caller | Arguments | IOP handler | Effect |
|---|---|---|---|---|---|
| 'SPUP' pdispu2 | 0 | (none) | - | 0x001B00 | returns 0x3790 (module info) |
| 'SPUP' | 3 | `pdispu2_request_status` 0x0025C0F8 (no callers) | - | 0x000F6C | recomputes the voice-active mask; answer word 0 |
| 'SPUP' | 4 | `pdispu2_send_state` 0x0025C140 (NOWAIT, from `snd_flush_voices`) | `SpuState`, 0x350 bytes | 0x0010A4 | applies the block (below); answer: the mask (0x40 bytes to `g_002FCA00`), read next frame by `pdispu2_status` 0x0025C500 |
| 'SPUP' | 5 | `F_0025C280` 0x0025C280 (`sound_init`: 1, 1, 1) | 3 words | 0x001E24.. 0x001E54 | `sceSdSetCoreAttr(SPDIF_MODE, ...)` |
| 'SPUV' pdispu2 | 1 / 2 | `F_0025C088` / `F_0025C0C0` (no callers) | {spuAddr, buf, size} | 0x0019D0 (dir 1 / 0) | EE <-> SPU RAM transfer through the IOP (direction of each not confirmed) |
| 'PDIP' pdistr | 2, 3, 1 | `pdipipe_open` / `pdipipe_read2` / `pdipipe_close` | file streaming | 0x002298 | |
| 'PDIP' | 4 | `pdipipe_read2(h, spuAddr, size, 1)` 0x0025D5F0, from `file_read_sound_bank` 0x002418D0 | {h, spuAddr, size} | 0x002298 -> pdispu2 export 35 | file data -> SPU RAM (`sceSdVoiceTrans` queue): the samples of every sound bank |
| 'PBGM' pdistr | 1 | `pdibgm_start` 0x0025C8C8 (from `ads_play` 0x0022AC98) | {lsn, sectors, loop, bufSize 0x40000, host name} | 0x000A70 -> 0x0004E8 | opens the .ads, starts the reader and the stream voices |
| 'PBGM' | 2 | `pdibgm_stop` 0x0025C9D0 | - | 0x000AB8 -> 0x0006B4 | stops |
| 'PBGM' | 3 | `pdibgm_set_volume` 0x0025C9F8 | {clamp(v, -1, 1) x 32768} | 0x000AC8 -> 0x0006F8 | the volume object's word 0 (as a halfword) |
| 'PBGM' | 4 / 5 | `pdibgm_pause` / `_resume` (RaceBGMPS2) | - | 0x000AF8 / 0x000B08 | clock pause flag (export 47 / 48) |
| 'PBGM' | 6 | `pdibgm_update` 0x0025CA78 (music thread) | - | 0x000B18 | status: {s64 time us, s64 pos, id, ended} to `g_002FCE00` |
| 'MPG1' pdistr | 1, 2, 4 | `pdimpeg_open / _feed / _close` (T522) | open mode 2 = with sound | 0x00127C | a PSS: video to the EE, the audio to the stream player |
| 'MPG2' | 3 | `F_0025D178` | 2 doublewords | 0x00127C | (movie timing; not analysed) |

### 1.4 What pdispu2 does with the state block ('SPUP' 4, 0x0010A4)

`SpuState` (engine/pdi `T520_pdispu2.h`): 48 x `SpuVoiceState` {s16 flags,
s16 pitch, u16 volL, u16 volR, s32 addr (bytes), u16 adsr1, u16 adsr2},
2 x `SpuCoreState` {u16 0x3FFF x 2 (unused), u16 EVOL L / R, u8 effect
enable, u32 MMIX, sceSdEffectAttr}, 2 unused halfwords, `dirty`.

Voice flags: 0x001 key-off, 0x002 key-on, 0x004 PMON, 0x008 noise,
0x010 VMIXL (dry L), 0x020 VMIXEL (reverb L), 0x040 VMIXR, 0x080 VMIXER,
0x100 one-shot (ends at its ENDX), 0x200 set pitch, 0x400 set volume,
0x800 set start address + ADSR. `spu_state_init` gives every voice 0x50
(dry, no reverb); the EE rebuilds bits 0xFA3 every flush (0xA0 = the zone's
reverb flag, 0x100 = zone `unk_0A` != 0, which GT3's banks set exactly for
the samples that do not loop).

Order (one call, per core where it says so):
1. Per core masks from the flags; `active |= keyon`; `keyOffPending =
   (keyOffPending | keyoff) & ~keyon`; `oneShot = flag 0x100` (.bss 0x3870 +
   core x 0x2C: +0x20 / +0x24 / +0x28).
2. Voices to key on: VOLL / VOLR = 0 (their volume comes after the key-on).
3. KOFF (queued to the switch thread 0x000E24, which writes KOFF / KON), wait 100 us.
4. If `dirty`: `sceSdSetEffectAttr` per core.
5. Per core: effect enable (core attr), MMIX, EVOLL / EVOLR.
6. Per voice: PITCH (0x200); VOLL / VOLR (0x400, not for key-ons); SSA + ADSR1 + ADSR2 (0x800).
7. Per core: PMON, NON, VMIXL, VMIXEL, VMIXR, VMIXER (all 24 voices, the BGM's 22 / 23 too: dry).
8. KON; then the key-on voices' VOLL / VOLR. Wait 100 us.
9. Status (0x000F6C): `ended = (ENDX & oneShot) | {keyOffPending voices with ENVX == 0}`;
   `active &= ~ended`, `keyOffPending &= ~ended`; answer =
   `active[0] | active[1] << 24` (64 bits).

pdispu2's start-up (0x001F28): sceSdInit; MVOL 0; EEA 0x1DFFFF / 0x1FFFFF
(the reverb work areas end there; ESA 0x1D8800 / 0x1F8800 written but
libsd places ESA below EEA by the mode's size); clear the STUDIO_3 area,
effect mode 0; all voice volumes 0; PMON / NON 0; VMIXL / VMIXR all,
VMIXEL / VMIXER none; MMIX 0xFC0 (core 0: voices dry + wet, sound-data
input dry) / 0xFCC (core 1: also core 0's output dry); EVOL 0x7FFF; AVOL
core 1 0x7FFF; BVOL 0; then MVOL 0x3FFF.

### 1.5 The BGM stream (pdistr + pdispu2)

pdistr 'PBGM' queues commands to a thread (0x0008F4; table 0x2828:
0 volume, 1 start, 2 pause, 3 resume, 4 stop). Start (0x0004E8) opens the
file and starts the reader (0x000100): it parses the head (SShd type 1 ->
the PCM path, pdispu2 exports 58 / 59, core 1 ADMA input at BVOL; type 0x10
-> the ADPCM path, export 52 / 53), then copies the body into an IOP FIFO
in 0x1800-byte pieces; with the loop flag it seeks back to the body
(0x00031C) and counts the loop.

pdispu2's ADPCM stream (export 53, 0x000498) plays core 1 voices 22 (left)
and 23 (right) from two 0x800-byte rings in SPU RAM (0x1D8000, 0x1F8000):

- voice setup (0x000068): SSA = the half to play, ADSR1 0x000F, ADSR2 0,
  pitch = rate x 4096 / 48000 (0xEB3 for 44.1 kHz), volume gate open,
  IRQA at the next quarter, KON; stop (0x000134): pitch 0, gate closed,
  KOFF;
- each 0x800-byte chunk of the FIFO (0x400 left + 0x400 right) is copied
  into one half of both rings; the loop flags of the half's first and last
  block are patched (0x0001B0: half 0 starts with loop-start, half 1 ends
  with loop-end + repeat), so the voices circle the ring without a break
  and the ADPCM history carries on;
- the IRQ handler (0x000304) follows the voice by quarters (NAX): entering
  a half, it asks for a refill and advances the clock by 0x700 samples
  (export 45); a refill not done by the middle of the next half stops the
  voices (an underrun; this is also how a pause silences them);
- volume (callback 0x000424): left VOLL = right VOLR = (v x 1 x 1 x gate
  chain, 0x001C30, clamped to 16 bits) >> 1 & 0x3FFF, where v is the
  'PBGM' 3 value stored as a halfword (32768 = 0x8000 = unity); the other
  volume of each voice stays 0;
- the clock (exports 41..49): `pos += 0x700` per half entered, wrapping
  past the song's length (`id` counts the wraps), `time = pos x 10^6 /
  (pitch x 48000 / 4096)` us; status `ended = pos >= length`.

RaceBGMPS2 (T130) reads the status every frame: markers by `time`, a new
song when `ended`, a restart of its markers when `id` changes.

A PSS movie with sound goes through the same player: pdistr's 'MPG1'
(0x00127C) demuxes private stream 1 (substream 0xFF, a 4-byte prefix per
payload) and feeds it to the reader as if it were an .ads file;
`mpegif::feed` (0x0025EF00) sets the BGM volume to 1.0 first. GT3's
opening / ending movies carry exactly an .ads image (ADPCM 44.1 kHz stereo,
interleave 0x400).

PC: a mod's MP4 movie in a PSS's place (movies.md 5) has AAC sound, which
is not re-encoded to SPU2 ADPCM for the BGM voices: decoded to 16-bit PCM,
it goes through `snd_native_pcm_*` (sound_native.h), mixed into
`snd_native_render`'s output after the SPU2, resampled to 48 kHz (4-point
cubic), at the gain the BGM voices would give it (the BGM volume's chain,
core 1's MVOL), about 2 s buffered, started, held and timed
(`snd_native_pcm_played_us`) as the pushed .ads is.

### 1.6 Game-side users (game/sound)

| Unit | Functions | Uses |
|---|---|---|
| T468 | `se_play_program` 0x0022BBF8, `GtSoundHandle::play` 0x0022BD00 / `stop` 0x0022BEA0, `GtSoundChannel` (Pod), `GTSOUNDINSTRUMENT::transfer` 0x0022BFE0 / `load` 0x0022C0B8 / `loadFile` 0x0022C140 | voices; banks: header to EE RAM, samples to SPU RAM (`file_read_sound_bank`) |
| T469 | `se_init` 0x0022C3E8, `se_play` 0x0022C658 | gt3sys.ins at SPU 0x1E0000, gt3se01 / gt3se02.ins from 0x5040, rn_01.es, (gt3pod.ins), se.inf |
| T463 | `music_init` 0x0022B058, `music_load` 0x0022B338, `music_play` 0x0022B460, `music_set_volume` 0x0022B2F8 | reverb / effect setup; music.ins at SPU 0xAC000; the sequencer |
| T464 / T466 | `bgm_play*`, `ads_play` 0x0022AC98, `ads_set_volume` 0x0022AC78, `ads_stop` 0x0022AE18 | groups 0..0x11 sequenced, 0x12..0x22 ADS |
| T176 | `CarSound::load` 0x001A6A60, `updateEngine` 0x001A7140, `updateSlot` 0x001A74A0, `playOneShot` 0x001A6ED8 | engine banks (`sound/engine/%05d`, `sound/<kind>/%05d`) in a 0x25800-byte SPU area per car from 0xAC000; volumes may be negative (behind the listener: phase-inverted, as the SPU2's signed volumes allow) |
| T177 | `RaceAmbientSound::update` 0x001A79C0 / `stop` 0x001A8000 | ambience: the effects "air", "city", "crowds", "sea" (gt3se02.ins zones 0, 7, 6, 9, looped) levelled, panned and Doppler-pitched from the course's sound part (T458, 2.6) for each view's camera; the "tunnel" / "tunnel2" pairs (zones 2 / 3, 4 / 5) and the tunnel reverb (EVOL) from the runway's attr2 |
| T458 | `course_sound_relocate` 0x00229370, `course_sound_locate` 0x002293F0, `course_sound_mix` 0x00229450 | the course file's sound part (CourseData +0x8C, "CRSS"): positions only, no samples |
| T130 | `RaceBGMPS2::update` 0x0017DDF0, `pause` / `resume` | BGM status, volume, pause |
| T529 | `mpegif::feed` 0x0025EF00 | movie audio volume |

### 1.7 SPU RAM map (bytes)

| Range | Content |
|---|---|
| 0x000000..0x00503F | left to the chip (its sound-data input and output capture areas); GT3's allocator starts at 0x5040 |
| 0x005040.. | gt3se01.ins 0x005040..0x046E60, gt3se02.ins ..0x079580, rn_01.es ..0x093D50, (gt3pod.ins): must end below 0xAC000. Loaded once (`se_init`), never written again |
| 0x0AC000.. | menus: music.ins (0x0AC000..0x1D2E00); races: the cars' engine banks, 0x25800 each (8 cars end at 0x1D8000; the largest pair of the disc's banks is 0x25170 bytes), loaded by `file_req_run` in `RaceBase::virtual_8`. Nothing else is uploaded during a race; the course file adds no samples |
| 0x1D8000..0x1D87FF | BGM ring, left |
| 0x1D9021..0x1DFFFF | core 0 reverb work area (STUDIO_3: 0x6FE0 bytes, ending at EEA) |
| 0x1E0000..0x1F5DFF | gt3sys.ins |
| 0x1F8000..0x1F87FF | BGM ring, right |
| 0x1F9021..0x1FFFFF | core 1 reverb work area |

## 2. Data formats

### 2.1 SPU2 ADPCM

16-byte blocks of 28 samples: byte 0 = shift (bits 0-3) | filter (bits
4-7, 0..4; 5..15 act as 0), byte 1 = flags (1 loop end: ENDX and jump to the
loop start address; 2 repeat: without it the voice stops at a loop end;
4 loop start: LSA = this block), 14 bytes of 4-bit samples (low nibble
first). `s = (nibble << 12 >> shift) + (p1 x f0 + p2 x f1 + 32) >> 6`,
clamped; filters (60, 0), (115, -52), (98, -55), (122, -60) / 64. The
history continues across blocks and loop jumps. GT3's banks: looped samples
end with flag 3 (loop start flag 4 / 6 earlier), one-shots end with flag 1.

### 2.2 Sound banks (.ins "INST", .es "ENGN")

Common head (`SoundBankHeader`, little endian): +0 magic, +4 self (0 in the
file), +8 header size kept in EE RAM, +0xC SPU base (0 in the file), +0x10
sample bytes, +0x14 file offset of the samples (the header part, rounded).
`file_read_sound_bank` reads the head, the header part into EE RAM and
streams the samples to the SPU address; `inst_relocate` / `engn_relocate`
add the load address to the pointers and the SPU base to the sample
addresses.

INST: +0x18 zone count, +0x1C zones, +0x20 program count, +0x24 program
pointer table. Zone (0x18 bytes, also `SoundProgram` of the effect banks):
+0 SPU address / 8, +6 s16 volume (0x4000 = 1), +8 base note (8.8
semitones: the note played at pitch 0x1000 = 44.1 kHz), +0xA one-shot
length (non-zero only for samples that do not loop; becomes voice flag
0x100), +0xC priority, +0xD pan (0..127), +0xE flags (bit 0: reverb send),
+0x10 / +0x11 key range (the effect banks: the note played), +0x12 / +0x13
bend range down / up, +0x14 ADSR1 | ADSR2 << 16. Program: u8 zone count,
u8 pan, s16 volume, zone indices. GT3: gt3sys 14 zones, gt3se01 26, gt3se02
10, music.ins 214 zones / 110 programs (105 one-shots, 109 loops).

ENGN: +0x18 / +0x20 two sets {u32 count, segment pointer}. Segment (0x10):
s16 base rpm, s16 rpm low, s16 rpm high, s16 volume, u32 pitch (0x1000 =
44.1 kHz at base rpm), u32 SPU address (bytes). GT3's engine banks: 4..9
segments in set 0, set 1 empty, all looped, ~24 kHz samples.

### 2.3 Music: music.seq ("SEQG"), music.inf ("MSEQ")

music.seq: +0 "SEQG", +4 self, +8 count, then 0x48-byte entries: u16
volume, u16 (unused), s32 tempo, 16 track offsets (0 = none). A track is
byte code (`seq_run_tracks`): first a delta (MIDI-style variable length),
then events each followed by a delta: `1nnnnnnn vel len(VLQ)` note,
`01hhhhhh llllllll` bend (14 bits - 0x2000), `001ccccc` loop start (count
c+1), `00` end, `01 c` loop start, `02` loop end (count 0xFF = forever),
`03 p` program, `04 v` volume (v / 127), `05 p` pan, `06 t t t` tempo
(24-bit), 7..0x1F nothing. Ticks: the sequence advances
`ceil((tickAcc) / tempo)` ticks a frame with `tickAcc += 480,000,000 / 60`.
GT3 has 9 sequences (the menu music).

music.inf: +0 "MSEQ", +4 1, +8 base (relocation), +0xC group count, +0x10
track count, +0x14 groups {track offset, count}; a track (0x14 bytes):
group name, file name, title, artist (string offsets), sequence index.
18 groups (mini01 .. home01) over the 9 sequences.

### 2.4 Streamed BGM: bgm/ads.inf ("MADS"), bgm/*.ads

ads.inf: +0 "MADS", +4 1, +8 base, +0xC total tracks, +0x10 17 groups
{track offset, count}; a track (0x14): group, name, title, artist offsets,
markers offset. Markers (0x10C): base, rate (44100), length (samples),
count[32] and times[32] (marker times per channel, in samples). Group 2
(the race songs) has 25 tracks with markers on 4 channels; the menu
jingles have none.

.ads: head 0x28 bytes: "SShd", 0x18, type (0x10 ADPCM / 1 PCM), rate,
channels, interleave, loop start, loop end, "SSbd", body size. Body: the
channels interleaved by `interleave` bytes, each a continuous ADPCM stream
(first block flag 6, last flag 3, others 2). All 66 GT3 files: ADPCM,
44100 Hz, stereo, interleave 0x400, no loop points, body a multiple of
0x800; length in samples = body / 32 x 28.

### 2.5 sound/se.inf ("GTSE")

+0 "GTSE", +4 1, +8 base, +0xC count, then {name offset, code}: code =
bank << 24 | program A << 12 | program B. `se_play`: A == B plays A twice
(hard left, hard right), else A alone. GT3: 42 entries.

### 2.6 The course sound part ("CRSS", CourseData +0x8C)

A course file (`courses/<region>/<name>`, read whole into the course arena,
relocated by `course_data_relocate`) points at +0x8C to its sound part.
**It holds no samples and nothing of it reaches SPU RAM**: it places and
levels the ambient effects of gt3se02.ins (1.6, T177). Little endian, offsets
relative to the part's start (`self` +4 is 0 in the file; T458
`course_sound_relocate` adds the part's address):

- head: +0 "CRSS", +4 self, +8 0, +0xC the part's size, +0x10 1, +0x14 five
  group offsets: 0 "air", 1 "city", 2 (not played: `course_sound_mix` sets
  channel 2 to {1, 1, 1}), 3 "crowds", 4 "sea";
- group: s32 count, then count source offsets;
- source (`SoundSource`, 0x30 + 0x10 n): u32 emitter count n, 8 bytes 0,
  f32 level (runtime), two section ends {x, y, z, lapDist (runtime)} (group
  3 only: `course_sound_locate` finds them on the runway; the level follows
  the cars on that part of the lap), then n emitters {x, y, z, volume}.

Per frame and view, each emitter adds volume x level / max(1, d / 32), a
sin / cos pan from the camera's side axis and a Doppler term (c = 5.524167
m per frame). Groups 0 and 1 with any source play at full level. Checked on
all 80 course files of `courses/US` (python over `gt3vol.py`): 75 have the
part (not the four circle tests and L_slalom80r4), every count, position and volume is finite (volumes 0..3.0; midfield:
air 1 source, crowds 13 sources / 15 emitters), the layout matches the
structs of `T458_course_sound.h`, which hold no pointer-sized or polymorphic
member that MSVC would lay out differently.

## 3. The native back end

### 3.1 Files

| File | Content |
|---|---|
| `src/engine/sound/native/spu2_adpcm.*` | the block decoder, sample extent walker |
| `src/engine/sound/native/spu2.*` | `spu2::Spu2`: 64 MB RAM (PC, ROADMAP 18: the PS2's 2 MB layout first, windows above it for banks bigger than their PS2 place - effects, music, cars; T468 `sound_bank_sizes`), 48 voices (PC: 64 slots per core, the pool's extras in 24..63), interpolation, ADSR, mixer (float with headroom, or the hardware's clamps), cores; libsd-shaped register API |
| `src/engine/sound/native/spu2_reverb.*` | `spu2::Reverb`: one core's reverb (libsd presets, half-rate FIR, work area in the SPU RAM) |
| `src/engine/sound/native/ads_stream.*` | `spu2::AdsStream`: .ads player as a voice source; read callback + feeder thread, or push mode (movies) |
| `src/engine/sound/native/sound_native.*` | the C API: pdispu2 / pdistr commands as functions, one global SPU2, the lock |
| `src/engine/sound/native/CMakeLists.txt` | static library `gt3_sound_native` |
| `tools/sndtest/` | `sndtest`: the test tool, with a test copy of the EE voice manager / sequencer (`ee_sound.cpp`); `sndgame`: the port's own `sound_driver.cpp`, `sound_seq.cpp`, `T520`, `T521` compiled with the game's options over the native back end (`game_side.cpp`, `sndgame.cpp`); `sndplay`: an .ads through `src/platform/audio.cpp` (kernel streaming / WASAPI) in real time |
| `src/platform/audio.*`, `audio_ks.*` | the output (3.5): the stage, the rate converter, kernel streaming, the WASAPI fallback |
| `src/game/app/pc_sound.*` | one music-thread flush per game tick, with the tick's vblank time (3.4) |

No Windows or game dependency (C++17, `<thread>`, `<mutex>`).

### 3.2 The SPU2 model

Per 48 kHz tick, per voice (hardware behaviour from nocash's psx-spx and
PCSX2's SPU2, reference only):

- pitch counter += min(pitch, 0x3FFF) (PMON: x (32768 + previous voice's
  output) >> 15); each 0x1000 consumes a sample (decoding the next block
  when needed: loop end -> ENDX, jump to LSA, stop without repeat; loop
  start -> LSA unless LSAX was written);
- 4-point gaussian interpolation, phase = counter bits 4..11; the table is
  built from the published construction (nocash / Near), which reproduces
  the hardware's 512 values exactly;
- ADSR: attack (linear / exponential, 7-bit rate), decay (exponential to
  (SL + 1) x 0x800), sustain (linear / exponential, up / down, 7-bit
  rate), release (linear / exponential, 5-bit rate); the rate model:
  `counter += 0x8000 >> max(0, shift - 11)`, `level += step << max(0, 11 -
  shift)`, exponential: slower above 0x6000 going up, x level going down;
  a step when the counter passes 0x8000;
- out = sample x envelope >> 15, then x volume >> 15 per side (fixed
  mode: the 15-bit register x 2, signed: negative volumes invert);
- dry / wet gates (VMIX*), the core mix (MMIX), the reverb on the wet sum
  (x EVOL), MVOL; core 0's output x core 1's AVOL into core 1's dry input.

Not modelled (GT3 never uses them): noise (NON), volume sweeps, the ADMA
sound-data input (PCM streams), IRQs, ECHO / DELAY parameters.

### 3.3 The API (sound_native.h) and the RPC -> function map

| PS2 (EE caller -> IOP) | Native function |
|---|---|
| pdispu2 / pdistr module start | `snd_native_init()` (before `sound_init`) |
| 'SPUP' 4 `pdispu2_send_state(st)` | `snd_native_send_state((const SndNativeState *)copy)`; the EE side keeps copying the block and clearing `st->dirty` |
| answer word 0, `pdispu2_status()` | `snd_native_voice_active_mask()` |
| 'SPUP' 3 `pdispu2_request_status()` | `snd_native_request_status()` |
| 'SPUP' 5 `F_0025C280(a, b, c)` | `snd_native_set_output_mode(a, b, c)` (nothing) |
| 'PDIP' 4 `pdipipe_read2(h, spuAddr, size, 1)` | read the bytes, then `spu_memory_write(spuAddr, data, size)` (engine/sound/spu_memory.h; defined by the library) = `snd_native_write_spu` |
| 'SPUV' 1 / 2 | `snd_native_write_spu` / `snd_native_read_spu` |
| 'PBGM' 1 `pdibgm_start(lsn, sectors, loop, ...)` | `snd_native_bgm_start(read_fn, user, loop)` |
| 'PBGM' 2 `pdibgm_stop()` | `snd_native_bgm_stop()` |
| 'PBGM' 3 `pdibgm_set_volume(v)` | `snd_native_bgm_set_volume((int)(clamp(v, -1, 1) * 32768))` |
| 'PBGM' 4 / 5 `pdibgm_pause()` / `_resume()` | `snd_native_bgm_pause()` / `snd_native_bgm_resume()` |
| 'PBGM' 6 `pdibgm_update()` | `snd_native_bgm_get_status((SndNativeBgmStatus *)&g_002FCE00)` |
| 'MPG1' open mode 2 + the demuxed audio | `snd_native_bgm_open_push()`, `snd_native_bgm_push(data, n)`, `snd_native_bgm_push_end()`; `snd_native_bgm_played_us()` (fine clock) |
| (the SPU2's output) | `snd_native_render(float *out, int frames, double now)`: 48 kHz stereo float, the scheduled engine (3.4); `snd_native_mix(int16_t *out, int frames)`: the same clamped to 16 bits (the test tools) |
| (PC: the voice pool, 3.4) | `snd_native_send_state_ext(st, extra, n, time)`, `snd_native_voice_active_mask128(out)` |

`SndNativeState` and `SndNativeBgmStatus` are binary identical to the EE's
`SpuState` and `BgmStatus` (static_asserts).

### 3.4 Threads

- The game thread(s) call the command functions; the output's real-time
  thread calls `snd_native_render` (the test tools: `snd_native_mix`). One
  mutex serialises them, so a state block is applied atomically between
  two renders.
- **The scheduled engine** (`snd_native_set_scheduled(1, clock)`, set by
  `audio::init` with `sched::now_seconds`; off in the test tools, which mix
  right after each block themselves). On the PS2 the IOP applied each state
  block within a millisecond of its vblank, so in the SPU2's output
  consecutive blocks are 800 or 801 samples apart (48 kHz / 59.94 Hz), and
  each block held exactly one game frame's changes. Here:
  - *One block per game tick.* The game ticks run when the frame loop gets
    to them (`game/app/pc_frame.h`), not at the timer's VBLANK_E, so the
    music thread woken by the timer flushed two ticks' changes in one block
    or none (13 % of the traced kerb-rumble restarts merged: the loud
    bursts). Now `pc_frame_tick_end` wakes it after each tick
    (`pc_sound_tick`, `game/app/pc_sound.cpp`; `music_pc_wake` in T463,
    `SignalSema`: priority 6 preempts the render thread, so the flush runs
    at once), with the tick's own vblank time (a catch-up's ticks one
    vblank apart, ending at the vblank the frame is due by). The timer's
    VBLANK_E handler only wakes it while the ticks stall (more than 4
    vblanks behind: a load), so the music keeps time; after a stall of over
    0.25 s the ticks pick up from the present.
  - *Each block at its sample.* T520 hands the block its time
    (`pc_sound_block_time`: the music thread takes the latest wake-up's when
    it wakes, `pc_sound_flush_begin`; its semaphore counts to 1, so two
    wake-ups before it runs are one flush); the engine queues
    it and `snd_native_render` applies it at frame `time x 48000 + off +
    delay`, splitting the mix there: blocks 800 / 801 samples apart as on
    the PS2, whatever the output's period. `off` maps the clock to the
    render's frame counter (fitted over the render calls: their jitter, the
    device clock's drift; refitted after a jump of over 50 ms). `delay`
    covers how late blocks arrive: the lateness of the last 240 blocks (4
    s), but the 3 latest (a hitch's blocks apply late instead), plus the
    largest render call of late and 1 ms; it rises at once and falls slowly
    (2 % a block), at most 60 ms; a block later than 60 ms (a stall) is not
    counted. Typically 3-6 ms at 60 frames a second.
  - *A note's tick.* Blocks applied late (a hitch's, past the delay) were
    applied back to back at one sample, so a note keyed on in one tick and
    off in the next sounded for no sample (user report 2026-10-24: missed
    notes and sound effects, now and then). The PS2 never gave a note less
    than a tick, so a block that keys off - or on again - a voice keyed on
    less than 800 frames before waits until then (`SCHED_MIN_NOTE`); the
    blocks after it wait with it, the others catch up at once. The trace
    counts them (`... held for a note's tick`).
  - *The status.* The active mask counts the voices the queue still keys on,
    and is recomputed 10 frames (~200 us) after each block, as pdispu2
    answered. A block's key-ons enter it as the block applies (PC): a render
    call ending within those 10 frames left them neither queued nor active
    until the next call, and `snd_flush_voices` could take such a voice for
    ended and give it to the next note or sound (the same report).
  - `snd_native_schedule_info` gives the delay, the queue and the counts of
    late blocks and of blocks applied early (a queue over 64: no render);
    the output logs them every minute and at each output change (`audio:
    scheduling delay ...`).
- **Headroom.** With the scheduled engine the SPU2 model mixes without its
  16-bit clamps (the core sums, the core 0 -> core 1 input, the outputs;
  only the reverb's input keeps one): `Spu2::mix_float` gives float
  output, and the output's limiter (3.5) turns a loud pile-up down instead
  of clipping it.
- **The voice pool.** The EE voice manager has 126 voices (46 the PS2's:
  `SndVoiceBank` grew by 80, `SND_VOICES_EXTRA`); the native SPU2 has 64
  voice slots per core (64-bit masks). EE voice 0..45 is the PS2's mapping;
  extra voice e is core `e & 1`, voice `24 + (e >> 1)`. The extras travel
  next to the block (`g_spuVoiceExt`, `pdispu2_send_state_ext`), the
  status as 128 bits (`pdispu2_status128`): bits 0..45 the PS2's voices,
  46..125 the extras. Fewer steals of sounding voices in a race (a stolen
  rumble or squeal restarted elsewhere was another burst).
- The BGM player reads its file on its own feeder thread through the read
  callback (offset-based: a loop re-reads from the body start), into
  128 KB FIFOs per channel; the voices take blocks from the FIFOs on the
  audio thread. Lock order: the sound lock, then the player's lock; the
  feeder never takes the sound lock.
- `snd_native_set_bgm_threaded(false)` and `snd_native_bgm().pump()` give
  deterministic offline rendering (sndtest).

### 3.5 Integration in gt3pc (done)

| Piece | File | What it is now |
|---|---|---|
| EE voice manager, sequencer, engine voices | `src/engine/sound/sound_driver.cpp`, `sound_seq.cpp` | the decomp's `sound_driver.c` / `sound_seq.c` compiled as C++ with the game's options (in `GT3_ENGINE_SOURCES`); logic unchanged. PC changes: `SND_RELOC` assigns through the field's type; `g_003029E8` (the ASCII -> full-width table T108 reads) is defined `const` as T108 declares it (C++ puts the type in the name); the voice pool of 126 (3.4): `SND_NUM_VOICES` = `SND_VOICES_TOTAL`, `snd_flush_voices` writes voices 46.. to `g_spuVoiceExt` and reads the 128-bit status |
| 'SPUP' / 'SPUV' client | `src/engine/pdi/T520_pdispu2.cpp` | the RPC objects are gone: `pdispu2_send_state(_ext)` copies the block (and the extra voices) with interrupts off, clears `dirty` and calls `snd_native_send_state_ext` with the block's time (`pc_sound_block_time`, 3.4); `pdispu2_status` = `snd_native_voice_active_mask` (mirrored in `g_002FCA00[0..1]`), `pdispu2_status128` the pool's; `pdispu2_request_status`, `F_0025C280` (no-op), `F_0025C088` / `F_0025C0C0` (SPU RAM write / read) map to the native calls; `pdispu2_init` is empty; `spu_state_*` unchanged |
| 'PBGM' client | `src/engine/pdi/T521_pdibgm.cpp` | `pdibgm_start(lsn, sectors, loop, bufSize, host)` stops the old stream, then plays through `snd_native_bgm_start` with a read callback over gt3.vol from sector `lsn` (`platform/disc.h`), or over the `host:` file from byte `lsn` (`platform/host_file.h`), `sectors` x 2048 bytes; the callback runs on the player's feeder thread (native: no game kernel call). `pdibgm_update` copies the native status into `g_002FCE00` with interrupts off; the other functions are one call each; `pdibgm_set_volume` keeps the clamp and the x 32768 |
| bank samples to SPU RAM | `src/engine/pdi/T523_pdipipe.cpp` (engine agent), `src/engine/io/T503_vol_thread.cpp` | `pdipipe_read2(..., toSpu = 1)` -> `spu_memory_write` (`engine/sound/spu_memory.h`), defined by `sound_native.cpp`; synchronous, so the samples are in before `file_read_sound_bank` returns (the IOP transfer was a blocking RPC too). `file_read_sound_bank` names the range for the trace first (`spu_memory_name`, 3.6). PC: the write waits (sleeping on the game scheduler, 10 ms steps, 2 s at most) until no voice sounds in the range (`spu_memory_voices_reading`: block or start address there). On the PS2 a race read its course and cars first, seconds after the menu's `bgm_stop`, so the menu music's notes in music.ins (keyed off at the sequencer's next tick, releasing for up to ~1.4 s) had ended before the engine banks went there; here they were 0.15 s old and played the new engine data, looping to stale addresses (user trace, 2026-10-18) |
| native SPU2 / BGM player | `src/engine/sound/native/` | the static library `gt3_sound_native`, its own (standard) options, linked into `gt3_platform` |
| sound output | `src/platform/audio.{h,cpp}`, `audio_ks.{h,cpp}` | `audio::init()` (from `WinMain`, after `sched::init`, before the game threads): `snd_native_init()`, the scheduled engine on (3.4), then a supervising thread that opens the output `[Audio] Output` asks for (gt3pc.ini; the settings menu, F2): **kernel streaming** (`audio_ks`: the default render endpoint's KS filter, found through the device topology, its render pin opened directly - WaveRT with the driver's notifications or a polled position, else standard streaming packets -, 48 kHz stereo when the pin takes it, periods of `[Audio] Buffer` ms, default 2, on a thread with MMCSS "Pro Audio" at critical priority; no Windows audio engine in between, a pin of one instance - most onboard and HDMI devices - is the game's alone while it plays, other programs silent on that device, and cannot be opened while another program holds it; a pin of several instances, as USB interfaces often have, plays beside the Windows audio engine. The single-instance case is kept but said: the log's `audio: WARNING`, a notice over the picture once per run, `KS EXCLUSIVE` on the menu's `RUNNING` line. A crackle - an underrun, after the stream's first 0.5 s; with WaveRT notifications also a stall of a whole buffer or more, which brings the play position round to where it is expected (the time since the last look against how far the position moved: more than 3/4 of a buffer unaccounted for; user report 2026-10-24, crackles heard but none counted on onboard audio at a 2.7 ms buffer; and a refill that took longer than what was left of the playing half at its notification, by the clock - a stall of 1.6 .. 2.7 ms there left the position past the half check's tolerance and not yet round; and a refill done with less than half the hardware's FIFO left of the playing half - the DMA fetches ahead of the register's position: crackles at the movie -> menu change with no refill late by the register, three with 13 .. 15 frames left of a 32-frame FIFO, 17 and more where nothing was heard) - raises `[Audio] Buffer` by 0.5 ms (saved; the output restarts); a stream that fails even at 20 ms leaves `auto` to WASAPI until `Output` changes; each shows a notice over the picture for 2 s ("Audio buffer insufficient. Raising buffer by 0.5ms...", "Persistent insufficient audio buffer. Switching output..."). The Windows audio engine applied the game's volume mixer slider and, where the device's volume and mute are software (`IAudioEndpointVolume::QueryHardwareSupport`), those too: with kernel streaming they are read every 50 ms - the slider through the process's own session, `IAudioSessionManager::GetSimpleAudioVolume`, which also lists the game in the mixer - and applied by the output stage); with `auto`, else **WASAPI shared** at the engine's smallest period (`IAudioClient3::InitializeSharedAudioStream`; else the default ~10 ms), event-driven, in the engine's mix format; with `ks` and the device busy, no output (tried again every 2 s). A busy device (`audio_ks::start_busy`: a pin with no free instance, or one refusing every format while others hold it) puts a notice over the picture for 8 s: with `auto`, "PLAYING THROUGH WASAPI INSTEAD"; with `ks`, "NO SOUND: TRYING AGAIN EVERY 2 S", once until the output restarts. Every stop silences the kernel stream's buffer first (it looped its last periods while the pin went down), and closing the window stops the output (`audio::shutdown`; the process used to end with it running: user report 2026-10-24, crackles at the end of a session that the dump did not have). `GT3PC_AUDIO_TIMING=1` (`diag\audio_crackle.cmd`, with `GT3PC_AUDIO_DUMP`): every 10 s and at a stop, the WaveRT refills' timing (`audio: timing of N refills ...`: how far into its half the hardware was at the notification, how much was left after a refill, tight / late ones, the longest gap between notifications, the longest refill, the longest wait of a render for the sound lock) and at the start the hardware latency's parts (FIFO, chipset, codec). A bank's samples go into the sound memory in 64 KB pieces, the lock let go between them (`snd_native_write_spu`; music.ins in one copy held the render back as the menu's banks came in). Reopens on a change of `[Audio]`, of the default device (`IMMNotificationClient`) or a failure; with no device the engine keeps rendering at real-time pace so voices end and the BGM clock runs; plays in the background. A device rate other than 48 kHz: cubic (Catmull-Rom) conversion, the source frames of each output period rendered in one call. Output stage (48 kHz float): -4 dB master gain, DC blocker (pole 0.9995, ~4 Hz), peak limiter (-1 dBFS, stereo-linked, instant attack, 100 ms release). `snd_native_set_output_latency` gets the output's latency (BGM / movie sync). `GT3PC_AUDIO_DUMP=<file.wav>`: every sample after the stage is also written there (16-bit; header refreshed every second). The settings menu's `RUNNING` line: the output and its latency + the scheduling delay. Log lines `audio: ...` |
| game/sound | unchanged but for the flush per tick and the trace | `sound_init`, `music_init` (VBLANK_E handler from the platform's timer), `se_init`, `ads_play`... run as on the PS2. PC: T463's VBLANK_E handler wakes the music thread only while the game ticks stall, `music_pc_wake` (3.4). Trace-only additions (3.6): `course_sound_trace` (T458), `amb_trace` (T177) |
| movies | `T522_pdimpeg.cpp` (movie agent) | with sound (`pdimpeg_open` mode 2) the demuxer sends each MPEG private stream 1 payload whose first byte is 0xFF, minus its 4-byte prefix, to `snd_native_bgm_push` after `snd_native_bgm_open_push()` (`mpegif::feed` has set the BGM volume to 1.0); keeps what is not taken (a full FIFO: ~5 s ahead) and pushes it later; `snd_native_bgm_push_end()` at the end, `snd_native_bgm_stop()` to stop. `snd_native_bgm_played_us()` is the audio clock (0.6 ms steps) for A/V sync |

### 3.6 Diagnostics: `GT3PC_SND_TRACE`

Not part of the PS2's behaviour; off unless the variable is set. With
`GT3PC_SND_TRACE=1` (or `2`) the log gets `snd:` lines, each with the
mixer's clock `t=` (seconds of sound mixed since start-up; with
`GT3PC_AUDIO_DUMP=<file.wav>` set too, that is the time in the WAV, within
the output's latency, a few ms):

| Line | Where | Meaning |
|---|---|---|
| `snd: tracing sound memory uploads, ...` | `audio::init` | the trace is on |
| `upload <file> -> SPU a..b (n bytes); covers <older uploads>` | `file_read_sound_bank` -> `snd_native_name_region` | every bank upload, what it overwrites (in a race: the engine banks over music.ins) |
| `  N voice(s) still sounding in that range: cXvY at <addr> (envelope E) ...` | idem | voices that will now play the new data from where they are (garbage if audible: E is the envelope level, 0..0x7FFF) |
| `SUSPICIOUS key-on cXvY (EE voice n) at <addr> [<file> +<offset>]: <problem>: pitch, volume, ADSR, flags, header` | `snd_native_send_state`, before the KON | the start address is in memory nothing uploaded, or is not ADPCM, or is not the start of a sample |
| `key-on ...` (level 2) | idem | every key-on, with the bank and offset it plays |
| `SUSPICIOUS read: cXvY ..., keyed on at <addr> [...], <what> at <addr> [...]` | the mixer (`Spu2::next_sample`), reported at the next block | the voice reached memory nothing uploaded, data that is not ADPCM, or a loop end that jumps to a loop address it never passed (stale LSA) |
| `summary of N frames: K key-ons (C cut a sounding voice), ... voices sounding V (peak P)` | every 600 frames (level 2: 60) | voice churn; suspicious lines beyond 40 per period are only counted |
| `course sound CRSS (no samples: ...): sources air a, city b, group 2 c, crowds d, sea e; n emitters, volume x..y %; crowds sections on the runway k of d` | `course_sound_locate` (T458) | the course's sound part at the race's start (2.6) |
| `ambient[ (NOT FINITE OR HUGE)]: views, gain, air L R P vN, city ..., crowds ..., sea ..., tunnel T vA/B, tunnel2 ..., reverb` | `RaceAmbientSound::update` (T177) | the course sound's mix in thousandths (left, right, pitch factor, the voice or -1) every 300 frames (level 2: 60), at once when a value is not finite or beyond +-10^6 (on x86 it would reach a voice as a 0x80000000 volume or pitch; the EE's FPU had no Inf / NaN and saturated) |
| `upload to SPU a waited N ms for M voice(s) sounding there[ (gave up: some still sound)]` | `pdipipe_read2` (T523) | the upload waited for the voices still playing its range (3.5) |
| `EE key-on car C slot S (<slot>[: surface, squeal, load]) -> voice V / NO VOICE, prio P, l L r R pitch X, SPU a; was off: <why>` (level 2) | `GtSoundHandle::play` (T468), context from `RaceEntryCar::updateSound` (T121) and `CarSound::updateSlot` (T176) | every voice a sound handle keys on (a car slot or another handle: ambient), l / r / pitch factor in thousandths, the wheel's surface code, squeal volume (thousandths) and load (N) for a wheel slot; why the handle had no voice: `stolen N frames ago (voice v at prio p, by a prio q request)` (`snd_voice_alloc`), `ended` (`snd_flush_voices`: the status said silent), `stop(0)` (zero volume), `stop(1)` (`CarSound::stop`, ambient stop). At most 300 EE lines a second, the rest counted (`(N EE lines not logged ...)`) |
| `EE voices: F free, O owned (priority a..b, n at 254/255), L ownerless keyed on (k of them for over 5 s), R keyed off and releasing` (level 2) | `snd_flush_voices` (sound_seq.cpp), every 60 flushes | the EE's voice table: a leak (voices nobody keys off) grows in "over 5 s"; R counts voices in their release, which only requests of their own priority or louder can take |

The checks rest on two facts of the disc's data, verified over all 1,216
sound banks (`sound/`, `music/music.ins`): every one of the 4.6 million
blocks has filter <= 4, shift <= 12, flag byte <= 7
(`adpcm_header_plausible`), and every one of the 8,768 sample starts (INST
zones, ENGN segments) is the bank's first block or follows a block with the
loop-end flag. An offline run over the real banks in the race layout (the
effect banks, music.ins, an engine pair over it, scratch program
`trace_test.cpp` against `gt3_sound_native`) keyed on all 281 zones and
segments with no suspicious line, and caught the three wrong key-ons it was
given (inside a sample, unwritten memory, the reverb work area) and the two
voices' reads. Cost when on: two byte tests per 16-byte block a voice
fetches, the region table on uploads.

## 4. Verification (sndtest, build/scratch/sound/verify.py)

- ADPCM: the 254 zone samples of gt3sys / gt3se01 / music.ins dumped by
  `sndtest dump` equal an independent Python decoder bit for bit.
- The voice path end to end: `sndtest raw` renders of looped game samples
  (gt3se02 zones 6 and 9 at pitch 0xC35, gt3se01 zone 17 at 0x1000; 6 s,
  several loops) equal, sample for sample, an independent Python model of
  decode with loop jumps (history carried), pitch counter, gaussian
  interpolation, envelope, voice volume and the core 0 -> core 1 chain.
- Gaussian table: equal to the hardware table; rows sum to 0x7F80 +- 1.
- Pitch: an encoded 1 kHz loop at pitches 0x0800 / 0x0C00 / 0x0EB3 /
  0x1000 / 0x2000 / 0x3FFF peaks at 500 / 750 / 918.70 / 1000 / 2000 /
  3999.76 Hz (error <= 0.0003 %). The sequenced music's spectral peaks sit
  on equal-tempered A440 semitones (most weight within +-10 cents), so the
  note -> pitch -> 48 kHz chain is right.
- Loops: no step at the loop points of an exactly periodic loop beyond the
  signal's own slope (the 99.9th percentile step = the sine's maximum
  slope); a looping .ads wraps without a step (max step 614 at the wrap vs
  5956 in the music) and its render follows an independent decode with the
  history carried over the wrap (correlation 0.997).
- ADSR: the envelopes of the game's ADSR pairs (A5FF/5FCF, 80FF/5FC0,
  000F/0003, 91EA/528F, 80CC/504F, BA7F/5FCE) equal an independent Python
  model of the rate rules sample for sample; attack rate 0 = 3 ticks,
  release rate 0 = 2 ticks.
- Voice status: a one-shot voice leaves the active mask after its end; the
  EE copy then frees it (seq renders reach 46 voices with stealing).
- BGM: race_a01 30 s matches an independent decode resampled at pitch
  0xEB3 (correlation 0.9984, level -0.06 dB, 2 samples of interpolation
  delay); status time advances by 1792-sample chunks, `ended` at the
  end of a non-looping song (pos = length 444416 for str01a), `id`
  increments per loop; pause freezes the clock and silences, resume
  continues; the opening movie's audio through the push path correlates
  0.9999 with an independent decode.
- Reverb: an impulse through STUDIO_3 gives first reflections at 40 / 48 ms
  (L / R), decorrelated tails, RT60 about 2.6 s.
- Threads: `sndtest live` (audio pull every 10 ms, 60 Hz music thread,
  feeder thread) runs in real time without underruns; with `--stress`
  (BGM restart / pause / resume every 0.75 s) no deadlock.
- The integrated code (`sndgame`): the port's `sound_driver.cpp` /
  `sound_seq.cpp` / `T520` compiled with the game's options render the
  music sequences sample for sample like the test copy (menu sequence 6,
  25 s, 2.4 M samples identical); `T521`'s `pdibgm_start` reading
  `bgm/race_a01.ads` from gt3.vol by sector gives the file render's audio
  (correlation 0.99999999, level x 0.8 = the volume asked); its status,
  pause / resume and loop (`id` 0 -> 1 -> 2) behave; music and BGM play
  together; the engine voice (`engine_update`) sweeps.
- The output (`sndplay`, through `src/platform/audio.cpp` on this PC's
  default device): WASAPI took 48 kHz 16-bit stereo directly, 1440 frames
  (30 ms); the `GT3PC_AUDIO_DUMP` file holds the stream at 0.3115 of the
  source level (0.5 volume x 0.63 gain = 0.315), correlation 0.996 with an
  independent decode, DC under 1 LSB, no gaps.
- `gt3pc.exe` itself does not link yet (graphics, movies, VU0 files of other
  agents); the sound group is gone from `build/unresolved_grouped.txt`.
- Key-on / key-off sequences (2026-10-18, scratch `vsim/rumble.cpp`: the
  port's sound_driver / sound_seq / T520 over the native SPU2, driving one
  GtSoundHandle-like slot as the user's trace shows the kerb rumble: gt3se01
  zone 15 on one frame, zero volume the next; `vsim/hwmodel.py`: an
  independent model of the psx-spx voice rules fed with the KON / KOFF the
  EE sent): with PS2 timing (a block, then 800 samples) the native output
  equals the model sample for sample (2 s, max |diff| 0, also with the
  blocks at 480-sample positions). Timing then decides the result: 20 ms
  envelope steps > 3 dB per 0.5 s over six pitches (0.8..2.0): PS2 timing
  4.3, blocks applied at 10 ms device bursts 8.5 (peaks at full scale),
  paced output 4.5; one sustained rumble voice alone 4.5 (the sample pulses
  by itself). With 1 game frame in 8 merged into the next flush (as 13 % of
  the traced restarts were) paced 7.8.
- Paced output under threads (scratch `tt/pace_live.cpp`: blocks at
  59.94 Hz up to 6 ms late, the device pulling 480 frames every 10 ms, 5 s):
  2 underruns, both at the start.

## 5. Open questions

- The exact moment the hardware stops a voice at a one-shot's end (PCSX2:
  when the sample after the last is fetched) and the first-sample timing
  after a key-on (the counter starts at 0 here) are approximations within a
  sample or two.
- 'SPUV' 1 / 2 directions, and 'MPG2' function 3 (two doublewords: movie
  timing?) were not analysed further; GT3 does not call 'SPUV'.
- Status timing: pdispu2 answered with the mask as of ~200 us after the
  block; the scheduled engine computes it 10 samples after the block is
  applied (3.4), but the EE reads it at its next flush, which here comes
  after the render reached the block (the delay, a few ms) rather than one
  vblank later: a voice end may still be seen one frame later.
- A paused BGM stops at once here; on the PS2 the voices ran on until the
  IRQ handler noticed the missing refill (up to ~20 ms), and a resumed
  stream restarts at the next half (a short skip). Not reproduced.
- The reverb's output when the effect enable bit is off: computed from the
  (unwritten) buffer as PCSX2 does; GT3 enables effects before any reverb
  send.
- The overall output level: the model is unity gain from a full-scale
  voice to the output; `src/platform/audio.cpp` applies -4 dB (PCSX2's
  0x4FFF / 0x8000) and a DC blocker. To be checked by ear against the
  console once the game runs.
- Not yet heard in the running game (it does not link yet): the boot /
  menu music, effects and engines should be checked with
  `GT3PC_AUDIO_DUMP` and by ear when it does.
- "Random garbage sounds during a race" (user report, 2026-10-18; menus and
  engines fine). Not found by reading; what was checked and holds: the
  course's sound part has no samples (2.6) and its data is sane on every
  course; nothing is uploaded during a race, and the uploads at its start
  (`file_req_run`: the engine pairs, 0x25800 apart from 0xAC000) are
  synchronous, in disc order (every `sound/engine` file before every
  `sound/<kind>` one, so bank 1 is placed after bank 0), fit their areas and
  overlap neither the effect banks (0x5040..0x93D50) nor the reverb work
  areas; every zone / segment start of the disc's banks is a valid sample
  start; GT3's looped samples all pass a loop-start block before their loop
  end (no stale loop address), one-shots end with flag 1 and carry the
  one-shot flag (zone +0xA), loops do not; the voice path (`GtSoundHandle`,
  `engine_update`, `snd_flush_voices`, the native KOFF / SSA / KON order and
  status) matches pdispu2; the MSVC layouts of `GTSOUNDINSTRUMENT` /
  `EngineSound` (vptr at +0) are only used through their members. Two
  differences from the PS2 remain possible sources: (1) loading is near
  instant here, so menu-music voices still in their release when a race
  starts play the engine banks written over music.ins (`upload ... still
  sounding` lines); (2) a float -> int conversion of an Inf / NaN or of a
  value beyond 2^31 gives 0x80000000 on x86 where the EE saturated (5.3 of
  runtime.md), in the volumes / pitches of `GtSoundHandle::play`,
  `CarSound::updateEngine` / `playOneShot`, `sound_voice_priority` (the
  `ambient (NOT FINITE OR HUGE)` line covers the course sound's part).
  The first `GT3PC_SND_TRACE=2` run (a 6-car race, garbage heard at
  t = 162..166): no suspicious key-on or read in the race; (1) confirmed at
  both race starts (9 / 4 menu-music voices under the engine uploads, three
  stale-loop reads): fixed by the upload wait (3.5). In the race the pool is
  full (48 of 48 sounding from the start, t = 103.4 / 133) and 70..90 % of
  100..366 key-ons a second take a sounding voice: a steal ring among the
  quietest sounds (tyre roll `on_road` gt3se01 zone 20 of distant cars at
  -40 dB, the wind rn_01, far engines), and in 163..166 the player's car on a
  kerb: `rumble` (zone 15, -5.5 dB, release 0.34 s) keyed on every 2..6
  frames on a free voice while the previous one releases. Checked against
  the PS2: `snd_voice_alloc` equals retail (0x00269798: a voice of the same
  priority is taken, oldest first); the native status never frees a looping
  voice (scratch `voicesim`: the port's sound_driver / sound_seq / T520 over
  the native SPU2, 58 handles over 46 voices for 20 s: 42,724 losses, all
  steals, 0 by the status; a rumble toggled every 3 frames keeps ~14 voices
  in release); the turbo / rumble restarts at an unchanged volume follow a
  stop(0) at zero volume (no other sound takes their old voice: it was
  keyed off, then freed). The second run's EE lines (spikes heard at
  t = 134..142): the player's kerb rumble (wheel slots 7 / 14 / 21 / 28) is
  restarted every 1..4 frames, always after `stop(0)` (the wheel's load
  under 612 N on every other frame: the game's own logic, as on the PS2),
  each restart on a new voice while the last copy releases. The voice rules
  are not the difference: an independent psx-spx model renders that
  sequence identically to the native SPU2 (4. Verification). The timing
  was: blocks took effect at the next 10 ms device burst, so the copies'
  key-ons / key-offs fell 480 / 960 / 1440 samples apart instead of the
  PS2's 800 / 801, which doubled the 20 ms level steps of the pattern and
  drove the sum to full scale (the WAV: up to 970 samples a second at the
  output's ceiling in 136..140). Fixed by the paced output (3.4). Left: in
  13 % of the traced restarts two game frames reached one flush
  (`stop(0) 0 frames ago`: a game frame late, the next one early, around
  the music thread's vblank tick), which the PS2's steady 60 Hz frame does
  not do; synthetic, it adds about 3 steps per 0.5 s. It follows the
  game / renderer frame times, not the sound code.
