# Gran Turismo 3 A-Spec for Windows (GT3 PC)

A native Windows port of Gran Turismo 3 A-Spec (SCUS-97102), built from the
decompiled game.
(GT3 with GT Concept's game-logic updates). The game's own code is compiled
for Windows; everything the PlayStation 2 did in hardware is rewritten
natively (Direct3D 11 graphics, a native sound mixer, a native MPEG-2
decoder, Win32 files, threads and input). Nothing is emulated.

- [docs/ROADMAP.md](docs/ROADMAP.md): what is done and what comes next.
- [docs/PORT.md](docs/PORT.md): how the port is built and the rules it follows.
- Notes per area: [system](docs/system.md), [runtime](docs/runtime.md),
  [graphics state](docs/pgl.md), [textures and effects](docs/graphics.md),
  [models](docs/models.md), [sound](docs/sound.md), [movies](docs/movies.md),
  [launcher](docs/launcher.md).

## Playing

You need your own copy of the game: the US disc image of Gran Turismo 3
A-Spec (SCUS-97102).

1. Run `gt3launcher.exe`, choose **Install game files...** and pick the disc
   image (`.iso`). The launcher unpacks the game files into `data\` next to
   the game (5.85 GB, half a minute on an SSD).
2. Choose the display (windowed, borderless window or fullscreen, the
   resolution and refresh rate) in the launcher, then press **Play**, or run
   `gt3pc.exe` directly. In the game, Alt+Enter switches between the window
   and full screen.

Saves live in `memcard1\` and `memcard2\` next to the game (memory card
slots 1 and 2), one folder per PS2 save.

Controllers: a DualShock 4 (or any controller SDL3 knows: DualSense, Xbox,
Switch Pro...) is a DualShock 2. The first one connected is player 1, the
second player 2. Buttons go by position (bottom face button = Cross),
Options = Start, Share = Select. L2 / R2 are analog (the trigger travel is
the pressure), and the game's vibration drives the rumble. `SDL3.dll` must
be next to `gt3pc.exe`; without it the keyboard still works.

Keyboard (player 1, together with the first controller):

| Key | Button | Key | Button |
|---|---|---|---|
| arrows | D-pad | Enter | Start |
| Z | Cross | Backspace | Select |
| X | Circle | Q / W | L1 / R1 |
| A | Square | E / R | L2 / R2 |
| S | Triangle | numpad 4 6 8 2 | left stick |

The launcher's Controls tab remaps the controllers (a gamepad and a
keyboard / mouse input for each button), sets up a steering wheel for each
player (a Logitech Driving Force to the game; with both connected, the
player uses the wheel or the controller, as chosen there) and their dead
zones. A
controller's Guide (PS / Xbox) button opens the settings menu (F2); held
1.5 s, the Esc menu.

## Building

Visual Studio 2022 or 2026 (Build Tools are enough) with the C++ x86/x64
tools, the C++ CMake tools and a Windows 10/11 SDK. On a new machine,
`setup.cmd` gets them: it adds what is missing to an installed Visual
Studio, or installs Visual Studio Build Tools 2026, then configures
`build\` (`setup.cmd -Check` only reports).

```
setup.cmd                     :: once per machine
build.cmd                     :: everything, into build\bin\
build.cmd --target gt3pc      :: the game only
```

The game finds the extracted disc of this folder (`gt3_disc\files\data\`,
unpacked once with `build\bin\gt3launcher.exe --unpack gt3_disc\files\gt3.vol
--to gt3_disc\files`) by itself when run from `build\bin`; `--data <folder>`
points it elsewhere.
Its log is `gt3pc.log` next to the executable.

## Layout

| Path | What |
|---|---|
| `src/game/` | the game: the decompiled C++ (menus, races, physics, HUD, sound logic) |
| `src/engine/` | Polyphony's libraries: kept where portable, native replacements for the hardware parts (`*/native/`) |
| `src/platform/` | Windows: window, Direct3D 11, audio, files, pads, memory cards, the game-thread scheduler |
| `src/main/`, `src/launcher/` | the game's entry point; the launcher |
| `include/` | the game's headers (from the decomp) and the port's versions of the PS2 SDK headers |
| `tools/` | `gt3vol.py` (list / extract gt3.vol), `import_decomp.py`, and test tools |
