/* settings.h - the settings of GT3 PC (gt3pc.ini).
 *
 * gt3pc.ini lives in the game folder (paths::game_dir()); the launcher
 * writes it (docs/launcher.md, "Settings"), the game reads it at start and
 * writes back what the player changes in the game itself (the window's size
 * and place).  A missing file or key gives the default.
 *
 *   [Display]
 *   Mode=windowed | borderless | fullscreen
 *   Width=0, Height=0       fullscreen resolution (0: the desktop's)
 *   Refresh=0               fullscreen refresh rate in Hz (0: the nearest to 60)
 *   WindowWidth=1280, WindowHeight=960   the window's client size
 *   WindowX, WindowY        the window's place (missing: centred)
 *   HudMaxAspect=21:9       the widest the race HUD's canvas gets (W:H or a
 *                           number, 4:3 .. 32:9); a wider window centres it
 *                           (game/app/pc_aspect.h)
 *   SpanMonitors=           monitors the picture spans in borderless /
 *                           fullscreen mode (borderless either way), their
 *                           device names (\\.\DISPLAYn) with commas, any order
 *                           (left to right by their desktop places); empty:
 *                           one monitor
 *   SplitMonitor=auto       the 2-player split screen's second monitor
 *                           ([Graphics] SplitScreen=monitors): a device name,
 *                           or auto (the one nearest the game's)
 *
 *   [Graphics]              (the in-game settings menu, F2, writes it)
 *   AntiAliasing=off | fxaa | msaa2 | msaa4 | msaa8
 *                           fxaa: the race views' 3D picture filtered before
 *                           the HUD; msaa*: every picture multisampled
 *                           (docs/renderer.md "Anti-aliasing")
 *   RenderScale=100         the pictures' resolution, percent of the window's
 *                           (10 .. 400); above 100 they are filtered down to
 *                           it (supersampling) (docs/renderer.md "Targets")
 *   Lod=on | off            off: the models' levels of detail unused (always
 *                           the full detail; the split screen draws the full
 *                           course) (docs/renderer.md "Detail and distance")
 *   RenderDistance=100      percent of the course's own draw distance
 *                           (100 .. 400)
 *   FrameCap=0              the shortest frame time in ms (a frame rate cap:
 *                           1000 / the rate), 1 .. 100; 0: none
 *   VSync=off | 1 | 2 | 3   vertical sync with at most that many frames
 *                           queued (docs/renderer.md "Frame rate")
 *   Fov=100                 the driving views' field of view, percent of
 *                           the camera's angle (50 .. 200; not the mirror)
 *   ReplayFov=100           the same for the replays' cameras
 *   SplitScreen=horizontal | vertical | monitors   the 2-player split screen:
 *                           one view above the other, side by side, or one
 *                           monitor each (from the next race or replay)
 *   SplitHud=digital | analog   the split screens' meters
 *   SingleHud=analog | digital  a single player's meters
 *   HudSafeZone=100         the race HUD's part of the window in percent (75 ..
 *                           100): below 100 the HUD is scaled down about the
 *                           window's centre to fit it (game/app/pc_aspect.h)
 *
 *   [Audio]                 (the settings menu writes it too)
 *   Output=auto | ks | wasapi   ks: kernel streaming straight to the driver
 *                           (the device is the game's alone while it plays);
 *                           wasapi: through the Windows audio engine; auto:
 *                           kernel streaming, else WASAPI (docs/sound.md 3.5)
 *   Buffer=2                the output period in ms (1 .. 20; the device
 *                           holds about two)
 *
 *   [Game]                  (the settings menu writes it too)
 *   Language=auto           the game's language from the next start: auto
 *                           (the installed disc's own), japanese, english_us,
 *                           english_uk, french, german, italian, spanish
 *                           (platform/language.h)
 *   Version=US              US | EU | JP: the version the launcher last
 *                           played, played when the game starts without
 *                           --data (platform/paths.h reads it)
 *   SpeedUnit=auto          auto | kmh | mph: speed and distance (km/h, km, m
 *                           or mph, miles); auto: the language's own
 *   PowerUnit=auto          auto | ps | hp | bhp | cv | kw
 *   TorqueUnit=auto         auto | kgfm | ftlb | nm (game/app/pc_units.h; the
 *                           settings menu changes them at once)
 *   StartingCredits=auto    auto | 15000 | 18000: a new game's money (auto:
 *                           the version's own, its product config's
 *                           "initial": USA / Europe 18,000, Japan 15,000);
 *                           in Japanese x 100 (yen), as every price
 *                           (game/app/pc_new_game.h)
 *
 *   [Debug]                 (the launcher's Debug tab; read at start)
 *   Console=off | on        on: the log (gt3pc.log: the PC side's messages
 *                           and the game's printf) also in a console window
 *                           of its own (platform/log.h open_console)
 *   FileLog=off | on        on: every file the game reads (with its size),
 *                           writes or misses in the log ("file: ...",
 *                           platform/host_file.h)
 *   SyncLog=off | on        on: what makes the game thread wait for the
 *                           renderer's D3D thread, each caller once ("pgl
 *                           sync: ...", engine/pgl/native/pgl_queue.h)
 *   FillGarage=off | on     a new game's garage: one of every car of the
 *                           database in every colour (game/app/pc_new_game.h)
 *   FillMoney=off | on      a new game's money: 100,000,000 (x 100 in
 *                           Japanese)
 *   UnlockArcade=off | on   every course of the arcade modes in each of them
 *                           (Cote d'Azur in two players, slalom80r in single
 *                           race...), all open (a new game's and a loaded
 *                           one's); off, the product config decides
 *   UnlockLicenses=off | on every licence held (each test at least bronze; a
 *                           new game's and a loaded one's)
 *
 *   [Controller1], [Controller2], [Wheel1], [Wheel2]   the controls'
 *                           mappings and dead zones (the launcher's Controls
 *                           tab; the settings menu writes the dead zones):
 *                           platform/input_map.h, read by load
 *
 *   [CarKinds]              (no launcher UI; read at start) the enhanced
 *   <car code>=<kind>       engine's special kind for a car code, beyond the
 *                           built-in table (T282): for a car whose model was
 *                           transplanted (the Pod's on another car), e.g.
 *                           celica_gt_four_95=pod.  Kinds: pod, sl, wing_gtr,
 *                           wing_xf, wing_cien, wing_w12, none (or the
 *                           number). */
