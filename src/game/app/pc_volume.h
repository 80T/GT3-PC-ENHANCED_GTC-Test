/* pc_volume.h - the player's volumes of the sound effects ([Audio] MenuSe,
 * RaceSe, ReplaySe; platform/settings.h, the F2 menu, the launcher's Audio
 * tab): levels for the game's own options that switch them on and off
 * (GameOptions menuSe, raceSe, replaySe), applied where those options act.
 * No original: new PC code.  (The music's: T521 pdibgm_pc_music.) */
#ifndef GAME_APP_PC_VOLUME_H
#define GAME_APP_PC_VOLUME_H

#include "game/game.h"

/* 0 .. 1: the menus' sound effects (menu_play_se, T233). */
f32 pc_menu_se_volume(void);
/* 0 .. 1: a race's (replay 0) or a replay's sound effects - the cars' and
 * the course's (car_sound_update_se_switch, T176, keeps it in
 * g_pcRaceSeLevel each frame). */
f32 pc_race_se_volume(int replay);

/* PC (2026-11-11, user: "Car clash sound effect volume doesn't scale the
 * same as everything else (f2 menu's volume control at 5% still has it
 * relatively loud)"): the player's level is applied to an effect's SPU
 * volumes AFTER the game's own clamp to the SPU's range (pc_se_out, in
 * GtSoundHandle / GtSoundChannel::play, se_play_program, T468, and the
 * voices CarSound writes itself, T176), so every effect scales by it.
 * Before, the level scaled the game's volume ahead of that clamp: the
 * clashes (their level x 32 / distance, from the car's own view up to
 * ~60 times the SPU's full scale) stayed clamped at full volume down to a
 * few percent.  The game's mix, its clamps and the voices' priorities are
 * the original's at any level.
 * The level of the effects played now, by the thread playing them: the
 * race's sound code and the menus' set it for their effects
 * (PcSeLevelScope), 1 elsewhere. */
extern thread_local f32 g_pcSeOutLevel;
/* The race's / replay's level this frame (car_sound_update_se_switch). */
extern f32 g_pcRaceSeLevel;
struct PcSeLevelScope {
    f32 saved;
    explicit PcSeLevelScope(f32 level) : saved(g_pcSeOutLevel) { g_pcSeOutLevel = level; }
    ~PcSeLevelScope() { g_pcSeOutLevel = saved; }
};
/* A clamped SPU volume at the level. */
static inline int pc_se_out(int vol)
{
    return (int)((f32)vol * g_pcSeOutLevel);
}
/* Nothing heard at the level (the voice is not keyed, as at volume 0). */
static inline int pc_se_silent(void)
{
    return !(g_pcSeOutLevel > 0.0f);
}

#endif /* GAME_APP_PC_VOLUME_H */
