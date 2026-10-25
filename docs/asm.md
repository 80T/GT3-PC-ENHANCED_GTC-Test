# The game's inline assembly, rewritten

What each VU0 / EE / MMI asm block of the game (and of the two model-set
files of the engine) computed, and what replaces it. The patterns are in
[pgl.md](pgl.md) section 9; the helpers in `src/engine/pgl/native/vu0_game.h`
(namespace `vu0g`, over pgl's register file `pgl::g_vu0`, pgl_math.h). The
conversions of [runtime.md](runtime.md) 5.3 are at the end.

Rules followed everywhere:

- Registers that live beyond one asm block are `pgl::g_vu0`'s: vf20..vf31
  (pgl's screen registers, combined and current matrices), the rigid body's
  vf20..vf27, and any register read after a pgl call or in a later block
  (T451's vf1 / vf2, T454's vf5 / vf7). Registers used inside one block
  (vf16, most vf1 / vf2) are locals.
- ACC chains left to right (`vmulax`, `vmadday`, `vmaddaz`, `vmaddw`), the
  same operands and constants; `vdiv` / `rsqrt.s` with the VU / EE result
  for a zero divisor (+-max float); `vftoi4` truncating and saturating;
  `vclipw` shifting the 24-bit clip register as `cfc2 vi18` reads it.
- `vnop`, `vwaitq`, `pref`, empty volatile asm: nothing.
- Every change carries a `/* PC: ... */` comment.

Verification: `tools/vu0game` runs each block both ways on random inputs -
the port's C++, and a reference that executes the original instruction
sequence one instruction at a time (VU0 lanes, ACC, Q, clip flags, 128-bit
GPRs, HI / LO) - and requires identical bits (292,031 checks, 0 failures).
`tools\msvc.cmd cmake -S tools/vu0game -B build/tools/vu0game -G Ninja
-DCMAKE_BUILD_TYPE=RelWithDebInfo`, `tools\msvc.cmd cmake --build
build/tools/vu0game`, `build\tools\vu0game\vu0game.exe`.

## Car and tyre models

| File | Block | Computes | Port |
|---|---|---|---|
| `race_car_model_internal.h` | `vu0_get_translation` / `vu0_read_translation`, `vu0_get_y_axis` | xyz of vf31 / vf29 (the current matrix's translation / y row) | `vu0g::vf(31)`, `vf(29)` |
| | `vu0_project` (RaceCarModel::drawTires) | v = vf24..vf27 x (p, 1); Q = 1 / v.w; v = min(v, vf21) x Q (all four lanes) | `vu0g::projectCombined` |
| | `ee_rsqrt` | `rsqrt.s` | `vu0g::eeRsqrt` |
| `tire_model_internal.h` | `vu0_get_vf31`, `vu0_get_vf28` | vf31 / vf28, all four lanes | `vu0g::vf(n)` |
| | `ee_rsqrt` | `rsqrt.s` | `vu0g::eeRsqrt` |
| `T180_race_car_model.cpp` | `vnop` before `vu0_load_screen_regs` | - | removed |
| `T447_tire_model_base.cpp` | `TireModelBase::clipTest`, per box corner | vf1 = vf24..vf27 x (corner, 1) (the 12-byte corner's w lane, z's sign extension, unused); vf2 = vf1 * vf20 (vBB0: x, y halved, the guard band); vclipw vf1 (`.word 0x4BE109FF`), cfc2; vclipw vf2 (`.word 0x4BE211FF`), cfc2; vf1 / vf2 stored for their w sign | `vu0g::xformW1`, `pgl::vu::mul`, `vu0g::clipw` (the full clip register, so the C fix-up for w < 0 is unchanged) |

## Rigid bodies (`T167_rigid_body.cpp`)

The body matrix of the gadget being processed stays in vf20..vf23 (rotation
rows, position) and its transpose in vf24..vf27 across the functions of a
step, as on the PS2: all in `g_vu0`.

| Block | Computes | Port |
|---|---|---|
| `vu0_load_vf20_23` | vf20..vf23 = the matrix | `vu0g::load` into `vf(20..23)` |
| `vu0_set_vf23` | vf23 = (t, w = z's sign extension) | `vu0g::load` |
| `vu0_xform_vf20` | v x vf20..vf23, w from v | `vu0g::xform` |
| `vu0_transpose_vf20_to_vf24` | vf24..vf27 = transpose of vf20..vf23 (w lanes too) | `vu0g::transposeVf20ToVf24` |
| `rb_rotate` / `rb_rotate_inverse` | v x vf20..vf22 / vf24..vf26 (`vmaddz`, no translation) | `vu0g::xform3` |
| `ee_rsqrt` | `rsqrt.s` | `vu0g::eeRsqrt` |

Also: `g_0035216C` / `g_0049C3E0` (Gadget::step's function-local static and
its guard, spelled out as globals) had no definition in the decomp (its GCC
14 link placed them by address); T167 defines them.

## Effects

| File | Block | Computes | Port |
|---|---|---|---|
| `T159_flare.cpp` | `vu0_xform_point` | (p, 1) x vf28..vf31 | `vu0g::xformW1` |
| | `vu0_xform_vec4` | v x vf28..vf31, w from v; all four lanes stored | `vu0g::xform` |
| | `vu0_xform_dir` | v x vf28..vf30 (`vmaddz`) | `vu0g::xform3` |
| | `vu0_get_translation` | vf31 | `vu0g::vf(31)` |
| | `vu0_sync`, `ee_rsqrt` | `vnop`, `rsqrt.s` | nothing, `vu0g::eeRsqrt` |
| `T173_race_smoke.cpp` | `u058_xform_point` | (p, 1) x vf28..vf31 (view space) | `vu0g::xformW1` |
| | `u058_xform_plane` | vf20..vf23 = the plane matrix (left there: emitStrip reloads them), plane x vf20..vf23 | `g_vu0` vf20..vf23, `vu0g::xform` |
| | `u058_project_vertex` | vf24..vf27 x (p, 1); Q = 1 / w; min vf21; x Q; vf22 + v x vf23; vftoi4 (x, y 12.4, z and fog x 16) | `vu0g::projectScreenFixed` |
| | `U058_PREF`, `u058_rsqrt` | `pref` of a stack word, `rsqrt.s` | nothing, `vu0g::eeRsqrt` |

## Model sets (engine, `T451_model_set.cpp`, `T454_sprite_model.cpp`)

| File | Block | Computes | Port |
|---|---|---|---|
| `T451` | `modelset_sphere_depth` | vf1 = the sphere centre, **then** `vu0_sync_state`, then w of vf1 x vf24..vf27 (w = 1), at least 0.0001 | vf1 is `g_vu0`'s across the call. On the PS2, when the matrices are dirty the sync (VU0 micro 0x120: vf1..vf3 = the new combined rows 0..2; with PGL_CAP_SCALE vf1 = the scale) overwrites vf1, so the depth is that of the combined matrix's row 0, not of the sphere; pgl_math's sync does the same, so the port keeps the PS2's texture-LOD choice. |
| | command 0x0A (frustum test of n points) | vf1 = vf24..vf27 x (p, 1); vf2 = vf1 * vf20; vclipw vf1; vclipw vf2; cfc2 (vf1's flags in bits 6..11, vf2's in 0..5) | `vu0g::xformW1`, `pgl::vu::mul`, `vu0g::clipw` twice |
| | `modelset_set_projection_scale` | vf1 = (0, 0, -1, 1), vf2 = (-1, 0, -1, 1) (vmove + vsubw); after `vu0_load_projection`: both x vf20..vf23 (own w); after `vu0_load_screen_regs_nosync`: vf22 + v x vf23; the pixels per unit = abs(x2 - x1) x scale | vf1, vf2 in `g_vu0` (live across both calls), `vu0g::xform`, `vu0g::viewport` |
| `T454` | `spriteset_begin_draw` | vf5 = (0, 0, 1, 0) (vmr32 of vf0, before `vu0_load_modelview`); vf6 = (0, 1, 0, 0); vf20..vf23 = the rigid inverse of the modelview (vsub.xyz vf23 = -t; the 3x3 transposed through qmfc2 / pextlw / pextuw / pcpyld / pcpyud / qmtc2 with w = 0; vf23.xyz = -t R^T); vf5 x vf20..vf22 (the camera's z axis in model space); vf7.xyz = vf6 x vf5 (vopmula / vopmsub, vf7.w kept); stored whole to `g_0049D1A0` | vf5 in `g_vu0`; `vu0g::billboardAxis` (`pgl::invertRigid` = the MMI transpose + products, checked bit for bit against the instruction sequence) |
| `model_set_internal.h` | `u102_rsqrt` | `rsqrt.s` | `vu0g::eeRsqrt` |

## Memory card icons (`T077_memorycard_icon.cpp`)

| Block | Computes | Port |
|---|---|---|
| `mmi_pcpyh` | the weight's low halfword in the four halfwords | `vu0g::pcpyh`: `0x0001000100010001 * (u16)w` |
| the mesh loops of `icon_model_draw` (2, 4, 6, 8 shapes) | `pmulth` then `pmaddh` per shape over the shape's (x, y, z, next) s16 quad and the pcpyh'd weight: per lane the signed 16 x 16 products summed in 32 bits, wrapping (LO.w0 x, LO.w1 y, HI.w0 z); the result register's low doubleword (LO.w0, HI.w0) gives x = bits 12..27, z = bits 44..59; `pmflo` (LO.w0, LO.w1) gives y = bits 44..59 | `vu0g::pmaddhChain` (`mmi_morph`); the C extraction lines unchanged; checked against the 8-lane instruction semantics and the formula of pgl.md 9 |
| `icon_model_create`'s `register int callerS1 __asm__("$17")` | the heap index from the caller's $s1 (MemorycardOtherPS2File::readIcon's `this`, non-null) | `heap = 1` |
| `icon_rsqrt`, `load64u` | `rsqrt.s`; an unaligned 8-byte load (a packed struct MSVC does not pack) | `vu0g::eeRsqrt`; `memcpy` |

## Others

| File | Block | Port |
|---|---|---|
| `T414_race_random.cpp` | `mfc0 $1` (COP0 Random: the TLB index, 0..47) | `__rdtsc() % 48` |
| `setting_sliders_internal.h` (T082) | `EMPTY_LOOP_BODY`: an empty volatile asm keeping an empty loop | `((void)0)` |
| `T397`, `T405`, `T406` (cameras), `T055` (logger), `T078` (card warning) | only `ei` (already `EI()`) | unchanged |

## Float to unsigned (`include/pc_ee.h`)

`ee_f2u(f)` is libgcc's fptoui (`fp_bit.c` 0x0027AEA0): 0 below 1 (negative,
NaN included), 0xFFFFFFFF from 2^32 (and infinity), else truncated; checked
against fp-bit's algorithm on 200,000 random bit patterns. Used at
`machine_test_new_record` (T053), `LoggerRecorder::record` (T055: speed,
throttle, brake), `Color8::setClamped` (T188), `Rectangle::drawShadow`
(T256), the examination messages' alphas (T141, T146 x 3) and
`MusicPlayList::orderShuffle` (T430). T452's `anim_frame_reset / step` are
double -> int (`dptoli`, signed truncation as MSVC's), not fptoui: unchanged.

`RaceChampionshipInformation::getEntryByOrder` (T151): with no entry of
that order the PS2 copied an entry from address 0; the port copies a static
zeroed one and logs it once.
