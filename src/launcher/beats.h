/* beats.h - beat markers made from a song's sound (the song manager's
 * "Generate markers", new songs as the launcher adds them; docs/sound.md
 * 2.4 "Beat markers").
 *
 * The discs' race songs have four marker channels (ads.inf), which the
 * replay cameras follow (T386): 1 every beat, 2 every bar's first beat, 3
 * the cut points (about one a bar, plus accents: fills, hits), 4 the same as
 * 3 (the US disc; the JP disc's are denser).  Made here from the sound:
 *   - the onset strength: the rise of 24 bands' levels (log-spaced, 30 Hz ..
 *     5 kHz) every 10 ms, at ~11 kHz;
 *   - the tempo: the player's (the song manager asks for it; the discs'
 *     songs: 95 .. 190 beats a minute);
 *   - the beats: the onsets that best fit that period (dynamic programming:
 *     each beat's score is its onset plus the best earlier beat's, less a
 *     penalty for a spacing off the period - D. Ellis, "Beat Tracking by
 *     Dynamic Programming", 2007), which follows a drifting tempo;
 *   - the bars: every 4th beat, from the beat where the sound changes most
 *     from the half bar before to the half bar after (chords change at bar
 *     lines);
 *   - the cuts: the bars, and in a bar its strongest accent when it stands
 *     well out (about as many cuts as the discs' songs have).
 * Against the discs' own markers (45 songs of the US and JP discs, their
 * tempos given; scratchpad beateval, 2026-10-26): beats F 0.73 within
 * 70 ms (0.9 and more on most songs; the misses half a beat off or a
 * quarter beat off the disc's grid), bars F 0.56 (the phase right on 21 of
 * the 23 songs whose beats are), ~50 ms of CPU a minute of music.  Tried
 * and dropped: the low bands weighted, the kick's hits choosing the beats'
 * or the bars' phase (worse on these songs).
 * No other code: the analysis is this file's. */
#pragma once

#include <cstddef>
#include <vector>

namespace beats {

/* Seconds from the song's start; the tempo the beats keep (median). */
struct Result {
    std::vector<double> beats, bars, cuts;
    double bpm = 0;
};

/* The tempos the analysis takes (beats a minute). */
constexpr double kMinBpm = 40.0, kMaxBpm = 260.0;

/* The analysis of `frames' samples of one channel (-1..1) at `rate', its
 * tempo `bpm' (the player's: the beats are found at that period, a drift
 * from it followed); false when there are no beats to find (too short,
 * silent) or the tempo is outside kMinBpm .. kMaxBpm. */
bool analyze(const float *mono, size_t frames, int rate, double bpm, Result *out);

} // namespace beats
