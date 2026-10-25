# GT3 PC: port design and conventions

How the Windows port of `decomp/core_gt3enhanced` is put together, and the
rules every part of it follows. [ROADMAP.md](ROADMAP.md) has the passes.

## The rule: an honest port

The game runs as a native Windows program. Nothing emulates the
PlayStation 2: no EE / VU / GS / IOP / SPU2 emulation, no PCSX2 code
embedded, no interpreter of VU microcode or IOP modules.

- **Game code** (`src/game/`) is the decompiled C++ compiled for Windows,
  changed only where the PS2 shows through: VU0 / MMI inline assembly is
  rewritten as plain C++ that computes the same thing, and calls of the PS2
  kernel or hardware go to the platform layer.
- **The engine** (`src/engine/`) keeps its portable parts (allocator,
  printf, inflate, volume table, loaders, fonts, controller logic, the sound
  sequencer...) and replaces every hardware part with native code: graphics
  on Direct3D 11, sound through a native mixer, movies through a native
  MPEG-2 decoder, files through Win32, controllers, memory card files.
- **PS2 data formats stay.** The game's files are read as they are on the
  disc: textures in GS formats with their CLUTs, models as the VIF packets
  the VU1 microcode was fed, sound banks in SPU2 ADPCM, movies as PSS. The
  native code decodes these formats (a format decoder is not an emulator).
  Where a format can only be understood from what PS2 hardware does with it
  (SPU2 ADPCM filters and ADSR envelopes, the IPU bitstream, the GS CLUT
  layout), PCSX2 or Sony's documentation is the reference, kept to the
  minimum; the code is written for this port.

## Platform

- Windows 10 / 11, **32-bit x86**. The data files hold 32-bit pointers that
  the game relocates in place (`*_relocate` functions everywhere), and the
  game's structures mirror them; a 64-bit build would mean rewriting every
  loader. A 32-bit program built `/LARGEADDRESSAWARE` gets 4 GB on 64-bit
  Windows.
- **MSVC** (Visual Studio Build Tools 2026, v14.51), C++17, static CRT,
  CMake + Ninja. The Windows SDK (Win32, Direct3D 11, DXGI, WASAPI), and
  one third-party library: SDL3 for the game controllers (roadmap
  "Controller support 1"; `cmake/sdl3.cmake` downloads the pinned official
  release into `third_party/`, and `SDL3.dll` ships next to the game,
  delay-loaded: without it the game runs with the keyboard).
