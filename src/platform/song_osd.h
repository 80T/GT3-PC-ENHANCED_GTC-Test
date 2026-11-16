/* song_osd.h - PC (ROADMAP 55, no original): the race's song changer, a
 * panel at the bottom left of the picture (docs/sound.md 2.4 "The song
 * changer").
 *
 * Its hotkeys (input_map.h [Hotkeys] SongMenu, PreviousSong, NextSong: M,
 * Comma, Period on the keyboard, a controller's Select, D-pad up, D-pad
 * down) open and close it and, while it is open, step to the previous / next
 * race song.  It works in a live race alone, while the race's frames run:
 * the race's music update (T130 RaceBGMPS2::update, through
 * game/app/pc_race_songs.h) calls race_frame every frame - not while the
 * race is paused, nor in a replay or a demo; then it is not drawn and its
 * hotkeys do nothing.  The game's controls all work meanwhile (a device's
 * press of a hotkey reaches the game too).  Drawn with the panels
 * (platform/overlay.h) in their 5 x 7 font: upper case, accents left off.
 * No Windows headers: game code can include it. */
#pragma once

struct ID3D11RenderTargetView;

namespace song_osd {

/* SongMenu (the window thread): opened / closed; nothing outside a live
 * race. */
void toggle();
/* PreviousSong (-1) / NextSong (+1), while it is open. */
void step(int dir);

/* The race's thread, each frame of a live race: it may show and its keys
 * act; returns the step asked for since (-1, +1; 0 none), one a call. */
int race_frame();
/* The race is over (or a new one starts): closed. */
void race_end();
/* What it shows (the race's thread): the race song playing (title nullptr:
 * none), its place among the play list's songs (number 1 .. count; count
 * 0: not shown).  title / artist: the game's text (the ANSI code page). */
void set_song(const char *title, const char *artist, int number, int count);

/* The panel over the picture while it is open in a live race (the present,
 * as the other overlays). */
void present(ID3D11RenderTargetView *backbuffer);

} // namespace song_osd
