# pgl on Direct3D 11: state, geometry and VU0 math

pgl is Polyphony's graphics library (`decomp/core_gt3enhanced/src/engine/pgl`,
canonical header `include/engine/pgl/G001_flare.h`): an OpenGL-like layer
over GS register shadows, VIF1 / GIF packets, VU0 macro mode and the VU1
microcode. [PORT.md](PORT.md) sets the plan: keep the state model (the
`GsContext` shadows the game and pgl set), drop the packets and the
hardware, draw with Direct3D 11. This file covers everything in pgl except
textures / render targets / frame-buffer effects and the VU1 models (other
notes): the frame flow, the state API, the GS state -> D3D11 mapping, the
coordinate / depth / fog / colour conventions, the immediate primitives, and
the VU0 math with the catalogue of the VU0 / MMI inline assembly in the game.

Code: `src/engine/pgl/native/`

| File | What |
|---|---|
| `pgl_math.h/.cpp` | `Vu0` (the VU0 register file and data memory, one instance `g_vu0`), the micro subroutines written out, the matrix library and matrix stacks, sin / cos / atan / random as the VU0 computes them, the game's own float library (`fm::`, fastmath.c), projection / clip helpers, the environment-map math; `ctx*` templates for the entry points that touch `GsContext` |
| `pgl_state_gs.h/.cpp` | GS register field decoders, `translate()`: shadows -> `DrawState` (blend, depth, alpha-test passes, destination alpha, write mask, scissor, offset, fog), state keys, reference GS / native blend evaluators |
| `pgl_state_prim.h/.cpp` | the GS vertices of the immediate primitives (sprites, sprite strips, lines, line strips, 3D lines, 2D fans), window / native coordinates, Z -> depth |
| `tools/pglmath` | tests (`pglmath`, `pglmath -v` error bounds, `pglmath -table` the blend table below) |

Plain C++17, no Windows or game dependency. The entry points that read or
write `GsContext` members are templates over the context type: the port
passes its `GsContext` (the member names of G001_flare.h), the test tool the
mirror `PglMathCtx`. No layout copy of `GsContext` is needed; the decomp
header itself does not compile under MSVC (GCC attributes, `EGCS_SIZE`), so
the port's own copy of it is what the templates see.

## 1. The VU0 model

On the PS2 the current matrix of `GsContext.matrixMode` lives in VU0
registers vf28..vf31 (rows, row-vector convention: `v' = v.x r0 + v.y r1 +
v.z r2 + v.w r3`, translation in row 3; 16 floats in memory are OpenGL's
column-major layout of the same matrix). The combined matrix (modelview x
projection) is in vf24..vf27, rebuilt by `pgl_sync_matrices` when
`matrixDirty` is set. The other modes' matrices are parked in VU0 data memory
(mode m at qword 4 + 4m) and the three matrix stacks too (vi11 = 0x10
modelview, 16 deep; vi12 = 0x50 projection, 4 deep; vi13 = 0x60 texture).
Game code reads and writes these registers directly (section 9), so the port
keeps them: `pgl::Vu0 g_vu0` holds vf0..vf31, ACC, Q, I, R, vi0..vi15, the
CLIP flags (vi18) and the 4 KB data memory. No instruction is interpreted:
the library and the rewritten game code compute on these fields in C++, in
the order the VU did (ACC chains left to right, same constants), so results
match the PS2 within float rounding (the VU and the EE FPU round toward zero
and have no denormals / Inf / NaN; SSE rounds to nearest: <= 1 ulp per
operation). Division by zero gives +-max float as on the VU (`vu::div`,
`vu::rsqrt`); `vu::ftoi` saturates.

One `g_vu0` is shared by every thread, like the one VU0 of the PS2 (the EE
kernel did not save COP2 on a thread switch): the game's thread order is what
keeps it consistent, and the port's scheduler keeps that order (PORT.md).

The VU0 microcode (0x00349600) has five micro subroutines (decoded by
`build/scratch/pgl/vu0dis.py`); `pgl_math.cpp` writes each out:

| vcallms | Computes | Port |
|---|---|---|
| 0x000 | vf28..31 = vf1..4 x vf28..31 (current = m x current: glMultMatrix, m applied first); vf1..3 = new rows 0..2 | `vu0Micro000` |
| 0x090 | vf28..31 = vf28..31 x vf1..4 (current = current x m) | `vu0Micro090` |
| 0x120 | vf24..27 = vf28..31 x qwords 8..11 (combined = modelview x parked projection) | `vu0Micro120` |
| 0x1D0 | vf1 = sin(vf1) per lane: `(((c3 x^3 + c5 x^5) + c7 x^7) + x) + c9 x^9`, c3 = -0.166666567 (0xBE2AAAA4), c5 = 0.0083330255 (0x3C08873E), c7 = -0.000198074136 (0xB94FB21F), c9 = 2.60188699e-6 (0x362E9C14); arguments within +-pi/2 | `vu0Micro1D0`, `vu0SinPoly` |
| 0x2C0 / 0x2D0 | vf4 = atan: t = (b - a) / (b + a) (0x2D0, a = vf1.x >= b = vf2.x >= 0; 0x2C0: t = (x - 1) / (x + 1)), `pi/4 + c1 t + c3 t^3 + ... + c15 t^15` (Sony's VU series, c1 = 0.999999344 ... c15 = -0.00405405788) | `vu0Micro2D0`, `vu0Micro2C0`, `vu0AtanPoly` |
| 0x240 | a reflection-vector routine (v - 2 (v.n) n, rsqrt): no caller in GT3 | not written |

Measured (`pglmath -v`): `vu0_sincos_deg` max error 1.2e-6 over -720..720
degrees, `vu0_atan2f` 2e-7.

## 2. The frame

### Packets on the PS2

- **Packet buffers** (T001): two 0xA0000-byte buffers; the render thread
  alternates them (`gfx_set_packet_buffer`). `GsContext.cur` writes forward,
  `end` moves backward for scratch (`gfx_alloc_tail`).
- **`gfx_begin_packet`**: at `cur` a `GsFrameHeader` {vif1Chain, gifChain},
  the reserved first tag of the GIF list, the VIF1 list start (BASE 0 /
  OFFSET 0x1B3: the VU1 double buffer; STCYCL 4/4, STMASK). Every register
  dirty, `matrixDirty = 1`, `vu1MatrixSent = 0`, `halfHeight = fieldShift =
  0`, statistics 0, `packetOpen = 1`.
- **Two lists**: list 0 is the VIF1 DMA chain (DIRECT blocks of GIF data for
  2D primitives and register writes, VIF UNPACK + MSCAL for VU1 work), list 1
  the GIF (PATH3) chain used for image uploads. `gfx_select_list` switches by
  ending one list with a reserved next-tag slot. Each list has its own emit
  mask: list 1's is 0, so register shadows are only ever sent on list 0.
  `gfx_put_irq_marker` puts SIGNAL / WAIT / READBACK markers that the VIF1 and
  DMAC handlers count (`g_gfx.pathSync16..18`) to order the two lists (an
  upload on list 1 then WAIT on list 0 before the draws that use it).
- **Register shadows**: setters write `GsContext.reg[]` / `gen[]` and set
  `dirty` bits; `gs_flush_dirty_regs` (called by every primitive path) emits
  the dirty & enabled shadows as A+D before the primitive. Context-1 bits 0..12,
  context-2 bits 16..28 (one shadow serves both GS contexts), general 32..41.
- **Sub-packets** (`gfx_begin_subpacket` / `gfx_close_subpacket` /
  `gfx_call_subpacket`, only the debug logger T087): a pair of DMA call
  targets recorded once and replayed with call tags; registers the
  sub-packet wrote are dirty again after it.
- **`gfx_end_packet`**: end tags on both lists, `packetOpen = 0`.
- **`gfx_kick_frame_lists`**: starts both DMA chains (VIF1 and GIF) of the
  frame header.
- **`gfx_dma_sync_frame`** waits for both chains; **`gfx_sync_pipeline`**
  for DMA, VIF1, VU1 and GIF to go idle (with forced resets after 0x1000000
  polls).
- **`gfx_vsync(n)`**: n > 0 waits until n-1 vblanks passed since the last
  call, then one more, returns the elapsed count; n < 0 returns 1 at once if
  -n passed, else waits for them and returns 0; n == 0 returns the counter.
  Then `GsContext.field` = the GS CSR.FIELD sampled at the vblank (0 when not
  interlaced).
- **Readbacks** (`gs_download_image`: GSBuffer::download, flare_readback):
  READBACK markers on both lists, a transfer header with the IRQ bit and a
  `GsReadbackRec`; the interrupt handlers do the download when the VIF1
  chain and the GS FINISH have both arrived, then call
  `readbackCallback(madr, arg)` and resume both lists.

### The render thread (T002_render_thread.cpp)

```
loop:
    mc_manager_tick; obj = requested object; pad update
    packet buffer = the other one (gfx_set_packet_buffer)
    if obj:
        obj->update()                       (game logic, physics: also VU0)
        gfx_begin_packet
        obj->render(frame buffer[cur])      (all pgl drawing: builds frame N)
        gfx_dma_sync_frame                  (waits for frame N-1's chains: VRAM it used can be freed)
        obj->virtual_3(); vram flush released
        gfx_end_packet
    priority 1; gfx_sync_pipeline           (frame N-1 completely drawn)
    gfx_vsync(-1)                           (at most one frame per vblank)
    frame->cur flips; frame->buf[cur].setDisplay()   (shows frame N-1's buffer: gfx_set_display_fb)
    if obj: gfx_kick_frame_lists(NULL)      (the GS starts on frame N)
    priority 7
```

The GS draws frame N while the CPU builds frame N+1: one frame of latency.
The DMA reads the packet's referenced data (vertex arrays, alloc_tail
buffers, model data) at kick time, not at call time; a few callers fill a
buffer after the call that references it (`vu1_unpack_xyz`:
RaceTirePacket::setDiscVertices).