- `setup.cmd` (`tools/setup.ps1`) readies a machine: the x86 tools, CMake +
  Ninja and a Windows SDK from an installed Visual Studio 2022 / 2026 (the
  missing components added with its installer), else a new Build Tools
  2026 (Microsoft's bootstrapper, signature checked); then it configures
  `build/` (afresh when it was configured in another folder or with a
  compiler that is gone). `-Check` only reports.
- `build.cmd` builds everything into `build/`; `tools\msvc.cmd <command>`
  runs a command in the x86 MSVC environment (the newest Visual Studio with
  CMake + Ninja too, else the newest with the x86 tools).

## Architecture

```
 src/game/      the game (decompiled C++; menus, races, physics, HUD, sound logic)
   |  gfx_* pgl_* gs_* modelset_* tex1_* font_*      file_* vol_*   mc_*   snd_* se_* bgm_*
 src/engine/    pgl (graphics library) -> renderer   io   memcard   sound driver / mixer
                media (MPEG-2, IPU images)  input (pads)  pdistd (alloc, printf, inflate)
   |
 src/platform/  window, Direct3D 11 device, audio device, keyboard / pads, files,
                game threads (PS2 priority rules), interrupts (vblank), timer, log
```

### Threads and timing

The game was written for the EE kernel: one CPU, threads with priorities,
a running thread keeps the CPU until it blocks (semaphore, sleep, vsync
wait) or a higher-priority thread becomes ready. The render thread (priority
7) draws and updates the current game object every frame while the main
thread (priority 9) runs the menu and race flow; the code relies on that
order and has no locks between them. The port keeps the rule:

- **Game threads** are native threads of which exactly one runs at a time
  (the others wait on the scheduler). The scheduler switches at kernel calls
  (`WaitSema`, `SignalSema`, `SleepThread`, `WakeupThread`,
  `ChangeThreadPriority`, `RotateThreadReadyQueue`, vsync waits...) and at
  explicit preemption points in long engine loops (file reads, inflating),
  always to the highest-priority ready thread.
- **Interrupt handlers** (the vblank handlers of the graphics driver and of
  the music sequencer) run on their own native thread, as real interrupts
  do: concurrently with the game thread, excluded only by the sections that
  disable interrupts (`DI` / `intr_suspend` / `iSignalSema` rules).
- **Vblank** ticks at 59.94 Hz (NTSC) from a high-resolution timer in pass 1.
  The game's frame logic counts vblanks; frame-rate independence is a later
  pass.
- Native services that were IOP modules (file streaming, the SPU2 mixer,
  BGM streaming) are ordinary native threads with their own locks, outside
  the game scheduler.

### Memory

The game heap (`malloc` / `memalign` / `free` of newlib, the pdistd
allocator, `operator new`) is a fixed arena allocated at start-up, bigger
than the PS2's ~24 MB (pass 1: 128 MB). Code that sized pools from the free
heap keeps working. The scratchpad (0x70000000) and uncached address views
do not exist: the engine code that used them is rewritten.

### Graphics

The pgl library (`engine/pgl`) is an OpenGL-like layer over GS register
shadows, VIF / GIF packets and VU1 microcode. The port keeps its state
model (the `GsContext` register shadows that game code and pgl functions
set) and replaces the packet building and the hardware with a Direct3D 11
renderer:

- Render state comes from the GS register shadows at draw time (ALPHA,
  TEST, ZBUF, FRAME, TEX0 / TEX1 / CLAMP, TEXA, FOGCOL, SCISSOR, PRIM bits),
  mapped to cached D3D11 state objects.
- VRAM addresses stay the handle space: frame buffers, render targets, image
  uploads and CLUT uploads register their address range; TEX0 / TBP / CBP
  resolve to native textures decoded from the uploaded data.
- Prebuilt VU1 models (VifModel parts) are decoded once into vertex and
  index buffers; shaders do what the VU1 programs did (transform, lighting
  modes, environment maps, fog).
- Immediate primitives, sprites and text go through dynamic vertex buffers.
- Every render target is drawn at the output resolution whatever its GS
  size, so the 640 x 224 field buffers come out at full vertical resolution.
  Pass 1 shows the game at 4:3 in the window.

### Sound

The EE side (`engine/sound`, `game/sound`) keeps its logic and talks to a
native mixer instead of the pdispu2 / libsd IOP modules: 48 voices with SPU2
ADPCM decoding, ADSR envelopes, pitch and volume, the reverb, and the BGM
stream (`.ads`) player. The mixer runs on the audio device thread.

### Files

The game reads every file loose from the `data\` folder next to it, through
the game's own volume code (`engine/io`) with the volume mounted as the
development builds' loose `host:data/` files (type 3); `host:` reads are
Win32 file reads. The launcher unpacks the disc's `GT3.VOL` into `data\`.
(Pass 1 read `gt3.vol` itself, by sectors.) See `docs/launcher.md` and the
roadmap's "Game directory files".

## Source layout

| Path | Content |
|---|---|
| `src/game/`, `include/game/` | the game, from `core_gt3enhanced` |
| `src/engine/`, `include/engine/` | the engine: kept portable code + native replacements |
| `src/platform/` | Win32 / Direct3D 11 / audio / threads |
| `src/main/` | the executable's entry point |
| `src/launcher/` | the launcher |
| `tools/` | Python tools (`gt3vol.py`) and C++ test tools, one folder each |
| `docs/` | this file, the roadmap, per-area notes (`docs/<area>.md`) |
| `gt3_disc/` | the extracted disc (development only; not shipped) |

## Conventions

- C++17 for new code. Decompiled code keeps its comments, names and
  `// FUNC 0x...` address markers: they tie it to the decomp and the retail
  binary. When the port changes decompiled code, a `/* PC: ... */` comment
  says what and why.
- New native code explains its PS2 origin where it replaces something (which
  function, which IOP module, which hardware behaviour), in the comment style
  of the decomp: plain sentences, offsets in hex.
- No third-party code. Windows SDK only, apart from SDL3 for the
  controllers (above), used as its released DLL.
- Libraries that decode PS2 formats (models, textures, sound, movies) are
  plain C++ with no Windows or game dependency, so test tools can use them
  alone.
- Each test tool lives in `tools/<name>/` with a `CMakeLists.txt` that works
  both included from the top level and on its own:

  ```cmake
  cmake_minimum_required(VERSION 3.25)
  project(pssdump LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  get_filename_component(GT3PC ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
  add_executable(pssdump main.cpp ${GT3PC}/src/engine/media/native/mpeg2.cpp)
  target_include_directories(pssdump PRIVATE ${GT3PC}/src)
  ```

  Built alone: `tools\msvc.cmd cmake -S tools/pssdump -B build/tools/pssdump
  -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo`, then `tools\msvc.cmd cmake
  --build build/tools/pssdump`.
- `python tools/gt3vol.py list|extract|extract-all` reads files out of
  `gt3_disc/files/gt3.vol` (gzip-packed entries are inflated). The game reads
  `gt3_disc/files/data/`, unpacked from it by `gt3launcher.exe --unpack
  gt3_disc\files\gt3.vol --to gt3_disc\files`.