#pragma once

namespace settings {

enum class DisplayMode { windowed, borderless, fullscreen };

struct Display {
    DisplayMode mode = DisplayMode::windowed;
    int width = 0, height = 0;      /* fullscreen; 0: the desktop's */
    int refresh = 0;                /* fullscreen; 0: the nearest to 60 Hz */
    int windowW = 1280, windowH = 960;
    bool windowPlaced = false;      /* windowX / windowY valid */
    int windowX = 0, windowY = 0;
    float hudMaxAspect = 21.0f / 9.0f;  /* the race HUD canvas's widest aspect */
    char spanMonitors[256] = "";    /* SpanMonitors (empty: none) */
    char splitMonitor[64] = "";     /* SplitMonitor (empty: auto) */
};

enum class AntiAliasing { off, fxaa, msaa2, msaa4, msaa8 };
constexpr int kAntiAliasingCount = 5;

/* Reads gt3pc.ini (after paths::init). */
void load();
const Display &display();

/* The window's client size and place in windowed mode, written to
 * gt3pc.ini at once. */
void save_window(int x, int y, int w, int h);
/* [Display] Mode, changed in the game (the settings menu's SCREEN MODE,
 * which switches the window to it too), written to gt3pc.ini at once. */
void set_display_mode(DisplayMode m);
/* [Display] MenuAspect=original | stretch: the menus (every frame without a
 * race view) at 4:3 with bars, or stretched over the window, their car
 * models and texts squeezed back to their shape (game/app/pc_aspect.h).
 * Any thread; changed in the game (the settings menu), written at once. */
bool menu_stretch();
void set_menu_stretch(bool on);
/* The race HUD canvas's widest aspect w:h, changed in the game (held to
 * 4:3 .. 32:9), written to gt3pc.ini at once. */
void set_hud_max_aspect(int w, int h);

/* [Graphics]: read by any thread (the renderer each frame), changed in the
 * game (the settings menu) and written to gt3pc.ini at once. */
AntiAliasing anti_aliasing();
void set_anti_aliasing(AntiAliasing aa);
/* The samples per pixel of an MSAA mode (1 for the others). */
int msaa_samples(AntiAliasing aa);
const char *anti_aliasing_name(AntiAliasing aa);     /* the ini value */
/* [Graphics] RenderScale: the pictures' resolution in percent of the
 * window's (100: the window's own). */
constexpr int kRenderScaleMin = 10, kRenderScaleMax = 400;
int render_scale();
void set_render_scale(int percent);
/* [Graphics] Lod: the levels of detail used (the original game), else the
 * full detail always. */
bool lod_enabled();
void set_lod_enabled(bool on);
/* [Graphics] RearViewBumper=on, RearViewChase=off: a live race's rear-view
 * mirror with the bumper camera (the original's) / the chase camera (none
 * in the original), on or off (game/app/pc_detail.h). */
enum class RearViewCamera { bumper, chase };
constexpr int kRearViewCameras = 2;
bool rear_view(RearViewCamera c);
void set_rear_view(RearViewCamera c, bool on);
/* [Graphics] RenderDistance: percent of the original draw distance. */
constexpr int kRenderDistanceMin = 100, kRenderDistanceMax = 400;
int render_distance();
void set_render_distance(int percent);
/* [Graphics] FrameCap: the shortest frame time in ms (0: no cap; else
 * kFrameCapMin .. kFrameCapMax, kept to 0.001 ms). */
constexpr double kFrameCapMin = 1.0, kFrameCapMax = 100.0;
double frame_cap_ms();
void set_frame_cap_ms(double ms);
/* [Graphics] VSync: 0 off, else vertical sync with at most 1 .. 3 frames
 * queued. */
int vsync();
void set_vsync(int frames);
/* [Graphics] Fov: the driving views' field of view in percent of the
 * camera's own (kFovMin .. kFovMax; 100 the original). */
constexpr int kFovMin = 50, kFovMax = 200;
int fov_percent();
void set_fov_percent(int percent);
/* [Graphics] ReplayFov: the same for the replays' cameras (the attract
 * mode's demo races too). */
int replay_fov_percent();
void set_replay_fov_percent(int percent);
/* [Graphics] SplitScreen: the 2-player split screen's views one above the
 * other (horizontal, the default), side by side (vertical), or each on a
 * monitor of its own (monitors: player 2 on [Display] SplitMonitor,
 * borderless - the game window too meanwhile; side by side when there is
 * no second monitor).  A race takes it at its start (game/app/pc_aspect.h
 * pc_split_side). */
enum class SplitScreen { horizontal, vertical, monitors };
constexpr int kSplitScreenCount = 3;
SplitScreen split_screen();
void set_split_screen(SplitScreen s);
/* [Graphics] SplitHud=digital | analog: the split screens' meters (the
 * original's digital set, or the single player's analog ones);
 * SingleHud=analog | digital: a single player's (the original's analog
 * sets, or the split screen's digital one).  A race takes a change at once. */
bool split_hud_analog();
void set_split_hud_analog(bool on);
bool single_hud_digital();
void set_single_hud_digital(bool on);
/* [Graphics] HudSafeZone: the race HUD's part of the window in percent
 * (100: all of it, the original layout). */
constexpr int kHudSafeZoneMin = 75, kHudSafeZoneMax = 100;
int hud_safe_zone();
void set_hud_safe_zone(int percent);

/* [Audio]: read by the audio thread, changed in the game (the settings
 * menu: the output restarts) and written to gt3pc.ini at once. */
enum class AudioOutput { automatic, ks, wasapi };
constexpr int kAudioOutputCount = 3;
AudioOutput audio_output();
void set_audio_output(AudioOutput out);
const char *audio_output_name(AudioOutput out);     /* the ini value */
/* [Audio] Buffer: the output period in ms (kept to 0.1 ms). */
constexpr double kAudioBufferMin = 1.0, kAudioBufferMax = 20.0;
double audio_buffer_ms();
void set_audio_buffer_ms(double ms);
/* Bumped by each [Audio] change (the audio thread restarts the output). */
int audio_generation();

/* [Game] Language: kLanguageAuto, or a language of platform/language.h (0
 * Japanese .. 6 Spanish, the game's Country values).  The game takes it at
 * its start; changed in the game (the settings menu), written at once. */
constexpr int kLanguageAuto = -1;
int language();
void set_language(int lang);
const char *language_name(int lang);    /* the ini value ("auto", "english_uk"...) */
/* [Game] SpeedUnit / PowerUnit / TorqueUnit: kUnitAuto (the language's
 * preset) or the game's UnitSettings value (speed: 0 metric, 1 imperial;
 * power: 0 PS, 1 HP, 2 BHP, 3 CV, 4 kW; torque: 0 kgf.m, 1 ft.lb, 2 Nm).
 * Any thread; changed in the game, written at once; units_generation bumps
 * with each change (the game applies it at its next tick). */
constexpr int kUnitAuto = -1;
constexpr int kSpeedUnits = 2, kPowerUnits = 5, kTorqueUnits = 3;
int speed_unit();
int power_unit();
int torque_unit();
void set_speed_unit(int u);
void set_power_unit(int u);
void set_torque_unit(int u);
int units_generation();

/* [Game] StartingCredits: 0 (auto: the version's own) or the credits (15000,
 * 18000); a new game takes it.  Changed in the game, written at once. */
int starting_credits();
void set_starting_credits(int credits);

/* [CarKinds], in the file's order: the car code's name and the kind's
 * (as written, trimmed). */
struct CarKind {
    char code[48];
    char kind[16];
};
int car_kind_count();
const CarKind &car_kind(int i);

/* [Debug] Console: the log in a console window too. */
bool debug_console();
/* [Debug] FileLog: the files read and written in the log. */
bool debug_file_log();
/* [Debug] SyncLog: the waits for the renderer's D3D thread in the log. */
bool debug_sync_log();
/* [Debug] FillGarage, FillMoney (a new game's), UnlockArcade, UnlockLicenses
 * (a new game's and a loaded one's): game/app/pc_new_game.h. */
bool debug_fill_garage();
bool debug_fill_money();
bool debug_unlock_arcade();
bool debug_unlock_licenses();

const char *mode_name(DisplayMode m);

} // namespace settings
