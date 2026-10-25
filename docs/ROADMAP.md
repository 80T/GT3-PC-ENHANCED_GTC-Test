# GT3 PC (pc_enhanced) roadmap

The Windows port of Gran Turismo 3 A-Spec, built from the decompiled
`decomp/core_gt3enhanced` (the GT3 + Lupo 2 enhanced engine). Work goes in
passes. Each item is ticked off when it is done and tested.

## Pass 1: the most basic honest port

The game runs natively: the game code is compiled for Windows, and every part
of the PlayStation 2 backend is rewritten for Windows. There is no emulation
of the EE, GS, VU, IOP or SPU2, and no PCSX2 code is embedded. PCSX2 is a
reference only where a PS2 data format needs it (SPU2 ADPCM and envelopes,
for example), and as little as possible. There is no bootstrap port: the core
boots by itself.

Done and accepted by the user on 2026-10-19 (the baseline).

- [x] Project: CMake + MSVC, 32-bit Windows (the data files carry 32-bit
      pointers that the game relocates in place), C++ throughout.
- [x] Game code from `core_gt3enhanced` compiled with minimal changes (the
      VU0 / MMI inline assembly rewritten in C++).
- [x] Platform layer: window, timing (59.94 Hz game tick), threads with the
      PS2's priority rules, logging.
- [x] Files: `gt3.vol` read from the game folder (the volume code stays).
- [x] Graphics: the pgl / GS library re-implemented on Direct3D 11 (2D,
      text, textures, models, effects), shown at 4:3.
- [x] Input: keyboard, as a DualShock 2 (enough to test the game).
- [x] Sound: sound effects, engine sounds, music sequencer and streamed BGM
      through a native mixer.
