# The launcher (gt3launcher.exe)

An optional, lightweight window to set things up before running the game.
It starts barebones: it installs the game files from the user's disc image
and starts the game. Settings are added to it pass by pass. The game itself
(`gt3pc.exe`) never needs the launcher once the files are installed.

Source: `src/launcher/` (plain Win32, C++17, x86 like the game, Windows SDK
and the game's `src/platform/input_map.cpp`, static CRT; SDL3 delay-loaded
for the Controls tab, as in the game). Built with the rest (`build.cmd`), or
alone with `build.cmd --target gt3launcher`; the output is
`build/bin/gt3launcher.exe`, `SDL3.dll` next to it.

## The window

A small fixed-size window (640 x 360 at 100% scaling) over the launcher's art
(`launcher_bg.png`, 640 x 360, built in as a resource and scaled to the
window's DPI by WIC), the operating system's own controls and message font,
per-monitor DPI aware (v2), and the game icon (`gt3pc.ico`).

The settings are in six tabs, on a darkened panel at the top left (every
setting of `gt3pc.ini` the game's F2 menu has, the display ones only the
launcher sets, the control mappings and the debug console), each written to
`gt3pc.ini` as soon as it changes:

| Tab | Settings |
|---|---|
| Display | Display mode, Resolution, Refresh rate, Vertical sync, Frame rate cap, Span monitors |
| Graphics | Anti-aliasing, Resolution scale, Levels of detail, Render distance, Field of view, Replay field of view |
| Audio | Audio output, Audio buffer |
| Game | Split screen, Split screen 2nd monitor, Split screen HUD, Single player HUD, Race HUD max width, HUD safe zone |
| Controls | Controller 1 / 2, Wheel 1 / 2: Mapping, Calibration (below) |
| Debug | Debug console: the game's log (`gt3pc.log`: the PC side's messages and the game's own printf) also in a console window of its own, from the start of the run; Log file loads: each file the game reads (`file: data\... (N bytes)`; a packed one as its `.gz`), writes or misses, in the log (the console too); Log D3D syncs: what makes the game wait for the renderer's D3D thread (`pgl sync: ...`, each caller once) |

Each tab is a page (`src/launcher/panel.*`, a child window over the art)
that scrolls when it holds more than fits: a scroll bar shows then, the
mouse wheel scrolls the page under the pointer, and the focus moving to a
control out of view scrolls it in.

### Controls

The tab edits `[Controller1]`, `[Controller2]`, `[Wheel1]`, `[Wheel2]`,
`[DebugCamera]` and `[Hotkeys]` (`src/launcher/controls.*`; the format:
`src/platform/input_map.h`). At the top the profile - Controller 1 / 2,
Wheel 1 / 2, and Debug camera: the debug camera's controls (renderer.md
"Debug camera"; move, look, speed, field of view, lock) from the first
gamepad and the keyboard / mouse, with dead zones of its own; its look's
keyboard / mouse slots also take the mouse's movement (the capture: the
cursor moved 48 pixels one way) - and three sub-tabs:

