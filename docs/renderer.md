# The renderer: pgl on Direct3D 11

GT3's graphics library pgl (G001) kept its state in GsContext and wrote GS /
VIF / DMA packets that the PS2 hardware executed. In the port pgl stays the
decomp's C (src/engine/pgl/pgl_*.cpp, compiled as decompiled code with the
prelude) with its packet writing replaced by **recordings**; the native
renderer (src/engine/pgl/native, library `gt3_render`, standard C++ and
D3D11) keeps them in a command list, **executes** the list when the frame is
kicked and **presents** the displayed frame buffer. Nothing emulates the GS
or the VUs: the commands carry what the packets meant (register values,
primitives, model parts, uploads); the executor keeps the GS register state
they describe and draws with D3D11.

Companion notes: docs/pgl.md (state mapping, coordinates, VU0 math),
docs/graphics.md (texture registry, targets, effects), docs/models.md (VU1
models and their shader math).

## 1. Files

| File | Side | Role |
|---|---|---|
| `pgl/pgl_port.h` | decomp | GsContext as `g_pglCtx`, register views, constants, the bridge |
| `pgl/pgl_driver.cpp` | decomp | gs_display_driver / gs_driver(_core) / gs_context: setters, packets, vsync, vblank handler, clears, frame copies |
| `pgl/pgl_core.cpp` | decomp | pgl.c: matrices, attrib stack, 2D primitives, uploads / downloads, direct GIF blocks |
| `pgl/pgl_vu0.cpp`, `pgl_lighting.cpp`, `pgl_prim.cpp` | decomp | VU0 matrix / math (through pgl_math), lighting blocks, VU1 strips, model_draw |
| `pgl/pgl_tex1.cpp`, `pgl_font.cpp`, `pgl_flare.cpp`, `pgl_particles.cpp`, `pgl_color_filter.cpp` | decomp | Tex1 sets / texregs, text, lens flares, weather, colour filters |
| `native/pgl_bridge.h` | both | the C interface (plain types) between the two sides |
| `native/pgl_render.cpp` | native | recording into command lists, frame flow, timing, crash log |
| `native/pgl_exec.cpp` | native | execution: GS registers, GIF decoder, primitive assembly, VU1 draws, effect recipes |
| `native/pgl_targets.cpp` | native | render targets, presentation, readbacks, frame dumps |
| `native/pgl_textures.cpp` | native | the VRAM address registry, CLUT buffer, texture cache |
| `native/pgl_models.cpp` | native | decoded VifModel meshes (gt3vif) |
| `native/pgl_prewarm.cpp` | native | the worker that decodes the models and Tex1 sets the game loads ahead of their first draw (section 7) |
| `native/pgl_d3d.cpp` | native | HLSL, input layouts, state-object caches, bound-state cache, dynamic streams |

Every native call made from a game thread runs under `fpu::DefaultFpuMode`
(platform/fpu.h) - once per execution / presentation / immediate upload, not
per draw; a readback callback into game code runs in the game's mode again
(`fpu::ScopedFpuMode`).

## 2. Frame flow

| Game | Renderer |
|---|---|
| `gfx_begin_packet` | a new frame command list |
| setters, `gs_flush_dirty_regs` | `CMD_REGS`: the dirty & enabled shadows, as the A+D block |
| 2D primitives, clears, frame copies | `CMD_PRIM` (GS vertices, pgl_state_prim), `CMD_AD` |
| `gfx_open_direct` / `gfx_alloc_direct` blocks, the text renderer | `CMD_GIF`: the block's pointer, decoded at execution (PACKED / REGLIST / IMAGE) |
| `pgl_draw_strip`, strip batches, `tri_draw16` | `CMD_STRIP`, `CMD_TRI16` with the VU1 matrix block |
| `model_draw`, `vif_model_draw` | `CMD_MODEL` per part: model, part, lighting program and block, PRIM override, preXyz, flare results |
| uploads (`gs_upload_image`, `tex1_upload_packet`) | `CMD_UPLOAD` / `CMD_TEX1_UPLOAD` (the `_sync` forms act at once) |
| `gs_download_image` | `CMD_READBACK` |
| sub-packets (debug logger, flare reduction) | recorded sub-lists, `CMD_SUBCALL` |
| `gfx_end_packet` | the list is complete |
| `gfx_kick_frame_lists` | **execute** the list |
| `gfx_set_display_fb` | **present** the target at that address (4:3, black bars, `Present(0)`) |
| `gfx_set_display_enable(0)` | present black |

Pointers are read at execution, as the DMA read them; what the PS2 wrote
into the packet itself is taken at recording. `CMD_TEX1_UPLOAD` carries the
set's VRAM base of that moment: `tex1_upload_packet` built each transfer's
BITBLTBUF (DBP) into the packet and only referenced the pixels, and a set is
linked for other blocks (`tex1_set_vram_base`) several times a frame - the
VRAM allocator evicts and reloads course sets (Laguna Seca's model04 sets
alone need 6051 blocks of the ~4670 left beside the race screen and the env
map), `TexSet::load` moves a wheel face for every wheel. Execution is
synchronous at the kick, so `gfx_sync_pipeline` / `gfx_dma_sync*` return at
once. The vblank (`INTC_VBLANK_S`, the platform's 59.94 Hz timer thread) runs
pgl's handler: the 64-bit counter, `vsyncCallback`, the waiter wake-ups;
`gfx_vsync(n)` keeps the original semantics.

## 3. Execution

- **GS state**: both contexts' registers and the general ones, PRIM,
  PRMODE, the vertex attributes (RGBAQ, ST, UV, FOG), BITBLTBUF / TRXPOS /
  TRXREG. TEX2 rewrites TEX0's PSM and CLUT fields; TEX0 / TEX2 with CLD
  load the CLUT buffer (section 6).
- **Vertex queue**: the PRIM type's (point, line, line strip, triangle,
  strip, fan, sprite); XYZ2 / XYZF2 kick, XYZ3 / XYZF3 and ADC do not; a
  PRIM write restarts it. Each triangle starts with its kicking vertex (the
  flat-shading colour).
- **Draw state**: `pgl::gs::translate` of PRIM.CTXT's context; passes for
  the alpha test / AFAIL; dual-source blending (o1.a = C / 128); UNORM
  targets keep the exact alpha byte. States that have no exact fixed-function
  form are logged once per distinct value (`pgl: inexact GS state ...`).
  Cs C and Cs C + Cd (0x88, 0x48, 0xA8, 0x68: C = As or FIX) multiply in the
  shader, as the GS does ((Cs C) >> 7, the sum clamped; `ColorPremul::TimesC`,
  src ONE): exact for any C. As reaches 0xFF (2.0) and FIX 0xFF on the GS, a
  blend factor only 1.0: before, the flare queries' result texels (Cs As,
  the shape's alpha 2.0) came out at half, and flat above half visibility.
- **DATE** (destination alpha test): the pixel shader reads the destination
  alpha from the target's copy (slot t1) and discards where its bit 7 is not
  DATM (no colour, no Z written). The copy is taken when a DATE draw state is
  set up, unless every draw into the target since it was a DATE draw: a run
  of DATE draws (a model's env-map passes, one per VU1 part) reads the alpha
  as it was before the run, so one copy per run. Inexact only where a DATE
  draw that writes alpha overlaps itself or an earlier draw of its run (the
  GS sees those writes; logged once, `DATE draws that write alpha`).
- **C = Ad** (blends weighted by the frame's alpha, 0x80 = 1.0; docs/pgl.md
  "ALPHA"): Cs Ad + Cd and Cs Ad (0x58: the course's glint and env passes,
  the cars' and wheels' env passes) are drawn twice with Cs x DEST_ALPHA,
  the first time without the alpha and Z writes (Ad / 255 twice = Ad / 128;
  within two units); every other form (0x54: the road reflections) in the
  shader from the target's copy (t1, refreshed before each such draw) with
  blending off, exact. A 24-bit frame reads Ad as 0x80 (FIX 0x80). Before,
  0x58 multiplied o0 = Cs 255 / 128 by DEST_ALPHA (saturating above Cs =
  0x80: too dark) and 0x54 weighted by DEST_ALPHA / INV_DEST_ALPHA (Ad / 255:
  the reflections at half strength); both were logged as `inexact GS state
  (blend C = Ad)`, which no longer appears.
- **Batching**: GS-space primitives accumulate while no register that
  affects the draw changes; uploads, readbacks, VU1 work and effect recipes
  flush first. A TEXFLUSH ends a batch that samples its own target (or its Z
  buffer): the primitives after it read the ones before (the flare query's
  in-place reductions, a buffer blurred over itself), so the next one
  resolves its texture again (a fresh copy, below). D3D11 bindings go through a cache of what is bound
  (`set_targets`, `set_blend` ... in pgl_d3d.cpp), constant buffers are
  rewritten only when their contents change, vertices / indices stream into
  one dynamic buffer each (NO_OVERWRITE appends; full, it starts again with
  DISCARD, new memory). A draw's own vertices stream last before it: the
  GS batch is drawn first (the VU1 strips streamed theirs before vu1_draw
  drew the batch, and when the batch's write restarted the stream the
  strip's draw read new memory where its vertices never were - a key of the
  name entry, the OK bar, gone for a frame every couple of seconds in the
  menus: user report, found with GT3PC_FLICKER's GPU counts).
- **GIF decoder**: tags read as data: PACKED (PRIM, RGBAQ, ST, UV, XYZF2,
  XYZ2, FOG, A+D, the 64-bit registers), REGLIST, IMAGE (host-to-local
  transfers, completed into registry uploads).
- **VU1 draws**: the vertex shader does what the programs did
  (docs/models.md 5): the combined matrix, the GS viewport mapped straight to
  the target's NDC (any mirroring kept), depth = GS Z / 2^bits of the Z
  format, fog F, STQ, the lighting programs (DEFAULT, DIFFUSE, SPEC2 / 4,
  LOCAL, ENVMTX, ENVMAP, CONST, the flare program), morphing. Culling: +0xA
  CULL_BACK, +0x14 CULL_FRONT, FrontCounterClockwise = TRUE, swapped for a
  mirrored viewport; program 0 draws nothing.
- **The D3D thread** (2026-10-24, ROADMAP item 15; `pgl_queue.h`): the
  executor (exec_list on the game thread that kicks the frame) works out the
  draws - the GS state, the registry, the targets, the models - and hands
  their Direct3D calls in order to a thread of their own, which alone uses
  the immediate context while it has calls queued: the two run side by side,
  and the D3D thread goes on with a frame's end (its last draws, the GPU
  timer, the Flush) while the game thread starts the next ticks. A call is a
  closure copied into a 32 MB ring (the executor waits when it is full;
  it publishes 256 at a time and reads how far the D3D thread got only when
  the ring seems full or at a sync - read at every call, that counter,
  written by the D3D thread after each, cost a cache miss across the cores
  per call and ate what the thread saved: user, "minor improvement"),
  holding what it needs by value: Direct3D objects as pointers, numbers,
  data copied along (vertices, constants, texture rows) - never the
  executor's structures, which go on changing. The binding cache and the
  streams' / constants' ring positions are the executor's (what has been
  queued). A Direct3D object the executor lets go (`Com::reset`) is released
  on the D3D thread after the calls queued before it. Any other use of the
  context - `dc()` outside a closure, `gpu::context()`, the swap chain
  (Present, the back buffer) - first waits until the D3D thread has run
  everything (a sync, `gpu::set_context_guard`): readbacks, the GPU timers'
  results, the presentation, the debug aids stay as they were and right; the
  frequent calls (bindings, constants, streams, draws, target copies and
  resolves, clears, the effect recipes' passes, surface uploads) are queued.
  The verbose monitor shows the D3D thread's busy time (D3D THREAD, apart:
  it runs alongside EXECUTE), the game thread's time waiting for it (D3D
  WAIT) and the syncs that waited (D3D SYNCS: two a frame are expected -
  the presentation waits for the frame's draws, then for the picture's
  blit when an overlay is shown or at its last call); `GT3PC_SYNC_LOG=1`
  (or the launcher's Debug tab, [Debug] SyncLog) logs who asked,
  `GT3PC_D3D_THREAD=0` turns it off. The frame's start reads the GPU
  timers' results before anything is queued.
  Device calls (resources made) stay on the executor's thread (the device is
  free-threaded).
- **The executor thread** (2026-10-24, ROADMAP item 15; `pgl_render.cpp`):
  a kicked frame's list runs on a thread of its own - `pglr_kick` hands it
  over and returns - while the game thread runs the next ticks and records
  the next frame, as the PS2's DMA ran it while the EE went on. The game
  waits for it where it waited for the DMA (`gfx_dma_sync_frame` after the
  draw, `gfx_sync_pipeline`: `pglr_sync`), so its own rules hold: the packet
  buffer it records into is double-buffered (T002 flips it each frame), the
  arrays it hands over by reference stay until that wait, and the post-draw
  slot (`virtual_3`) and the palette poses at the frame's end run after it.
  Every entry point that touches the renderer's state waits for it too
  (`exec_wait`: the presentation, an immediate upload, a Tex1 set loaded, a
  sub-list recorded again, any use of the context through gpu.h's guard).
  A frame with a readback the game reads (not the flare strip's, read on
  the GPU, unless GT3PC_FLARE_LOG) executes at the kick, as before. The
  frame monitor takes a lock (the executor reports its profile and the GPU
  times); EXECUTE is then the game thread's wait for the executor (what the
  frame waited), its own time EXEC THREAD in the verbose view.
  `GT3PC_EXEC_THREAD=0` executes at the kick.

## 4. Pixels, depth, colour