- [x] Movies: a native MPEG-2 decoder for the PSS movies and IPU images.
   - [x] 2026-10-24 (user: "standard H.264 video ... for modding only";
         "regular audio rather than the .ads format"; "Frame rate limit
         should be 59.94"): an `.mp4` (H.264 / AAC) beside a `.pss`, or
         alone, plays in its place through Windows Media Foundation, up to
         59.94 pictures a second, scaled into the movie's slot with its
         aspect kept; its sound decoded to PCM and mixed at the BGM's gain
         (movies.md 5).
   - [ ] Tested by the user.
- [x] Memory card: saves and loads to files.
- [x] Launcher: optional, minimal window with a black background; installs
      the game files from the user's ISO and starts the game.
- [x] Game icon: `gt3pc.ico`.

## Later passes

Listed in the order given; each is planned in steps when its pass starts.

1. **Game directory files.** Load every file loose from the game directory
   instead of from a volume: like HostFS, but as the default and only way
   the game loads its files. File loading and streaming may need updating.
   - [x] Launcher: unpacks GT3.VOL from the disc image into `data\` (the
         packed files inflated and CRC-checked; a marker written last; a
         half-done install never replaces `data\`); `--unpack` and the window
         also take an earlier version's `gt3.vol`, then offer to delete it.
   - [x] Game: `vol_init` mounts the original's loose `host:data/` device
         (type 3) instead of the disc volume; `<name>.gz` beside a file is
         still read packed, as by the original.
   - [x] The course and car lists read directory listings from the volume's
         TOC: the game lists `data\` as a TOC at mount time, so files added
         there are listed too.
   - [x] Streams (BGM `.ads`, movies) read the loose files to their end; no
         sector, ISO 9660 or volume read is left in the game.
   - [x] Tested by the user (2026-10-19).
2. **Controller support 1.** Basic support for testing the game: DualShock 4
   through SDL (as used with PCSX2).
   - [x] SDL3 3.2.26 (the version PCSX2 ships), the official x86 package
         downloaded by CMake and checked by SHA-256; `SDL3.dll` next to the
         game, delay-loaded (missing: keyboard only).
   - [x] A controller thread polls SDL every 2 ms; hot-plug: the first
         controller is port 1 (with the keyboard), the second port 2.
   - [x] DualShock 2 mapping: buttons by position, raw sticks, L2 / R2
         pressure from the triggers, other buttons full pressure; the game's
         two motors drive the rumble; the light bar shows player 1 / 2.
   - [x] Tested by the user (2026-10-19).
3. **Full progressive scan rendering everywhere.** Remove all interlaced
   game logic, since it will never be needed. Menus and races both render
   progressive. (GT3 never had progressive scan.)
   - [x] The display mode non-interlaced: no field (`ctx->field` 0, no flip
         per frame, `gs_get_field_mode` answers FRAME), no half-line offsets;
         the race screens plain frames (no FRAME_SHIFT).
   - [x] The race picture copied 1:1 (`blitField`: no field variants, no
         deflicker blur).
   - [x] The game's waits for a field and the afterimage's field correction
         removed.
   - [x] Interlaced movies: their own field cadence, each field on its own
         lines (59.94 progressive pictures a second, no bob).
   - [x] Ghost cars at half strength (the mean of the frame with and without
         the ghost), not drawn on one field's rows.
   - [x] The data logger's (Analyzer's) graph drawn once (2026-10-24, user):
         no vertical deflicker blur (the buffer blended in again a line up
         and down), the speed curves not sent five times a pixel apart
         (T087).
   - [x] Tested by the user (2026-10-19).
4. **Button sensitivity scaling.** Remove it entirely. Every analog input
   function runs the full 0 to 100% range.
   - [x] Pressure buttons (`RaceInput`, T413): linear over the whole
         travel. The original was full at 17.5 % of it for the throttle,
         32.5 % for the brake, 10 % for the handbrake, 30 % for the d-pad's
         steering.
   - [x] Sticks in a race (steering, and any function put on a stick):
         linear from the centre, full at both ends, no dead zone. The
         original's curve (T472) had a dead zone of +-37 around the centre and
         clipped the last 16 steps of each end; the menus keep it for their
         directions.
   - [x] Analog pedals (`car_process_input`, T342): no 10 % minimum lift.
   - [x] Tested by the user (2026-10-19). (A stick's centre and dead zone:
         the launcher's calibration, later.)
5. **Save files.** Live loose inside the game's directory, in two folders
   that stand for in-game memory cards 1 and 2.
6. **Configurable resolution.** Fullscreen resolution chosen from the ones
   the operating system offers; borderless windowed; windowed.
   - [x] Settings file `gt3pc.ini` (launcher writes, game reads; the game
         keeps the window's size and place).
   - [x] Launcher: Display (windowed, borderless window, fullscreen),
         Resolution (the OS's modes for fullscreen, 4:3 sizes for the
         window), Refresh rate (fullscreen).
   - [x] Game: a resizable window, a borderless window over its monitor, or
         DXGI exclusive fullscreen (entered again after Alt+Tab); Alt+Enter
         switches window / full screen; no pointer over a full-screen picture.
   - [x] The game draws at the size shown: the render targets follow the
         window and the display mode (resampled once the size settles).
   - [x] Tested by the user (2026-10-19).
7. **Frame pacing / vertical sync.** Simple but effective options: a "frame
   rate cap" (really a frame time cap), and vertical sync Off or a 1-3 frame
   buffer.
   - [x] Built 2026-10-20 (renderer.md "Frame rate"): `[Graphics] FrameCap`
         (ms; the F2 menu sets it as a frame rate or as the ms value under
         it), `VSync` off / 1 / 2 / 3 frames queued.
   - [x] Tested by the user ("Good", 2026-10-20).
8. **Dynamic aspect ratio.** Remove the original anamorphic settings from the
   code (the two settings stay dormant in the external scripting / UI files;
   that is a separate change later). The aspect follows the window or
   fullscreen resolution instead:
   - Elements scale from a base 4:3 aspect, taking the full height of the
     screen at 4:3 or wider.
   - Taller than 4:3: HUD elements are capped at 4:3 and the vertical FOV
     expands.
   - Wider, up to 21:9: the HUD canvas follows; beyond 21:9 it is capped at
     21:9 and the horizontal FOV expands. The race HUD gets some adjustability
     of its canvas's maximum width.
   - Menus stay 4:3 at all times (they already do; what changes is no more
     stretching, and text stays in line with the 4:3). The race side is fully
     dynamic.
   - Examples: 1792x1080 is between the base and the maximum width, so the
     HUD is anchored to the edges. 3017x1080 is beyond 21:9, so the HUD
     canvas does not exceed 2520 px width. 1080x1080 is narrower than 4:3, so
     the HUD canvas does not exceed 810 px height and the vertical FOV
     increases.
   - Special attention: the licence test pre-race menus are 4:3; entering the
     race switches to the dynamic aspect.
   - [x] Built 2026-10-19 (renderer.md "Aspect"): race frames fill the window
         (squeezed into the GS frame, unsqueezed by targets at the window's
         size), Hor+ wider / Vert+ taller; the HUD canvas held to 4:3 ..
         `HudMaxAspect` (gt3pc.ini, 21:9), side-aligned elements at its
         edges; race menus / results / mirror / flare / vignette follow; the
         16:9 option dormant; the short menu and the licence pre-race menu
         stay 4:3; split screen top / bottom.
   - [x] User: "Good"; then a blurry HUD with letter bounds off at very tall
         aspects. Reworked 2026-10-19: one per-frame mapping, HUD draws
         (`pc_hud_begin` / `_end`: projection, font and renderer from it), the
         renderer's texel centres in design pixels (`CMD_DESIGN`), per-axis
         target scales up to 12, aspect held to 1:4 .. 6:1.
   - [x] Rework tested by the user ("Fixed"; then the split screen's camera,
         the bumper / chase switch and the race-start fade fixed: "Good").
9. **Completely dynamic frame rate and delta time.** All hard-coded timings
   and physics calculations consistent at any frame rate. Animated parts of
   course and car models (course flags; tween animations such as mudflaps
   and windscreen wipers) may need capping at 60 Hz so they do not animate
   too fast or too slow.
   - Decided with the user (2026-10-19): the simulation keeps its fixed
     59.94 Hz tick (physics, timings, replays and ghosts identical at any
     frame rate); frames are drawn between ticks, interpolated.
   - [x] The render loop: a tick per vblank; between ticks, in a running
         race on a display above 61 Hz, frames as fast as they go (no vsync,
         tearing allowed for variable refresh displays - user, 2026-10-19;
         one frame queued, the other game threads running meanwhile).
   - [x] Interpolation: the cars through GT3's own slow-motion accessors,
         the camera's view and projection blended between ticks (no blend
         across a cut).
   - [x] Per-tick model animations stay at 60 Hz (flags, wipers, mudflaps,
         smoke); draw-time state changes only in the tick's frame.
   - [x] The HUD at the frame rate (user, 2026-10-19): needles, bars,
         steering, times and fades blended between ticks (`pc_tween`); the
         licence trophies posed at the shown time.
   - [x] The car and course model animations at the shown time (user,
         2026-10-19): the tracks posed per race view, the cars' wheel shake /
         steering morphs and lamp blinks; the palette (CLUT) animations at
         the frame's end, the wing and the Pod's tail / colour blended.
   - [x] `GT3PC_FRAME_RATE` / `diag\frame_rate_144.cmd` to try it on a
         60 Hz display (a cap).
   - [x] Frame time monitor, F3 (renderer.md "Frame rate").
   - [x] Any frame rate (user, 2026-10-19: 90 under RTSS choppy, below 60
         slowed down): the frame clock is when Present returns, the ticks
         due by it run first (caught up below 60), the camera blended over
         the ticks between draws.
   - [ ] Tested by the user.
10. **Dynamic aspect ratio 2: menus.** Most menu assets can scale
    dynamically, except the backgrounds (mostly static images, a few FMV
    movies). The benchmark menu is the licence centre, which is diagonal.
    - [x] 2026-10-24 (user: "Menu aspect ratio control: Original (4:3),
          Stretch ... Menu car models and text"): `[Display] MenuAspect`, the
          F2 menu's DISPLAY > MENU ASPECT and the launcher's Display tab;
          stretched, the menus fill the window and their car models (the
          menu car render, the GT mode car list, the prizes) and texts (the
          font's glyphs and advances, alignment kept) are squeezed back to
          their shape (renderer.md "Aspect").
    - [x] 2026-10-24 (user: "Tackle Race views drawn inside a menu"): the
          race menus' race, the licence pre-race window and the logger's
          replay window take the menu's squeeze (projection, mirror, flare).
    - [x] 2026-10-24 (user: "The data logger's replay window should be able
          to have corrected HUD while stretch is active"): the logger's two
          race HUDs (steering, accel / brake monitor, bar tachometer, gear)
          squeezed back to their shape toward their side of the frame
          (`pc_hud_menu`; renderer.md "Aspect").
    - [ ] Tested by the user.
11. **Levels of detail.** On or off. Off removes their use from the code
    completely; the LODs in the models simply stay dormant.
    - [x] Built 2026-10-19 (renderer.md "Detail and distance"): `[Graphics]
          Lod`, F2 menu; off: every model / tyre / texture level 0, real tyres
          in the mirror and road reflection, the split screen draws the full
          course (main vision list, full far clip, env map, cones, lights,
          smoke, tyre blur, sparks), the mirror the main list.
    - [x] Tested by the user ("Good").
12. **Render distance.** Adjustable (probably in the Env0 or vision list
    handling).
    - [x] Built 2026-10-19 (renderer.md "Detail and distance"): `[Graphics]
          RenderDistance` 100 .. 400 %, F2 menu; far clips and light cut-offs
          scaled, the course from the vision points within the extra distance
          too.
    - [x] Tested by the user ("Good").
    - [x] 2026-10-24 (user: "Rear view Toggle, Bumper cam and Chase cam: On /
          Off"): `[Graphics] RearViewBumper` (on: the original) and
          `RearViewChase` (off; on adds the mirror to the chase camera), the
          F2 menu's GRAPHICS > REAR VIEW ("high impact on frame rate") and
          the launcher's Graphics tab (game/app/pc_detail.h, T378).
    - [ ] Tested by the user.
13. **Anti-aliasing.** Tried-and-true options: FXAA and MSAA.
    - [x] Built 2026-10-19 (renderer.md "Anti-aliasing"): off / FXAA (the
          race views' 3D before the HUD) / MSAA 2x, 4x, 8x (every target,
          resolved where read); `[Graphics] AntiAliasing`; the in-game
          settings menu (F2, launcher.md "Settings") for it and the settings
          to come.
    - [x] Tested by the user ("Good"; then the left / top edge smear with
          MSAA fixed: "Good").
14. **Texture filtering.** Bilinear or trilinear.
15. **Multi-threading.** Use additional CPU cores for various calculations.
    - [x] 2026-10-24 (the options' first, user): the lens flare's occlusion
          results read on the GPU by the glow draws, so the CPU no longer
          waits for the GPU mid-frame in every race frame (renderer.md 8).
          User: "Good".
    - [x] 2026-10-24: the F3 monitor's breakdown (off / simple / detailed /
          verbose; detailed: update, record, execute, GPU wait - the
          execution's blocked time, from the thread's CPU cycles - output,
          present, pace, other, GPU time per frame; verbose: the GPU's time
          by section and EXECUTE's CPU profile; renderer.md "Frame rate").
          The profile showed the race CPU-bound in EXECUTE (GS stream 1.95
          ms, mesh check 1.26, D3D submit 1.05, texture lookups and states
          0.9, uploads 0.4).
    - [x] 2026-10-24: the mesh check hashes a model's header and part table
          each frame, its part data only in turn (and the models the game
          rewrites, marked by vif_model_relocate / TStripBuilder::finish);
          renderer.md 7. User: "Good" (mesh check 1.26 -> 0.14 ms, 129 ->
          156 FPS).
    - [x] 2026-10-24: a CLUT load the buffer holds already is skipped, the
          palette a texture bind reads is kept until the next load; GS
          STREAM's profile split (GS FLUSH, CLUT LOAD, TARGET SET, GIF
          uploads under UPLOADS; GS vertices, batches, CLUT loads counted).
          It showed GS STREAM was CLUT LOAD (1.68 ms, 1 473 loads): the
          memo keyed by the global upload serial missed between uploads.
    - [x] 2026-10-24: the CLUT memo keyed by the tags of the CLUT's blocks
          (kept across frames), loads the buffer holds skipped by row,
          palettes kept by the rows' contents (renderer.md 6). Screenshot:
          156 -> 181 FPS, EXECUTE 5.6 -> 4.3 ms, CLUT LOAD 1.68 -> 0.67.
    - [x] 2026-10-24: front caches for CLUT loads and texture binds (stable
          tags, a tag epoch), a part's batch range, the D3D states kept with
          the draw state, an object-lookup cache, the registry's 64-frame
          housekeeping without a std::set (renderer.md 9). User: "Minor
          improvement" (183 FPS). The bumper camera: the full-detail mirror
          is a second scene (~1000 more parts); user: expected, leverage the
          hardware better.
    - [x] 2026-10-24: the shader constants in a ring (Direct3D 11.1 constant
          buffer offsets) instead of a DISCARD rename per change; CONSTANTS
          and CB WRITES in the verbose profile (renderer.md 9). User: "Minor
          improvement, no glitches" (187 FPS).
    - [x] 2026-10-24: the D3D thread (user: "Two-thread executor next"):
          the executor queues its Direct3D calls (closures in a ring) for a
          thread that alone uses the context while they run; any other use
          of the context waits for it (renderer.md 3; GT3PC_D3D_THREAD=0
          off, GT3PC_SYNC_LOG=1; D3D THREAD and D3D SYNCS in the verbose
          view). User: syncs at 5; the present pass and the timers' read
          queued / moved: syncs at 2, D3D WAIT 0.07 ms, "another minor
          improvement" - EXECUTE unchanged (4.46 ms): the D3D calls were
          only ~0.9 ms, and each push read the D3D thread's counter (a cache
          miss across the cores).
    - [x] 2026-10-24 (user: "Start with 1, then 2"): the queue's executor
          keeps its own view of the D3D thread's progress (reads it only
          when the ring seems full), publishes 256 at a time; the CLUT /
          texture front caches held per VRAM region (64 generations)
          instead of one epoch every upload moved (CLUT SLOW, TEX SLOW
          counted). User: "Another minor improvement" (181 FPS, EXECUTE
          4.49): half the lookups still slow (634 / 1502, 736 / 1550) -
          256 slots for several hundred CLUTs / textures a frame, used in
          the same order each frame. Front caches 2048 / 4096 slots; VRAM
          GENS counted. Screenshot: 186 FPS, CLUT SLOW 441, TEX SLOW 701,
          VRAM GENS 192 a frame (every region a few times: the course's
          sets streamed, the wheel faces per wheel) - region checks fail.
    - [x] 2026-10-24: the front caches checked exactly by what they read (a
          CLUT's blocks' tags, a set texture's TBP0 tags; regions only for
          images / composed); gs::translate memoized; the last colour /
          depth target lookups kept (g_targetsGen). Screenshot: 193 FPS,
          EXECUTE 4.12, CLUT SLOW 125, TEX SLOW 220, TEX LOOKUP 0.23 - but
          CLUT LOAD still 0.56: the copy of 256 entries at nearly every load
          (parts take turns with a few CLUTs).
    - [x] 2026-10-24: CLUT loads note their rows' sources only; the
          halfwords copied when a palette is worked out (clut_lookup).
          Screenshot: 202 FPS, EXECUTE 3.91, CLUT LOAD 0.32.
    - [x] 2026-10-24: the executor thread: a kicked frame runs while the
          game thread runs the next ticks and records the next frame, waited
          for at the game's own DMA waits (gfx_dma_sync_frame /
          gfx_sync_pipeline) and at every entry point that touches the
          renderer; readback frames at the kick; EXEC THREAD in the verbose
          view, EXECUTE = the game thread's wait (renderer.md 3;
          GT3PC_EXEC_THREAD=0 off). Screenshot: 235 FPS (from 202), frame
          4.25 ms, EXEC THREAD 3.70; RECORD counted the game's DMA wait
          inside it (3.72): it leaves that to EXECUTE now. User: satisfied
          but for stutters at the same spots every lap.
    - [x] 2026-10-24: the caches kept by memory and age, not frames
          (models 256 MB, textures 512 MB, LRU beyond, 10 minutes unused);
          set textures keyed by image + load stamp (renderer.md 9).
    - [x] 2026-10-24 (user: "Also decode the course's models and textures
          ahead of time on a worker thread"): the models and Tex1 sets the
          game loads are copied on the game thread and decoded on a worker
          (`pgl_prewarm.cpp`), taken at the first draw when they still match
          (renderer.md 7, "Decoding ahead"; GT3PC_PREWARM=0 off; a log line
          per load with what the draws took and what still came late).
    - [ ] Tested by the user.
16. **Anisotropic filtering.** Beware of textures with RGB values but no
    alpha: PCSX2's implementation shows unintended colours on textured
    objects there.
17. **Controller / keyboard and mouse support 2.** Needs game file changes:
    **consult the user first.** Any controller the operating system
    recognises.
    - [x] Built 2026-10-23 (user request; no game file changes): mappings
          per DualShock 2 / Driving Force control, a gamepad or wheel slot
          and a keyboard / mouse slot (`platform/input_map.h`, gt3pc.ini
          `[Controller1]`, `[Controller2]`, `[Wheel1]`, `[Wheel2]`); the
          launcher's Controls tab (Mapping, Calibration); dead zones and each
          player's device, also in the F2 menu's CONTROLS > PLAYER 1 / 2;
          Wheel 1 / 2 from SDL joysticks as Logitech
          Driving Forces (no force feedback yet); with both connected, each
          player uses one of them (`[WheelN] Input`, the launcher; merged,
          a resting wheel overrode the stick's dead zone); the mouse's
          buttons and wheel; the Guide
          button opens the F2 menu (held 1.5 s: the Esc menu); the F2 menu
          in categories.
    - [x] 2026-10-24 (user: "Launcher hotkey configurator"): the game's
          hotkeys (F2, Esc, F3, Alt+Enter, F5, F6, F8, F9) in gt3pc.ini
          `[Hotkeys]`, a key with exact modifiers each (or none), set in the
          launcher's Controls > Hotkeys (launcher.md "Controls"); the menus
          and the pause indicator name the keys set; a hotkey's key no
          longer reaches the game. Also (user): the pause indicator hides
          3 s after each F5; the F2 menu's DISPLAY > SCREEN MODE (windowed,
          borderless, fullscreen) switches at once and saves `[Display]
          Mode`.
    - [x] 2026-10-24 (user: "The chase cam camera control mappings need to
          be adjustable in the launcher"): Chase cam left / right / up / down
          in every profile (`CameraLeft` .. `CameraDown`; defaults the right
          stick and J / L / I / K, none on a wheel), the Mapping tab's first
          rows; the free orbit reads them from the player's device's
          profile, so a wheel player can orbit too.
    - [x] 2026-10-24 (user: "Debug Camera"): F7 (`[Hotkeys] DebugCamera`)
          in a race: a free camera on view 0 (move X / Z, Y, look, speed,
          field of view; the Debug camera profile in the launcher), the HUD
          and mirror off, the players' input at rest, "DEBUG CAMERA ENABLED"
          for 2 s; flies while F5-paused too (renderer.md "Debug camera").
    - [x] 2026-10-24 (user: car sounds glitched under it; "Lock camera and
          give control back to car"; "Looking should be mapped to the
          mouse"): the sounds stay at the game's camera (`PcDebugcamBypass`
          in `RacePS2Base::virtual_29`); Lock (R3 / F) holds the camera and
          gives the players their input back; the look on the mouse's raw
          movement (`mouse:move*` bindings), the cursor hidden and confined
          while flying.
    - [x] 2026-10-24 (user: the player's car stayed invisible from the
          bumper view; "have it force the chase cam"): player 1's live view
          is the chase view while it is on (given back after), and every car
          is shown from the debug camera (a paused race, a replay's in-car
          views).
    - [ ] Tested by the user.
18. **Unlimited memory usage.** Remove every memory or file size limit coded
    in: car and course models, menu assets, scripting and the rest.
    - [x] Built 2026-10-24 (user: "Any memory / file size limitations coded
          in should be removed"): a survey of every limit (six areas), then:
          the game heap 1.5 GB (else the largest that fits, below 2 GB
          first; was 128 MB), reserved first thing; the course read by its
          real size into a grown arena unit (was 13 MB, cut silently); each
          car model, tyre and wheel file in a buffer of its own size (were
          0xA8000 blocks, overflowing into the next car); the replay memory
          its own; cameras, the logger map, trophies and prize cars by size;
          menu heaps that fall back to the main heap when full (4 x larger
          too), menu car buffers grown per car, big stack buffers moved to
          the heap; the sound RAM 64 MB, each bank at its PS2 place when it
          fits, else in an effects / music / cars window above 2 MB, headers
          sized by the file; reads with a capacity (no more whole files into
          fixed buffers), the inflater bounded, movies sized by the stream,
          longer paths; carcolor / racemode tables by size; Tex1 sets that
          find no VRAM drawn from the set (renderer.md 6), 1024 VRAM
          records, 16 MB packet buffers, 64 font files, 1024 font faces;
          more gadgets (1024) and flares (255 + 255). Retail files keep
          their PS2 places and sizes. Format limits stay (save arrays, file
          field widths, 14-bit VRAM addresses, the 8 MB memory card), now
          bounds-checked. Every file over its retail size logs a `PC:` /
          `snd:` line.
    - [x] 2026-10-24 (user: "The save arrays do need to be expandable for
          modding purposes, at least garage cars, events, courses, music
          tracks"): the garage grows in blocks of 200 slots (cars and
          wheels; up to 100000, a bigger count is taken for a damaged
          save; the menus' checks at 200 - the dealers, the used car shop,
          the prize cars - ask `GameStatus::pcGarageFull`); events, course best times, arcade and free run records,
          prize sets and the music play list sized from the data at start
          (the retail sizes the minimum; the parts past the PS2 arrays on
          the heap, `src/game/save/pc_save_ext.*`). Saves are GameStatus
          version 8.0: as 7.1 but those records written as name-keyed lists
          (event, course, prize set name, song code), so a mod adding or
          removing one does not move the others' results; entries whose
          names the data lacks are kept and written back. PS2 7.x saves
          still load (positionally, as before); older builds refuse 8.0
          ("different version"). The time trial's 10 fixed AT races stay;
          the 8 MB card holds about 15000 cars.
    - [ ] Tested by the user.
19. **Sound engine rewrite** (user, 2026-10-20). Fix the loud bursts in races,
    more voices and headroom, and DAW-class latency through a custom output
    rather than the standard Windows audio path.
    - [x] Built 2026-10-20 (sound.md 3.4, 3.5): one music-thread flush per
          game tick, each state block applied at its vblank's sample (the
          scheduled engine, an adaptive delay of a few ms); mixing with
          headroom and a -1 dBFS limiter; 126 EE voices (2 x 64 SPU2 slots);
          kernel-streaming output straight to the driver (WaveRT /
          standard streaming, 2 ms periods by default), WASAPI as the
          fallback; `[Audio] Output` / `Buffer`, F2 menu with the running
          latency.
    - [ ] Tested by the user.
20. **Languages and other discs** (user, 2026-10-24: "the US version already
    has the other languages supported with their data, it's just not
    accessible"; the launcher to take the USA bundle, PAL, Japanese and GT
    Concept discs, any disc with CORE.GT3).
    - [x] Built 2026-10-24 (languages.md): `[Game] Language` (launcher Game
          tab, F2 GAME > LANGUAGE; from the next start): the country set at
          boot and after a load, the language's databases and product
          config; the launcher installs any disc with CORE.GT3 (root or a
          root folder), names the known releases, reads the core's database
          names and own language into the install marker. Checked against
          the six disc images (the disc check and the core's facts; not
          installed and played).
    - [x] 2026-10-24 (user: "GT Concept doesn't boot, which is expected
          ... a Play tab ... data_US, data_EU, data_JP ... JP also doesn't
          boot, a real investigation needed"; "a unified structure that the
          launcher places the files into"): a data folder per version, the
          launcher's Play tab (a row and a Play button each; `--data`,
          `[Game] Version`), other discs extracted as data only into
          `data_<boot file>`, an earlier `data\` moved to its version's;
          the unified structure (the Japanese disc's flat courses into
          `courses\JP\`) and the game's course folder by region (T179);
          the Japanese data: the boot crash (courses/US not found) and the
          arcade menus' wait for the missing `stream/course.ipic` fixed, the
          rest compared and compatible (languages.md "The Japanese
          version"). The PAL version booted in the user's run.
    - [x] 2026-10-24 (user: "The PAL movies need special speed control,
          they run too fast"): the 25 fps PAL movies paced by their own rate
          in `MenuBgMovie::update` (movies.md 4).
    - [x] 2026-10-24 (user: "Saves should be separate but can stay on the
          same memcard ... starting credits ... 4 Debug tab features"):
          each version's saves under its product code (BASCUS-97102,
          BESCES-50294, BISCPS-15009); `[Game] StartingCredits` (launcher,
          F2; x 100 in Japanese - the Japanese product config's own is
          15,000, 1,500,000 yen); the Debug tab's FillGarage, FillMoney,
          UnlockArcade, UnlockLicenses (game_start.md).
    - [x] 2026-10-24 (user: "Have unlock arcade tracks debug option unlock
          everything everywhere, but otherwise let the file determine
          things if it's off"): UnlockArcade merges the arcade modes' course
          lists (every course in every mode, Cote d'Azur in two players);
          off, the product config's lists. A list editor: later (user).
    - [ ] Tested by the user.
21. **Adjustable units** (user, 2026-10-24: "The game has several sitting
    dormant in the code, and also be careful of bugs with it too. Make sure
    the units actually adjust the values").
    - [x] Built 2026-10-24 (units.md): `[Game] SpeedUnit` / `PowerUnit` /
          `TorqueUnit` (launcher Game tab, F2 GAME), the shown units apart
          from the language's (the game's own ratios keep these); ten unit
          bugs fixed (CV, the computed torque, the thumbnails' double
          conversion, the total mileage, the launch speed, the trip factor,
          the torque ratios, the logger's miles / mph, the mph dial without
          images, "NM").
    - [ ] Tested by the user.

## Queued issues

Reported by the user, to be looked at later.

- [ ] Reflection glitch on a menu car (2026-10-24, not reproduced): the
      arcade car select's Lancer Evolution VII GSR in French Blue showed a
      magenta body, pink wheels and a noisy red / green pattern in the
      headlights. Gone after backing out of the menu and entering it again,
      and after a restart (renderer.md 11).
      - [x] 2026-10-24: a texture decoded from a set's bytes stayed until
            the set was loaded again; the decoded-texture cache now
            follows the bytes (fingerprints of the big uploads, renderer.md
            6 "Changed in place"), with a log line when a set changes in
            place. Awaiting: whether it comes back.
- [ ] Sequenced notes / sound effects sometimes missed (2026-10-24, in
      menus and in general play).
      - [x] 2026-10-24: two causes in the scheduled sound engine (sound.md
            3.4): late blocks applied back to back keyed one-tick notes on
            and off at the same sample (a block now waits for a voice's tick,
            `SCHED_MIN_NOTE`); and a voice just keyed on could look free to
            the EE between two render calls (key-ons now enter the answer
            at once). Awaiting the user's test.