- **Mapping**: a row per virtual control - first the chase camera's free
  orbit (Chase cam left / right / up / down: by default a controller's right
  stick and Controller 1's J / L / I / K, nothing on a wheel; renderer.md
  "Free camera"), then a DualShock 2's buttons, D-pad and stick directions,
  or a Driving Force's steering, pedals, 12 buttons and D-pad - with two
  slot buttons, the device's (Gamepad: the controller of
  that port, buttons and axes by position; Wheel: the wheel's joystick, raw)
  and the keyboard / mouse's. A click (or Enter / Space) waits 5 s for the
  input to take, Esc cancelling: slot 1 a key, mouse button or wheel notch
  (a hotkey's key without modifiers - F2, F3, F5, F6, F8, F9 by default - is
  the game's and refused); slot 0 a gamepad
  button (not Guide: the game's menu button) or axis moved past half, or a
  wheel's button, hat direction or axis (one resting at an end, a pedal, as
  the whole axis). Inputs held when it starts wait for their release. A
  right-click or Delete clears a slot; **Reset to defaults** puts back the
  profile's mapping (its dead zones and device kept). A wheel input from a
  joystick that Automatic would not pick names that joystick as the
  wheel's Device.
- **Calibration**: the profile's three dead zones (0 .. 50 %: the sticks'
  and the triggers', or the steering's and the pedals'); for a wheel what
  its player plays with while both the wheel and the controller are
  connected (Player N plays with: the wheel, the default, or the
  controller; the other is ignored, and with only one connected that one is
  used) and its Device (Automatic - the first, or the next, joystick SDL
  calls a wheel - or a connected joystick by name); and a live view of the
  analog controls after the dead zones.
- **Hotkeys**: the game window's own keys (not a profile's: the profile
  list hides) - Settings menu (F2), Esc menu (Esc), Frame time monitor (F3),
  Windowed / full screen (Alt+Enter), Pause / next tick (F5), Resume (F6),
  Flicker capture (F8) and Draw trace (F9), the last two the debug aids'
  (renderer.md "Debug aids"), and Debug camera (F7, in a race: renderer.md
  "Debug camera") - a key button each. A click waits 5 s for a
  key that is not a modifier and takes it with the Ctrl, Alt and Shift held
  (Esc too; a click cancels). Another hotkey's combination, Alt+F4 and,
  without modifiers, a key a control's keyboard slot has (in any profile)
  are refused, the button saying whose it is. A right-click or Delete leaves
  the hotkey without a key; **Reset to defaults** puts all of them back. The
  game reads them at its start, matches the modifiers exactly (F5 is not
  Shift+F5), and keeps a hotkey's key from the game (Alt+Enter no longer
  also presses Enter); its menus and the pause indicator name the keys set.

SDL runs on a thread of its own only while the tab is shown, and stops
before Play starts the game; without `SDL3.dll` the device slots say so and
only the keyboard / mouse slots take inputs.

The percentages (resolution scale 10 .. 400 % by 5, render distance 100 ..
400 % by 5, the fields of view 50 .. 200 % by 1, the HUD safe zone 75 .. 100
% by 1), the audio buffer (1 .. 20 ms by 0.1) and the Controls tab's dead
zones (0 .. 50 % by 1) are sliders with their value beside them (arrow keys
a step, Page Up / Down five), written when the slider is let go; a value in
the file between the steps shows as it is until the slider moves. A value
in `gt3pc.ini` that none of a list's entries stands for (a frame cap set as
a frame time in the game) keeps an entry of its own, marked "(as set)". The frame time monitor (F3) is not saved: it
stays in the game's menu. Ctrl+Tab / Ctrl+Shift+Tab (or Ctrl+PageDown /
PageUp) change tabs; the tab headers are in the Tab key's order too.

- **The Play tab** (the first, shown at start): a row per version -
  Gran Turismo 3 A-Spec (USA), (Europe), (Japan) - with its state
  (installed from which disc, incomplete, not installed) and a **Play**
  button, enabled when its data folder (`data_US`, `data_EU`, `data_JP`) is
  installed; under them the data-only extractions (`data_<boot file>`: GT
  Concept, demos), listed, not playable. Play starts `gt3pc.exe --data
  <its data folder>` from the launcher's folder (the working directory),
  writes `[Game] Version` (the last played: the game started without
  `--data` takes it) and closes the launcher. docs/languages.md "The
  versions and their files".
- **Install game files...** opens the standard Open dialog ("PlayStation 2
  disc image (*.iso)" / all files), checks the chosen image and asks before
  installing it: a version's disc into its data folder (its files there
  replaced), any other Gran Turismo 3 engine disc extracted as data only.
  While it unpacks, a progress bar and a **Cancel** button replace it; the
  window stays responsive. When the folder has a `gt3.vol` that an earlier
  version installed (and no `data_US`), it first offers to unpack that
  instead, without a disc image.
- **Exit**. Closing the window during an install asks first, then cancels it
  cleanly.
- A status line says what to do: choose a version to play, or install one
  (or that an old `gt3.vol` can be unpacked).