- A GS window position w (pixels, (X - OFX) / 16) is native **w s + 0.5**,
  s = native / GS size of the target. GS pixel x then covers native pixels
  [x s, (x + 1) s): integer-edged sprites (clears, copies, text, menus) fill
  whole blocks at any scale and are exactly the GS's pixels at s = 1. A
  sprite's texture coordinates are shifted by slope x (0.5 / s - 0.5) so
  texel centres stay where a 1:1 mapping puts them: copies between targets of
  the same native size stay pixel exact. 2D drawn for the 4:3 screen and
  squeezed by k into the GS frame (the race's HUD, `CMD_DESIGN`: section 5
  "Aspect") is 1:1 in design pixels, not GS pixels: its shift is slope x
  (0.5 / s - 0.5 k). Render-target textures keep the GS-pixel shift.
  The spread is half a texel each side of the GS sample point, so a point-
  sampled copy is exact only when the game samples inside the texel by more
  than 0.5 / s of one: `gs_copy_target` (`gfx_frame_effect`'s copies) offset
  its samples +0.375, which from s > 4 read the texel before for the first
  eighth of each - every copy a native line lower, REPEAT bringing the
  bottom line to the top (the menus' 640 x 224 buffers are that tall from
  about 1080 lines; the car views' ragged top line, user report). On PC it
  samples at the texel's centre (+0.5): the same texel on the GS, exact copies
  at any s.
- Lines (LINE, LINE_STRIP) are the GPU's own, one native pixel wide at any s.
  `gfx_set_line_width(px)` -> `CMD_LINE_WIDTH` (in order with the draws, every
  frame starting at 1) draws those that follow as quads px native pixels
  across, their ends squared off by half that: the data logger's speed curves
  (T087, 2 px: retail thickened them by sending them five times a pixel
  apart, a blur at the PC's resolutions).
- Depth: Z / 2^24 (Z24) in D32_FLOAT_S8X24, tests GREATER / GEQUAL, clear 0.
- Colours in GS units in the shader (0x80 = 1.0 for modulate and alpha);
  texture function, TCC, TEXA alpha modes, fog, the alpha test with its gate,
  FBA, the blend's colour pre-multiplications, SCANMSK by GS row parity.

## 5. Targets and presentation

- One native resource per (base, colour / depth, size). Scales sx = the
  width of the picture shown / 640, sy = its height / 448 (the back buffer's
  4:3 rectangle, or a race frame's whole picture: "Aspect"), times the
  resolution scale ([Graphics] RenderScale, 10 .. 400 %, the settings menu's
  RESOLUTION SCALE; the old 1 .. 6 limits are gone), each held to the
  texture limit (16384 native pixels for the 640 x 448 screen), the screen
  at most 64 M native pixels (beyond, both scaled down alike: every colour
  target of the screen's size is that large, its depth buffer twice, MSAA
  times its samples), at least 1/20. A W x H GS buffer is (W s) x (H s), a one-field buffer (W >= 512,
  H <= 256: the 640 x 224 frame buffers) (W s) x (2 H s): the field buffer
  and the 640 x 448 race screen have the same native size.
- Presented, a picture larger than its screen (the resolution scale above
  100 %) is filtered down: each screen pixel the mean of its part of the
  picture, ceil(ratio) x ceil(ratio) bilinear taps (16 x 16 at most;
  `ps_present`, `CbPresent` taps) - supersampling, the downsampling
  anti-aliasing, with MSAA too or alone. One bilinear tap would skip texels
  from 2 : 1 on. A smaller one is stretched bilinear.
- The scale follows the picture (roadmap item 6: window resizes, display
  modes): after a present (`follow_output`), once a new scale has held for
  15 frames, every target goes to it (`targets_rescale`): colour targets
  resampled (bilinear, content kept), depth targets re-created cleared;
  targets at GS resolution stay. A new resolution scale goes at once.
- Targets unused for 600 frames are released (`next_frame`). Beyond 8 sizes
  of one base's colour or Z buffer, the least recently used go too, once
  unused for 2 frames (logged once per base: `pgl: more than 8 sizes of
  the ... buffer at block ...`): a buffer whose size changes every frame
  otherwise kept a new resource per frame for 600 frames (the first
  side-by-side split screen's letterboxed views followed the aspect while
  the window was dragged: at a high scale with MSAA ~100 MB a frame, seconds
  of paging; user report).
- Colour R8G8B8A8_UNORM (alpha stored as the GS byte), depth
  D32_FLOAT_S8X24 (typeless, an SRV for depth copies). A depth buffer is made
  per colour-target native size (D3D11 needs equal sizes; a second size is
  logged), so frames of different GS sizes but one native size share it (the
  640 x 224 field buffers and the 640 x 448 race screen), as they share the
  GS's Z words. A Z buffer has no GS size of its own: its **GS geometry**
  (size, FBW: what a Z texture read, i.e. a depth copy, maps its texels by)
  is the one of the last frame that wrote it (`depth_target_written`, and
  `depth_frame_target` for a Z-format FRAME), logged once per change
  (`pgl: Z buffer at block ... now holds a WxH frame's depth (it held a ...
  one)`).
- A draw that samples its own target samples a copy (made when the target
  changed since the last copy).
- **GS resolution for scan-masked effect buffers**: a colour target at most
  64 GS pixels wide that receives a scan-masked draw (SCANMSK 2 / 3) is
  switched to one native pixel per GS pixel for good, its pixels resolved
  into the new resource (`target_gs_resolution`, logged once per size: `pgl:
  the WxH buffer at block ... takes scan-masked rows: drawn at GS resolution
  from now on`). The only one is T449's wheel-face blur (w x 3w, w <= 64):
  the face drawn still on every row and turned on the odd rows, then shrunk
  2:1 by a bilinear sprite sampling exactly between two rows, i.e. their
  mean. At s native rows per GS row the shrink's two taps fell on native
  rows of one GS row (still or turned, irregularly), and the blurred face
  came out in horizontal stripes (user report, race wheels). The ghost cars
  scan-mask the race screen (640 wide) and keep the output scale.
- **GS resolution for the flare occlusion buffers** (the same switch, point
  samples): a colour target of at most 64 x 64 that a draw reads as its own
  texture (the occlusion query's 64 x 64 work buffer, halved in place:
  `reads itself`), and one of at most 64 x 64 drawn from such a buffer
  already at GS resolution (the 64 x 8 result strip: `is drawn from a
  GS-resolution buffer`). The halvings are bilinear sprites whose taps fall
  midway between two GS pixels (exact 2 x 2 means); at s native pixels per
  GS pixel (s = 2.14 here) they averaged native pairs, the last texel was no
  longer the box's mean, and the copy's first native column was the pixel
  left of the box (x0 s is not a whole native pixel). An offline model of
  the copy, halvings and store (boxes 2..32, every partial visibility)
  put the native result up to 64 of 255 off the GS's, depending on where the
  box fell; at GS resolution it is the GS's. The copy then takes the screen's
  alpha (the shape's depth test, at the output scale) at the GS pixel
  centres. Each buffer switches on its first such draw (one query of one
  frame per new address at the output scale). No other buffer of the game
  fits (the road reflection buffers are 64 x 128).
- Presentation: the target at the displayed address, its display rectangle,
  linear, into the largest 4:3 rectangle of the window (a race frame: of its
  aspect), bars black. The
  display offset (screen adjust) is not applied.
- Readbacks: the GS pixels of the colour target that holds the source block
  (the registry's owner; the last target used there when nothing owns it)
  resolved (point samples) into a w x h target, copied out through a staging
  texture within the frame: the CPU waits for the GPU, as the PS2 stalled its
  lists. The flare result strip used to be delivered one frame late; texel q
  is the frame's q-th occlusion query and the lights queried change from
  frame to frame, so the glows got other lights' visibility (section 8). Its
  readback is no longer done (only for `GT3PC_FLARE_LOG`): the glows read
  their texels from the strip on the GPU (section 8), so no race frame waits
  for the GPU there.
  The owner, not the target used there last, matters for the car icons
  (section 8): car i's 64 x 32 icon buffer lies at the base of car i - 1's
  128 x 128 reflection buffer, both used in the icons' frame; the older one
  won the "last used" tie, so every icon but car 0's (the player's) was read
  from a reflection buffer cleared black with alpha 0x80: an opaque black box
  (`pgl: readback of block ... from the 64x32 target that holds it (the last
  one used there is 128x128)` shows the case).

### Fields

The PS2 displays one 224-line field per vblank (interlaced NTSC), offsets the
odd field half a line, and the game draws the menus into 640 x 224 field
buffers. The port is progressive throughout (roadmap item 3, "Full
progressive scan rendering everywhere"):

- The display mode is non-interlaced (`gfx_init_default`, the render
  thread's `setDisplayMode(0, 2, 1)`), so there is no field: `ctx->field`
  stays 0, `gfx_set_display_fb` flips nothing, `gs_get_field_mode` answers
  `PGL_FIELD_FRAME`, and `gfx_set_field_mode` drops every half-line offset
  (OFY = tgtA.yoff).
- The one-field buffers keep their GS size (640 x 224, the VRAM layout of
  graphics.md 7.1); the renderer draws them at the full 448-line height, so
  menus and races are drawn at full vertical resolution. The race screens
  are plain frames (`RenderContext.unk18` = 0: no `FRAME_SHIFT`).
- `GSBuffer::blitField` (T514) is one unshifted copy: no field variants and
  no deflicker blur (its passes blended the lines around each one).
- The game's waits for a field (the boot fade, the arcade and GT-mode menu
  exits) are gone; the afterimage has no field correction.
- Interlaced movies (two fields stacked in each frame): `MenuBgMovie` keeps
  its own field (`s_moviePhase`, T253): a frame on every other update, its
  upper field shown then and its lower one on the next update, each sampled
  bilinear and placed on its own lines (`fieldOffset`, +-0.25 field texel in
  `MpegImage::movieUV`): 59.94 progressive pictures a second, no bob.
- Ghost cars: the PS2 drew them on the other field's rows (SCANMSK), which
  blitField's filter averaged with the rows in between. Now
  `gfx_half_blend_begin` / `_end` (T131): the renderer keeps a copy of the
  screen at the begin and at the end draws it over the screen at 50 %
  (`CMD_HALF_BLEND`, `exec_half_blend`): the mean of the frame with and
  without the ghost, RGB (the ghost's alpha stays). The ghosts are drawn
  after the other cars (`drawCars`' second pass): a car drawn after one
  cannot show through it, and lost the parts behind the ghost to what was
  behind both - side by side, the distance sort swapped the two every few
  frames and it flickered like z-fighting (user report; on the PS2's ghost
  rows the order did not matter).
- Kept: the 12.3 coordinates of half-height drawing (buffer geometry), and
  the scan mask of the tyre blur (T449: rows of its own small buffer, a
  supersampling trick, not a field).
- Jitter antialiasing removed (user, 2026-10-19: the menu cars looked less
  crisp than in a race): the menu car (`CarRender::draw`, T245, up to five
  passes) and the trophies (`MiniPrize::drawAntialiased` T145, two passes;
  `draw_trophy_ring` T058, four to seven) drew the model several times
  shifted by fractions of a GS pixel (`gfx_offset_xy`) and averaged the
  passes. The renderer maps XYOFFSET to native pixels at the output scale,
  so at 1080p the shifts spanned a pixel or more and the mean blurred the
  model. They are drawn straight: the car's passes only cross-fade the
  paint when its colour changes (one pass otherwise), the trophy ring keeps
  its passes only for its depth of field (eye on a circle, no jitter). The
  needle and steering-wheel copies of the HUD keep their sub-pixel steps
  (part of their motion blur).

### Aspect

Roadmap item 8, the dynamic aspect (`src/game/app/pc_aspect.*`): the
menus stay 4:3 (pillarboxed, or stretched: below); a frame with a race
view fills the window at its aspect A.

- **The menus' stretch** (roadmap item 10; `[Display] MenuAspect=original |
  stretch`, the F2 menu's DISPLAY > MENU ASPECT, the launcher's Display
  tab; user, 2026-10-24): a frame without a race view shows its 640 x 448
  picture over the whole window (`gfx_set_menu_aspect` -> the renderer's
  `g_menuAspect`, kept until changed, given to a frame that set no picture
  aspect of its own at `pglr_end_frame`: the targets at the window's scales,
  sx != sy, so it is sharp). Stretched as a whole - the pictures, the
  movies, the boxes - but two things are squeezed back by kx = (4/3) / A
  (ky = A / (4/3) for a window taller than 4:3), so they keep their shape:
  - the 3D car models (`pc_menu_squeeze_projection`, beside the dormant
    16:9 option's `x 0.75`: `CarRender`'s projection, T245 - the menu cars
    everywhere -, T107's GT mode car list, T460's prizes), about their
    projection's centre: a car keeps its place in the stretched layout;
  - the texts (`font_set_squeeze`, pgl_font.cpp): while `font_vprintf`,
    `font_puts`, `font_putc` or `font_text_width` runs, the font's scale and
    spacing times (kx, ky), so the glyphs and their advances shrink; the
    alignment's measure (`font_apply_align`) takes the squeezed text, so a
    left, centred or right aligned text stays on its anchor, and a layout
    sized by `font_text_width` (a highlight behind a text) shrinks with it.
  - a race view inside a menu (`pc_aspect_menu_view`: the race menus' race
    behind them, T112; the licence test's pre-race window, T103; the data
    logger's replay window, T055): the frame stays a menu frame, but
    meanwhile `pc_aspect_kx / ky` answer the menu's squeezes, so the view's
    3D - its projection (`proj_y_scale`, `drawSubView`'s 4:3 branch), the
    mirror, the lens flare's sprites - is squeezed as the menu's cars are
    (`pc_menu_squeeze_projection` steps aside then: no second squeeze).
  - the data logger's race HUD (its replay window: `RaceSolitaire::virtual_45`
    draws the two cars' race displays - the steering, the accel / brake
    monitor, the bar tachometer, the gear: racedetail.db's
    `RACE_MODE_LOGGER`, all left aligned - into 640 x 448 draw areas at x 0
    and 560, the first car's meters at the frame's left edge, the second's
    at its right): between `pc_hud_menu(anchorX)` and `pc_hud_menu_end`
    each display's HUD units are squeezed by (kx, ky) toward its side's
    edge (anchor 0 for the first, 80 - the frame's right edge - for the
    second; y toward the top): `RaceDisplay::virtual_3`'s projection takes
    `pc_hud_menu_window` (left = anchor (1 - 1 / kx), 640 / kx wide), and
    `pc_hud_begin` maps the system font so - its scale (kx, ky), its
    offset -left - with the font's menu squeeze off until `pc_hud_end` (no
    second squeeze). The meters keep their shape where the 4:3 layout put
    them.
  A race frame turns both off (`pc_aspect_race_frame`); the race's own
  mapping then applies. The afterimage of a race's first frame takes the
  stretched picture's aspect (`pc_aspect_prev`: the aspect as shown).

- The game still draws into its 640 x 448 GS frame, squeezed: a race view
  (`RacePS2Base::drawView` / `drawSubView`) calls `pc_aspect_race_frame`,
  whose first call of the frame takes the window's aspect (held to 1:4 ..
  6:1, beyond them black bars; a minimised window keeps the last one) and
  the frame's whole mapping (A, kx, ky, the HUD canvas), tells the renderer
  (`gfx_set_picture_aspect` -> `pglr_set_picture_aspect`, recorded with the
  frame), and the projection
  (`proj_y_scale`, T131) takes `vu0_scale(kx, ky)`: kx = (4/3) / A when
  wider than 4:3 (the view shows more to the sides), ky = A / (4/3) when
  taller (more above and below). The renderer then sizes the targets for the
  window - separate scales sx = W / 640, sy = H / 448 (`pgl_targets.cpp`,
  `targets_frame_aspect` before the frame executes; a change between the
  4:3 picture and the window's rescales at once, window resizes after 15
  frames as before) - which undoes the squeeze at full resolution, square
  pixels, and presents the frame over the whole window.
- The game's anamorphic 16:9 option is dormant (`GameOptions::apply` keeps
  `RenderContext::wide` 0, `fetch` leaves the saved option alone; T512's
  console aspect too): its code drew the same squeeze for 16:9 (x 0.75, an
  853-wide HUD, wide-aligned elements 106.67 out, the font 0.75), and those
  places now draw it for A from `pc_aspect`.
- The HUD in HUD units (640 x 448 = the 4:3 screen; 640 x 224 for a split
  screen's view): the window spans `pc_hud_window(height)` (320 +- 320 / kx
  wide, height / 2 +- (height / 2) / ky high: 320 +- 240 A when wider than
  4:3, 224 +- 224 (4/3) / A when taller), RaceDisplay's ortho (T154). Its
  canvas has the aspect C = A held to 4:3 .. `HudMaxAspect` (gt3pc.ini, 21:9
  by default): elements aligned to a side (`RACE_ALIGN_WIDE`:
  `race_align_pos(f)`, the time displays, the lap counter, the logo, the
  minimap's dots) move by `pc_hud_shift` = 240 C - 320 to its edges; beyond
  the cap the canvas stays centred; taller than 4:3 it is the 4:3 band in
  the middle. Examples: 1792 x 1080 (between 4:3 and 21:9) the HUD at the
  window's edges; 3017 x 1080 a 2520 px canvas in the middle; 1080 x 1080 an
  810 px high band, more vertical field of view.
- The HUD safe zone s (gt3pc.ini `HudSafeZone`, 75 .. 100 %, the F2 menu;
  taken per race frame): the window spans 1 / s as many HUD units
  (`pc_hud_window`) and a HUD unit is s times the GS pixels (`pc_hud_kx /
  ky`: the font, the design squeeze of the texel centres, the race menus'
  scissors), so the HUD, the race menus and the mirror (`drawMirror`'s
  rectangle) are s times as large about the window's centre, the canvas and
  its side-anchored elements with them. Full-screen fills still reach the
  window's edges (`pc_hud_rect`, `pc_hud_screen_fill`); the replays'
  vignette takes the window without it (`pc_view_window`).
- **Split screen** (the 2-player battle; gt3pc.ini `SplitScreen`, the F2
  menu, taken at a race's or replay's start: `pc_split_side_take` in
  `RaceDisplay::virtual_0`): horizontal, one view above the other (race
  mode 7), vertical, side by side (race mode 8 and `RaceSplitBattle::
  render`'s side by side branch, which retail kept for its dormant 16:9
  screen, `g_002F424C`; retail letterboxed its 3D to 8 / 11 of the height,
  the PC draws the half's full height, user request), or two monitors
  (below). **Each view is a screen of its own, as a single player's** (user
  request, after retail's split layouts - mode 7 made for 640 x 224, mode 8
  for retail's 426.67 x 448 16:9 half - had been stretched, scaled and
  patched view by view): its aspect a = A (w / 640) (448 / h) for its GS
  rectangle w x h (`pc_view_scales`) gives its own squeezes kx / ky from the
  4:3 base, as the frame's give a single player's - the 3D (`drawSubView`
  in a race frame: the base projection squeezed by them, no zoom; wider
  than 4:3 more to the sides, taller more above and below), the mirror
  (`drawMirror`: drawView's rectangle in its 640 x 448 design, placed as the
  view's HUD maps it) and the HUD: a **HUD region** (`pc_hud_region(x0, y0,
  x1, y1)`, set around each view's `virtual_45`: `pc_hud_window`,
  `pc_hud_shift` and `pc_hud_begin` answer for the region - its squeezes,
  canvas (C = a held to 4:3 .. `HudMaxAspect`), shift; the font's and the
  renderer's scale its GS pixels per HUD unit, `pc_hud_kx / ky`) with the
  single player's layouts (`setLayout`: RACE_MODE_SINGLE, a replay
  RACE_MODE_REPLAY). So a 16:9 window's halves (8:9) show the 4:3 band of
  the HUD across each half, its views one above the other (32:9) the HUD the
  view's height, its canvas held to the cap in the middle.
  Two monitors (`SplitScreen=monitors`, `PC_SPLIT_MONITORS`): the side by
  side branch with the frame drawn over the screens' canvas
  (`pc_split_monitors_frame`, "Screens" below) once the second monitor is
  open (until then, or with no second monitor, the vertical split): each
  player's view as wide as its monitor's share of the canvas
  (`pc_split_monitor_x`; 320 for two alike). The result board and the pause
  menu are drawn once per monitor, each in its region
  (`race_menu_begin_draw`: the region's draw area; the scissors about its
  middle, `pc_hud_center_x / _y`); on one monitor once over the window.
- **HUD style** (`SplitHud`, `SingleHud`, the F2 menu; `pc_hud_style`, laid
  out again at once when it changes: `pc_hud_style_changed`): analog, the
  layouts' meter sets (`layoutMeters`: onboard 0, remote 1, by the camera);
  digital, `kPcDigitalSet` in their place - race mode 7's set, 224 lower in
  the single player's 448-high layout (the frame, the digital speed, the bar
  tachometer, the gear), with the battery gauge above it. `SingleHud`
  chooses a single player's, `SplitHud` the split screens' views' (which
  show the single player's layouts).
- **Screens** (multi-monitor; `platform/gpu.h`): screen 0 is the game
  window's; extra screens are borderless windows over other monitors
  (`window::create_screen`: owned by the game window, never activated)
  with swap chains of their own. Borderless only: a first version put them
  in exclusive fullscreen too, and several exclusive swap chains lagged
  badly (user test) - so while any extra screen is wanted, the fullscreen
  mode's game window leaves exclusive fullscreen for borderless over its
  monitor and takes it again after (`suspend_fullscreen`; a span in the
  fullscreen mode is borderless all along). A flip model swap chain shows
  nothing after a fullscreen transition until ResizeBuffers, even at the
  same size: the back buffers are made again after each (`g_forceResize`;
  the first version skipped it at 1920 x 1080 both ways and player 1's
  monitor went blank, user report), and after a failed Present (logged:
  `gpu: present to ... failed`). Two users: the **span**
  (`SpanMonitors` with a borderless / fullscreen mode: the game window on
  its monitor of the span - its own, else the middle one,
  `window::set_full_monitor` - and the others extra) and the **split
  screen's second monitor** (opened while the race asks each frame,
  `pacing::want_split_screen`, closed 4 s after - a load between the race
  and its replay keeps it: `SplitMonitor`, or the monitor nearest the
  game's). The **canvas**: the
  screens side by side at screen 0's height (the span's left to right as on
  the desktop, the split screen's screen 0 first), each as wide as its
  picture there. A race frame drawn over it (`gfx_set_picture_multi`: the
  span's race frames, the two-monitor split screen's) takes the canvas's
  aspect (`pc_aspect_race_frame`) and size (`picture_scales`), and the
  present shows each screen its part (`present_now`); every other frame
  (the 4:3 menus) is screen 0's, the others black. The overlays (F2, F3,
  the tick step) are screen 0's. The extra screens present first, with the
  game window's interval; the game window's present is the one that waits.
  The HUD canvas is centred on the whole picture (three monitors: on the
  middle one; `HudMaxAspect` 16:9 keeps it there).
- **HUD draws**: `pc_hud_begin(layout, height)` .. `pc_hud_end()` around
  every 2D draw designed for the 4:3 screen (RaceDisplay T154: layout
  `PC_HUD_CANVAS`; the race menus T197 and the examination messages T146:
  `PC_HUD_SCREEN`, the 4:3 screen in the middle). One mapping makes all of
  it: the projection (the caller's, `pc_hud_window`), the font (it draws in
  GS pixels: `setScale(kx, ky)` squeezes the glyphs, `setOffset` puts the
  texts placed in screen units - the rank, the time limit - where the
  projection puts that layout's units), and the renderer
  (`gfx_set_design_scale(kx, ky)` -> `CMD_DESIGN`, in order with the draws;
  every frame starts at 1:1): a sprite's texel-centre shift (section 4) is
  taken in design pixels. Without it the shift assumed one texel per GS
  pixel: a glyph squeezed by ky = 0.42 (1080 x 1920) has 2.4 texels per GS
  pixel, so its texels moved by 0.9 instead of 0.2 - the neighbouring
  glyph's row bled in at the top, the last row was cut, and the samples sat
  between texels (user report: blurry HUD, letter bounds off at very tall
  aspects; wide ones had it horizontally). The font's open glyph batch is
  cut at both ends (recorded under the mapping it was drawn for). Render
  targets drawn as textures keep the GS-pixel shift (1:1 to their native
  texels). `gfx_offset_xy`'s steps are design pixels too (the dials' scale
  marks and needle, the steering wheel: 8 passes at 1/8 shifted by up to
  0.75; in GS pixels a 1080 x 1920 window spread them over 3 native rows,
  user report): the offset follows the steps' running sum squeezed and
  rounded, so a sequence summing to 0 returns exactly. Limits of drawing squeezed: positions are 1/16 GS pixel (0.27
  native px at 1080 x 1920), and a scissor is whole GS pixels (the race
  menus' result bars: up to 2.4 HUD units at 9:16).
- Overlays: the race menus (pause, replay options, result board:
  `race_menu_begin_draw`, T197) draw their 640 x 448 in the middle at 4:3
  proportions (the window's ortho, a HUD draw, the scissor squeezed); a
  coloured rectangle covering that whole screen (the three pause menus'
  dimmer `bg1`, x -129, 896 wide: 1.87:1 at most) reaches the window's edges
  (`pc_hud_screen_fill`, from `Rectangle::virtual_2`, T256); the
  licence / time trial results (T146 / T141) fill the window with their band
  (`exam_fill_window`), their texts a HUD draw (`exam_font_window`); the
  trophy's projection (T460) and the menu cars'
  (T245, T107) take the squeeze in a race frame (1 in the menus); the flare
  (T159: 0.911 ky / kx), the vignette ring (T183: the window's extent) and the
  mirror (T131: its rectangle squeezed around the screen's centre, its
  projection against it: the 4:3 mirror at its 4:3 size) follow.
- The race's first frame cross-fades from the frame shown before it (T131
  `captureAfterimage`: 0.5 s), a 4:3 menu or loading screen: drawn over the
  whole race frame it was stretched to the window (user report). The
  capture keeps the aspect of the frame it came from (`pc_aspect_prev`) and
  is drawn into that picture's rectangle (`pc_aspect_picture_rect`), its
  black bars fading alike: the screen fades from what was shown.
- 4:3 on purpose: the short menu over a race (T112) and the licence test's
  pre-race menu with its race window (T103) wrap their race view in
  `pc_aspect_menu_view`, so the frame stays the 4:3 picture. The split
  screen stays one view above the other (the 16:9 option put them side by
  side).

### Anti-aliasing

Roadmap item 13 (`[Graphics] AntiAliasing`, the F2 settings menu; the
renderer reads the setting before each frame executes):

- **MSAA 2x / 4x / 8x** (`targets_frame_settings`): every target at the
  output scale is multisampled - colour R8G8B8A8 and depth D32S8 at the
  same count (D3D11 binds them together: `depth_target` matches the colour
  target's count). The device's most up to the count chosen (both formats
  checked; none below feature level 10.1). A change re-creates them all
  before the frame: colour kept (point samples from the old resolve), depth
  cleared. Targets at GS resolution (the scan-masked wheel blur, the flare
  occlusion buffers) stay single-sampled.
- A multisampled target is read only through its **resolve** - `copy` /
  `copySrv`, made when the target changed since (`gen`): `target_srv`,
  `target_copy_srv`, `target_tex1` stand for its texture everywhere (draws
  sampling it, DATE / C = Ad copies, the colour filter, the afterimage's
  capture, half-strength draws, readbacks, flare queries, presentation).
  Colour: `ResolveSubresource` (the samples' mean: the edges smoothed, the
  pixels inside unchanged). Depth: no format resolves, so a full-screen pass
  (`ps_depth_resolve`, ps_4_1) writes sample 0 of each pixel into the copy's
  depth. Alpha-tested edges (foliage, fences) stay aliased (no alpha to
  coverage: the GS's alpha test is exact). The copy is refreshed where the
  target was written since (section 9) for the draws that read their own
  target (DATE, C = Ad, the target as their texture); a read of the whole
  picture refreshes all of it (`target_copy_srv(t, true)`: `target_srv` /
  `target_tex1` - other targets' draws, presentation, readbacks -, FXAA's
  source, the colour filter). Read through the partial refresh, the
  anti-aliased race views lost pieces of the cars (a roof, a tail light,
  the wheels as they turned) where the copy still held an earlier refresh's
  pixels (user report, both MSAA and FXAA; off was clean).
- On a multisampled target a GS position w is native **w s** (no + 0.5,
  section 4): with it the integer edges of the 2D lay on the pixel centres,
  half of an edge pixel's samples outside, and each full-screen copy left the
  picture's left column and top row half the old contents - a smear along
  the left and top edges (user report). The sprites' texel shift follows
  (slope x (pix / s - 0.5 k), pix = 0 or 0.5): 1:1 copies stay exact.
- **FXAA** (`CMD_POST_AA`, `exec_post_aa`, `ps_fxaa`: FXAA 3.11's quality
  algorithm, sub-pixel blend 1/2): the race views' 3D picture filtered in
  place right after it is drawn, before it goes into the frame and the HUD
  (`gfx_post_antialias` in `RacePS2Base::drawView` / `drawSubView`), so the
  HUD and the menus stay sharp. The menus' 3D (cars, trophies) gets none
  (MSAA covers them). Luma from the colour; the GS alpha byte kept.

### Free camera

The chase camera (`OnBoardCamera` mount 1, T378) swings round the car on
its own controls (user request; `game/app/pc_freecam.*`): each profile's
Chase cam left / right / up / down (gt3pc.ini `CameraLeft` .. `CameraDown`,
the launcher's Controls > Mapping, first rows; user, 2026-10-24), by
default a controller's right analog stick and Controller 1's J / L / I /
K, none on a wheel; read by `pad_pc::camera` from the profile of the
device the player drives with (the controller's, or the wheel's: a wheel
player orbits too once they are mapped). After `applyMountView` built
its view, `pc_freecam_apply` turns that matrix round a pivot 0.5 m above
the car's origin (its middle): in the camera's own axes - right, up (the
screen's, the side of the world's +y) and back (pivot to eye) - so no
handedness of the world is assumed; the eye and the view turn together,
the car keeps its place in the picture. Stick left / right: up to 180
degrees each side (stick right swings it round the car's left side, user
choice; at the stops in front of the car looking back at it); up: up to 45
degrees (looking down on the
car); down: up to 45 degrees, held so that the eye stays 0.3 m above the
road - the runway under it (`RaceCourse::getContact`, the road surface the
cars run on; off it the ground under the car), the pitch found by halving
(the eye's height grows with the pitch; held at the road the aim goes no
lower). The stick is additive (user request): while held it turns the aim,
its deflection the speed - on an exponential curve, (e^(3 a) - 1) / (e^3 -
1) past the dead zone (half way about 18%) of 360 degrees a second sideways
and 180 up / down - against a pull back behind that grows with the angle,
always on (user request: a spring, 1.8 times the angle a second - weak near
the middle, strong far out): a held deflection settles where the two meet
(full deflection at 200 / 100 degrees, past the stops; half way about 36 /
18), let go it returns (1/e in 0.56 s). The shown swing
eases toward the aim (35%
of the way each tick), per camera: the split screen's two each with its
player's controls, the `RaceInput` the camera reads. Only the chase mount,
not while looking back; a pad with sticks, or a wheel; on an axis the
controller settings give a function (throttle / brake on the right stick:
`ControllerConfig::keyAxis`) the camera leaves out the bindings the virtual
right stick's controls have too (the stick stays the game's; others still
turn the camera). The view is per tick and blended between ticks as any
(`pc_view_interpolate`).

### Debug camera

User, 2026-10-24 (`game/app/pc_debugcam.*`, `platform/debug_camera.*`). In
a race or a replay the DebugCamera hotkey (F7, `[Hotkeys]`) turns a free
camera on or off, "DEBUG CAMERA ENABLED" / "DISABLED" over the picture for
2 s; elsewhere the key does nothing, and a race's start and end
(`RacePS2Base`'s constructor and destructor) turn it off. It replaces view
0 (the single screen; player 1's half of a split screen) where the game
asks the camera manager: `CameraManager::applyView` puts its view matrix in
place of the camera's (every pass - the view, the road reflection, the
environment map - so they agree), `getPositionX / Y / Z` give its eye (the
course's vision list, the cars' levels of detail and the sound's listener
follow it), `isDrawBackMirror` says no, `virtual_12` / `F_00209370` show
every car (the bumper view hid the player's own car: user report
2026-10-24). A live race's player 1 is put in the chase view meanwhile
(`OnBoardCamera::update`, `pc_debugcam_mount`: the view button overridden,
the player's view given back when the camera is turned off), so the sound
is the chase view's too. The race view's projection takes
its field of view after `pc_fov_apply`, and the view is not blended between
ticks (it moves every frame by itself; the cars still are). The HUD - the
race display, the result board, the pause menu (`RacePS2Base::render`'s
overlays, `RaceSplitBattle::virtual_45`) - is not drawn, and the players'
controllers and wheels give the game nothing while it flies (at rest:
`pad_pc`, `wheel`). The sounds stay at the game's camera (user report
2026-10-24: the car sounds glitched): `RacePS2Base::virtual_29`, each car's
sound and the ambient sound, runs under a `PcDebugcamBypass`, so their
listener, panning (`applyView`) and Doppler are the chosen camera's - the
driver's head in the bumper view - not the debug camera's (its eye against
the camera's last eye made a huge listener speed: the Doppler swung).

Lock (a press: "DEBUG CAMERA LOCKED", again "UNLOCKED") holds the camera
where it is and gives the players their controllers and wheels back: the
race driven as seen from it, the HUD still off.

It starts where the camera was: its eye, heading, pitch and field of view
(read from the view matrix: the rotation's columns, the projection looking
down -z; the matrix rebuilt in the same handedness and axis lengths). The
Debug camera profile (`[DebugCamera]`, the launcher's Controls tab: the
first gamepad, with that profile's dead zones, and the keyboard / mouse)
flies it once a frame by the real time (`pc_debugcam_frame`, from
`pc_frame_render_begin`) - also while the tick step pauses: F5-paused, the
frames go on being drawn at the last tick (one a vblank without
interpolation) so the camera can look round a frozen moment.

| Control | Default (gamepad / keyboard) | Does |
|---|---|---|
| Move forward / back / left / right | left stick / W S A D | in the horizontal plane of the heading (X and Z) |
| Move up / down | right / left trigger / E Q | vertically (Y) |
| Look up / down / left / right | right stick / the mouse's movement | heading 120 degrees a second, pitch 90 (to +-89); the mouse 0.05 degrees a raw count |
| Move speed up / down | R1 / L1, mouse wheel | x4 a second held, x1.25 a notch; 0.5 .. 500 m/s, 15 at first ("DEBUG CAMERA SPEED ..." for 1 s) |
| Field of view up / down | D-pad up / down, Page Up / Down | 45 degrees a second held, 5 a notch; 10 .. 150 degrees vertically ("... FIELD OF VIEW ...") |
| Lock camera (drive) | right stick click / F | held, the players driving; a press again flies it |

An analog input counts past a 10% dead zone, squared (fine near the
middle). The mouse's movement is the raw input's (`WM_INPUT`: relative,
not stopped at the screen's edges; `window::mouse_motion`); while the
camera flies and no menu is shown the cursor is hidden over the window and
confined to it (let go with the focus, the lock or the camera off).

### Detail and distance

Roadmap items 11 and 12 (`[Graphics] Lod`, `RenderDistance`, the F2 menu;
`game/app/pc_detail.*`). Both take effect at the next frame (the rear view
below at the camera's next tick).

- **Levels of detail off** - the full detail everywhere, the models' lower
  levels left dormant:
  - every model's LOD command (T451 0x07: car bodies, wheels, course parts,
    menu cars, prizes) takes level 0, whatever its callback would choose;
  - the course's texture LOD takes set 0, the largest (the race views by
    size, the split screen's and the mirror's forced set 2);
  - the tyres take level 0 (`RaceTireModel::selectLOD`; an explicit level
    above 0 too, still view-tested): the real tyres in the mirror
    (`drawCars` LOD 2) and in the road reflection (`drawCarReflections`, which
    drew cylinders);
  - the split screen (`drawSubView`, sub != 0) draws the full course: the
    main vision list (`visionList`, its sprite flag `visionMain`) instead of
    `visionSub`, the course's full far clip (not half), the environment map
    (also with `ENV_SUB_NO_ENVMAP`), the gadgets (cones), the lamps and
    course lights (also with `ENV_SPLIT_NO_LIGHTS`) with their occlusion
    test, the smoke with its ground clipping, the tyre blur and the sparks;
  - the mirror draws the main vision list (not `visionMirror` /
    `visionMirrorAlt`), at 120 m in the split screen too.
  The environment map's own simplified course (`envModel` / `envVision`,
  T276) has no full version: unchanged. Every full-detail version is in the
  course and car files (the split screen loads the normal course).
- **Render distance** (100 .. 400 %): the far clips (`drawView`,
  `drawSubView`, the mirror) and the lights' cut-off distances
  (`FlareClass::project`'s `far`) times the factor. A vision point lists the
  course parts seen from it within the course's own distance, so beyond
  100 % the course is drawn from the parts of every point of the main list
  within (factor - 1) x `farClip` of the nearest one too
  (`pc_vision_parts`: each part once, the farther ones first, cached per
  point; `course_draw`, and the course lights in `addCourseLights`). The
  cars, the smoke and the gadgets follow the far clip. Depth precision near
  the camera is unchanged (one depth step ~ distance^2 / (2^24 near): about
  1.3 m at 1500 m).
- **Field of view** (`[Graphics] Fov` for the live race, `ReplayFov` for
  the replays and the attract mode's demos, 50 .. 200 % each, the F2 menu;
  user, 2026-10-20): the race views (`drawView`, `drawSubView`; not the
  mirror) have their projection widened or
  narrowed once it is set - camera, pixel aspect, the window's aspect, the
  split screen's zoom - by `pc_fov_apply`: the vertical half-angle read from
  the matrix (its y scale = 1 / tan), times the percent, held to 5 .. 85
  degrees, both axes scaled by tan(old) / tan(new). It is an angle percent
  of each camera's own (bumper, chase, in-car differ). The models' view
  test (T451's frustum command) and the flare's projection (`FlareClass::
  begin` keeps the matrix) use the scaled matrix, so nothing is culled at
  the wider edges; the course's vision points list parts by position, not
  by direction. The levels of detail still choose by distance.
- **Rear view** (`[Graphics] RearViewBumper`, `RearViewChase`, the F2
  menu's GRAPHICS > REAR VIEW; user, 2026-10-24): the live race's mirror
  (`drawMirror`, 240 x 64 at the top centre, the view behind with the
  course's mirror vision list, the cars at LOD 2, the smoke) is drawn while
  the camera's `isDrawBackMirror` says so. `OnBoardCamera` (T378) set it for
  the bumper camera (mount 0) not looking back; now for the bumper camera
  while RearViewBumper is on (the default) and for the chase camera (mount
  1) while RearViewChase is on (off by default; the mirror's rectangle and
  view as the bumper camera's, from the chase camera's place). The split
  screen's views take the same settings. Each mirror draws the course and
  the cars once more: the F2 menu's help says "high impact on frame rate".

### High-resolution images

The car icons (user, 2026-10-19: "increase resolution of car icons in
races"): `CarIconMaker` (T184) draws each car of a race into a 64 x 32 buffer
when the race loads - at the output scale here, so the buffer holds 64 s x
32 s native pixels - and reads it back (`GSBuffer::download`) into its
`CarIcon`; the race display (T157 `RaceCarIconDisplay`) and the result board
(T200 `CarIconItem`) upload those 64 x 32 pixels again (`loadImage`) and draw
them, nearest. The readback alone would bring the icons back to 64 x 32.

- `registry_hires_readback` (pgl_textures.cpp, from `readback`): a CT32
  readback of up to 256 x 256 from a target with more than one native pixel
  per GS pixel keeps that rectangle's native pixels (from the resolve under
  MSAA) in an image of its own, keyed by a hash of the GS pixels read back
  (at most 64, the least recently used dropped). The flare result strip is
  at GS resolution: skipped.
- `registry_upload`: a CT32 upload into a surface whose pixels hash to such a
  key marks that rectangle of the surface as the image (`hkey`); another
  upload over it ends that.
- `texture_bind`: a surface texture whose region (CLAMP's region clamp) is
  exactly that rectangle is sampled from the image, bilinear (`TexBinding::
  linear`: the game's nearest was for 1:1 pixels). Any other use of the
  surface reads its 64 x 32 pixels as before. Pixels the game changed after
  the readback hash differently: the low-res icon.

## 6. Textures (the registry)

See docs/graphics.md 5. Owners per GS block in two layers (RGB / Z bytes, top
byte). Objects:

- **Tex1 sets** (upload groups): parsed once (gstex), laid out once into a
  TexSpace of their blocks; recognised again by their header and upload
  table (no parse), moved sets keep their layout (block deltas); small
  uploads (<= 4 KB: CLUTs, animated) are hashed each upload. Each upload is
  placed at the base recorded with it (section 2), and every block a set
  owns keeps the base of the copy that wrote it: a set moved within the
  frame leaves its earlier copies in VRAM, and the draws recorded before the
  move (their TEX0 / CBP point there) decode that copy. The registry used to
  read the set's base when the list executed - its last one of the frame -
  so every copy but the last was registered at the last one's blocks, and the
  draws that used an earlier copy decoded whatever owned their TBP / CBP then:
  another set's bytes through another set's CLUT, or a render target (a T4
  of a target: skipped). User reports: Laguna Seca's grass (dark magenta
  with green speckles: its base texture and its LOCAL specular pass, the
  noise texture added with Cs Ad + Cd) and the buildings' windows (bright green:
  the ENVMTX pass of the course's sky sphere map, Cs Ad + Cd again), and the
  race cars' wheel faces (wrong colours, or none: the face is linked anew for
  every wheel by `TexSet::load`, usually at another block, and only the
  wheels drawn from the face's last copy of the frame matched).
  `GT3PC_TEX1_LOG=1` (section 10) lists the sets uploaded at two bases in
  one frame.
- **Surfaces**: PSMCT32 image uploads land in a CPU image of their buffer
  (one per DBP / DBW; a memcpy per upload: the movies' and IPU pictures'
  16 x 16 macroblocks); the surface's GPU texture is updated once, when a
  draw samples it.
- **Images**: other uploads (the kanji glyphs' T4, ...), data copied.
- **Targets**: marked when drawn into.
- **Planes** (`pfx_split_rgb_planes`, the afterimage): one colour channel of
  a captured frame standing for the top bytes of a PSMCT32 buffer; they own
  the top-byte layer only, so the CT24 frame buffers and Z24 buffers whose
  unused top bytes the PS2 put them in keep their own layer. A PSMT8H / T4HL
  / T4HH texture at a plane's first block (TBW = its FBW) reads it through the
  CLUT: a full-screen pass (`ps_plane`) turns the plane's bytes into the
  palette's colours at the capture's native size, again when the palette or
  the capture changes.

TEX0 resolves through its blocks' owners: a target (sampled directly, or at
the page / block offset of a texture inside it; CT24 alpha = TA0, CT16 alpha
TA1 / TA0 by bit 7), a depth target (Z formats), a surface read as itself,
a set (decoded, cached by content: set identity, TEX0 fields relative to the
set, palette, TEXA where the texels use it - CT24 / CT16; an indexed
texture's palette holds it already) - for sets and surfaces the owner of TBP0's block
decides, since the 2^TW x 2^TH space usually reaches into other objects'
VRAM that is never sampled -, an image upload owning the whole footprint
(decode_linear), else a composition of the owners' uploads in write order. Nothing registered: the draw is skipped and logged
once (`texture at block ... has no source`).

A PSMCT32 texel is both layers: its RGB is the RGB layer owner's, its alpha
the top byte's. A draw that writes no colour channel (FBMSK covers R, G and
B) uses only the alpha - the GS never mixes texel RGB into the alpha it
writes or tests - so when the owner of TBP0's top byte is a colour target,
that target is sampled whoever owns the RGB bytes. A target drawn alpha-only
owns only the top-byte layer: CarIconMaker's mask (section 8) was read
through the owner of RGB bytes it never wrote.

The CLUT buffer is one store of 512 halfwords, as the GS's 1 KB: a CT16 /
CT16S entry e is halfword e, a CT32 entry e its low half at e and its high
half at e + 256. Each TEX0 / TEX2 load reads the CLUT owner's data through
gstex::ClutBuffer and writes it there in its own CPSM (loads repeated come
from a memo keyed by what they read: the CLUT fields and the tags of the
blocks, a block's tag changing with each write to it - a Tex1 set uploaded
again unchanged keeps its tags; a load that would write what the buffer's
rows hold already is skipped, and a texture's palette is kept by what its
rows hold; CLUT loads and texture binds are kept by their TEX0 fields in
front caches checked against what they read: a CLUT's key worked out
again from its few blocks' tags, a Tex1 set's texture by TBP0's two tags
(the set, its base, its data version), an image's or a composed texture
by the generations of the VRAM regions it reads - 64 of 256 blocks, moved
when a tag in them changes; the course's sets streamed in and the wheel
faces loaded per wheel move every region a few times a frame; a load
only notes which load each of the buffer's 32 rows holds now, the
halfwords copied in when a palette is worked out); a texture
reads it in its TEX0's CPSM. So a CT32
load changes only the halfwords it covers of a CT16 palette: the kanji
glyphs' CT32 CLUT inside a line of the menu font (CT16) - with a separate
CT32 array and the last load's CPSM the rest of the line was read from the
CT32 array, a car name brown and blotchy after its "・" (user report).

**Changed in place.** A decoded texture of a Tex1 set is kept under the
set's identity (its image and load stamp, `tex1_relocate`) and the hashes
of the uploads under its footprint. The stamp moves only when the set is
loaded again; the PS2 uploaded every frame from RAM, so bytes that changed
in place - or were read while being written - showed for a frame at most,
where the PC kept the texture decoded from them until the set was loaded
again (user report, section 11: a menu car's reflection garbled, magenta
body and pink wheels, until the menu was entered again). So every upload of
a known set hashes its uploads again - the small ones (<= 4 KB: CLUTs)
whole as before, the big ones by a fingerprint of 32 words spread over them
(`sample_hash`) - and one that changed is laid out again, moves the set's
data version (its block tags, so the CLUT loads and the front caches) and,
through the keys, has its textures decoded anew. The textures decoded ahead
(section 7) carry the fingerprints of the bytes copied at the load, so they
are taken only while the set still holds them. The log has `pgl tex1: frame
N: set ...'s upload i (n bytes) changed in place (no new load): decoded
again` (the first 40).

**Sets without VRAM** (ROADMAP 18, no memory limits). The game's VRAM
allocator (T513, 4 MB; about 2.35 MB left for textures past the frame and
depth buffers) streams the sets a frame uses through VRAM; on the PC it is
bookkeeping only, the renderer decodes from the sets in RAM. A set that
finds no room even after the allocator gave back every released block (a
set bigger than the free VRAM; on the PS2 `loadTex1` returned -1 and the
draws sampled whatever was at the set's old address) is now neither linked
nor uploaded (`tex1_overflow`, pgl_tex1.cpp; also `TexSet::load`, which
linked it at -1, onto the frame buffer): `gfx_set_texture_regs` names the
set with each of its textures selected (`pglr_texture_source` ->
`CMD_TEX_SOURCE`, the TEX0 as it goes out), and `texture_bind` binds a TEX0
equal to the named one from the set itself (its group, never marked in the
owner map, at the base it is linked for), as `registry_clut_load` loads a
CLUT at its CBP from the set - not through the front caches (the set owns
no block). A texture of another set ends it (`NULL`). The log has `PC: Tex1
set ... found no VRAM: drawn from the set` for each such set. The
allocator's block records are 1024 (64: a frame's 65th set found no room
with VRAM left), the frame's packet buffers 16 MB each (0xA0000; the
records grow up from the start, the vertex / text / sprite arrays down from
the end, unchecked: `gfx_alloc_tail` logs a crossing). The 14-bit block
fields (TBP0, CBP, DBP) stay: one set is at most 16384 blocks (4 MB), and a
texture at most 1024 x 1024.

Filtering: TEX1 MMAG / MMIN (LOD = K for LCM and FST primitives); mip levels
other than 0 are not used (logged); wrap REPEAT / CLAMP / REGION_CLAMP (the
decoded texture holds the region); REGION_REPEAT drawn as REPEAT (logged).

## 7. Models

model_draw / vif_model_draw parts: the VifModel decoded once with
gt3vif::decodeModel into immutable vertex / index buffers, cached by address
and checked once per frame against a hash of its header and part table; its
part data is hashed only in turn (each model every 32 frames, spread) and,
once found changed under the same table (logged), every frame. The game
marks the models it writes (`pglr_model_loaded`: vif_model_relocate;
`pglr_model_written`: TStripBuilder::finish through `gfx_model_written` - the
tyre and meter packets rebuilt in place), decoded again at their next draw.
Parts with positions from
vu1_unpack_xyz decode per draw into the dynamic stream. Per batch of the part:
PRIM = (part PRIM & ~primMask) | (primAttr & primMask), the lighting program
and its block from the EE's pgl_send_*, morph weight, the flare program's
occlusion texel (0: not drawn).

### Decoding ahead

The first time a stretch of the course came into view its models and
textures were decoded at that draw (user report: spikes at the same spots on
the first lap; from the second lap on the caches hold them, section 9). Now
what the game loads is decoded ahead on a worker thread (`pgl_prewarm.cpp`,
below normal priority, jobs in load order):

- a **model** (`vif_model_relocate` -> `pglr_model_loaded`): its header, part
  table and parts' data are copied on the game thread and the table hashed;
  the worker decodes the copy (`decodeModel` with a resolver into it), hashes
  its data as `data_hash` would and makes the immutable buffers. The model's
  first draw takes that mesh when the model's table and data still hash so
  (else it decodes as before). A mesh no draw took goes after 10 minutes; one
  a draw decoded itself before the worker was done is dropped by the worker;
- a **Tex1 set** (`tex1_relocate` -> `registry_tex1_loaded`): parsed and its
  uploads' bytes copied on the game thread (as loaded, before any VRAM link),
  with its load stamp. The worker lays it out and decodes each texture the
  way a draw decodes it from the set - its CLUT loaded as pgl loads it
  (`pgl_clut_load`), TEXA the last set texture bound had (it matters only
  for CT16 palettes and CT24 / CT16 texels) - under the key `texture_bind`
  works out (`tex_key`, `group_key`: the set's image and stamp, TBP0's
  place in the set, the palette's content, ...). The executor moves them
  into the texture cache at a miss and at each frame's end; a draw finds them
  there by its key. Anything that does not match (another palette, a CLAMP
  region of the draw's own) is decoded at the draw as before.

The worker never reads the game's memory (the game links a set again for
every VRAM block it gets, and may free what it loaded) and never queues a
Direct3D call: the device is free-threaded, and what it releases it releases
at once (`q::release_here`; nothing of it was used yet). At the end of a
load the log has a line: `pgl: decoded ahead N models (X MB) and M textures
(Y MB) in T ms; since the last line the draws took A models and B textures
decoded ahead, decoded C models and D Tex1 textures themselves` - after a
race, the first numbers count what came ahead and the last what still came
late. `GT3PC_PREWARM=0` turns the worker off (section 10).

## 8. Effects

| Effect | Status |
|---|---|
| Clears, frame-buffer copies / fades (`gfx_frame_effect`, gs_copy_target, gs_fill_target_b), blitField, Blur2D, accumulation, reflections, mirror, env maps, smoke, glows | drawn through the generic path (sprites with the game's blend / texture state; targets sampled through copies) |
| Depth copies (a Z-format FRAME drawn from a Z texture) | native: SV_Depth from the source depth's SRV |
| Tyre smoke / dust (T173 RaceSmoke) | generic path. The courses draw it straight into the frame buffer (`draw(0)`; the dirt courses too), depth-tested against the 640 x 224 copy of the race depth (`depth.blit(unkAC4.cur)`, T131); `CourseEnv.smokeDivX / Y` = 2 / 2 (the reduced offscreen pass of graphics.md 8.15) only in `route5`, `r_route5` (wet) and the four `L_circle*` licence tests (US files; midfield holds floats there, read as 1). The dust flickered every other frame: `unkAC4.buf[1]` (block 0x8C0) shares its native depth with `bufB38`, which the menus' car renders (T245, T058) pair with 640 x 224 frames; the depth target kept that first GS size, so every frame drawn with `unkAC4.buf[1]` copied the race depth squashed 2:1 and the smoke failed its depth test. Fixed by the depth geometry rule (section 5) |
| Colour correction (`pfx_color_correction`) | native recipe (curve texture, the three clamped matrix passes) |
| Course glints (model04: NONE base, alpha-only gloss pass, LOCAL pass with a noise texture, ALPHA 0x58; docs/models.md 7) | generic VU1 path, C = Ad as in section 3. Super Speedway (user report: blue tint, overbright lavender patches on the asphalt in the bumper cam): base = set 0 tex 1 (grey asphalt) x vertex (62, 62, 65); gloss = tex 30, a 32 x 32 T4 whose CLUT alpha is 0x3F..0x80, written alone (FBMSK 0x00FFFFFF); glint = tex 28 (256 x 128, lavender-white noise, mean 144 / 146 / 152) x light B0's specular (0.83, 0.80, 0.75) x material 5's (0.60, 0.58, 0.54) x 128 x r^4 (shininess 4: LOCAL_S6), added with Cs Ad + Cd, FOGCOL 0. Cs <= 127 there, so the old C = Ad form was already right for it: the cause is not found yet (the patches' added light is near neutral where this pass adds a warm tint: see `GT3PC_PGL_SKIP`). The LOCAL programs' alpha is 0x80 (was 0x7F in the shader). In the F9 trace (diag/road_1_plain.log, frame 2374; texture numbers 1-based as `texmat_select` counts them) model04's first set lies at block 0x3AE0: the road's base passes are tex 1 (`0x3B20 T4 256x128 cbp 0x3C1D csa 0`, 25 draws) and tex 29 (`0x3AE0 cbp 0x3C0E csa 6`, 14 draws), both grey asphalt (texel means 102 / 101 / 103, 104 / 105 / 106), program 0, fogged (PRIM 0x3C), vertex colour (57, 56, 56, 0x80), 51 .. 58 on all of them (mdldump; the (62, 62, 65) above is not in the file); the glint's tex 28 (`0x3AE0 csa 4`, 46 draws) is the same 4-bit texels as tex 29 through palette 4 instead of 6, drawn on the base's vertices; the gloss (tex 30, `0x3CF4 32x32 cbp 0x3C08`) is a mesh of its own 3 - 35 cm above the road (no vertex shared), vertex (31, 30, 47, 0x80). Every TEX0 of the race view matches a course set at one consistent base; texdump's decodes of these TEX0 values (the CSA 4 / 6 slices of the T8-loaded CSM1 CLUT) and the registry's paths (copies, CLUT memo, cache keys) show no mismatch. The RGB-only decals blended with 0x44 (set 4's tex 7 light grey strips, tex 21 tyre marks ...) were in none of the `GT3PC_PGL_SKIP` runs (`alpha` is the alpha-only gloss). Next capture: `GT3PC_PGL_PROBE` at a patch pixel (section 10) |
| Wheel-face and tread motion blur (T449 drawWheelTexture / drawTreadBlur, the camera's car: 16 passes) | generic path; the face's w x 3w buffer at GS resolution (section 5), so the scan-masked interleave and the 2:1 shrinks average the still and turned faces as on the GS (they gave stripes at the output scale); the tread's 128 x 64 buffer stays at the output scale (its passes are 1:1 shifts). The still face is a `TexSet` linked anew per wheel (section 6). The wheel model (docs/models.md 7) adds the env map to the rim and the face with DATE + Cs Ad + Cd |
| Lens flare occlusion (flare.c `flare_occlusion_query`: a 10 x 10 box per lamp / course light / sun - box alpha cleared, the shape at alpha 2.0 depth-tested at the light's depth (lamps, course lights) or at Z 0 (the sun: only where nothing but sky was drawn), the box copied to the work buffer, halved to one texel, stored as Cs As into the result strip) | **native** (`CMD_FLARE_QUERY`, `exec_flare_query` / `ps_flare`): the box clear and the shape are drawn as on the PS2 (the frame's alpha stays the game's); the copy, the halvings and the store are one native pass: every GS pixel of the box is tested at its centre against the screen's Z buffer with the shape's ZTST and Z, A = the share passing x 255, and (Cs A) >> 7 / alpha A written into the query's texel of the result strip. The glow draws (`flare_draw_result_disc`, the VU1 flare program) read their texel's R byte from the strip on the GPU - `vif_model_draw` gives each its texel q (`PglrModelPart.flareQ`), `exec_model` binds the strip to the vertex shader (t0), `vs_vu1` loads it (0: not drawn, the triangle dropped by the clipper) - so `flare_readback` no longer waits for the GPU mid-frame: its readback is recorded but skipped (done only for `GT3PC_FLARE_LOG`, whose glow lines show that copy's texels). The emulated chain (GS-resolution buffers, TEXFLUSH batches, the exact Cs As store) never gave usable results at the output scale: in a `GT3PC_FLARE_LOG` capture every result texel was 0, so route5's lamp glows never showed and the sun only by chance (user reports) |
| Weather drops (refraction of the frame) | generic path, frame sampled through a copy |
| Vignette, transitions, menus, text, kanji | generic path |
| Afterimage (T131 captureAfterimage / drawAfterimage: race start, replay camera cuts and car switches) | native capture + generic draws: `pfx_split_rgb_planes` copies the frame shown last (its native pixels) and registers its R, G, B as the three planes (section 6); drawAfterimage's three PSMT8H sprites read them through its identity CLUT and draw each into one channel (FBMSK as the RenderTargetWriteMask, (Cs - Cd) FIX + Cd). Before, the split was skipped and the PSMT8H draws read whatever the registry had at those blocks' top bytes: nothing (not drawn) or a stale upload, which made the red-plane draw (FBMSK 0xFFFFFF00) write ~0 into the frame's red: the replay turned dark cyan for the afterimage's 0.5 s - `crossFadeTime` (user report, car switch in a replay) |
| DATE (destination alpha test) | native: the destination alpha from a copy of the target (section 3). The menu cars' body passes (every GTM1 car model, part 0: `DATE+ datemode(LESS)` around the env-map packets drawn with blend 0x58 Cs Ad + Cd) depend on it: the env reflection goes only where the paint's alpha < 0x80, not over the windows (alpha 0x80 from an alpha-only pass), the pixels an earlier env pass already covered (its As 0x80) or the cleared background (0x80). Drawn without the test it added the showroom env map's warm ceiling lights a second time at full strength (Ad / 0x80 = 1); in the floor reflection (T245 mirror pass, added at 0.65 alpha over the dealer's bluish floor, whose blue is near saturation) this is the likely cause of the yellow tint (user report, dealer showroom; to be confirmed with `GT3PC_PGL_TRACE`, section 10) |
| Car icons (T184 CarIconMaker at a race start; shown by the result board T200 and the split-time display T157) | generic path + readback (graphics.md 8.21). One frame, per car: the model into a 128 x 64 target (its env map a 128 x 128 buffer cleared black, alpha 0x80), the alpha normalised by a DATE sprite (0x80 where bit 7 is set), the mask (alpha > 0x40, cleared and drawn alpha-only), the colour smoothed onto itself, both halved into the 64 x 32 icon (alpha from the mask, RGB from the colour), the icon read back. Three port faults made the user report (every opponent's icon a black box, the player's on a black box): DATE was not native (the sprite set alpha 0x80 everywhere: no transparent background); the readback took the target used last at the icon's block, car i - 1's reflection buffer (section 5); the halving read the mask through the owner of its RGB bytes, which the alpha-only mask never writes (an old frame, an earlier car's textures: the icons' alpha came from there). Fixed: DATE native, readbacks from the block's owner, alpha-only reads of PSMCT32 by the top byte's owner (section 6). `GT3PC_READBACK_LOG` (section 10) shows each icon's readback |
| AA1, dithering, PABE, COLCLAMP off, partial FBMSK, 16-bit frames | ignored / approximated, logged as inexact GS state |
| GS local-to-local / local-to-host transfers inside GIF blocks | not native (logged) |
| CLUT loads from render targets | not native, the old CLUT kept (logged) |

### Frame rate

Roadmap item 9 (`src/game/app/pc_frame.*`): the game keeps its fixed tick.
The render thread (T002) runs the pads, the object's update and its
post-draw slot (`virtual_3`: the course's CLUT animations, the cars' model
animations) once per 59.94 Hz vblank, as on the PS2, so the physics, the
timings, the replays and the ghosts do not depend on the frame rate.

- On a display faster than 61 Hz, while a race asks for it (it runs,
  unpaused, not behind a menu: `RacePS2Base::virtual_29`), the loop also
  draws frames between the ticks, as fast as they go: no vsync (the user's
  variable refresh tests, 2026-10-19; the options come with roadmap item 7),
  one frame queued (`SetMaximumFrameLatency(1)`), so Present waits only for
  the GPU - outside the game CPU (`sched::run_outside`: the other game
  threads run meanwhile, as during a blocking kernel call). Present(0) with
  `DXGI_PRESENT_ALLOW_TEARING` in a window or borderless when the system
  has it (the swap chain is made with `ALLOW_TEARING`; a variable refresh
  display then follows the frames), plain Present(0) in exclusive
  fullscreen; the log says which ("gpu: no vsync, tearing allowed"). The
  first version waited for vsync, with a floor at 1.25 x the refresh rate.
  A frame between
  ticks is presented, executed and drawn at priority 120, below every game
  thread: it takes only the time they leave (first version: at the render
  thread's 7 with the wait inside Present it starved the main thread, and the
  race never started). Only there: any other frame keeps 7 (T002
  `pc_frame_prio`), as on the PS2, where no game thread below it runs while
  it draws - the menus share their objects with the main thread without
  locks (a frame without a tick in the menus' one frame per vblank, drawn at
  120, let the main thread in: the name entry's letters flickered, user
  report). (No frame latency waitable object: D3D11 refuses
  exclusive fullscreen with one.)
- The frame clock and the ticks (`pc_frame_ticks`, after each frame is
  presented and executed): the clock is when the last Present returned -
  the moment GT3PC_FRAME_RATE's wait, a limiter hooked into Present (RTSS),
  the GPU or the display let the frame go. T002 flips to a frame after
  drawing the next one, so every frame is presented the same distance after
  its clock and the shown times follow the display. (The first version took
  the time when the frame's drawing began: it moved with the update on the
  tick frames and with the execution, a few ms either way, and 90 frames a
  second under RTSS were choppy.) The next iteration runs the ticks due by
  the clock - the vblanks at or before it since the last tick
  (`sched::vblanks_at`, the last 31 vblank times): none for a frame between
  ticks, several when the frames are slower than the ticks (caught up: the
  game keeps its speed at any frame rate, also in the menus' one frame per
  vblank; a caught-up tick runs the memory card manager and the pad
  manager, not into another object; only the frame's last tick reads the
  pads, the others repeat the last read, so a press shows once and code
  that looks for it in the draw sees it: system.md 5.5), the post-draw slot
  once after them. More than 8 due
  is a stall (a load): one tick, the rest dropped. `gfx_vsync_mark` keeps
  the driver's vsync state as `gfx_vsync(-1)` did, in one frame per vblank
  too: the vblank of a tick caught up counts as waited for (marked only
  while interpolating, the next `gfx_vsync(-1)` returned at once for it and
  the frame after ran no tick). A line every 10 s while it runs free, or
  when ticks were caught up or dropped: "frame: N frames, M ticks (C caught
  up, D dropped)", in one frame per vblank with ", F frames without one"
  when a frame ran no tick (none expected).
- The shown time lies up to one tick before the last tick (`g_pcViewLag` =
  1 - the fraction of a tick from the last tick to the clock), at any frame
  rate. The cars through GT3's interpolating
  accessors (T302 `view_t`: the slow-motion replays' `prev x t + cur x (1 -
  t)`, the lag in the car's steps; with the car's own offset - the time
  trial ghost's `interpBias`, its lap's fraction of a tick - up to t = 2,
  past the previous step at the last step's rate: held at 1 the ghost stood
  still for part of every tick, jittering, user report); the camera's
  view and projection applied at the tick's state, then blended between the
  ticks of the view's last two draws (`pc_view_interpolate`: the eye
  linearly, the rotation by normalized quaternion lerp, the projection
  element by element; several ticks apart when the frames are slower than
  the ticks, the camera taken as moving evenly between; not across a camera
  cut: another camera or mount of the view - the view button's bumper /
  chase switch moved the eye only a few metres and was blended over a tick,
  a stretch and a fast zoom (user report) - or 20 m, about 45 degrees, a
  third of the zoom). One blend per screen: the camera manager's current
  view (T131 `pc_view_key`); the split screen draws both players through one
  manager, and keyed by the manager player 2's screen got player 1's camera
  (user report). A view not blended (a cut, its first ticks) is drawn
  wholly at the tick: `pc_view_interpolate` sets `g_pcViewLag` to 0 for the
  rest of the view - the cars and the HUD at the shown time under a camera at
  the tick moved against it until the next tick, the bumper / chase switch in
  motion showing an in-between frame (user report) - and each view starts
  from the frame's lag again (`pc_frame_lag`). The rear-view mirror (`drawMirror`) is blended too, keyed
  apart from its view (at the tick's camera it jittered under the cars drawn
  at the shown time: user report); its view is mirrored (z flipped), blended
  unmirrored (column 2 negated) and mirrored again.
- The HUD at the frame rate (`pc_tween`): the values the race display sets
  once a tick are recorded after its update (`RaceDisplay::virtual_2`: the
  tachometer, the speedometer and the digital speed, boost, the accel /
  brake bars, the steering wheel, the battery gauge, the lap and total
  times, the logo's fade, the timers of the messages, the event board and
  the car icons (their fades); the time trial's and the licence tests'
  result fades) and drawn at their blend between the last two ticks: the
  display's draw (`virtual_3`, a `PcTweenScope`) sets them and puts the
  tick's values back after. A step the value's kind does not expect is not
  blended (a timer started again, a new lap: that tick's frames show it),
  nor a value the draw itself changed (the countdown starts the title's
  message in the draw). The values are only written inside their owner's
  draw, so a freed display is never touched. The needles' blur and the
  enhanced trails (accel / brake, steering) follow every frame: each spans
  the frame's motion. Blinks and whole-unit digits (gear, lamps, odometer)
  stay per tick; the minimap's dots use the cars' interpolated positions.
- The trophies (licence results; `MiniPrize`): their key animation time is
  recorded once a tick (`PrizeBase::update`, `pc_tween_loop`: across the
  loop's end too) and the draw poses them at the blended time (T459's own
  key interpolation), the tick's pose back after.
- The models' animations at the shown time (`PcAnimPose`, T131: around
  each race view, `drawView` / `drawSubView`). The animation tracks (T452:
  angles, frame numbers, morph positions between frames, cycles) step once
  a tick and write their value into the model's command streams, which the
  draw reads; between ticks `modelset_pose_anims` writes the value at
  `cur + speed x ofs` (ofs = -dt x the lag, wrapped as the steps wrap)
  without touching `cur`, and writes the tick's back after the view. The
  course's animated models (`course_pose_anims`) and every car
  (`RaceCarModel::pcPoseBegin` / `End`): its tracks (wipers, moving parts of
  transplanted models), the body switches the draw reads (the wheel shake
  and steering morphs, from the interpolating accessors at the shown time)
  and the lamp blink phases; the moving wing and the Pod's tail and colour
  fade (morphs set once a tick from counters) blended between the last two
  ticks (`pc_tween`, a jump such as a new colour category not blended).
  Only what stepped in the last tick moves (`pc_anim_stepped` from the
  steps, `pc_anim_offset`): paused, or a car whose model is off, stays. The
  poses happen inside the race's own draw, so no freed model is touched.
- The palette (CLUT) animations at the shown time: a channel crossfades
  between key palettes (`lerp`), so `clut_anim_pose` (pgl_tex1) steps a
  copy of each channel by -dt x the lag (the step walks back over the keys
  too; frozen channels left alone, as the step leaves them) and applies it.
  The renderer reads the palettes when the frame's lists execute (at the
  kick, after the race's draw and after `virtual_3` applied the tick's),
  so they are written at the frame's end (`pc_frame_at_end`, registered by
  every race view): the course's (`course_pose_clut_anims`, every
  animation) and each car's (`RaceCarModel::pcPosePalettes`, its
  `animSelect`: the Pod's face). When the frames stop being interpolated
  the states' colours are written back once. A texture with an animated
  palette is decoded again for every new palette (the cache keys on the
  palette's hash), so every frame between ticks.
- What stays at 60 Hz: smoke, sparks, rain, the Pod's state. Draw code
  that changed state per frame now does it only in the tick's frame
  (`g_pcTickFrame`): the afterimage capture on a camera cross-fade.
- The frame time monitor (F3, `platform/frame_monitor.*`; F3 cycles off,
  simple, detailed, verbose - the F2 menu's FRAME TIME MONITOR too): over the top
  left of the picture, the time between the frames presented (CPU side,
  when Present returns: the cadence the display, the GPU or an outside
  limiter gives) as a rolling graph of the last 300 frames, the height
  following the frames (5 / 10 / 20 / 33.4 / 66.7 / 133.5 ms), a line at the
  display's refresh period, red a frame over twice the average.
  - Simple: the frame rate and the mean frame time above it, nothing else.
  - Detailed: the minimum, maximum and 99th percentile; the refresh rate,
    vsync, tearing, exclusive fullscreen; the graph orange for a frame after
    a tick, green between ticks, with a line at the tick. Below it the
    breakdown of the render thread's time between two Presents, AVG / MAX
    per part over the graph's frames, and a second graph with each frame's
    parts stacked: UPDATE (the ticks: pads, the object's update, the sound's
    flush; `pc_frame_ticks` to `pc_frame_render_begin`), RECORD (the game's
    draw code, `pglr_begin_frame` .. `pglr_end_frame`), EXECUTE (`pglr_kick`:
    the list into Direct3D) without GPU WAIT - the part of the execution the
    render thread did not run, its CPU cycles (`QueryThreadCycleTime`, at
    the TSC's rate measured against QPC) against the time: the driver
    holding a call while the GPU is behind (with vsync off Present does not
    wait, the next frame's calls do: a GPU-bound race showed EXECUTE as long
    as the GPU's time), a readback's Map (its measured wait when longer) -
    OUTPUT (`pglr_display` to Present: the picture into the back
    buffer, the overlays), PRESENT (the Present call), PACE (`pc_frame_wait`:
    the cap's or the vblank's wait), OTHER (what is left: the game's other
    threads, the scheduler). GPU: the frame's commands on the GPU (the
    execution's timestamp queries, read a few frames later and matched to
    the frame), a white mark per frame.
  - Verbose: the detailed view and a panel at its right: the GPU's time by
    section, AVG / MAX - the sections of `GT3PC_GPU_SECTIONS` (other,
    scene, effects, copy, replay blur, weather, smoke, flare, post; section
    10), timed while this view is shown (or the switch is set) and matched
    to the frames; EXECUTE's CPU time by kind of work, AVG / MAX - the
    renderer's profile (`ProfScope`, pgl_render.h: each kind's own time,
    what it calls that is profiled too left out; TSC ticks scaled by the
    frame's QPC; on only in this view): GS STREAM (register writes,
    vertices, GIF blocks into batches), MODELS (exec_model), VU1 STRIPS,
    UPLOADS, EFFECTS (the recipes, the flare queries), READBACK, STATE
    (compute_state), TEX LOOKUP (texture_bind), TEX DECODE (a cache miss),
    TARGET COPY (target_copy_srv: copies, resolves), D3D SUBMIT
    (draw_passes: the bindings and draw calls), VTX STREAM (the dynamic
    buffers), MESH CHECK (model_mesh: the lookup, the hashes), MESH DECODE
    (a new or changed model, a preXyz part), GS FLUSH (flush_batch: a GS
    batch's bounds and vertex stage), CLUT LOAD (registry_clut_load), TARGET
    SET (CMD_TARGET), CONSTANTS (bind_vs_cb / bind_ps_cb writes), OTHER (the
    command loop), D3D THREAD (section 3: its busy time, apart - it runs
    alongside) and D3D WAIT (the game thread waiting for it), EXEC THREAD
    (the executor thread's time: with it, EXECUTE in the breakdown is the
    game thread's wait for it), OUTSIDE
    (EXECUTE less its wait and the list: the set-up, the flush, the queries,
    the frame's end); and what a frame executed, AVG: draw calls, draw
    states, texture binds and decodes, model parts, GS commands, uploads,
    target copies, GS vertices, GS batches, CLUT loads and memo misses,
    composed texture spaces, constant-buffer writes, the waits for the D3D
    thread (D3D SYNCS), the CLUT loads and texture binds their front caches
    did not hold (CLUT SLOW, TEX SLOW), the VRAM regions whose tags changed
    (VRAM GENS).
  Drawn with its own D3D state on the back buffer just before Present (pgl's
  bindings are forgotten after, `d3d_invalidate`); frames are recorded while
  it is hidden. `GT3PC_PGL_STATS=N` still logs record / execute / present /
  GPU means every N frames.
- Menus draw one frame per tick, as before; on a 60 Hz display nothing
  changes (no frames between ticks, no lag).
- **Frame cap and vertical sync** (roadmap item 7; `[Graphics] FrameCap`,
  `VSync`, the F2 menu):
  - The cap is a frame time (ms, 1 .. 100, kept to 0.001; the menu sets it
    as a frame rate - the usual rates in a ring with OFF between the highest
    and the lowest, a held key stopping at OFF, a rate matched within 0.2 %
    since the ms round - or as the time in 0.01 ms steps, 0.1 once held
    1.5 s, 1 once held 4 s; held keys repeat at most 15 times a second).
    `pc_frame_wait` keeps the frames
    at least that far apart by time, a schedule advanced by the period
    (restarted when a frame is a period late): slept to within 0.8 ms
    (`sched::wait_until`: DelayThread, the high-resolution timer), the rest
    spun, to a few microseconds. With a cap the race draws frames between
    the ticks on any display, at the cap (below 60 a second too: the ticks
    caught up); the menus' one frame per vblank waits more only for a cap
    longer than the tick. `GT3PC_FRAME_RATE=<Hz>` (10 .. 1000;
    `diag\frame_rate_144.cmd`) overrides it, a test switch. A limiter
    outside the game (RTSS) works the same way: the frame clock follows
    Present.
  - VSync off: Present(0), tearing allowed (as above), one frame queued.
    VSync N (1 .. 3): Present(1), at most N frames queued
    (`SetMaximumFrameLatency(N)`) and N + 1 buffers in the flip model swap
    chain (`ResizeBuffers` when it changes; `gpu::apply_vsync`, before a
    frame's back buffer is taken). With vsync the frames between the ticks
    go at the display's rate (the frame clock is still Present's return);
    a cap below the refresh rate paces them below it.
  - The F3 monitor shows both ("VSYNC 2  CAP 6.944 MS").
  - Cap OFF with VSync off: the race's frames run uncapped on any display
    (before, a 60 Hz display stayed at one frame per vblank: user report);
    with VSync on and no cap, a 60 Hz display keeps one frame per vblank. The
    menus are one frame per vblank in every case (nothing to interpolate).
    Whatever still holds the frames back shows in the log every 10 s:
    `frame: N frames (R a second, uncapped | capped at C | one per vblank),
    ... Present waited W ms a frame` - a W near the frame time is the GPU
    (or the display, a limiter) setting the pace, not the cap.
- **Tick step** (user, 2026-10-20; `platform/frame_step.*`): F5 pauses,
  then each F5 (or its autorepeat, at most 4 queued) runs one 59.94 Hz tick;
  F6 runs again. Paused, `pc_frame_ticks` gives no tick. A race drawing
  frames between ticks goes on drawing them at the last tick (lag 0), at its
  usual rate and cap: held still, the GPU and the CPU fell to their idle
  clocks, deeper the longer the pause, and the first frames after F6 ran slow
  while they rose (user report). Else (menus: an object is never drawn
  without a tick) it holds the render thread: no draw, the picture shown
  presented again about every vblank (`gfx_present_again` ->
  `pglr_present_again`), DelayThread between so the game's other threads
  run. Either way the F2 / F3 overlays stay drawn, and the indicator (top
  right, "PAUSED  TICK +N" over the keys set) for 3 s after each F5 (user,
  2026-10-24), the pause going on without it. F5 and F6 are the Pause and
  Resume hotkeys (`gt3pc.ini [Hotkeys]`, the launcher's Controls >
  Hotkeys; F2, F3, Esc, Alt+Enter, and F8 / F9 of the debug aids below, are
  hotkeys too).
  A step runs one tick at the last vblank and draws its frame at the tick
  (the frame clock a tick past it: no interpolation lag). Running again goes
  on from the present (the paused vblanks not caught up). The sound pauses
  with it: the timer's VBLANK_E no longer wakes the music thread
  (`pc_sound_vblank`), so the sequencer moves only with the stepped ticks,
  and the output's stage fades out over 5 ms and stops mixing (voices and the
  BGM stream hold), fading in on F6; the blocks of the stepped ticks apply
  then (late). Works in menus, races and replays alike.

## 9. Performance

Where the time went before the measures below (user reports: menu movies
hanging, attract races at half speed) and what was done:

| Cause | Measure |
|---|---|
| every 16 x 16 movie macroblock (560 a frame) became an image object with copied data, and the movie texture was composed and created anew every frame | PSMCT32 uploads write into a surface (memcpy); one `UpdateSubresource` per frame at use |
| the movie texture's 1024 x 256 space reached other objects' blocks, so every bind composed and decoded it anew (6.7 - 8.9 ms per frame in the movie menus) | sets and surfaces decide by TBP0's owner |
| owner-map marking computed the block of every pixel (`image_block_mask`) | one `block_number` per block cell |
| every textured model part reloaded its CLUT through TexSpace reads | CLUT loads memoized per frame (key: CLUT fields, owner, serials). 2026-10-24: the serials changed at every upload (80 a race frame), the memo missed between them (CLUT LOAD 1.7 ms, 1 500 loads): keyed by the CLUT's block tags instead, kept across frames; a load the buffer holds skipped; palettes kept by the buffer rows' contents (CLUT LOAD 0.67 ms). Then: an image's and a target's tags stay the same when they mark the same blocks again, a global epoch moves only when a tag changes, and CLUT loads and texture binds of the decoded-texture path are kept by their TEX0 fields while it stays (front caches) |
| each model part's draw scanned all the model's batches for its own | the parts' batch ranges kept with the decoded mesh |
| stutters at the same spots every lap (user): decoded models went after 600 frames unused, decoded textures after 300 - at 235 frames a second 1.3 to 2.5 seconds - so every stretch of the course coming back into view decoded its models and textures again (and a Tex1 set dropped and met again had a new id, so new texture keys) | kept by memory and age: models up to 256 MB of decoded vertices, textures up to 512 MB, the least recently used going first beyond that (never one used in the last 5 seconds), anything unused for 10 minutes; a set's textures keyed by its image and load stamp; sets kept 10 minutes unowned |
| the first lap still decoded each stretch of the course where it came into view (user) | models and Tex1 sets decoded ahead on a worker as the game loads them (section 7, "Decoding ahead") |
| each draw looked up its blend, depth and rasterizer states in hash maps | kept with the draw state, found once |
| each draw state translated the GS registers and searched the targets (a map and a list) | `gs::translate` memoized by the registers it reads (a pure function), the last colour / depth target lookup kept while no target is made, goes or changes size (`g_targetsGen`) |
| the registry's object lookups (a hash map) | a direct-mapped slot in front |
| the texture registry's housekeeping (every 64 frames) put the 32768 owner-map entries into a std::set | objects flagged through the owner map, looked up where the owner changes |
| every Tex1 upload was parsed again | sets recognised by header and upload table; layouts kept across VRAM moves |
| the flare readback waited for the GPU every frame | delivered one frame late from the previous copy (cached staging textures); **withdrawn**: the late texels belonged to other lights (section 8), the readback waited again. 2026-10-24: the glow draws read the strip on the GPU (the vertex shader loads the query's texel), the readback skipped: no wait, the same frame's results |
| redundant D3D11 bindings, constant-buffer maps per draw | bound-state cache, constant buffers only when changed. 2026-10-24: a model part's constants change nearly every draw (some 3500 DISCARD renames of one small buffer a race frame): the VS / PS constants go into a 4 MB ring of 256-byte slots (NO_OVERWRITE; DISCARD at the wrap, the other stage's bound constants written again first), bound at their offset (Direct3D 11.1 `*SetConstantBuffers1`; without the driver's support, as before; logged at start) |
| the frame's commands sat in the driver until the next Present | `Flush` after each execution |
| a draw that reads its target (DATE, C = Ad, the target as a texture) refreshed the target's copy whole: with MSAA 8x a full resolve, 35 - 45 a race frame | the copy refreshed only where written since (`target_written_rect`): a GS batch by its vertices, a VU1 draw by its batch's object-space box projected through C (`vu1_area`; the decoded batches keep their box, `batch_bounds`), else the scissor; a resampled target readable as a created one (graphics.md 8.16) |
| the heat-haze / rain drops: a refresh and a draw each (6.3 ms of GPU in replays at MSAA 8x) | drops join one batch unless they read what another drew since the copy (`feedback_check`, `g_fbRects`): 1.8 ms at 8x, 1.1 ms without MSAA |

Measured in the acceptance run (GTX 1660 SUPER, 1280 x 960 window, scale
2.14, build\bin; before the TBP0-owner rule, the block-cell marking and the
Flush): the frame period holds 16.68 ms (60 Hz) in the menus and in the
attract races; per frame recording 0.2 - 0.3 ms, execution 3 - 4 ms in
races (1000 - 2400 draws, 50 - 150 k vertices), 6.7 - 8.9 ms in the movie
menus (the composition, fixed since), 0.3 - 0.4 ms during the opening movie,
presentation 0.1 ms. The GPU time read 16 ms in races only because the
commands waited for the next Present (the Flush is since); it needs a new
reading.

## 10. Debug switches (environment variables)

| Variable | Effect |
|---|---|
| `GT3PC_PGL_STATS=N` | every N frames: command counts, draws, skipped batches, the target copies' whole and partial refreshes (`pgl stats`), and the timing line (`pgl time`: frame period, recording, execution, presentation, GPU time, mean / max). Without it the timing line comes every 600 frames |
| `GT3PC_GPU_SECTIONS=N` | GPU time by section, the means of every N frames (`pgl gpu sections`): the race view marks (`gfx_gpu_section`, T131 drawView) scene, effects (flare queries, particles, FXAA), copy (into the target), replay blur, weather (the drops), smoke, flare, post (colour correction, afterimage, frame effect); the rest of the frame is "other". Timestamp queries at the marks, read 4 frames later without waiting. `diag\gpu_sections.cmd` runs it |
| `GT3PC_FRAME_DUMP=dir` | writes the displayed buffer (native size, 24-bit BMP) as `dir\frame_NNNNN.bmp` |
| `GT3PC_FRAME_DUMP_EVERY=N` | every N-th displayed frame (default 60) |
| `GT3PC_FLICKER=1` (or N, 30 .. 600) | keeps the last 180 (N) pictures presented, each drawn into a 640 x 448 slot on the GPU without the overlays (none while F5 pauses), and the draws their frames executed (`pgl_flicker.cpp`: kind, count, PRIM, target, texture, its mean and view, hashes of the state, the pixel shader constants and the vertices, the first vertex's colour and texel coordinates, the GS pixels it can touch; skipped batches with the reason), with the frame's events (a vertex / index stream restarted, a texture decoded, a target made or re-made, a buffer that could not be mapped, and before every draw the D3D state checked against the binding cache: `d3d_check_bound`), and what the GPU did with each draw (a pipeline statistics query: vertices, primitives, pixel shader runs) and whether it held what was written (its constant buffers and vertex / index ranges copied back right after it, against the binding cache's constants and the streams' CPU shadows: `d3d_verify_copy`), read a few frames later. F8 reads them back and finds the pictures that differ from both neighbours where those agree (a channel off by more than 40): for the six with the most such pixels, the three pictures as `flicker_N_M_a/b/c.bmp` in `GT3PC_FLICKER_DIR` (default `flicker` next to the executable) and `pgl flicker` lines - where the pixels are, the draws of its frame that differ from both neighbours' at the same index where those agree, its draws into the displayed buffer that touch the pixels; `flicker_N.txt` lists every picture (frame, buffer, pixels changed, pixels off from both neighbours). `diag\flicker_capture.cmd` runs it |
| `GT3PC_DEPTH_LOG=1` | logs (first 600 lines, with the frame number) every change of a depth target's GS geometry (`pgl depth: frame N: Z buffer at block ... now holds a WxH ...`) and every Z texture read (`pgl depth: frame N: Z texture at block ... read as the WxH pixels of the Z buffer at block ...`): in a race the depth copy alternates blocks 0x0 / 0x8C0 and must read 640x448 from both |
| `GT3PC_AFTERIMAGE_LOG=1` | logs (first 400 lines, with the frame number) every afterimage capture (`pgl afterimage: frame N: the frame at block ... split into the planes at blocks 0x0 / 0x8C0 / 0x1180 ...`) and every PSMT8H / T4HL / T4HH texture with the owner of its block's top byte (`... T8H texture at block 0x0 (TBW 10, CSA 0): the top byte is a plane (R)`): after a capture drawAfterimage's three textures must read the planes R, G, B; and every target write that takes a plane's top bytes (`... the plane at block 0x0 loses its bytes to a write of the target at block ... (created as psm ..., GS WxH, FBW ...)`), the cause of a cyan / magenta / yellow afterimage (`diag\afterimage_log.cmd`) |
| `GT3PC_TEX1_LOG=1` | logs (first 4000 lines) every Tex1 set uploaded at a second VRAM base within one frame (`pgl tex1: frame N: set 0x... (B blocks) uploaded at block 0x..., earlier this frame at 0x...`): the allocator's evictions (course sets in a race) and the wheel faces' `TexSet::load` per wheel. Each such line was a set whose earlier copies the draws used to miss (section 6) |
| `GT3PC_READBACK_LOG=1` | logs (first 400 lines, with the frame number) every readback that did not also run in the frame before - the flare strip once per run of frames, each car icon at a race start once: `pgl readback: frame N: block 0x... 64x32 from the 64x32 target at 0x... (the block's owner): A of 2048 pixels with alpha, C with colour`. A good icon comes from a 64 x 32 target, with alpha on the car's outline only (A well below 2048, not 0) and colour on most of those pixels; `nothing owns the block`, a 128x128 target or A = 2048 with C = 0 is the old black box |
| `GT3PC_FLARE_LOG=1` | logs (first 600 lines, every frame) each readback with the R byte of its first 48 texels (`pgl flare: frame N: readback of block 0x... 64x8 (target 64x8, native 64x8), R: ...`: the flare queries' results in query order; the target's native size 64x8 once the strip is at GS resolution) and, at the frame's end, the texel each flare glow / result disc was drawn with, in draw order (`pgl flare: frame N: glows (draw order), texel R: ...`; 00 = not drawn). In a still scene the two lists stay the same from frame to frame; a glow that flickers shows as a texel that changes |
| `GT3PC_PGL_SKIP=kinds` | leaves out the draws of the listed kinds (comma separated), to find which pass makes a wrong picture: `local`, `envmtx`, `envmap`, `lights` (DIFFUSE / SPEC2 / SPEC4), `const` - VU1 model parts by lighting program; `ad` - draws blended with C = Ad; `alpha` - draws that write only the alpha; `self` - draws that sample their own target (the in-car heat-haze / rain drops, blurs). Logged once at the first draw (`pgl: GT3PC_PGL_SKIP=...`) |
| `GT3PC_PGL_TRACE=1` | F9 in the game window logs the draws of the next frame executed, one `pgl trace N:` line each: GS or VU1 (and the lighting program), FRAME (base, PSM, GS size, FBMSK), ZBUF, PRIM, ALPHA / FIX, alpha test, DATE / DATM, depth test, the texture (TBP, TBW, PSM, size, TFX, TCC, CBP, CSA, target or upload; for a decoded upload the mean of its texels, `mean R G B a A`: the palette the draw really got), then what else makes the colour: `vertex mean` (a VU1 part's mean vertex colour, modes with a colour stream), `spec` (the LOCAL programs' specular colour, L[4]), `fogcol` (FOGCOL, fogged draws) with the VU1 fog `f` / `start` (F = f + 0.1 + (255 - f) min(start, w) / w); draw states that were skipped, with the reason; the afterimage captures and colour corrections; each lens-flare query with the depth it saw (`light Z ...; N px in the shape, M passed; scene Z lo .. hi; A a, result r`). Each press traces two frames: the second, unless `GT3PC_PGL_PROBE` set points, probes the centres of the first frame's flare queries the shape passed in (up to four, the most pixels passing), so its `pgl probe` lines show which draws paint those pixels (`diag\flare_trace.cmd`) |
| `GT3PC_PGL_PROBE=x,y[;x,y...]` | with `GT3PC_PGL_TRACE=1`: up to four GS pixels of the draw buffer (the race screen is 640 x 448, 0,0 top left); in the frame F9 traces, each point's pixel is read back after every draw into a colour target holding it and logged when it changed: `pgl probe (x,y) of 0x1A60 640x448 after trace N: R G B a A (was ...)`, N the `pgl trace` line of the draw. The passes that make a wrong pixel's colour, in order, with what each left there. Each read waits for the GPU (the traced frame only: a second or so) |
| `GT3PC_D3D_THREAD=0` | no D3D thread (section 3): the executor makes its Direct3D calls itself, as before (the closures run at once). For comparing, or to rule the thread out |
| `GT3PC_EXEC_THREAD=0` | no executor thread (section 3): a frame's list executes at the kick on the game thread, as before. For comparing, or to rule the thread out |
| `GT3PC_PREWARM=0` | nothing decoded ahead (section 7, "Decoding ahead"): models and textures decoded at their first draw, as before. For comparing, or to rule the worker out |
| `GT3PC_SYNC_LOG=1` (or the launcher's Debug tab: Log D3D syncs) | logs who made the executor wait for the D3D thread (a use of the context outside the queue: `pgl sync: <function> < <caller> ...`, each call chain once, at most 64): the calls to queue next when the verbose monitor's D3D SYNCS stays above one or two a frame |
| (none: always on) | a crash logs the exception, the registers and the stack's return addresses (symbols from the PDB) and writes `gt3pc_crash.dmp` next to the program (`src/platform/crash.cpp`) |
| (none: always on) | closing the window logs every game thread (`sched: thread N, priority P (I at start): <state>; <entry>`: running, ready, sleeping, delayed, in a native call, or waiting for a semaphore with its count and waiters, "interrupts off" inside a DI section) and the return addresses on its stack (a scan: stale ones too). A softlock closed with the window leaves where each thread stood (`sched::log_threads`, `src/platform/scheduler.cpp`) |

## 11. Open issues

- **User report 2026-10-24, not reproduced:** in the arcade car select,
  the Lancer Evolution VII GSR in French Blue showed a magenta / purple
  body, pink wheels and a noisy red / green pattern in the headlights. Gone
  after backing out of the menu and entering it again, and after a
  restart. Read as one texture (the reflection's, most likely) decoded from
  wrong bytes and kept until its set was loaded again; the cache now
  follows the sets' bytes (section 6, "Changed in place"). Not confirmed:
  if it comes back, the `pgl tex1: ... changed in place` lines and a run
  with `GT3PC_PREWARM=0` / `GT3PC_EXEC_THREAD=0` tell more.
- The partial copy refresh (`target_written_rect`) missed some of a race
  frame's car draws (section 5, "Anti-aliasing"): the whole-picture reads
  refresh all of it now, but which draw's written rectangle falls short was
  not found (`vu1_area`, the GS batches' bounds and every target write were
  checked). The draws that read their own target still use the partial
  refresh; were it short there, a DATE / C = Ad pass would read stale
  alpha in that spot (no report of it).

- DATE reads the alpha from before its run of DATE draws (section 3): exact
  per pixel would need the alpha's bit 7 kept in a stencil (or rasterizer-
  ordered views). The cars' DATE env passes write alpha (FBMSK 0), so where
  one of their triangles overlaps an earlier one of the run the GS would
  block the second; within a convex, back-face-culled shell that is rare.
- A plane is found by the owner of its first block's top byte (as sets and
  surfaces, section 6): an upload that lands in that block after the capture
  takes the whole plane, where the PS2 lost only that block's bytes.
  Captures follow the frame's native pixels (the PS2's planes are 640 x 224
  bytes).
- Mipmaps (MXL > 0): the native mip chain and the GS LOD rule.
- Lines and points are one native pixel wide at scale s.
- A region-clamped texture inside a render target clamps at the target's
  edges, not at the region (the replay blur levels).
- C above 1.0 (As or FIX > 0x80) is exact only in Cs C [+ Cd] (section
  3); in the other forms (lerps, subtractions) the blend factor still stops
  at 1.0. The flare queries use Cs As only.
- tools/pglmath still expects 0x48 / 0x68 / 0x88 / 0xA8 as SrcC blends
  (testBlendGT3) and prints premul modes from a five-entry table: it needs
  `ColorPremul::TimesC` (section 3).
- The flare's `results` array (the game's copy of the strip) is no longer
  filled (but with `GT3PC_FLARE_LOG`): the glows read the strip on the GPU,
  and no game code reads the array.
- A Z buffer written through frames of two GS sizes holds one native
  picture: the last writer's geometry wins (exact for the game's flows: each
  depth copy reads a Z buffer the frame before it just wrote). Rows that
  only the other frame wrote on the GS (e.g. rows 224..447 after a 640 x
  224 draw) are not kept apart.