### What the native renderer does at each point

| PS2 point | Native action |
|---|---|
| `gfx_init` | D3D11 device / swap chain (platform); `vu0Reset` + `ctxInitVu0Constants`; `gfx_context_init`'s C as is; vblank timer callback (59.94 Hz) for `g_gfx.vblankCount`, `field`, `vsyncCallback`; no microcode, no DMA set-up |
| `gfx_set_packet_buffer`, `gfx_alloc_tail`, `gfx_set_packet_access` | unchanged: the packet buffers stay plain memory (scratch for the game's vertex arrays) |
| `gfx_begin_packet` | start a frame command list; reset the per-frame bookkeeping (`dirty` / `written` / `enable` keep their C meaning but no longer gate anything) |
| a primitive call | append a command: the primitive (PRIM, vertices or pointers + count), a `DrawState` snapshot (`pgl::gs::translate(regsOf(ctx), prim)` plus the texture registers, target A / B and Z buffer) and, for VU1 work, a snapshot of vf24..vf27 and the viewport vectors. Pointers are kept, not copied: they are read at execution, as the DMA read them |
| `gfx_open_direct` / `gfx_alloc_direct` blocks the game fills itself (RaceSmoke::draw, weather particles, font, colour filters) | keep the block; decode its GIF tags (PACKED / REGLIST, A+D register writes, PRIM / RGBAQ / ST / UV / XYZF2 / XYZF3) at execution: a format decoder of the game's own data, not hardware emulation |
| `gfx_select_list`, `gfx_put_irq_marker`, sub-packet tags | record the list number for logs only: commands run in record order, which satisfies every SIGNAL / WAIT / READBACK ordering |
| `gfx_begin/close/call_subpacket` | record a command sub-list; a call replays it with each draw's state = the call-time shadows overridden by the registers the sub-packet itself wrote (what the PS2 did: registers it did not write come from whatever the GS had) |
| `gs_flush_dirty_regs` | nothing to send; state is read at draw time |
| `gfx_end_packet` | close the list |
| `gfx_dma_sync_frame`, `gfx_sync_pipeline`, `gfx_dma_sync` | return at once when the previous frame has executed (synchronous execution at kick); with an asynchronous executor: wait for it |
| `gfx_kick_frame_lists` | execute the frame's commands: draw everything, run readbacks (GPU -> CPU copy into `madr`, then `readbackCallback`) |
| `gfx_vsync` | wait on the platform vblank counter with the same n semantics; `ctx->field` from the native field toggle (or 0, roadmap pass 3) |
| `gfx_set_display_fb` / `GSBuffer::setDisplay` | present the native target registered at that VRAM address (it holds frame N-1, complete), scaled to the window at 4:3; `gfx_set_display_enable(0)`: present black |
| `gs_upload_image_sync`, `tex1_upload_to_vram` | immediate texture upload (texture notes) |
| `gfx_sync_pipeline` in `tex1.cpp` | wait for any pending execution before the upload |

Readback timing: on the PS2 a download completes while the CPU builds the
next frame; natively it completes at the kick, earlier, which only makes the
data ready sooner.

## 3. API

Counts are call sites in `src/game` and `src/engine` outside pgl (from
`build/scratch/pgl/calls.py`). "Shadow" names the `GsContext` fields /
register fields written; D = marked dirty in both contexts.

### Driver, display, timing

| Function | Calls | Semantics | Native |
|---|---|---|---|
| `gfx_init(full, inter, omode, ffmd)`, `gfx_init_default(omode)` | 1, 1 | one-shot init (game_init: interlaced NTSC, frame mode; hot start field mode); context init (COLCLAMP 1, PRMODECONT.AC 1, ZMSK 0, primAttr IIP, depth range 0..1, color 0xFFFFFFFF, colorTex 0x80808080, cullFace BACK, vu1ProgAlt 1, vu1Prog 0x33E, texture defaults TEXA.TA1 0x80 / TEX0.TCC / TEX1 bilinear, VU0 constants, light 0 and material GL defaults) | see section 2 |
| `gfx_change_display_mode`, `gfx_store_display_mode` | 1, - | inter / omode / ffmd, SMODE2 (field 1, frame 3, progressive 2), re-programs the CRTC | remember; pass 1 shows 4:3 progressive |
| `gfx_set_display_area`, `gfx_set_screen_size`, `gfx_set_display_fb`, `gfx_set_display_enable` | 2, 2, 1, 3 | DISPLAY2 / DISPFB2 / PMODE shadows (MAGH / MAGV from the screen size) | the displayed buffer and its rectangle; present (section 2) |
| `gfx_vsync(n)` | 149 | frame pacing (above) | platform vblank |
| `gfx_perf_counter0` | 8 | CPU cycles (per-vblank accumulator + COP0 PCR0): seeds cameras' random generators | a high-resolution counter |
| `gfx_dma_lock/unlock/sync/send`, `vu_upload_microcode`, the handlers | engine only | DMAC channels, semaphores, waiter lists | no DMA; sync returns 0 |
| `gfx_sync_pipeline`, `gfx_dma_sync_frame` | 2, 1 | wait for the paths / the frame's chains | section 2 |

### Packets and frame

| Function | Calls | Semantics | Native |
|---|---|---|---|
| `gfx_set_packet_buffer(buf, size)` | 3 | cur = buf, end = buf + size (segment bits) | keep |
| `gfx_set_packet_access(mode)` | 2 | cached / uncached / accelerated segment of the buffer pointers | store the mode |
| `gfx_begin_packet`, `gfx_end_packet`, `gfx_packet_is_open` | 3, 3, 3 | frame packet (section 2) | command list |
| `gfx_begin_subpacket`, `gfx_close_subpacket`, `gfx_call_subpacket` | 1, 1, 5 | recorded / replayed DMA call targets (debug logger) | command sub-list |
| `gfx_select_list(which)` | 24 | list 0 VIF1 / list 1 GIF; returns the previous; no switch to 1 while PATH3 is masked | record order |
| `gfx_put_irq_marker(code)` | 15 | SIGNAL / WAIT / READBACK | nothing |
| `gfx_kick_frame_lists(hdr)` | 3 | start both chains | execute |
| `gfx_alloc_tail(qwc)` | 82 | qwc qwords from the end of the packet buffer (the game's vertex / colour / UV arrays for `pgl_draw_strip`, `strip_draw` ...) | unchanged; dereferenced at execution |
| `gfx_open_direct(qwc)` | 2 | a DIRECT block the caller fills with GIF data | decoded at execution |
| `gs_flush_dirty_regs` | 6 | emit dirty shadows (no packet: forget them) | nothing |
| `gs_prim_attr_with(bits)` | 1 | primAttr \| bits (RaceSmoke's PRIM) | as is |

### Switches: `gfx_enable` (148) / `gfx_disable` (262)

| Capability | Shadow | Native |
|---|---|---|
| `PGL_CAP_PATH3` 1 | path3Masked (MSKPATH3 in a packet) | nothing |
| `PGL_CAP_TEXTURE` 2 | primAttr TME | PRIM.TME of the immediate paths that take primAttr's TME (strip batches, models' override) |
| `PGL_CAP_DEST_ALPHA` 3 | TEST.DATE, D | DATE (section 4) |
| `PGL_CAP_ALPHA_TEST` 4 | TEST.ATE, D | alpha test passes |
| `PGL_CAP_DEPTH_TEST` 5 | on: TEST.ZTST = zTestMethod, ZBUF.ZMSK = zMask, ZTE; off: ZTST ALWAYS, ZMSK 1 (ZTE stays 1); depthTest; D | depth state |
| `PGL_CAP_BLEND` 6 | primAttr ABE | blend enable |
| `PGL_CAP_DITHER` 7 | DTHE | ignored (never enabled by GT3) |
| `PGL_CAP_FOG` 8 | primAttr FGE | fog |
| `PGL_CAP_ANTIALIAS` 9 | primAttr AA1 | ignored (flag); MSAA later |
| `PGL_CAP_TEXGEN` 0xA, `_COLORGEN` 0xB | texGenEnable / colorGenEnable (pgl_draw_strip's callbacks) | as is (CPU) |
| `PGL_CAP_COLOR_CLAMP` 0xC | COLCLAMP | UNORM saturation (only ever enabled) |
| `PGL_CAP_COLOR_MATERIAL` 0xD | colorMaterial | as is (CPU lighting) |
| `PGL_CAP_CULL_FACE` 0xF, `_VU1_PROG_33E` 0x11 | cullEnable / vu1ProgAlt, then `gs_select_vu1_prog` | cull mode of VU1 draws (below) |
| `PGL_CAP_SCALE` 0x12 | scaleEnable, matrixDirty | combined matrix scale |
| `PGL_CAP_TEX_MATRIX` 0x13, `_NORMAL_MATRIX` 0x14 | texMatrixEnable / normalMtxEnable | `xformTexcoords`; env matrix (VU1 notes) |
| `PGL_CAP_FLAG24` 0xE, `_FLAG27` 0x10 | flag bytes no pgl code reads | as is |
| `PGL_CAP_CPU_LIGHTING` 0x64, `PGL_CAP_LIGHT0 + i` | lightFlags 0x80; lights[i].enable, lightFlags 1 | as is (lighting state) |

### Depth

| Function | Calls | Semantics | Native |
|---|---|---|---|
| `gfx_set_depth_buffer(addr, psm)` | 1 (GSBuffer::setZBuffer) | ZBUF ZBP / PSM, D; depthScale = the format's max Z (Z32 4294836224, Z24 16776191, Z16 65531); spriteZF = (int)depthScale \| 0xFF000000 (2D primitives nearest, no fog); `gs_update_depth_range` | the Z target; depth = Z / 2^bits |
| `gfx_depth_func(f)` | 9 | zTestMethod = {NEVER, ALWAYS, GREATER, GEQUAL}[f] for GL NEVER / ALWAYS / LESS / LEQUAL (GS Z grows toward the viewer); TEST.ZTST while depth testing, D | `Compare` |
| `gfx_depth_mask(write)` | 46 | zMask = !write; ZBUF.ZMSK while depth testing, D | depth write |
| `gfx_set_depth_offset(o)`, `gs_set_depth_bias_at(dz, x, y, z)` | 2, 1 | depthOffset / depthBias, then the viewport z: viewScale.z = (near - far) depthScale / 2, viewCenter.z = ((1 - (near + far) / 2) + depthBias) depthScale + depthOffset + 1 | `ctxSetDepthBiasAt` (the bias the binary computes is ~0: the w lanes after the divide) |
| `gs_set_clear_depth(d)` | 6 | clearDepth = clamp(1 - d, 0, 1) | clear value clearDepth x depthScale -> depth |

### Alpha test, blend, masks

| Function | Calls | Semantics | Native |
|---|---|---|---|
| `gfx_alpha_func(atst, ref)` | 5 | TEST.ATST, AREF = (int)clamp(ref x 128, 0, 255) (ref 1.0 = 0x80), D | alpha test |
| `gfx_alpha_func_raw(atst, ref)` | 9 | ATST, AREF = ref | alpha test |
| `gfx_alpha_fail(afail)` | 6 | TEST.AFAIL (KEEP / FB_ONLY / ZB_ONLY / RGB_ONLY), D | passes |
| `gfx_dest_alpha_func(mode, ref)` | 3 | TEST.DATM = 1 for GEQUAL, 0 for LESS (ref unread), D | stencil test |
| `gfx_blend_func(abcd, fix)` | 83 | ALPHA low byte = abcd, FIX = (int)clamp(fix x 128, 0, 255), D | blend |
| `gfx_blend_func_raw(abcd, fix)` | 1 | the same, raw FIX (model sets) | blend |
| `gfx_color_mask(r, g, b, a)` | 11 | frame write mask of whole bytes | write mask |
| `gfx_set_frame_write_mask(mask)` | 38 | FRAME.FBMSK = ~mask (1 bits written), D | write mask |

### Draw buffer, window, offset, field

| Function | Calls | Semantics | Native |
|---|---|---|---|
| `gfx_set_draw_buffer(addr, psm, w, h)` | 1 (+ GSBuffer::setDrawBuffer) | tgtA = {base, fbw = (w + 63) / 64, psm, w, h, xoff = (0x800 - w/2) << 4, yoff = (0x800 - h/2) << 4, xend, yend}; FRAME FBP / FBW / PSM, XYOFFSET = (xoff, yoff), D; halfHeight = 0; draw area = (0, 0, w, h) | render target A |
| `gfx_set_target_b(addr, psm, w, h)` | 1 | tgtB only (effects, clears bit 2) | target B |
| `gfx_set_draw_area(x, y, w, h)` | 3 | winX..winH (y, h halved when halfHeight); SCISSOR = the window, D; viewScale.xy = (w/2, -h/2), viewCenter.xy = ((xoff + 16x + 8w) / 16, (yoff + 16y + 8h) / 16); vu1MatrixSent = 0 | scissor + VU1 viewport |
| `gfx_set_scissor(x, y, w, h)` | 3 | SCISSOR = (x, x + w - 1, y, y + h - 1), y / h halved with halfHeight, D; viewport untouched | scissor |
| `gfx_offset_xy(dx, dy)` | 11 | XYOFFSET += (dx x 16, dy x (halfHeight ? 8 : 16)), D: sub-pixel jitter of multi-pass antialiasing (meters, logger, car render) | window offset |
| `gfx_set_field_mode(mode)` | 7 | OFY = yoff + 0 / field x 8 / (1 - field) x 8 / 8, halfHeight, fieldShift (PGL_FIELD_*), D | section 5 |
| `gs_set_scanmask_mode(mode)` | 8 | SCANMSK.MSK: off / even / odd / the field's / the other field's rows not drawn | discard by GS row parity |
| `gfx_get_draw_buffer`, `gfx_get_draw_area`, `gs_get_field_mode`, `gs_get_scanmask_state`, `gfx_get_cull_face` | 1, 1, 9, 1, 2 | getters (draw area y / h doubled back) | as is |

### Fog

| Function | Calls | Semantics | Native |
|---|---|---|---|
| `gfx_set_fog_params(start, amount)` | 3 | fogStart / fogAmount; limits.w = max(start, 0), f = clamp(amount x 255, 0, 255): viewScale.w = 255 - f, viewCenter.w = f + 0.1 | per-vertex F (section 5) |
| `gfx_set_fog_color(rgb)`, `gfx_set_fog_color_f(r, g, b)` | 2, 1 | FOGCOL (bytes, or clamp(x, 0, 1) x 255), D; return the previous | fog colour |
| `pgl_get_fog_constants(out)` | 1 | {viewCenter.w, viewScale.w, limits.w} for RaceSmoke's CPU fog | as is |
| `gs_get_fog_color` | 2 | FOGCOL as r \| g << 8 \| b << 16 | as is |

### Clears and frame effects

| Function | Calls | Semantics | Native |
|---|---|---|---|
| `gs_set_clear_color(rgba)`, `gs_set_clear_color_f(r, g, b, a)` | 21, 4 | clearColor (floats: r, g, b x 255, a x 128, +0.5, clamped) | clear colour |
| `gs_clear_buffers(mask)` | 39 | a sprite over target A in 64-pixel strips with RGBAQ clearColor, Z = clearDepth x depthScale, TEST: colour only 0 (no Z access), colour + depth ZTE + ALWAYS, depth only ATE NEVER + AFAIL ZB_ONLY; ZBUF with ZMSK = zMask (so no depth clear while depth writes are off); the current SCISSOR and FBMSK apply; bit 2: target B, with the scissor rescaled, colour clearColorB, no Z | a draw (or ClearView when scissor = target and mask = all) honouring zMask, scissor, write mask |
| `gfx_frame_effect(kind, amount)` | 7 | full-screen copies between tgtA and tgtB in 64-pixel strips, point sampling, +0.375 px sample offset (PC: +0.5, renderer.md section 4); GT3 uses MIX_A_INTO_B (B = lerp(B, A, amount): ALPHA 0x64) and COPY_B_TO_A (A = B x amount: 0xA8, at the integer XYOFFSET) | effects notes |

### Texture state (the state side; the texture notes resolve TBP / CBP)

| Function | Calls | Semantics | Native |
|---|---|---|---|
| `gfx_set_texture_regs(t)` | 19 | CLAMP, TEX0 (CLD stripped), TEX1 = the block's (fields under the override block kept from the context); CLD != 0: TEX2 = TEX0 with PSMT8 / CSA 0 (the CLUT load); mipmaps: TEX1.K += lodBias << L, MIPTBP1 (/ 2 from MXL 4); D | texture binding + sampler |
| `gfx_tex_param(pname, v)` | 114 | one field: TCC, TFX (TEX0), MMIN, MMAG, LCM, MXL, K = v >> 4, L (TEX1), WMS, WMT (CLAMP), lodBias, TA0, TA1, AEM (TEXA), D | sampler / shader key |
| `gfx_set_texture_buffer(level, addr, psm, w)` | 5 | TEX0 TBP0 / TBW / PSM (+ TCC fix-up: a CT24 CLUT on an indexed format clears TCC) or MIPTBP1 / 2 level 1..6, D | texture binding |
| `gfx_set_texture_size(w, h)` | 3 | TEX0.TW / TH = log2 rounded up (1..10), D | texture size |
| `gfx_set_texture_function(tfx)` | 5 | TEX0.TFX, D | shader key |
| `gfx_set_texture_region(axis, min, max)` | 16 | CLAMP MINU / MAXU or MINV / MAXV (region clamp / repeat), D | sampler / shader |
| `gfx_texflush` | 18 | TEXFLUSH dirty (the texture cache flushed at the next flush) | the texture cache's invalidation point after uploads |
| `gfx_set_clut_mode(mode)`, `gs_set_clut(cbp, cpsm)`, `gs_set_clut_csa(csa)` | 1, 1, 1 | TEX2 PSM / CSA; TEX0 / TEX2 CBP / CPSM / CSM / CSA / CLD (+ TCC fix-up) | palette binding |
| `gs_tex_override_enable / _disable(what)` | 1, 1 | the override block: which TEX0 / TEX1 / CLAMP fields and PRIM bits the models take from the context | as is (state) |
| `texregs_init / _set_buffer / _set_size / _set_mode / _set_clamp_region`, `tex_vram_size` | 2, 2, 3, 4, 4, engine | build a GsTexRegs block in memory (no context) | plain C as is |

The decoders `gs::decodeTex0 / Tex1 / Clamp / Texa` give the fields.

### Colour

| Function | Calls | Semantics | Native |
|---|---|---|---|
| `pgl_color(rgba)` | 61 | color = rgba; colorTex = each byte x 129 >> 8 (0xFF -> 0x80), alpha kept; with colorMaterial gs_set_material_param(faces, kind, alpha byte only) | vertex colour of untextured / textured primitives |
| `pgl_color4f`, `pgl_color3f`, `pgl_color4fv` | 26, 14, 3 | `pack_rgba_floats_to_u32(r, g, b, a, 255)`: r, g, b x 255, a x 128, max(0) + 0.5, min 255, truncated | as is |

### Matrices (all in `pgl_math`)

| Function | Calls | Semantics | Port |
|---|---|---|---|
| `pgl_matrix_mode(m)` | 93 | park vf28..31 of the old mode (qword 4 + 4m), load the new one's; modes > 2 neither | `ctxMatrixMode` |
| `pgl_push_matrix`, `pgl_pop_matrix` | 125, 133 | vsqi / vlqd on the mode's stack (vi11..13); pop sets matrixDirty; no bound checks (compiled out in the original) | `ctxPushMatrix`, `ctxPopMatrix` |
| `vu0_load_identity_regs` | 78 | vf28..31 = I | `ctxLoadIdentity` |
| `vu0_reset_rotation`, `vu0_reset_translation` | engine, 4 | xyz of rows 0..2 = I (w kept); vf31.xyz = 0 | `ctxResetRotation`, `ctxResetTranslation` |
| `pgl_load_matrix`, `pgl_get_matrix` | 4, 27 | vf28..31 = m / m = vf28..31 | `ctxLoadMatrix`, `vu0GetMatrix` |
| `pgl_mult_matrix(m)` | 22 | current = m x current (glMultMatrix) | `ctxMultMatrix` |
| `pgl_mult_matrix_90(m)` | 2 | current = current x m | `ctxMultMatrix90` |
| `pgl_rotate(deg, x, y, z)` | 1 | glRotate: rows c I + (1 - c) a a^T + s [a]x (transposed), normalised axis, then 0x000 | `ctxRotate` |
| `pgl_translate(x, y, z)` | 77 | vf31 = x r0 + y r1 + z r2 + vf31 | `ctxTranslate` |
| `vu0_scale(x, y, z)`, `F_00257EF0` / `vu0_scale_inv_y(s)` | 38, 4 | rows 0..2 x (x, y, z); (1, 1/s, 1) | `ctxScale`, `ctxScaleInvY` |
| `vu0_rotate_x/y/z(deg)` | 33, 52, 39 | M = R M (row1' = c row1 + s row2 ...) | `ctxRotateX/Y/Z` |
| `vu0_transpose`, `vu0_invert_rigid` | 1, 11 | transpose; rigid inverse (3x3 transposed, t' = -t R^T, w of rows 0..2 -> 0) | `ctxTranspose`, `ctxInvertRigid` |
| `vu0_apply_matrix(dst, src)` | 21 | dst = src x current (w included); vf1 = dst | `vu0ApplyMatrix` |
| `pgl_frustum(l, r, b, t, n, f, focal)`, `pgl_ortho`, `glu_look_at`, `mat_reflect`, `proj_shift_depth` | 17, 12, 2, 1, 3 | GL matrices (the 2n terms of the frustum use `focal`), multiplied in; plain C over the routines above | the decomp C as is |
| `pgl_set_scale(x, y, z)` | 2 | scale.xyz, matrixDirty (used with PGL_CAP_SCALE: rows 0..2 of the combined matrix scaled) | `ctxSetScale` |
| `pgl_set_normal_matrix(m)` | 2 | normalMtx = m (the retail copy garbles unaligned matrices; callers pass aligned ones) | `ctxSetNormalMatrix` |
| `pgl_sync_matrices` (via the primitives, `vu0_sync_state`, `vu0_load_screen_regs`) | 2, 5 | vf24..27 = modelview x projection (0x120 in modelview mode; the parked copies in the others), scale; matrixDirty = 0, vu1MatrixSent = 0, lightFlags \|= 2 | `ctxSyncMatrices`, `ctxSyncState`, `ctxLoadScreenRegs` |
| `vu0_load_screen_regs[_nosync]`, `vu0_load_modelview`, `vu0_load_projection` | 5, 1, 1, 1 | vf20..23 = vBB0, limits, viewCenter, viewScale / the modelview / the projection | `ctxLoadScreenRegs[NoSync]`, `ctxLoadModelview`, `ctxLoadProjection` |
| `box_visible(lo, hi)` | 2 | 8 corners through vf24..27, AND of the clip flags | `boxVisible` (after `ctxLoadScreenRegs`) |

### sin / cos / atan / random

| Function | Calls | Port |
|---|---|---|
| `pgl_sincos_deg`, `pgl_cos_deg`, `vu0_sincos_deg` | 7, 1, engine | `pglSinCosDeg`, `pglCosDeg`, `vu0SinCosDeg` (vf1 = (sin, cos)) |
| `vu0_atan2f(y, x)` | 3 | `vu0Atan2` |
| `vu0_rand`, `vu0_rand_seed` | 25, 1 | `vu0Rand`, `vu0RandSeed` (the LCG half of vu0_rand_seed and `lcg_rand*` stay C) |
| `pseudo_angle` | engine | `pseudoAngle` |
| fastmath `sinf cosf sincosf tanf atanf atan2f expf logf powf` (engine/pdistd) | 38, 36, 10, 15, -, 15, 4, 4, 1 | `pgl::fm::*` (the port routes the game's calls there: same polynomials, same quirks) |

### Lights, materials, VU1 parameters (state; the VU1 notes consume it)

| Function | Calls | Semantics |
|---|---|---|
| `pgl_lightfv(light, pname, params)` | 36 | vectors to `gs_set_light_param_v` (POSITION normalised when w == 0, SPOT_DIRECTION always, both times the modelview: `lightVectorToEye`), scalars to `gs_set_light_param_f` (spot exponent clamped 0..128 + encoding, cutoff + `pgl_cos_deg`, attenuations) |
| `pgl_light_modelfv(pname, params)` | 7 | globalAmbient / local viewer (lightFlags 0x40) / eyeWorld |
| `pgl_set_const_color`, `pgl_set_const_vector` | 6, 6 | lighting mode 5: constColor = (rgb x 255, a x 128), constVector |
| `pgl_set_scalar_param`, `pgl_set_vec_param` | 8, 2 | qAF0 (kind 0, fades models), qB00 (2), qB10 (3) |
| `material_load`, `mat_scale_set_grey`, `gs_tex_override_enable/disable` | 2, 3, 1 | material 0 (scaled by matScale when enabled); the texture / PRIM override block |
| `pgl_set_morph_weight`, `pgl_set_part_tex_func` | 1, 3 | morphWeight; partTexFunc |
| `pgl_envmap_set_basis`, `pgl_envmap_point_at`, `pgl_set_envmap_params` | 1, 1, 1 | lighting mode 3: `envmapSetBasis`, `envmapPointAt`, `EnvMap::paramA/B`; `envmapBuildMatrix` builds the matrix pgl_send_mode3 sends |

### Primitives

| Function | Calls | GS output | Port |
|---|---|---|---|
| `gs_draw_flat_sprite(x0, y0, x1, y1)` | 5 | SPRITE, TME off, colour `color`, XY = offset + 16 x (no window origin!), Z / F = spriteZF | `prim::flatSprite` |
| `gs_draw_flat_sprite_strips` | 1 | the same in 64-pixel strips | `prim::flatSpriteStrips` |
| `gs_draw_textured_sprite[_f]` | 2, 12 | SPRITE \| TME \| FST, colour `colorTex`, XY = offset + 16 (x + winX), UV = 16 u + 8 (half texel) | `prim::texturedSprite[F]` |
| `gs_draw_textured_sprite_strips_f` | 19 | 32-pixel strips (texture cache), u interpolated by integer division | `prim::texturedSpriteStripsF` |
| `gs_draw_line[_f]` | 4, 1 | LINE, TME off, `color`, window origin added | `prim::line[F]` |
| `pgl_draw_line_strip_2d(xy, n)` | 2 | LINE_STRIP through vf24..27 (z = 0), clip codes: a segment outside one plane gets ADC (not drawn) | `prim::lineStrip2d` |
| `pgl_draw_line_3d(p0, p1)`, `pgl_draw_line_3f` | 2, 8 | LINE through vf24..27, homogeneous clip, divide, viewport, fog F | `prim::line3d` |
| `draw_poly2d(xy, uv, rgba, n)` | 1 | TRIANGLE_FAN \| IIP \| FST (\| TME), first two vertices XYZF3 | `prim::polyFan2d` |
| `pgl_set_sprite_depth(z)`, `gs_set_sprite_zf(z, fog)` | 5, 2 | spriteZF from an eye distance (projection, viewport, F = 0xFF when z <= 0) / raw (Z clamped 0..0xFFFFFF, fog > 255 -> 255) | `ctxSetSpriteDepth` / as is |
| `pgl_draw_strip(xyz, nrm, rgba, uv, n)` | 55 | VU1 primitive program, TRI_STRIP (section 6) | VU1 replacement |
| `strip_batch_begin/add/end`, `strip_draw`, `tri_draw16` | 4, 1, 6, 4, 1 | VU1 strips / 16-bit triangles (section 6) | VU1 replacement |
| `model_draw(m)` | 13 | prebuilt VU1 models | VU1 notes |

## 4. GS state -> Direct3D 11

`pgl::gs::translate(regs, prim)` produces a `DrawState`; the backend hashes
`packBlendKey`, `packDepthKey` and `packShaderKey` into blend / depth-stencil
state objects and pixel-shader permutations.

### Render-target conventions

- Colour targets UNORM8; a GS byte b is stored as b / 255 in every channel,
  alpha included: the frame alpha is data (readbacks, DATE, colour filters
  write R / G / B bytes into alpha planes, frame effects use A's alpha). A
  PSMCT24 frame stores no alpha (write mask RGB). COLCLAMP = 1 is UNORM
  saturation; GT3 never turns it off.
- The pixel shader computes the GS source colour Cs and alpha As (0..255,
  after the texture function and fog) and writes o0 = (Cs, As) / 255 and, for
  blending, o1.a = C / 128 (dual-source blending, SRC1_ALPHA; C = As or FIX,
  saturated at 1.0 = 0x80). FIX may instead go through the blend factor
  constant (BLEND_FACTOR).
- The alpha channel never blends (the GS writes As): alpha blend ONE / ZERO /
  ADD; FBA = 1 ORs 0x80 into As (32-bit frames; GT3 never sets it).

### ALPHA: (A - B) x C / 128 + D

A, B, D select Cs, Cd or 0, C selects As, Ad or FIX (0x80 = 1.0); the result
is clamped to 0..255. `translateBlend` writes each colour's coefficient as
k C + one (Cs: [A = Cs] - [B = Cs], [D = Cs]; Cd likewise), picks the D3D
operation from the signs (a lone -C term makes it SUBTRACT / REV_SUBTRACT)
and the factors ZERO / ONE / C / 1 - C; where a coefficient is 1 + C it moves
the factor into the shader output (o0 = Cs (1 + C), or o0 = C with DEST_COLOR
for Cd (1 + C)). C = Ad (`AdMode`, pgl_state_gs.h): DEST_ALPHA holds Ad /
255 where the GS uses Ad / 128, and a UNORM target clamps o0 to 1 before
blending, so no single fixed-function pass gives it. Cs Ad + Cd and Cs Ad
(0x58, 0x98) are drawn twice with o0 = Cs and src DEST_ALPHA - Ad / 255
each, Ad / 128 together -, the first pass without the alpha and Z writes
(Ad and the depth test stay as they were) and with the form's dst, the
second with dst ONE: within two units (each pass rounds where the GS
truncates once). Every other C = Ad form is computed in the shader with
blending off: Cd and Ad are read from a copy of the target (t1, the DATE
copy, refreshed before each such draw) and the result is the GS's
`((A - B) Ad >> 7) + D`, clamped - exact; only a draw's own overlapping
primitives read the target as it was before the draw. A PSMCT24 frame has
no alpha: the GS takes Ad as 1.0 there (it skips the multiplication), so C
= Ad into a 24-bit frame is translated as FIX 0x80. The table (`pglmath
-table`, sel = the ALPHA byte with C = As; + 0x10 for Ad, + 0x20 for FIX;
"C" = the dual-source / constant factor, "Ad'" = DEST_ALPHA):

| sel | GS colour | C = As / FIX | C = Ad |
|---|---|---|---|
| 00 | (Cs - Cs) C + Cs | off (Cs) | off (Cs) |
| 40 | (Cs - Cs) C + Cd | ZERO, ONE | ZERO, ONE |
| 80 | (Cs - Cs) C + 0 | ZERO, ZERO | ZERO, ZERO |
| 04 | (Cs - Cd) C + Cs | o0 = Cs(1+C); ONE, C, src-dst **inexact** | shader (Cd, Ad from a copy) |
| 44 | (Cs - Cd) C + Cd | C, 1-C | shader (Cd, Ad from a copy) |
| 84 | (Cs - Cd) C + 0 | C, C, src-dst | shader (Cd, Ad from a copy) |
| 08 | (Cs - 0) C + Cs | o0 = Cs(1+C); ONE, ZERO | shader (Cd, Ad from a copy) |
| 48 | (Cs - 0) C + Cd | C, ONE | 2 passes: Ad', ONE; Ad', ONE |
| 88 | (Cs - 0) C + 0 | C, ZERO | 2 passes: Ad', ZERO; Ad', ONE |
| 01 | (Cd - Cs) C + Cs | 1-C, C | shader (Cd, Ad from a copy) |
| 41 | (Cd - Cs) C + Cd | C, ONE, dst-src **inexact** | shader (Cd, Ad from a copy) |
| 81 | (Cd - Cs) C + 0 | C, C, dst-src | shader (Cd, Ad from a copy) |
| 05 | (Cd - Cd) C + Cs | off (Cs) | off (Cs) |
| 45 | (Cd - Cd) C + Cd | ZERO, ONE | ZERO, ONE |
| 85 | (Cd - Cd) C + 0 | ZERO, ZERO | ZERO, ZERO |
| 09 | (Cd - 0) C + Cs | ONE, C | shader (Cd, Ad from a copy) |
| 49 | (Cd - 0) C + Cd | o0 = C; Cd, ONE | shader (Cd, Ad from a copy) |
| 89 | (Cd - 0) C + 0 | ZERO, C | shader (Cd, Ad from a copy) |
| 02 | (0 - Cs) C + Cs | 1-C, ZERO | shader (Cd, Ad from a copy) |
| 42 | (0 - Cs) C + Cd | C, ONE, dst-src | shader (Cd, Ad from a copy) |
| 82 | (0 - Cs) C + 0 | ZERO, ZERO | shader (Cd, Ad from a copy) |
| 06 | (0 - Cd) C + Cs | ONE, C, src-dst | shader (Cd, Ad from a copy) |
| 46 | (0 - Cd) C + Cd | ZERO, 1-C | shader (Cd, Ad from a copy) |
| 86 | (0 - Cd) C + 0 | ZERO, ZERO | shader (Cd, Ad from a copy) |
| 0A | (0 - 0) C + Cs | off (Cs) | off (Cs) |
| 4A | (0 - 0) C + Cd | ZERO, ONE | ZERO, ONE |
| 8A | (0 - 0) C + 0 | ZERO, ZERO | ZERO, ZERO |

"Exact" means: for every Cs, Cd and C <= 0x80 the native result is the GS
result within one unit (the GS truncates `(A - B) C >> 7`, the GPU rounds);
`pglmath` checks every exact form on a grid (the two-pass C = Ad forms
within two units). Of the 81 non-reserved forms 77 are exact (25 of 27
with As, 25 of 27 with FIX, all 27 with Ad). C above
0x80 (As or FIX up to 0xFF = 1.99) saturates at 1.0 (FIX > 0x80 is flagged;
As is per pixel). Selector 3 is reserved (taken as 0 / FIX, flagged). For the
inexact forms the description gives the closest fixed-function form and sets
`INEXACT_BLEND_*`; a backend that needs them exactly draws with a copy of
the target bound as a texture (read Cd / Ad in the shader).

What GT3 blends with (every `gfx_blend_func` / `_raw` site and the driver's
own packets):

| ALPHA | Formula | Where | Native |
|---|---|---|---|
| 0x44 | (Cs - Cd) As + Cd | menus, HUD, smoke, logger, model sets, fonts | C, 1-C |
| 0x48 | Cs As + Cd | menus, smoke, memory card, meters | C, ONE |
| 0x42 | Cd - Cs As | menus (MENU_BLEND_SUB) | C, ONE, dst-src |
| 0x84 | (Cs - Cd) As | menus (MENU_BLEND_NO_DEST) | C, C, src-dst |
| 0x88 | Cs As | PGL_BLEND_SRC_ALPHA | C, ZERO |
| 0x64 | (Cs - Cd) FIX + Cd | fades, meters, GSBuffer::blitField (0.5), frame effect MIX | C, 1-C (FIX) |
| 0x68 | Cs FIX + Cd | glows, smoke compose, meters, colour filters (FIX 0x80), frame effect ADD | C, ONE |
| 0xA4 | (Cs - Cd) FIX | replay blur (BLEND_SUBTRACT) | C, C, src-dst |
| 0x62 | Cd - Cs FIX | frame effects ADD_A_TO_B / ADD_WHITE with a negative amount | C, ONE, dst-src |
| 0xA8 | Cs FIX | frame effects COPY_A_TO_B / COPY_B_TO_A | C, ZERO |
| 0xA9 | Cd FIX | frame effect SCALE_B | ZERO, C |
| 0x58 | Cs Ad + Cd | TirePacket (PGL_BLEND_ADD_DEST_A); model-set command 0x20: the course's LOCAL glint and ENVMTX passes, the cars' and wheels' env passes; `reflBlend` of route5_dry / route11 (and r_) | two passes: Cs x DEST_ALPHA, + ONE (Ad / 128 together). Was o0 = Cs 255/128 x DEST_ALPHA: right while Cs <= 0x80, too dark above |
| 0x54 | (Cs - Cd) Ad + Cd | `CourseEnv::reflBlend` of the US courses with road reflections (model24): route5 / r_route5 (wet), L_circle60_dry1 / _wet1, L_circle100_wet1 (most other courses name 0x54 too but have no model24) | in the shader from the target's copy. Was DEST_ALPHA / INV_DEST_ALPHA: Ad / 255, i.e. half the GS's weight at Ad = 0x80 (the reflections at half strength) |
| data | `CourseEnv::reflBlend` (course files), model-set command `gfx_blend_func_raw` (model files) | reflections, models | any: `DrawState.inexact` tells; log it |

FIX values above 0x80: `gfx_blend_func(MENU_BLEND_FIX, f[0])` and the other
variable FIX arguments are 0..1 in practice (alpha fades).

**PABE** (blend only where As bit 7 is set) and **DTHE** (dither) are never
enabled by GT3 (flagged if seen). **AA1** (coverage alpha of lines /
triangles): 4 enables (antialiased lines and edges); ignored in pass 1
(flag), MSAA / line AA later.

### TEST

- **Alpha test** (ATE, ATST, AREF): in the shader, As (after the texture
  function) compared with AREF (0..255): NEVER, ALWAYS, LESS, LEQUAL, EQUAL,
  GEQUAL, GREATER, NOTEQUAL.
- **AFAIL**: what a failing pixel writes. `translate` splits the draw into
  passes (`DrawState.pass[]`, the depth test applies to both):

  | Case | Passes |
  |---|---|
  | no test / ALWAYS | 1: everything |
  | NEVER | 1: the AFAIL writes only (KEEP: 0 passes) - gs_clear_buffers' depth-only clear (ATST NEVER, ZB_ONLY) |
  | KEEP | 1: discard failing pixels |
  | FB_ONLY | 2: passing pixels colour + Z; failing pixels colour only, no Z write (1 pass without test when Z writes are off anyway) |
  | ZB_ONLY | 2: passing pixels colour + Z; failing pixels Z only |
  | RGB_ONLY | 2: passing pixels everything; failing pixels RGB only |

  GT3 uses KEEP (model sets, menus, `gfx_alpha_func` / `_raw` 14 sites:
  GREATER 0x20 / 0x40 / 0x7F, GEQUAL 0.5, EQUAL, NOTEQUAL, LESS) and FB_ONLY
  (model sets, sprite models, race tyre model). Two passes draw all passing
  pixels before all failing ones; where primitives of one draw overlap with
  blending the order differs from the GS's per-primitive order (rare).
- **DATE / DATM**: only pixels whose destination alpha bit 7 equals DATM are
  drawn. D3D11 cannot read the bound target: keep a stencil bit = "alpha >=
  0x80" for targets that use DATE, refreshed by a full-screen pass (discard
  where alpha < 0x80) before the first DATE draw after alpha writes, and test
  it (EQUAL 1 for DATM 1, EQUAL 0 for DATM 0). Users: CarIconMaker (the
  icon's alpha normalised to 0x80 where bit 7 is set), model-set commands.
- **ZTE / ZTST**: ZTE = 0 (pgl's colour-only clears) = no depth access. ZTST
  NEVER / ALWAYS / GEQUAL / GREATER -> Never / Always / GreaterEqual /
  Greater: GS Z grows toward the viewer, so the native depth is Z / 2^bits
  with the "greater" tests and a clear to 0 (gs_set_clear_depth(1.0) stores
  0). `gfx_depth_func(LEQUAL)` (7 sites) is GEQUAL.

### ZBUF, FRAME, SCISSOR, XYOFFSET

- **ZBUF**: ZMSK = 1: no depth write. PSM: GT3's Z buffers are all PSMZ24
  (T512): a D32_FLOAT depth of Z / 2^24 holds every 24-bit Z exactly. Z
  beyond the format's max clamps (`prim::depthOf`).
- **FRAME.FBMSK**: 1 bits are not written; whole bytes map to the render
  target write mask (GT3: 0xFFFFFFFF, RGB, alpha only, single channels R / G
  / B / A, none). Partial bytes are flagged (none in GT3).
- **SCISSOR**: window pixels, inclusive: native rectangle [x0 s, (x1 + 1) s)
  with s the native / GS scale.
- **XYOFFSET**: subtracted from every primitive coordinate: window = (X -
  OFX) / 16 (section 5). The draw buffer's base offset plus `gfx_offset_xy`
  jitter and the field offsets.

### PRIM

Type (0 point .. 6 sprite), IIP (Gouraud; flat = the colour of the
primitive's last vertex: D3D's provoking vertex is the first, so flat
primitives either repeat the last colour on all vertices or use
`nointerpolation` with a reordered index), TME, FGE, ABE, AA1, FST (UV texel
coordinates, 10.4, instead of STQ), CTXT (context 2: only the colour filters
and the kanji font, which program both contexts themselves). The value each
path sends is in the table of section 3; primAttr carries IIP | TME | FGE |
ABE | AA1 (init IIP).

### Culling

Not a GS feature: the VU1 programs cull. `gs_select_vu1_prog`: base 0x320
(0x33E with PGL_CAP_VU1_PROG_33E), with PGL_CAP_CULL_FACE + 0xA for (BACK,
CCW) and (FRONT, CW) - the clockwise faces in GL terms -, + 0x14 for (FRONT,
CCW) and (BACK, CW), program 0 (nothing drawn) for FRONT_AND_BACK. Native:
the VU1 replacement draws with D3D11 culling of the same faces. D3D11 judges
the winding as the triangle appears in its y-up NDC, the orientation GL's is
judged in: GL-CCW front faces are D3D front faces with FrontCounterClockwise
= TRUE, so + 0xA is CULL_BACK and + 0x14 CULL_FRONT (docs/models.md 4.6; an
earlier version of this note said FALSE, which was wrong). The renderer
swaps the two when the viewport mirrors the picture (viewScale.x < 0 or
viewScale.y > 0), since the VU1 tested the winding before the viewport.
Immediate 2D primitives are never culled.

### FOGCOL and the fog coefficient

With PRIM.FGE the GS mixes the colour with FOGCOL by the vertex's F:
`C = F / 255 x Cs + (1 - F / 255) x FOGCOL` (the GS steps in 1/256; alpha
unchanged; `gs::fogChannel`), F = 255 no fog. pgl's F per vertex (VU1
programs, `pgl_draw_line_3d`, RaceSmoke on the CPU):

```
F = viewCenter.w + viewScale.w x min(w, limits.w) / w
  = (f + 0.1) + (255 - f) x min(1, start / w)      f = clamp(fogAmount x 255, 0, 255), start = fogStart
```

w = the clip w (the eye distance under a perspective projection): no fog
inside `start`, then toward F = f + 0.1 (fogAmount is what remains of the
colour at infinity). Before `gfx_set_fog_params` (init): F = 255. Sprites
take F from spriteZF (0xFF by default; `pgl_set_sprite_depth` computes it
like the above).

## 5. Coordinates, depth and colour

**12.4 primitive coordinates.** The GS takes X, Y as 16-bit 12.4 fixed point
in a 4096 x 4096 space and subtracts XYOFFSET. pgl centres each draw buffer:
`xoff = (0x800 - w/2) << 4`, `yoff = (0x800 - h/2) << 4`, so a 640 x 448
buffer spans 1728..2368 x 1824..2272. Window pixels: `(X - OFX) / 16`.

**Pixel centres.** The GS samples a pixel at its integer window coordinate;
D3D at x + 0.5. Native position = (window + 0.5) x (native size / GS size);
with that, the +8 half-texel UVs of pgl's sprites sample texel centres as on
the GS, and the GS top-left fill rule falls on the same pixels at 1:1.
(The renderer uses window x s + 0.5 instead - identical at s = 1 - so that
integer-edged sprites cover whole native blocks at any scale, and shifts
sprite texture coordinates to keep texel centres: docs/renderer.md
"Pixels".)

**The VU1 viewport.** viewScale = (winW / 2, -winH / 2, (near - far)
depthScale / 2, 255 - f), viewCenter = ((xoff + 16 winX + 8 winW) / 16, (yoff
+ 16 winY + 8 winH) / 16, (1 - (near + far) / 2 + depthBias) depthScale +
depthOffset + 1, f + 0.1), limits = (FLT_MAX x 3, fog start), vBB0 = (0.5,
0.5, 1, 1) (x, y halved: the guard band of the clip tests). A clip-space
point maps to `center + min(p, limits) / w x scale` (`projectClip`): x, y
GS coordinates in pixels (with the offset), z the GS Z, w the fog F. Y is
flipped (NDC +1 at the top), as in D3D.

**Depth.** GS Z (24 bits for XYZF, 32 for XYZ) grows toward the viewer:
near (NDC z -1) = depthScale + 1 + depthOffset, far (NDC z +1) = 1 +
depthOffset. Native depth = Z / 2^24 for PSMZ24 (exact in D32_FLOAT), tests
GREATER / GREATER_EQUAL, clear 0. 2D primitives default to Z =
depthScale (nearest, `gfx_set_depth_buffer`); `gs_set_sprite_zf` /
`pgl_set_sprite_depth` place them in the scene.

**Field rendering (12.3).** With halfHeight (GSBuffer::setField: field modes
1..4, 640 x 224 buffers) the game still addresses frame lines (0..447): pgl
halves y (sprite y << 3, draw area / scissor y and h >> 1) and offsets OFY by
half a field line (8 in 12.4 = one frame line) for the odd field
(`gfx_set_field_mode`). Natively, PORT.md draws every target at the output
resolution: a 224-line target becomes a full-height target (native y =
(window y + 0.5) x outH / 224), and the field offset is then a one-line
vertical shift that alternates per frame; until roadmap pass 3 removes field
rendering, the backend should drop it for half-height targets (y relative to
`tgtA.yoff` minus the field component) to avoid the shake. SCANMSK (rows of
one parity not drawn: the mirror, the race tyres) is a discard on the GS row
parity at native resolution (or ignored once rendering is progressive).

Decision (implemented, docs/renderer.md "Fields"): `gfx_set_field_mode`
drops every half-line offset (OFY = tgtA.yoff in all modes).  Since roadmap
item 3 the display mode is non-interlaced and there is no field at all
(`ctx->field` 0, `gs_get_field_mode` = FRAME); the movies keep their own
field cadence (T253).

**Colour range.** Vertex colours are bytes. Untextured: 0xFF = 1.0
(pgl_color as given). Textured MODULATE: Cs = Ct Cf >> 7, so a vertex colour
of 0x80 is 1.0 (pgl's `colorTex` = colour x 129 / 256 maps 0xFF to 0x80):
the shader computes `tex x vtx x 255 / 128`, clamped. Alpha: 0x80 = 1.0 for
blending (C / 128), for MODULATE (At Af >> 7) and in `pgl_color4f` (a x
128); textures use 0x80 for opaque. DECAL: Cs = Ct; HIGHLIGHT: Ct Cf >> 7 +
Af; TCC = 0: As = Af. (`gs::texFuncColor` / `texFuncAlpha`.) Lit VU1 models
scale their colours by 128 or 255 (VifModel.scaleFlags).

## 6. Immediate primitives

The 2D paths compute GS register values on the CPU; `pgl_state_prim` repeats
them bit for bit (same truncations, 16-bit wrap-arounds, the carry between X
and Y of `(Y << 16) + X` in the strip functions) and yields `GsVertex`
records (X, Y 12.4, Z, F, RGBA, UV, kick). The native recorder converts them
with `windowX/Y` (the XYOFFSET of the draw's `DrawState`) and `toNative`.

`pgl_draw_strip(xyz, normals, colors, uvs, n)` (and `strip_batch_*`,
`strip_draw`) run the VU1 primitive program instead; it receives, per strip
of 3..64 vertices (only while list 0 is current):

- the combined matrix vf24..vf27 and the viewport vectors (`pgl_send_matrix`
  to VU1 0xC1, when `vu1MatrixSent` is 0) and the dirty GS registers (0xD9);
- a GIF tag at 0xC0: NLOOP n, EOP, PRIM = primAttr | TRI_STRIP, TME forced
  by `uvs != NULL` (pgl_draw_strip) or primAttr's TME (strip batches), NREG 3
  {ST, RGBAQ, XYZF2};
- xyz (3 floats, UNPACK V3-32 -> 0), the strip lengths ((len - 2) x 3, S-8
  into the w of 0x40..), texture coordinates (2 floats, V2-32 -> 0x40) when
  given, colours (4 bytes, V4-8 -> 0x80) or one constant colour (`colorTex`
  when textured, else `color`) through STROW;
- before the upload, on the CPU: texture coordinates from `texGenFunc`
  (PGL_CAP_TEXGEN), through the texture matrix (PGL_CAP_TEX_MATRIX:
  `xformTexcoords`), colours from CPU lighting (lightFlags 0x80:
  `__pglShading`) or `colorGenFunc` - into `gfx_alloc_tail` buffers;
- MSCAL vu1Prog (minus 1 when the matrix was sent, minus 5 with registers;
  vu1Prog 0 draws nothing: cull FRONT_AND_BACK).

Native: a vertex shader with the snapshot of vf24..vf27 and the viewport
does what the program does: `clip = (x, y, z, 1) x combined`; screen as
`projectClip` (D3D: the clip position as is, D3D's viewport set to the GS
window (winX, winY, winW, winH) + 0.5 pixel, depth written as
`(viewCenter.z + viewScale.z z / w) / 2^24`); F as section 4; ST passed for
perspective-correct interpolation (the GS divides S / Q, the VU1 sends S x Q
with Q = 1 / w); culling as section 4. The program's exact behaviour (its
clipping / culling of off-screen triangles, the difference between 0x320 and
0x33E) is the VU1 notes' to confirm from the microcode.

## 7. pgl_math in the port

The port's `src/engine/pgl` keeps the decomp's C files and replaces the
functions whose body is VU0 / COP2 code by one-line calls, e.g.

```cpp
extern "C" void pgl_translate(float x, float y, float z)
{
    /* PC: VU0 macro code (0x00257EB4) -> pgl_math */
    pgl::ctxTranslate(*GS_CTX, pgl::g_vu0, x, y, z);
}
```

| Decomp | Replaced by |
|---|---|
| vu0_matrix_asm.c, vu0_current_matrix.c | the `ctx*` matrix templates |
| pgl.c: matrix mode / stack, sin / cos, `pgl_set_sprite_depth`, `vu0_clip_code`, `clip_line_homogeneous`, the line paths | `ctxMatrixMode` ..., `pglSinCosDeg`, `ctxSetSpriteDepth`, `clipCode`, `clipLineHomogeneous`, `prim::lineStrip2d` / `line3d` |
| vu0_math.c: `vu0_atan2f`, `vu0_rand`, `vu0_rand_seed` | `vu0Atan2`, `vu0Rand`, `vu0RandSeed` (+ the LCG store) |
| vu1_lighting.c: `pgl_sync_matrices`, envmap functions | `ctxSyncMatrices`, `envmap*` |
| gs_driver_core.c: `gfx_init_vu0_constants` | `ctxInitVu0Constants` |
| gs_display_driver.c: `gs_set_depth_bias_at` | `ctxSetDepthBiasAt` |
| gs_lights.c: `gs_set_light_param_v` vectors | `lightVectorToEye` |
| render.c: `vu0_clip_point`, `box_visible`, `material_load` | `clipPointCombined`, `boxVisible`, `vu::mul` |
| flare.c: `vu0_sync_state`, `vu0_load_*` | `ctxSyncState`, `ctxLoadScreenRegs` ... |
| vu1_prim.c: `pgl_xform_texcoords` | `xformTexcoords` |
| pdistd/fastmath.c | `fm::*` |

Compile `pgl_math.cpp` with `/fp:precise` (the default; no `/fp:fast`, no
FMA contraction): the ACC chains are written out operation by operation.

## 8. Testing

`tools\msvc.cmd cmake -S tools/pglmath -B build/tools/pglmath -G Ninja
-DCMAKE_BUILD_TYPE=RelWithDebInfo`, `tools\msvc.cmd cmake --build
build/tools/pglmath`, then `build\tools\pglmath\pglmath.exe` (328 checks):
sin / cos / atan / fastmath against hand values and the exact functions (and
their quirks: `fm::atan2f(1, -1) == 0`), the R LFSR sequence, every matrix
routine (glRotate against the axis rotations, rigid inverse, the three sync
modes giving one combined matrix, stacks), sprite Z / F through a frustum,
clip codes and the line clip, the environment map, all 256 ALPHA bytes
against the GS formula, the GT3 blend modes, TEST pass splits (clears, model
sets, FB_ONLY / RGB_ONLY), write masks, and the immediate primitives' GS
coordinates (640 x 448 buffer, field rendering, strips, lines, ADC).

## 9. VU0 / MMI inline assembly in the game and engine

The decomp's modern build (GCC 14, NON_MATCHING) still has 336 inline asm
statements in `src/game` and `src/engine` (the matching-only branches have
C equivalents already). `build/scratch/pgl/asm_extract.py` lists them
(`asm_sites.txt`): 243 in pgl itself (replaced by pgl_math, section 7), 47
in the game (24 of them VU0 code: T167 rigid body, T159 flare, T173 smoke,
the car and tyre models), 46 in the rest of the engine (mostly `ei`, the
MPEG routines, T451 / T454 model code). Each
pattern below is written with `pgl::g_vu0` (abbreviated `vu`) and the
`pgl::vu` helpers; "vfN.w from v" means the VU instruction took the vector's
own w (`vmaddw ..., vNw`), "w = 1" means `vf0w`.

### VU0 patterns

| # | Pattern (asm) | C++ | Sites |
|---|---|---|---|
| 1 | `lqc2 $vfN, 0(p)` / `sqc2 $vfN, 0(p)` | `vu.vf[N] = *(Vec4*)p` / `*(Vec4*)p = vu.vf[N]` | everywhere; game: T167 rigid body (vf20..23, vf24..27 as scratch matrices), T159 / race_car_model / tire_model (`sqc2 $vf31`, `$vf29`, `$vf28`: rows of the current matrix) |
| 2 | `qmtc2 r, $vfN` / `qmfc2 r, $vfN` (+ `pcpyld` / `ldl ldr lwl lwr` to build r) | the 128-bit value moved; unaligned loads are `memcpy` | T447 tire model, T454 sprite model, pgl |
| 3 | `vmulax.xyzw ACC, M0, vx; vmadday ACC, M1, vy; vmaddaz ACC, M2, vz; vmaddw d, M3, vw` | `d = vu::xform(&vu.vf[M0], v)` (w from v) | T159:144, T167:163, T173:191, T451:1031, pgl |
| 4 | the same with `vmaddw d, M3, $vf0w` | `d = vu::xformW1(&vu.vf[M0], v)` (point, w = 1) | T159:130, T173:171, T451:301, race_car_model:191, T447:249, pgl |
| 5 | `vmulax ACC, M0, vx; vmadday ACC, M1, vy; vmaddz d, M2, vz` | `d = vu::xform3(&vu.vf[M0], v)` (direction; d.w = M0.w x + M1.w y + M2.w z) | T159:158, T167:304 (vf20..22), T167:352 (vf24..26), pgl |
| 6 | `vdiv Q, $vf0w, $vfNw; vwaitq; vmulq d, s, Q` | `float q = vu::div(1.0f, vN.w); d = vu::scale(s, q)` | race_car_model:191, T173:221, pgl |
| 7 | `vmini d, a, $vf21` / `vmax` / `vminiw.z` | `vu::mini(a, vu.vf[21])`, `vu::maxi`, per lane | race_car_model:191 (clamp to limits), T173:221, pgl |
| 8 | `vmulaw ACC, $vf22, $vf0w; vmadd d, s, $vf23` | `d = vu::madd(vu.vf[22], s, vu.vf[23])` (viewport) | T173:221, T451:1041, pgl |
| 9 | 4 + 6 + 7 + 8 + `vftoi4` | `projectClip(p, viewport)` then `vu::ftoi4` per lane | T173:221 (RaceSmoke: screen XYZF of particles) |
| 10 | `vclipw.xyz $vfN, $vfNw` (also as `.word 0x4BE109FF` / `0x4BE211FF`) then `cfc2 r, $vi18` | `vu::clipw(vu, v, v.w); r = vu.clip` (vi18 keeps the last 4 judgements, 6 bits each: two clips then `r >> 6` is the first) | T447:249 / 278, T451:564, pgl |
| 11 | w < 0 fix-up after a clip: `((f >> 1) \| 0x2A) ^ ((f << 1) \| 0x15)` | C already; `clipCode()` does both | T447, T451, render.c, pgl |
| 12 | `vmul d, a, b` (lane-wise), `vadd`, `vsub`, `vaddq`, `vsubw.z` ... with masks | `vu::mul` / `add` / `sub`, or per lane for a dest mask (.x, .xz ...) | T447:249 (vf2 = vf1 x vf20: the guard band), T451:1024, pgl |
| 13 | `vmove d, s`, `vmr32 d, s` | `d = s`; `d = (s.y, s.z, s.w, s.x)` | T454:133 / 180, pgl |
| 14 | `vopmula.xyz ACC, a, b; vopmsub.xyz d, b, a` | `d.xyz = vu::cross(a, b)` (a x b; d.w kept) | T454:180, envmap |
| 15 | rigid inverse of vf20..23 (vsub.xyz + pextlw / pextuw / pcpyld / pcpyud transposition + products) | `pgl::invertRigid(&vu.vf[20])` | T454:180, vu1_lighting.c |
| 16 | `vrsqrt Q, $vf0w, $vfNx` / `vsqrt` | `vu.q = vu::rsqrt(1.0f, vN.x)` / `sqrtf` | normalisations, pgl |
| 17 | `ctc2 r, $vi10; vlqi $vfN, ($vi10++)` / `vsqi` / `vlqd (--$viK)` | `vu.vf[N] = vu.mem[vi10++ & 0xFF]` ... (the parked matrices and the stacks) | pgl only |
| 18 | `vcallms 0x0 / 0x90 / 0x120 / 0x1D0 / 0x2D0` | `vu0Micro000 / 090 / 120 / 1D0 / 2D0` | pgl only |
| 19 | `vrnext.x $vf1, R` / `vrinit R, $vf1x` | `vu0Rand` / `vu0RandSeed` | vu0_math.c |
| 20 | `vitof0` / `vftoi0` / `vftoi4` | `vu::itof0` / `ftoi0` / `ftoi4` | pgl |
| 21 | `vnop`, `vwaitq`, `.word` padding, `vnop` delays before `cfc2` | nothing | everywhere |
| 22 | `cfc2 $vi29` (VU1 busy), `ctc2 $vi28` (VU reset) | nothing (no VU1) | gs_display_driver.c, gs_driver_core.c |

### EE / MMI patterns

| Pattern | C++ | Sites |
|---|---|---|
| `rsqrt.s d, num, sq` (EE FPU) | `d = pgl::vu::rsqrt(num, sq)` (= num / sqrt(\|sq\|)) | race_car_model_internal.h:92, tire_model_internal.h:60, T167:206, T159:183, T173:244, model_set_internal.h:120 |
| `div.s` tails (fastmath expf / tanf) | `fm::expf`, `fm::tanf` | fastmath.c |
| `pcpyh d, x` | `d = 0x0001000100010001ull * (uint16_t)x` (the low halfword in all four) | T077:67 |
| `pmulth / pmaddh ... pmflo` (memory card icon morph, 1..8 shapes) | per output vertex: `x = (int16_t)((sum_k src_k[0] * w_k) >> 12)`, `y` with `[1]`, `z` with `[2]`; `src_k` the shape k's (s16 x, y, z), `w_k` the s16 weight, products 32-bit, sums wrapping in 32 bits (LO.w0, LO.w1, HI.w0) | T077:933 / 955 / 982 / 1014 |
| `pextlb / pextlh` (bytes -> words), `ppacb` (words -> bytes) | per-byte loops (C fallbacks exist: pgl_color, u115_spread_bytes) | pgl, mpeg |
| `pextlw / pextuw / pcpyld / pcpyud` transposes | `vu0Transpose`, `invertRigid` | pgl, T454 |
| `lq / sq` 128-bit copies | `memcpy` of 16 bytes | ipu_internal.h |
| `register int x __asm__("$17")` + empty asm (read the caller's $s1) | the caller's heap choice: icon_model_build's only caller passes heap 1 (its `this`); write `heap = 1` with a `/* PC: */` note | T077:351 |
| `ei`, `sync`, `sync.l`, `sync.p`, `pref`, `break`, `bc0f` loops, `__asm__("")` | the platform layer's interrupt enable / nothing / a debug break / the IPU wait | cameras, logger, memory card, vol device, system, IPU, smoke, sliders |
| `mfc0 r, $1` (COP0 Random) | any small pseudo-random value (RaceRandom seed) | T414:26 |
| `mfpc / mtpc / mtps` (performance counters) | a high-resolution counter | gs_driver.c, gs_driver_core.c |
| the MPEG motion-compensation and block routines (MMI, top-level asm) | the media notes' decoder | mpeg_player.c |

## 10. Open questions

- **VU1 primitive program 0x320 vs 0x33E** (PGL_CAP_VU1_PROG_33E, 12 enables
  / 3 disables in the game, 0x33E at init): what differs (fog, clipping,
  colour scale?) is in the VU1 microcode; the VU1 notes should decode it.
- **Culling winding**: section 4 takes the VU1 programs' "clockwise" (the
  +0xA variant) as GL winding in y-up clip space, as pgl's glCullFace /
  glFrontFace naming suggests; confirm from the microcode or by drawing a
  car (wrong winding shows the inside of the body).
- **ZTE = 0**: the colour-only clears rely on "no Z access"; GS documents
  ZTE = 0 as not to be used. Native: no depth test and no write (assumed;
  check against a capture).
- **AFAIL FB_ONLY / ZB_ONLY and the depth test**: the passes keep the depth
  test for failing pixels (assumed, as PCSX2 does).
- **Data-driven blends**: CourseEnv::reflBlend and model-set
  `gfx_blend_func_raw` values come from the disc; log `DrawState.inexact` in
  pass 1 to see whether any inexact form occurs.
- **R register at power-on**: unknown; `vu0Reset` seeds a non-zero value
  (an all-zero LFSR returns 0 forever). Car wheel shake (T345) draws from
  `vu0_rand` before the smoke seeds it.
- **VU / EE float rounding**: results differ from the PS2 in the last bit
  (round-to-nearest vs toward zero). Physics code that integrates with
  `fm::` and the VU0 routines can drift from PS2 recordings (replays, demo
  files) over long runs; whether replays stay in sync needs a test with a
  PS2-recorded replay.
- **`mat44_copy`'s slips** (pgl_set_normal_matrix with unaligned matrices)
  are not reproduced: both callers pass 16-byte-aligned stack matrices on the
  PS2; the port must keep those aligned (`alignas(16)`).