- At start, an earlier launcher's `data\` (one disc at a time) is moved to
  its version's folder (by its marker's boot file; `data_US` without one),
  unless that folder exists; the status line says so.
- After an install, if an old `gt3.vol` is still there, it asks whether to
  delete it (the game no longer reads it).
Enter plays the highlighted version (the last played when installed, else
the first installed; Install with none), Esc exits (or cancels an install),
Tab moves between the controls.

The background is drawn from one bitmap (`build_background` /
`draw_background` in `src/launcher/ui.cpp`): the art with the panel behind
the tabs and the settings darkened; the labels and the status line paint it
behind their text (a pattern brush of it from their place), the tab headers
too (owner-drawn: the chosen one white and underlined in red). The status
line and the progress bar keep left of the art's logo. Everything else stays
standard Windows UI.

## Installing the game files

The game reads its files loose from a data folder in its folder, one per
version (`data_US`, `data_EU`, `data_JP`; the "game directory files" pass
had one `data\`, pass 1 read the volume `GT3.VOL` itself). All of them come
from the disc's volume `GT3.VOL` (the US disc's: 3,876,222,976 bytes, 5,333
files in 57 folders, 1,705 of them gzip-packed). The installer:

1. Reads the image's ISO 9660 file system (`src/launcher/iso9660.*`, a
   read-only reader: path lookup, directory listing, reads at 64-bit
   offsets, multi-extent files). The image is opened read-only and never
   written. GT3's DVD carries GT3.VOL whole in its ISO 9660 tree (one
   extent), so the UDF side of the disc is not needed.
2. Checks the disc: a Gran Turismo 3 engine disc has `CORE.GT3`, the
   engine's main program, at its root or in a folder of the root (a demo
   disc's game); a disc without one is refused. Any region and release is
   taken - Gran Turismo 3 A-Spec (USA SCUS-97102 and PBPX-95503, Europe
   SCES-50294, Japan SCPS-15009), GT Concept (2001 Tokyo SCPS-15010, 2002
   Tokyo-Geneva SCES-50858, 2002 Tokyo-Seoul SCPS-56005), the demos: the
   known boot files (`SYSTEM.CNF`) give the release's name, any other is
   named by the disc's volume name, folder and boot file
   (docs/languages.md). The volume beside `CORE.GT3` - `GT3.VOL`, else
   another `*.VOL` with a RoFS table of contents - must lie whole inside the
   image file (a truncated image is caught here) and start with a RoFS
   table of contents (magic `RoFS`, or `AD 90 B9 AC` when the TOC is stored
   inverted; see `tools/gt3vol.py`). `CORE.GT3` is inflated (two load-source
   bytes, the u32 unpacked size, raw deflate: `tools/gt3/gt3pack.py`) for the
   car database files its code reads (`database/paramdb<region>.db`,
   `paramstr`, `paramunistr`), whose region gives the disc's own language.
   The boot file gives the data folder: SCUS_971.02 and PBPX_955.03
   `data_US`, SCES_502.94 `data_EU`, SCPS_150.09 `data_JP`, any other (and a
   demo disc's game in a folder) `data_<boot file>` (or `data_<folder>`),
   data only.
3. Lists the volume's files from its table of contents
   (`src/launcher/volume.*`, the layout of `tools/gt3vol.py`; names and
   extents are checked), checks the free disk space for the unpacked files
   (6,281,183,829 bytes), then, on a worker thread, reads the files in disc
   order (one pass over the image) and writes each to
   `<data folder>.part\<its path>` - the volume's path in the unified
   structure (`unified_path`): a volume with its courses in `courses\`
   itself (the Japanese disc) has them put into `courses\<the disc's
   region>\` as the others have (docs/languages.md). The packed ones are inflated
   (`src/launcher/inflate.*`, gzip / deflate, no third-party code) and checked
   against their CRC-32 and size; the game's loose-file device wants every
   file plain (a `<name>.gz` next to a file is still honoured, as by the
   original).
4. Writes the marker `<data folder>.part\gt3pc_install.txt` last - "key
   value" lines the game reads (`paths::install_info`): `release` (the
   disc's name), `boot`, `language` (the disc's own: `US`, `BR` for English
   (UK), `JP`...), `database` / `strings` / `unistrings` (its core's car
   database files), then `files`, `bytes`, `source` - then puts the folder
   in place (a new install of a version replaces its files): an installed
   `data_US` is renamed `data_US.old`, `data_US.part` becomes `data_US`, and
   `data_US.old` is deleted. An interrupted or cancelled install never
   leaves a partial data folder (the `.part` folder is deleted, or removed
   by the next install), and one without the marker is reported as
   incomplete. A folder with a file open
   in another program cannot be renamed, and right after the unpack an
   antivirus or the search indexer is often still reading the new files
   (user report 2026-10-24: access denied at the end, the files whole in
   `data.part`): each rename is tried again every 250 ms for up to two
   minutes, the status saying so; if the game is running (it holds files of
   `data` open), the swap then fails with a message to close it.

`--unpack` (and the window's offer) do steps 3 and 4 from a `gt3.vol` file
instead of the image; a `CORE.GT3` and `SYSTEM.CNF` beside it (the source
tree's `gt3_disc\files`) name the release and its data folder, else it is
taken for an earlier launcher's install (the US disc: `data_US`). An
earlier launcher's marker (without the keys) reads as Gran Turismo 3
A-Spec (USA).

## Settings

`gt3pc.ini` in the game folder (`src/launcher/display.*` writes the display
keys, `src/launcher/ui.cpp` the others - its option table; the game reads it
at start and its F2 menu writes the same keys: `src/platform/settings.*`).
Missing keys take their defaults.

```
[Display]
Mode=windowed            windowed | borderless | fullscreen
Width=0                  fullscreen resolution (0: the desktop's)
Height=0
Refresh=0                fullscreen refresh rate in Hz (0: the nearest to 60)
WindowWidth=1280         the window's client size
WindowHeight=960
WindowX=...              the window's place (the game writes it)
WindowY=...
MenuAspect=original      original | stretch (the Display tab; the in-game
                         menu too): the menus at 4:3 with bars, or stretched
                         over the window, their car models and texts keeping
                         their shape (renderer.md "Aspect")
HudMaxAspect=21:9        (the Game tab; the in-game menu too) the
                         widest the race HUD's canvas gets (W:H or a number,
                         4:3 .. 32:9); a wider window centres it (renderer.md
                         "Aspect")
SpanMonitors=            the monitors one picture spans in borderless or
                         fullscreen mode: their device names with commas
                         (\\.\DISPLAY1,\\.\DISPLAY2), any order (left to
                         right by their places on the desktop); empty: one
                         monitor
SplitMonitor=auto        the 2-player split screen's second monitor (player
                         2's screen; [Graphics] SplitScreen=monitors): a
                         device name (\\.\DISPLAY2), or auto (the one nearest
                         the game's)

[Graphics]               (the Display, Graphics and Game tabs; the in-game
                         menu too)
AntiAliasing=off         off | fxaa | msaa2 | msaa4 | msaa8 (renderer.md
                         "Anti-aliasing")
RenderScale=100          the pictures' resolution, percent of the window's,
                         10 .. 400; above 100 filtered down to the window
                         (supersampling) (renderer.md "Targets")
Lod=on                   on | off: off, always the full detail (the split
                         screen draws the full course) (renderer.md
                         "Detail and distance")
RenderDistance=100       percent of the original draw distance, 100 .. 400
FrameCap=0               the shortest frame time in ms (a frame rate cap:
                         1000 / the rate), 1 .. 100; 0: none
VSync=off                off | 1 | 2 | 3: vertical sync with at most that
                         many frames queued (renderer.md "Frame rate")
Fov=100                  the driving views' field of view, percent of each
                         camera's angle, 50 .. 200 (not the mirror;
                         renderer.md "Detail and distance")
ReplayFov=100            the same for the replays' cameras (the attract
                         mode's demo races too)
RearViewBumper=on        on | off: a live race's rear-view mirror with the
RearViewChase=off        bumper camera (the original's) / the chase camera
                         (none in the original); each draws the course and
                         the cars again (game/app/pc_detail.h)
SplitScreen=horizontal   horizontal | vertical | monitors: the 2-player split
                         screen's views one above the other, side by side, or
                         one monitor each (player 2 on SplitMonitor; side by
                         side without a second monitor) (from the next race
                         or replay; renderer.md "Aspect", "Screens"); the
                         second monitor borderless, and a fullscreen game
                         window borderless meanwhile
SplitHud=digital         digital | analog: the split screens' meters (the
                         original's bar tachometer set, or the single
                         player's dials)
SingleHud=analog         analog | digital: a single player's meters (the
                         original's dials, or the split screen's set)
HudSafeZone=100          the race HUD's part of the screen in percent, 75 ..
                         100: below 100 the HUD (its texts, the race menus,
                         the mirror) scaled down about the centre to fit it
                         (game/app/pc_aspect.h)

[Audio]                  (the Audio tab; the in-game menu too)
Output=auto              auto | ks | wasapi: kernel streaming straight to the
                         driver (on a single-instance device, other programs
                         are silent on it while the game plays), WASAPI
                         through the Windows audio engine; auto: kernel
                         streaming, else WASAPI (sound.md 3.5)
Buffer=2                 the output period in ms, 1 .. 20 (the device holds
                         about two); a crackle raises it by 0.5 by itself

[Game]                   (the Game tab; the in-game menu's GAME > LANGUAGE)
Language=auto            auto | japanese | english_us | english_uk | french |
                         german | italian | spanish: the game's language from
                         its next start; auto, each version's own. The lists
                         offer the languages whose menus an installed
                         version has (docs/languages.md)
Version=US               US | EU | JP: the version last played (the Play
                         tab writes it); the game started without --data
                         plays it
SpeedUnit=auto           auto | kmh | mph (speeds and distances)
PowerUnit=auto           auto | ps | hp | bhp | cv | kw
TorqueUnit=auto          auto | kgfm | ftlb | nm: the units shown; auto, the
                         language's own (units.md)
StartingCredits=auto     auto | 15000 | 18000: a new game's money (auto:
                         the version's own, USA / Europe 18,000, Japan
                         15,000); x 100 in Japanese, yen (game_start.md)

[Debug]                  (the Debug tab; read at the game's start)
Console=off              off | on: the log also in a console window (its
                         close button off - it closes with the game - and its
                         mouse selection off, which would hold the game's
                         threads while they write to it)
FileLog=off              off | on: each file the game reads (with its size),
                         writes or misses in the log; the size checks and
                         the probes for a packed copy are left out
SyncLog=off              off | on: what makes the game thread wait for the
                         renderer's D3D thread, each caller once ("pgl sync:
                         ..."; the same as GT3PC_SYNC_LOG=1)
FillGarage=off           off | on: a new game's garage has one of every car
                         of the database (with a model) in every colour
FillMoney=off            off | on: a new game's money 100,000,000 (Japanese
                         10,000,000,000 yen)
UnlockArcade=off         off | on: every course in every arcade mode, all
                         open (new and loaded games); off, the product
                         config's lists
UnlockLicenses=off       off | on: every licence held (new and loaded games;
                         game_start.md)

[Controller1]            (the Controls tab; the F2 menu writes the dead zones)
Cross=button:south;key:Z    a control: its device slot ; its keyboard / mouse
...                      slot (a missing key: the default; `Key=;' none);
                         CameraLeft, CameraRight, CameraUp, CameraDown the
                         chase camera's (every profile)
LeftStickDeadzone=0      percent, 0 .. 50; RightStickDeadzone, TriggerDeadzone
[Controller2]            the same (no keys by default)
[Wheel1]                 Device=auto (or a joystick's name), Input=wheel |
[Wheel2]                 controller (player 1 / 2's device while both are
                         connected), SteerLeft=joy:axis0-;, Gas=joy:axis2-full;,
                         ..., SteeringDeadzone, GasDeadzone, BrakeDeadzone
                         (src/platform/input_map.h)
[DebugCamera]            Forward, Back, Left, Right, Up, Down, LookUp,
                         LookDown, LookLeft, LookRight, Faster, Slower, FovUp,
                         FovDown, Lock (slot 0 the first gamepad; the look's
                         slot 1 by default mouse:moveup / movedown / moveleft
                         / moveright, the mouse's movement); the controller's
                         three dead zones

[Hotkeys]                (the Controls tab's Hotkeys; read at the game's start)
SettingsMenu=F2          a key's name (as key:<name>'s) after any of Ctrl+,
SystemMenu=Escape        Alt+, Shift+; none (or empty): no key; a missing or
FrameMonitor=F3          unreadable value the default. The log lists them
Fullscreen=Alt+Enter     ("settings: hotkeys ...")
Pause=F5
Resume=F6
FlickerCapture=F8
Trace=F9
DebugCamera=F7

[CarKinds]               (no launcher UI) the enhanced engine's special kind
celica_gt_four_95=pod    of a car code, beyond the built-in table (T282): for
                         a car carrying a transplanted model. Kinds: pod, sl,
                         wing_gtr, wing_xf, wing_cien, wing_w12, none (or the
                         number). The log lists them ("settings: car kind").
```

- **Windowed**: a resizable window; the list offers 4:3 client sizes that fit
  the primary monitor's work area (and the window's own size, when the game
  remembered another one). The game keeps the window's size and place when
  the player resizes or moves it.
- **Borderless window**: the window covers its monitor at the desktop's
  resolution.
- **Fullscreen**: exclusive fullscreen in one of the resolutions and refresh
  rates Windows offers for the primary monitor (`EnumDisplaySettings`), or
  the desktop's; the default rate is the one nearest to 60 Hz (the game
  runs at 59.94).
- **Span monitors** (borderless and fullscreen; greyed in windowed mode, the
  choice kept): one picture over monitors side by side. The list offers Off
  and every run of 2 or more neighbouring monitors, left to right (each one's
  left edge on the one before's right edge, 2 pixels either way, the two
  overlapping vertically by half the smaller height at least; for three in a
  row A B C: A + B, B + C, A + B + C), with the run's total width and
  smallest height: "27GL650F + BenQ GL2580 (3840 x 1080)". The monitors go
  by their friendly names (`QueryDisplayConfig`) when every monitor has one
  of its own, else by their device names (DISPLAY1 + DISPLAY2).
- **Split screen 2nd monitor**: player 2's monitor in the 2-player split
  screen on two monitors: Automatic, or any monitor ("DISPLAY2: BenQ GL2580
  (1920 x 1080)").

A saved monitor choice that no entry stands for (a monitor unplugged, or
monitors moved apart on the desktop) keeps an entry of its own, marked "(not
connected)" or "(not side by side)", and stays in `gt3pc.ini` until another
is chosen. The lists follow monitors plugged in or moved while the launcher
is open.

In the game, Alt+Enter (the Fullscreen hotkey) switches between the window
and the full-screen mode set here (borderless when the setting is
windowed), and the settings menu's DISPLAY > SCREEN MODE (windowed,
borderless, fullscreen) switches to a mode and saves it as `Mode`, on the
monitor the window is on (the rate nearest to the setting's that monitor has). The game
draws at the size of the picture shown times the resolution scale, so its
resolution follows the window (docs/renderer.md, "Targets"). Exclusive fullscreen is entered with the
window and the back buffers already at the mode's size, and the mode asked
again once in it: DXGI otherwise takes the mode nearest to the buffers (from
a 1324x993 window, 1280x1024: a 5:4 picture stretched over a 16:9 screen).
While in it the back buffer keeps the mode's size; the log's `gpu:
fullscreen on <display>: ... (the display runs WxH, N Hz)` shows the mode
applied.

In the game, F2 (or a tap of a controller's Guide button) opens the settings
menu (`src/platform/settings_menu.*`, drawn with `src/platform/overlay.*`
like the F3 frame time monitor), a list of categories: DISPLAY (the screen
mode, the menu aspect, vertical sync, the frame cap as a frame rate and, under it, as a frame
time, the frame time monitor), GRAPHICS (the resolution scale - 5 % a step, 10 once
held, Enter 100 %, shown with the window's size at it - the anti-aliasing,
the levels of detail, the render distance, the fields of view, REAR VIEW's
BUMPER CAM and CHASE CAM), HUD (the
race HUD's widest canvas, the HUD safe zone - 1 % a step, 5 once held, Enter
100 % - a single player's meters), SPLIT SCREEN, AUDIO and CONTROLS (PLAYER 1
/ 2, each with its DEVICE - the controller or the wheel while both are
connected, the Calibration tab's "Player N plays with" - and its CONTROLLER's
and WHEEL's dead zones; the mappings stay here in the launcher).
While it is open it takes the keyboard and the controllers (Up / Down
choose, Left / Right change - on a category Right opens it, Left closes it -
Enter / Space or Cross the next value or open / close, F2 / Esc / Backspace
or Circle close) and the game gets no input; the game itself goes on (pause
first in a race). A change applies at once and is written to `gt3pc.ini`.

Esc (or the Guide button held 1.5 s) opens the Esc menu (`src/platform/system_menu.*`, the same look, the
same input rules; Esc / Backspace or Circle close, Enter / Space or Cross
choose): Back (chosen when it opens, so an Enter right after Esc does
nothing), Restart (a new `gt3pc.exe` with the same arguments plus
`--restart-after <pid>`, which waits for this run to end before it opens
its log, window and audio device: the game from the beginning) and Exit (the
window closes, as its close button). Restart and Exit wait while a memory
card is being written - a file open for writing, or a change less than 2 s
ago (`memcard::busy`) - the menu saying so, Esc cancelling.

## Where things go

| File | Where |
|---|---|
| `gt3launcher.exe`, `gt3pc.exe` | the game folder |
| `data\` | the game files, installed into the launcher's folder (the game reads them from its own folder) |
| `data\gt3pc_install.txt` | the install marker, written last |
| `data.part\`, `data.old\` | only while installing |
| `gt3pc.ini` | the settings (written by the launcher and the game) |
| `gt3.vol` | not used any more (an earlier version's install; unpacked and deleted on request) |
| memory cards | `memcard1\` and `memcard2\` next to the game (written by the game, not the launcher) |

The game also finds `gt3_disc/files/data/` of the source tree and accepts
`--data <folder>` (the folder holding `data\`, `src/platform/paths.cpp`); the
launcher's Play button only looks next to itself.

## Command line

```
gt3launcher.exe                                        the window
gt3launcher.exe --install <image.iso> [--to <folder>]  install without a window
gt3launcher.exe --unpack <gt3.vol> [--to <folder>]     the same from a gt3.vol file
gt3launcher.exe --help
```

`--install` checks the disc and installs `data\` into `<folder>` (default:
the launcher's folder; created when missing), printing the progress.
`--unpack` does the same from a `gt3.vol` (an earlier install, or the source
tree's `gt3_disc\files\gt3.vol`). Exit codes: 0 installed, 1 failed, 2 not
a Gran Turismo 3 engine disc, 3 cancelled, 4 bad command line. Ctrl+C in the console
cancels the install (the `.part` folder is deleted).

`gt3launcher.exe` is a GUI program, so shells do not wait for it by
themselves: in cmd use `start /wait gt3launcher.exe --install ...` (the exit
code is then in `%ERRORLEVEL%`); in PowerShell pipe its output
(`.\gt3launcher.exe --install ... | Out-Host`); Git Bash waits. Output goes to
a redirected standard output, else to the parent's console; with neither (a
shortcut), the result is shown in a message box.

## Planned

Settings follow the roadmap's passes, each added to the launcher when its
pass is done:

- texture filtering (bilinear, trilinear), anisotropic filtering;
- the wheels' force feedback.
