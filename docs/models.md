# 3D models: prebuilt VU1 models and what the VU1 microcode did with them

How GT3 draws its 3D models on the PS2, and what the port does instead.
On the PS2 a model is a list of VIF1 packets that the EE never looks into:
`model_draw` sends them to VU1, whose microcode converts, lights,
transforms and clips the vertices and hands one GS triangle strip per
packet to the GIF. The port decodes the packets once into vertex / index
buffers (`src/engine/pgl/native/vif_model.*`, no VU code runs) and its
shaders compute what the VU1 programs computed, from the same state the
EE prepared for VU1 (the matrix block, the lighting block, the morph
weight, the GS register shadows).

- [1. Overview](#1-overview)
- [2. The VU1 microcode](#2-the-vu1-microcode)
- [3. The programs](#3-the-programs)
- [4. Conventions](#4-conventions)
- [5. Shader math](#5-shader-math)
- [6. Packet layout and vertex formats](#6-packet-layout-and-vertex-formats)
- [7. Model files](#7-model-files)
- [8. Packets the game builds](#8-packets-the-game-builds)
- [9. The decoder](#9-the-decoder)
- [10. Integration](#10-integration)
- [11. Verification](#11-verification)
- [12. Open points](#12-open-points)

Tools: `tools/mdldump/` (`mdldump` decodes model files to .obj + stats,
`objrender.py` renders an .obj to a PNG, `vu1dis.py` unscrambles and
disassembles the microcode). Decomp references are to
`decomp/core_gt3enhanced` (`// FUNC` addresses, GT3 US retail).

## 1. Overview

```
 ModelSet ("GTM1")            command streams (modelset_run_part, T451)
   parts[] ----------------->   0x04/0x05 draw packet n, 0x35 morph packet,
   packets[] -> VifModel        0x2B/0x2C texture, 0x06 switch, 0x07 LOD,
                 part[] VifPart   matrix / render-state ops
                   data, qwc, tex
                      |
 model_draw (render.c 0x0024D048)
   vifmodel_draw_lit / vifmodel_draw (vu1_lighting.c)
     per part: partTexFunc(tex) (texmat_select: texture + material),
               lighting block (mode != 0), morph weight (formats 1 / 2),
               dirty GS registers, DMA ref of the part, ITOP lightProg, MSCAL prog
                      |
 VU1: format converter -> lighting program -> transform / cull / clip -> XGKICK
```

`VifModel` (G001_flare.h): `self, ptr04, mode, scaleFlags, nParts, nVerts,
nPrims, part[nParts] {data, qwc, tex}`. `mode` bits 0..4 = lighting mode,
5..7 = vertex format; `scaleFlags` bit 0 / 1 = colour / alpha scale 255
(else 128). One part = at most 64 vertices (32 with a morphing format) in
one or more triangle strips, one GS PRIM.

What the files use (the decoder's sweep over every car, course, wheel and
menu model of the US volume, section 11): lighting modes 0..5 with format 0,
mode 1 with format 2 (`0x41`, morphing course objects), mode 0 with format 1
(`0x20`); PRIM always `TRISTRIP | IIP` with any of `TME FGE ABE AA1`. Format
3 only comes from `tri_draw16` (memory card icons, built at run time).

## 2. The VU1 microcode

`g_gsVu1Microcode` (gs_driver_core.c, `src/engine/pgl/vu1_microcode.inc`),
0x3DA8 bytes = 0x7B5 instructions, stored XOR-scrambled: `gfx_init`
(0x00250820) runs `xor_unscramble_with_lcg` (0x0025A1F8: each byte ^= the
low byte of `lcg_rand`, state = state * 0x5D588B65 + 1 from 0x32277070) and
uploads it to VU1 micro memory 0.

`python tools/mdldump/vu1dis.py --bin vu1.bin -o vu1.lst` writes the
unscrambled code and a listing (upper / lower pair per line, labels for the
EE's entries and branch targets, comments for the VU1 data addresses). The
listing lives at `build/scratch/models/vu1.lst`. Addresses below are
instruction indices (MSCAL / ITOP units).

### 2.1 Map

| Address | What |
|---|---|
| 0x000 | matrix load, called by the entry-1 prologues: vf28..31 = C (0xC1..0xC4), vf25 = viewScale, vf26 = viewCenter, vf24 = limits, vf23 = vBB0 |
| 0x009 | format 3 converter: xyz integers to floats (ITOF0), S T integers / 4096 (ITOF12) |
| 0x015 | format 2 converter: key-frame blend of 0x00 / 0x40 / 0x80 streams, floats |
| 0x02B | format 1 converter: the same, 0x80 stream integers (ITOF0 / FTOI0) |
| 0x045 | lighting dispatch of the lit entries: `XITOP vi01; JR vi01` |
| 0x048..0x04E | `VU1L_DEFAULT` (0x04E) and its format entries |
| 0x051..0x07B | `VU1L_SPEC2_S6` (0x057) |
| 0x07C..0x0A8 | `VU1L_SPEC2_S12` (0x082) |
| 0x0A9..0x0D7 | `VU1L_SPEC2` (0x0AF) |
| 0x0D8..0x10E | `VU1L_SPEC4_S6` (0x0DE) |
| 0x10F..0x145 | `VU1L_SPEC4_S12` (0x115) |
| 0x146..0x17E | `VU1L_SPEC4` (0x14C) |
| 0x17F..0x1A6 | `VU1L_DIFFUSE` (0x185) |
| 0x1A7..0x1DA | `VU1L_LOCAL_S6` (0x1AD) |
| 0x1DB..0x213 | `VU1L_LOCAL_S12` (0x1E1) |
| 0x214..0x250 | `VU1L_LOCAL` (0x21A) |
| 0x251..0x290 | `VU1L_ENVMTX` (0x257) |
| 0x291..0x2D4 | `VU1L_ENVMAP` (0x297) |
| 0x2D5..0x2F0 | `VU1L_CONST` (0x2DB) |
| 0x2F1..0x319 | the flare program (`FLARE_VU1_ITOP` 0x2F7, flare.c `vif_model_draw`) |
| 0x31A..0x355 | unlit entries 0x320, 0x32A, 0x334, 0x33E, 0x348, 0x352 with their prologues |
| 0x356..0x391 | lit entries 0x35C, 0x366, 0x370, 0x37A, 0x384, 0x38E with their prologues |
| 0x392 | transform, no clipping, no culling (0x320) |
| 0x3C4 / 0x3C7 | transform, no clipping, cull A / cull B (0x32A / 0x334; common code 0x3CA) |
| 0x419 | transform with clipping, no culling (0x33E) |
| 0x4AE / 0x514 | transform with clipping, cull A / cull B (0x348 / 0x352) |
| 0x57A..0x762 | the polygon clipper (Sutherland-Hodgman over 6 planes; state at VU1 0x366..0x3B3, outside both buffers) |
| 0x763 | colour filter program A (`color_filter.c`, `MSCALF 0x763`; not a model program) |
| 0x78D..0x7B4 | colour filter program B (`MSCALF 0x78D`) |

### 2.2 How the EE starts a program

`vu1Prog` (`gs_select_vu1_prog` 0x0025AA00) = 0x33E with
`PGL_CAP_VU1_PROG_33E` on (`vu1ProgAlt`, the default), else 0x320; with
face culling on + 0xA or + 0x14 (section 4.6), `FRONT_AND_BACK`: 0 =
nothing drawn. The base is the clipping switch: 0x33E clips, 0x320 does not
(the model sets' frustum command 0x0A turns 0x33E on only when a bounding
point is beyond the guard band; `modelset_end_draw` turns it back on).

The MSCAL address is adjusted by what the EE put in front of the part:
`- 1` when the matrix block was sent (`pgl_send_matrix`, prologue `BAL vi15,
0x000`), `- 5` when dirty GS registers were sent (`pgl_send_dirty_regs`;
the prologue sets vi14 = the register packet, XGKICKed before the vertices),
`- 6` for both; `+ 0x3C` for a lit model (mode != 0, `VU1_ENTRY_LIT`): the
lit entries set vi15 = the transform and branch to 0x045, which reads ITOP
(`vu1LightProg + vu1LightOfs`) and jumps there. The lighting program's
format entry (`-2` format 1, `-4` format 2, `-6` format 3) branches to the
converter with vi01 += 2 / 4 / 6, the converter returns to the lighting
program proper, which returns through vi15 to the transform with vf02 = the
first vertex and vi02 = TOPS + 1.

(`VU1L_LOCAL_S12 - 6` and `VU1L_LOCAL - 6` are the previous variant's
final `JR vi15`: format 3 + lighting mode 4 skips both the conversion and
the lighting.)

### 2.3 VU1 data memory

Double buffered: `gfx_begin_packet` sets BASE 0 / OFFSET 0x1B3, every UNPACK
has FLG (TOPS relative) and TOPS swaps at each MSCAL. Per buffer (TOPS
relative):

| Address | Contents | Sent by |
|---|---|---|
| 0x00..0x3F | xyz (0x20..0x3F: key frame 2 for formats 1 / 2) | the part |
| 0x40 w | the number of strips (UNPACK S-8, unmasked) | the part |
| 0x41.. w | per strip: 3 * (length - 2) | the part |
| 0x40..0x7F xy(z) | S T, or the normal for modes 2 / 3 (masked: w kept); 0x60..: key frame 2 | the part |
| 0x80..0xBF | RGBA (V4-8) or the normal (modes 1 / 4); 0xA0..: key frame 2 | the part |
| 0xC0 | the GIF tag: NLOOP vertices, EOP, PRE, PRIM, PACKED, NREG 3, REGS ST RGBAQ XYZF2 | the part (or `vifmodel_put_part`'s PRIM override) |
| 0xC1..0xC4 | C, the combined matrix (rows) | `pgl_send_matrix` |
| 0xC5..0xC8 | viewScale, viewCenter, limits, vBB0 | `pgl_send_matrix` |
| 0xC9..0xD8 | the lighting block of the mode (section 3.2) | `pgl_send_lights_*`, `pgl_send_mode*` |
| 0xD9.. | GIF tag A+D + dirty GS registers | `pgl_send_dirty_regs` |
| 0xF2 w | the morph weight (formats 1 / 2, STMASK 0x3F3F3F3F) | `vifmodel_send_lighting` |
| 0xF2..0x1B2 | output: the GIF tag, then ST, RGBAQ, XYZF2 per vertex | the program |

The matrix and viewport go to registers (vf23..vf31) when loaded and are
only sent when they changed; the lighting block is read from memory by
every run, so the EE sends it into both buffers (the `resend = 2` of
`vifmodel_draw_lit`).

## 3. The programs

Notation: `P` = vertex position (model space), `N` = normal as stored (not
renormalised by any program), `C` = the 4x4 at 0xC1 (rows), `cs` / `as` =
colorScale / alphaScale (128 or 255 from `scaleFlags`), `max0(x)` =
max(x, 0). Lighting is done in model space: the EE transforms the lights
and the eye into model space (`pgl_update_eye_lights`).

### 3.1 Format converters (0x009, 0x015, 0x02B)

Loop over NLOOP vertices (`ILW.x` of the GIF tag), writing back in place:

- **format 1** (0x02B): `w` = 0xF2.w; for i < NLOOP:
  `xyz[i] = lerp(xyz[i], xyz[0x20+i], w)` (xyz), `s40[i] = lerp(s40[i],
  s40[0x60+i], w)` (xyz lanes), `c80[i] = ftoi0(lerp(itof(c80[i]),
  itof(c80[0xA0+i]), w))` (xyzw; truncation toward 0).
- **format 2** (0x015): the same with `c80` as floats (normals).
- **format 3** (0x009): `xyz = itof0(xyz)` (plain integers, no scale),
  `st.xy = itof(st) / 4096`.

`lerp(a, b, w) = a (1 - w) + b w` (`vf20.w = 1 - w`).

### 3.2 Lighting programs

All write their result colour (integers) over 0x80..: the transform takes
RGBA from 0x80.

| ITOP | Program | Lighting block (0xC9 + k) | Result per vertex |
|---|---|---|---|
| 0x04E | `VU1L_DEFAULT` | - | 0x80 as it is (unlit colours) |
| 0x185 | `VU1L_DIFFUSE` | 0 dirX, 1 dirY, 2 dirZ (4 lights transposed), 3 A, 4..7 D0..D3, 8 (255,255,255,as) | `clamp(A + Σ Di max0(N·Li), 0, lim)` |
| 0x057 / 0x082 / 0x0AF | `VU1L_SPEC2_S6 / _S12 / SPEC2` | 0 (Lx0,Lx1,Hx0,Hx1), 1 (Ly..), 2 (Lz..), 3 A, 4 D0, 5 D1, 6 S0, 7 S1, 8 lim | `clamp(A + Σ Di max0(N·Li) + Σ Si max0(N·Hi)^e, 0, lim)`, e = 4 / 8 / 16 |
| 0x0DE / 0x115 / 0x14C | `VU1L_SPEC4_S6 / _S12 / SPEC4` | 0..2 dir, 3 A, 4..7 D, 8..10 halfX/Y/Z, 11..14 S0..S3, 15 lim | the same over 4 lights |
| 0x1AD / 0x1E1 / 0x21A | `VU1L_LOCAL_S6 / _S12 / LOCAL` | 0..2 dir, 3 E (eye, model space), 4 S0, 5 S1 | rgb = `S0 max0(L0 · R̂)^e` (light slot 0 only, no clamp), a = 128 (`vf09 = vf00 + I`, I = 127: w = 1 + 127; FTOI0.xyz keeps it) |
| 0x257 | `VU1L_ENVMTX` | 0..2 M (rows), 3 E, 4 K0 = qB00*qAF0, 5 K1 = qB10*qAF0 | S T = sphere map of the reflection; rgba = `rgba * (K0 + K1 min(V̂·N, 0))` |
| 0x297 | `VU1L_ENVMAP` | 0..3 the env-map matrix (w column: constants), 4 K0 = qB00, 5 K1 = qB10 | S T = panorama map of the reflection; rgba as above |
| 0x2DB | `VU1L_CONST` | 0 V = constVector, 1 lim, 2 K = constColor | `clamp(rgba * V + K, 0, lim)` |
| 0x2F7 | flare (`vif_model_draw`) | 0xC9 (r,g,b), 0xCA index, 0xCB.. 4 occlusion texels (V4-8) | `min(rgba * (r, g, b, o/255), 255)`, o = texel[index].x; o = 0: the part is not drawn |

Where (in the lighting block, all colour vectors are already multiplied by
(cs, cs, cs, as) on the EE; `lim` = (255, 255, 255, as)):

- `Li` = slot i's light direction (towards the light), `Hi` = its half
  vector `normalize(Li - viewDir)` (infinite viewer, `viewDir` = the eye's
  (0, 0, -1) in model space), slots without a light are 0.
- `A` = ambientColor = emission + globalAmbient * mat.ambient + Σ over all
  enabled lights of light.ambient * mat.ambient; `Di` = light.diffuse *
  mat.diffuse; `Si` = light.specular * mat.specular
  (`pgl_update_light_colors`).
- Only **directional lights with a colour** get a slot (`pgl_build_light_list`,
  max 4, `nVuLights`); positional lights add their ambient term only.
- The specular variant: material shininess < 6 → e = 4, < 12 → 8, else 16
  (powers by repeated squaring of max0(N·H); not gated by N·L).
- `V = P - E` (eye → vertex), `R = V - 2 (V·N) N`, `R̂ = R / |V|` (a unit
  vector when |N| = 1).

Which program runs (`vifmodel_setup_lighting`, `vifmodel_send_lighting`):

| mode | program |
|---|---|
| 0 | `VU1L_DEFAULT` (`vifmodel_setup_lighting` is skipped: the ITOP left from before is irrelevant, the unlit entry does not read it) |
| 1 | `pgl_send_lights_mode1`: `DIFFUSE` unless both a light and the material have a specular colour (lightFlags 0x10 and 0x08); then `SPEC2*` (≤ 2 VU lights) or `SPEC4*`; with the local-viewer flag: `LOCAL*` (< 2 VU lights) or `DEFAULT` (GT3 never sets the flag: no caller of `pgl_light_modelfv(1)`) |
| 2 | `ENVMTX` (`pgl_build_env_matrix`, `pgl_send_mode2`) |
| 3 | `ENVMAP` (`pgl_build_envmap_matrix`, `pgl_send_mode3`) |
| 4 | `LOCAL*` by shininess (`pgl_send_lights_mode4`) |
| 5 | `CONST` (`pgl_send_mode5`) |

The block is recomputed / resent when a light, the modelview or the
material changed (`lightFlags & 7`): the part-texture callback of a part
(`texmat_select` → `material_load`) changes the material **between parts**,
so the lit colours of a part use the material its own `tex` id selected.

### 3.3 Transform programs

All six share one shape (0x392 is the simplest), per vertex, with the clip
position `pc = (P, 1) · C`:

```
q      = 1 / pc.w
STQ    = (st.x * q, st.y * q, q)                      ; ST register (Q kept by the GS)
RGBAQ  = rgba (0x80, integers; the GS takes bits 0..7 of each word)
m      = min(pc, limits)                              ; limits.xyz = FLT_MAX, limits.w = fog start
X Y Z  = (m.xyz * q) * viewScale.xyz + viewCenter.xyz ; GS window coordinates
F      = (m.w * q) * viewScale.w + viewCenter.w       ; fog
XYZF2  = ftoi4(X, Y, Z, F) (+ 2048 on F: ADC)         ; X Y 12.4, Z bits 4.., F bits 4..11, ADC bit 15
```

- **Strips and ADC.** The output GIF tag is the part's (NLOOP = all
  vertices). The program walks the strip table (`ILW.w` 0x40, then 0x41..)
  and puts ADC (no drawing kick, `+2048` before `ftoi4`) on the first two
  vertices of every strip; so one GS triangle strip draws all the part's
  strips without joining them.
- **Culling** (0x3C4 / 0x3C7 and 0x4AE / 0x514): per triangle the z of
  `cross(n0 - n1, n1 - n2)` on the NDC positions `n = m.xyz * q` (the strip
  parity taken into account: it is the orientation of (v0 v1 v2), (v2 v1
  v3), ...); cull A (vu1Prog + 0xA) gives ADC to triangles with z ≤ 0 (CW
  in y-up NDC), cull B (+ 0x14) to those with z ≥ 0. The clipping + culling
  variants (0x348 / 0x352) add `3072` instead of `2048` for ADC (strip
  starts and culled triangles): bit 14 of the F word too, a bit the GS does
  not use (? a marker for the clipper).
- **Clipping** (0x33E family): `CLIPw` of each vertex against its w; a
  triangle with all three vertices outside one plane gets ADC; one partly
  outside the guard band (x, y beyond ±2w after `* vBB0 = (0.5, 0.5, 1,
  1)`, or z beyond ±w) goes to the clipper, which kicks the clipped polygon
  as a separate GIF packet. The 0x320 family does none of it (the game uses
  it only when everything is inside the guard band).

## 4. Conventions

### 4.1 Matrices

pgl uses row vectors: `v' = v · M`, the translation in row 3, matrices
stored row by row (16 floats, `pgl_load_matrix`). `pgl_mult_matrix(m)`:
`M = m · M` (m applied first, glMultMatrix); `pgl_translate`, `vu0_scale`,
`pgl_rotate`, `vu0_rotate_x/y/z`: `M = T · M` (glTranslate / glScale /
glRotate, right-handed, counter-clockwise angles).

`C` (0xC1) = `MV · P` (`pgl_sync_matrices`, VU0 micro subroutine 0x120:
vf24..27 = vf28..31 (modelview) × the projection). With `PGL_CAP_SCALE`
(`scaleEnable`) its rows 0..2 are multiplied by `GsContext.scale` x, y, z:
`C = diag(sx, sy, sz, 1) · MV · P` (the tyres are modelled at unit rim
radius).

The projection is OpenGL's (`pgl_frustum` 0x002555A8 = glFrustum with a
`focal` factor on x / y, `pgl_ortho` = glOrtho, both transposed): eye space
looks down -z, `w_clip = -z_eye`, NDC z in [-1, 1] (near -1).

### 4.2 Viewport and depth

`gs_update_scissor_and_viewport`, `gs_update_depth_range`,
`gfx_set_depth_buffer`:

```
viewScale  = ( winW/2, -winH/2, (near - far) * D / 2, 255 - f )
viewCenter = ( (xoff + 16 winX + 8 winW) / 16, (yoff + 16 winY + 8 winH) / 16,
               (1 - (near + far)/2 + depthBias) * D + depthOffset + 1, f + 0.1 )
limits.w   = max(fogStart, 0)            (gfx_set_fog_params; f = clamp(fogAmount * 255, 0, 255))
D          = depthScale: PSMZ24 16776191, PSMZ16/16S 65531, PSMZ32 4294836224
```

So X, Y are the GS window coordinates of the draw area (y down: the D3D
viewport transform of y-up NDC is the same mapping); `Z = trunc((zc/wc) *
viewScale.z + viewCenter.z)` grows **towards the viewer** (near → D + 1,
far → 1 with depth range 0..1), and the model sets test `ZTST GEQUAL`
(`gfx_depth_func(PGL_DEPTHFUNC_LEQUAL)` maps to GEQUAL).

For Direct3D 11 the shader can output the GS depth itself, normalised by
the Z format's range (2^24 for PSMZ24, 2^16, 2^32):
`SV_Position.z = (zc * viewScale.z + wc * viewCenter.z) / 2^bits`, with
the depth test mapped one to one (GEQUAL → `D3D11_COMPARISON_GREATER_EQUAL`,
clear value / 2^bits). That keeps depthOffset / depthBias / the depth range
exactly as the game sets them (car shadows and decals rely on
`gfx_set_depth_offset`, `proj_shift_depth`). A D32_FLOAT buffer has the
precision for it. (The programs write XYZF2, whose Z field has 24 bits:
the 3D views must use PSMZ24 or PSMZ16 buffers with them, never PSMZ32.)

### 4.3 Fog

`F = viewCenter.w + viewScale.w * min(wc, fogStart) / wc`: F = 255 (no fog)
up to w = fogStart, then `f + (255 - f) * fogStart / w` → f far away. The
GS blends `(F * C + (255 - F) * FOGCOL) / 255` when PRIM.FGE is set. The
fraction of fog colour is `1 - F / 255`. F is a per-vertex value (fog
start 0 gives a constant F = f + 0.1).

### 4.4 Colour

GS colours are 0..255 with 128 = 1.0 for texture modulation and alpha
(`ALPHA` FIX and `As` also /128). Unlit vertex colours are stored that way
(0x80808080 = white). Lit colours are `scale * light terms` with scale 128
(textured parts: the light result 1.0 lands on the GS's 1.0) or 255
(`scaleFlags` bit 0, untextured / environment parts), **clamped at 255, not
at the scale** (lit textured surfaces can reach 2x). Alpha likewise with
`alphaScale`, clamped at alphaScale. The env / const modes multiply the
stored integer colours.

### 4.5 Interpolation

The GS interpolates RGBA and F linearly in screen space and ST with
perspective (STQ). With `PRIM.IIP` clear the triangle takes the colour of
its **last** (kicking) vertex. In HLSL: colour and fog `noperspective`,
texture coordinates perspective; for flat parts `nointerpolation` with the
decoder's index order (each triangle starts with its kicking vertex, which
is D3D 11's leading vertex).

### 4.6 Winding and culling

The decoder emits every triangle in the orientation of its strip's first
triangle (v0 v1 v2, then v2 v1 v3, ...); the face normals of that winding
agree with the stored vertex normals (section 11), i.e. **front faces are
counter-clockwise in GL's y-up NDC**. With the GL-style projection, in
Direct3D 11: `FrontCounterClockwise = TRUE`, and by `vu1Prog`:

| vu1Prog | glCullFace / glFrontFace | D3D11 CullMode |
|---|---|---|
| 0x320 / 0x33E | culling off | NONE |
| + 0xA | (BACK, CCW) or (FRONT, CW) | BACK |
| + 0x14 | (FRONT, CCW) or (BACK, CW) | FRONT |
| 0 | FRONT_AND_BACK | nothing drawn |

(The mirror view draws with `PGL_FRONT_CW`: the same table covers it.)

## 5. Shader math

Constants the shaders need, in the order the EE already builds them for
VU1 (the port's `pgl_send_*` can fill a constant buffer instead of a VIF
packet; the VU0 asm of those functions becomes plain C++):

```hlsl
cbuffer PglFrame : register(b0) {        // pgl_send_matrix (0xC1..0xC8)
    float4x4 C;          // rows: v * C, scale rows already applied
    float4 viewScale;    // (winW/2, -winH/2, depth scale, 255 - f)
    float4 viewCenter;   // (cx, cy, depth centre, f + 0.1)
    float4 limits;       // (FLT_MAX x3, fog start)
    float4 vBB0;         // (0.5, 0.5, 1, 1): guard band (clipping only)
    float  morphWeight;  // GsContext.morphWeight (formats 1 / 2)
    float  zNorm;        // 1 / 2^bits of the Z format
};
cbuffer PglLights : register(b1) {       // the 0xC9.. block of the mode, 16 float4
    float4 L[16];
};
```

Common vertex part (all programs):

```hlsl
struct VsIn {                     // gt3vif::Vertex
    float3 pos : POSITION0;  float3 pos2 : POSITION1;
    float3 nrm : NORMAL0;    float3 nrm2 : NORMAL1;
    float2 st  : TEXCOORD0;  float2 st2  : TEXCOORD1;
    uint4  rgba : COLOR0;    uint4  rgba2 : COLOR1;   // R8G8B8A8_UINT
};
// format converters
float3 P  = morph ? lerp(i.pos, i.pos2, morphWeight) : i.pos;
float3 N  = morph ? lerp(i.nrm, i.nrm2, morphWeight) : i.nrm;
float2 ST = morph ? lerp(i.st,  i.st2,  morphWeight) : i.st;
float4 c  = morph ? trunc(lerp(float4(i.rgba), float4(i.rgba2), morphWeight)) : float4(i.rgba);
```

Lighting (`c` = colour in GS units 0..255, before the transform; the
`clamp` limit is the block's (255, 255, 255, alphaScale) qword):

```hlsl
// VU1L_DIFFUSE  (L[0..2] = light directions transposed: L[0] = (L0.x, L1.x, L2.x, L3.x) ...)
float4 dots = max(L[0]*N.x + L[1]*N.y + L[2]*N.z, 0);        // N·L for slots 0..3
c = clamp(L[3] + L[4]*dots.x + L[5]*dots.y + L[6]*dots.z + L[7]*dots.w, 0, L[8]);

// VU1L_SPEC2*  (e = 4, 8 or 16 by the variant)
float4 t = max(L[0]*N.x + L[1]*N.y + L[2]*N.z, 0);           // (N·L0, N·L1, N·H0, N·H1)
c = clamp(L[3] + L[4]*t.x + L[5]*t.y + L[6]*pow(t.z, e) + L[7]*pow(t.w, e), 0, L[8]);

// VU1L_SPEC4*
float4 dl = max(L[0]*N.x + L[1]*N.y + L[2]*N.z, 0);
float4 dh = pow(max(L[8]*N.x + L[9]*N.y + L[10]*N.z, 0), e);
c = clamp(L[3] + L[4]*dl.x + L[5]*dl.y + L[6]*dl.z + L[7]*dl.w
               + L[11]*dh.x + L[12]*dh.y + L[13]*dh.z + L[14]*dh.w, 0, L[15]);

// VU1L_LOCAL*  (specular only; light slot 0; no clamp; alpha 0x80)
float3 V = P - L[3].xyz;
float3 R = V - 2 * dot(V, N) * N;
float  r = max(dot(float3(L[0].x, L[1].x, L[2].x), R) * rsqrt(dot(V, V)), 0);
c = float4(trunc(L[4].rgb * pow(r, e)), 128);

// VU1L_ENVMTX  (L[0..2] = rows of the modelview rotation, L[3] = eye in model space)
float3 V  = P - L[3].xyz;   // N = the 0x40 stream here
float  iv = rsqrt(dot(V, V));
float3 R  = (V - 2 * dot(V, N) * N) * iv;
float3 Rv = R.x * L[0].xyz + R.y * L[1].xyz + R.z * L[2].xyz;
float  m  = 0.5 * rsqrt(Rv.x*Rv.x + Rv.y*Rv.y + (Rv.z + 1)*(Rv.z + 1));
ST = float2(0.5 + Rv.x * m, 0.5 - Rv.y * m);                 // sphere map, t down
c  = trunc(c * (L[4] + L[5] * min(dot(V, N) * iv, 0)));        // no clamp

// VU1L_ENVMAP  (L[0..2].xyz rows of envRot * modelview, L[3].xyz eye; w column:
//              L[0].w = -0.125, L[1].w = phase + 0.875, L[2].w = -paramA / 6, L[3].w = paramB + 0.5)
float3 V  = P - L[3].xyz;
float  iv = rsqrt(dot(V, V));
float3 Re = (V - 2 * dot(V, N) * N);
Re = Re.x * L[0].xyz + Re.y * L[1].xyz + Re.z * L[2].xyz;     // env space (y up)
float  th;                                                     // octant angle of (x, z), -1..7
if (Re.x >= Re.z) th = (Re.x >= -Re.z) ? Re.z / Re.x : 6 - Re.x / Re.z;
else              th = (Re.x >= -Re.z) ? 2 - Re.x / Re.z : 4 + Re.z / Re.x;
float  y  = Re.y * iv;
ST = float2(L[0].w * th + L[1].w, L[2].w * (y*y*y + 2*y) + L[3].w);
c  = trunc(c * (L[4] + L[5] * min(dot(V, N) * iv, 0)));

// VU1L_CONST  (L[0] = constVector, L[1] = (255, 255, 255, as), L[2] = constColor)
c = clamp(c * L[0] + L[2], 0, L[1]);

// flare 0x2F7  (L[0].rgb = (r, g, b); o = the occlusion texel's x, 0..255)
c = min(c * float4(L[0].rgb, o / 255.0), 255);               // o == 0: skip the part
```

(`trunc` = FTOI0, toward zero; the GS takes the low 8 bits of each
channel, so an unclamped result above 255 wraps on the PS2: the env / local
modes rely on their inputs staying in range.)

Transform, fog and depth:

```hlsl
float4 pc = mul(float4(P, 1), C);
o.pos = float4(pc.xy, (pc.z * viewScale.z + pc.w * viewCenter.z) * zNorm, pc.w);
                                              // D3D viewport = (winX, winY, winW, winH)
float F = viewCenter.w + viewScale.w * min(pc.w, limits.w) / pc.w;   // 0..255
o.fog   = F / 255;                            // noperspective; FGE: lerp(FOGCOL, colour, fog)
o.color = c / 128;                            // noperspective; GS units → 1.0 = 128
o.st    = ST;                                 // perspective (STQ)
```

Pixel side: texture function from TEX0.TFX (MODULATE: `Cv = Cf * Ct`,
with both in 1.0 = 128 units, alpha by TCC), then fog, then the GS state
(alpha test, blend, Z test) from the register shadows.

## 6. Packet layout and vertex formats

A part's VIF codes (as the files and `TStripBuilder` write them; FLG and
USN on every UNPACK, STMASK 0xC0C0C0C0 and STCYCL 4/4 in force):

```
UNPACK V3-32 (or V4-32) 1 -> 0xC0       the GIF tag (V3: its 4th word is not used)
UNPACK V3-32 n -> 0x00                  xyz
[UNPACK V3-32 n -> 0x20                 xyz, key frame 2]
UNPACK S-8 strips+1 -> 0x40             {count, 3 (len_k - 2) ...}
UNPACK V3-32 n -> 0x80 | V4-8 n -> 0x80 normals | RGBA
[... -> 0xA0                            key frame 2]
UNPACK_M V2-32 n -> 0x40 | UNPACK_M V3-32 n -> 0x40     S T | normals (modes 2, 3)
[... -> 0x60                            key frame 2]
NOP padding to the quadword
```

What the streams mean, by lighting mode:

| mode | 0x40 (xy / xyz) | 0x80 | S T in the shader |
|---|---|---|---|
| 0 NONE, 5 CONST | S T | RGBA | stored |
| 1 LIGHTS, 4 LOCAL | S T | normal | stored |
| 2 ENVMTX, 3 ENVMAP | normal | RGBA | generated |

Formats: 0 floats; 1 morph, 0x80 integers (so modes 0, 2, 3, 5); 2 morph,
0x80 floats (modes 1, 4); 3 xyz integers + S T / 4096 (modes 0, 1, 5). The
other pairs are broken in the microcode (the decoder flags them).

## 7. Model files

All are relocated in place: each holds offsets relative to a base word
(the address it was built for, 0), turned into pointers by the game's
`*_relocate` (32-bit pointers: the port stays 32-bit).

**ModelSet "GTM1"** (T451, `include/engine/gfx/T451_model_set.h`):

| Offset | Field |
|---|---|
| 0x00 | 'GTM1' |
| 0x04 | base (0 in the file) |
| 0x10 | u16 nParts, nPackets, nMaterials, nTex, nTexSets, nMaterialSets |
| 0x20 | parts: [nParts] command streams |
| 0x24 | packets: [nPackets] VifModel (each relocated by `vif_model_relocate`: its part data are relative to the VifModel) |
| 0x28 | materials: [nMaterials] GsMaterial records, 0x50 bytes (ambient, diffuse, specular, emission, shininess, flags (bit 0: scaled by the material scales, `GS_OVR_MAT_SCALE`), offset) - the current set |
| 0x2C | tex: [nTex] Tex1Image ('Tex1' texture sets: GsTexRegs per texture, upload records, CLUT variations / animations) |
| 0x30 | bounds: [nParts] sphere (x, y, z, r) |
| 0x34 | texSets: [nTexSets][nTex] texture LOD sets (by projected size, `modelset_tex_lod`) |
| 0x38 | materialSets: [nMaterialSets] (car colours: `modelset_select_material_set`) |
| 0x3C | anim: AnimSet (T452 tracks writing angles / frames / morph positions into the command streams) |

Command streams (`modelset_run_part` 0x002250D8 / `modelset_exec`
0x00225508; operands unaligned, little endian; branch offsets relative to
the offset field):

| Op | Operands | Effect |
|---|---|---|
| 0x00 | | end |
| 0x02 / 0x03 | u8 / u16 part | draw that part (recursive, render states set again) |
| 0x04 / 0x05 | u8 / u16 n | `model_draw(packets[n])` |
| 0x35 | u16 first, u16 count | k = floor(morph * count) (≤ count - 1); morphWeight = fraction; draw packets[first + k] (key frames k / k + 1 inside that packet) |
| 0x34 | f32 | morph position (clamped 0..1) |
| 0x06 | s16 id, u8 n, s16 off[n] | switch: k = callback->selectSwitch(id) (cars: lamps, wheel shake / steer morphs) |
| 0x07 | f32 sphere[4], u8 n, s16 off[n] | LOD: k = selectLOD(projected size) (0 = biggest = most detailed) |
| 0x08 / 0x09 | s8 / s16 | jump |
| 0x0A | u8 n, f32 xyz[n], s16 skip | frustum test: all outside one plane → skip; else clipping program on only if a point is past the guard band |
| 0x0B | | clipping program on |
| 0x0C / 0x0D / 0x0E | - / - / u8 | push / pop / matrix mode |
| 0x0F / 0x10 | f32[16] | load / multiply (m first) |
| 0x11 / 0x12 / 0x13 | f32 x3 / x3 / x4 | translate / scale / rotate (deg, axis) |
| 0x14 / 0x15 / 0x16 | f32 | rotate x / y / z |
| 0x17 / 0x18, 0x1A / 0x1B, 0x1D / 0x1E, 0x2D / 0x2E | | depth test, alpha test, destination alpha test, culling on / off |
| 0x19, 0x1C, 0x1F, 0x20, 0x2F | u8; u8 u8; u8; u8 u8; u8 | depth func, alpha func + ref, dest alpha mode, blend abcd + fix, alpha fail |
| 0x21 / 0x22 / 0x23 | u32 / - / - | fog colour / save / restore |
| 0x24, 0x25 / 0x26, 0x27, 0x28 | u32, -, f32, f32 x4 | frame write mask (inverted), depth writes off / on, depth offset, depth bias at a point |
| 0x29 / 0x2A | u8 | texture / material base of table 2 |
| 0x2B / 0x2C | u8 / u16 | texture set n: `texsel_set_table(tex[n]->tex)` (LOD set by size if enabled) |
| 0x31 | f32 x3 | env-map point-at (mode 3 azimuth) |
| 0x32 / 0x33, 0x36 / 0x37 | f32 / f32 x4 | qB10 / qB00 (scalar: (v, v, v, 0/1)) for modes 2 / 3 |

`VifPart.tex` = the `texmat_select` id: bits 0..8 = texture (1-based in the
GsTexRegs table of the current Tex1Image; 0x1FF = table 2 at its base: the
environment texture of `modelset_set_env_tex`), bits 9..15 = material
(1-based in the model's current material table; 0x7F = table 2).

**Containers**:

- car model file (cars/day|eve|night/<car>, menu/cars/<car>;
  `CarModelFile`, T456): +0 base, +4 CarModelInfo ('GTCI': axles, lamps,
  camera mounts, tyre table), +8 the body ModelSet (parts: 0 body, 1
  shadow, 4..7 extras drawn by T446), +0xC the tyre file, +0x10 the wheel
  file, +0x14 an extra block.
- wheel file 'GTTW' (wheel/<name>, menu/wheel/<name>; T447): +4 base, +0xC
  size, +0x10 flags, +0x1C the wheel ModelSet (part `wheel_file_lod`).
- tyre file 'GTTR' (tire/n<k>, menu/tire/n<k>): the tread texture and flags
  only; the tyre geometry is built at run time (section 8).
- course (courses/<region>/<name>; CourseData, T179): ModelSets at +0x04
  (the course: parts listed by the vision list point nearest the camera),
  +0x14 (env-map view), +0x24 / +0x34 (road reflections), +0x54, +0x58,
  +0x5C (sky), +0x60, +0x6C; a SpriteModel at +0x78 (billboards + its own
  packets); vision lists, runway, env, cameras, ... Course models are pre-lit
  (mode 0 with vertex colours) with specular (mode 4), env-mapped (mode 2)
  and morphing lit (0x41) objects.
- menu/course/*.cdt (course previews): +0 base, +4 a ModelSet (the course
  model, menu-coloured), +8 a table of points (a count, then floats), +0xC a
  'GTCM' block; the reader was not identified in the decomp.
- race/flare.bin: a bare VifModel (the flare disc / streak geometry).
- gadget/*.mdl: a bare ModelSet (cones; the rigid bodies of T167).

**Who draws them** (the calls the integrator will see):

| Code | Draws |
|---|---|
| T446 CarModel | body part 0 (`draw`: colour = texture frame + material set, the material scales on with the lamps' emission), part 1 the shadow squashed onto y = 0 in `constColor` / `constVector` (mode 5), parts 4..7 (ground passes: depth range shifted 0.005, y + 0.02); `beginFade` sets qAF0 = 1 - dirt (mode 2 reflections) |
| T180 RaceCarModel / T181 CarRenderCtx | the body at its matrix, the four tyres, lamp reflections; T181 picks up to 4 runway lights as GL lights 0..3 and the course's ambient |
| T447 / T449 / T450 / T462 | the wheel ModelSet part `wheel_file_lod`; the tyre packets built at run time (section 8), drawn with `PGL_CAP_SCALE` = the rim radius, mirrored by `vu0_scale(1, 1, -1)` + the other cull face |
| T179 course | model04 parts from the vision list point nearest the camera (+ SpriteModel parts), model6C, the road reflections (model24 / model34), model54 / 58 / 60 |
| T276 env map | the course's envSky + envModel into a 128x256 buffer, three 120° views, mapped through its embedded GTM1 (the map's shape): the texture of the cars' mode 2 parts (table 2, `modelset_set_env_tex`) |
| T167 rigid bodies | gadget ModelSets (cones): parts body and shadow |
| T459 / T460 prizes, T245 / T107 menu cars, T184 car icons | ModelSet parts (prize model and shadow) and car bodies with their own lights / ambient |
| T454 SpriteModel | billboards through the strip batcher, packet entries through `model_draw` |
| T159 / flare.c | race/flare.bin and the built-in sun models through `vif_model_draw` (program 0x2F7) |
| T160 race meters | a TStripBuilder dial packet, 8 passes at 1/8 alpha |

**Multi-pass streams** (command streams decoded from the US files; the
second pass of each pair has the same vertices, z writes off, and adds
`Cs Ad + Cd`: ALPHA 0x58, weighted by the alpha the first pass wrote):

- **Course** (courses/US/laguna, model04): most parts draw a NONE packet
  (pre-lit, textured) and a LOCAL packet (mode 4, textured with a grey
  noise texture of the set - e.g. tex 9 of set 4, 128 x 16 - lit by light
  0's specular x the material's: the glints on grass and asphalt) with
  `fogcol 0`; windows and other glass add an ENVMTX packet (mode 2) with
  texture set 14 (a 256 x 256 T8 sky sphere map, a 32 x 32 T4 sparkle) the
  same way.  Their textures come from the 15 sets of model04 (+ 15 smaller
  ones by LOD), 6051 blocks for the large ones: the allocator evicts and
  reloads them within a frame, so these draws use earlier copies of a set
  (renderer.md 2 and 6: the colours were garbage there - magenta grass with
  green speckles, green windows - while the registry read the sets' last
  base).  courses/US/super's road puts a gloss pass between the two: a NONE
  packet of a 32 x 32 T4 whose CLUT alpha is 0x3F..0x80, drawn with FBMSK
  0x00FFFFFF (alpha only), so the LOCAL glint (tex 28, lavender-white noise,
  material 5, shininess 4) is weighted by that map; some road parts have no
  gloss pass (Ad = the base's 0x80).
- **Wheel** (wheel/<name>, part 0 front / 1 rear with flags bit 0): the rim
  as a LIGHTS packet (texture 1 of `tex[0]`, alpha test GREATER 0), its env
  reflection (ENVMTX, texture 0x1FF = table 2 at base 0: the dynamic env map
  of `modelset_set_env_tex`, qB10 = 0.8) with DATE (DATM 0: only where the
  rim's own alpha < 0x80, not in its holes, which keep the 0x80 of
  `clear_frame_alpha`); then `texbase 1` and the face as a LIGHTS packet
  with texture 0x1FF = table 2 at base 1, the slot `RaceTirePacket::
  drawWheelTexture` fills (the still face's registers, or the blur buffer's),
  alpha blended (ALPHA 0x44, no alpha test), and its env reflection the same
  way as the rim's.

## 8. Packets the game builds

These are VifModels in RAM, decoded the same way; the integrator must
notice when they are (re)built (section 10).

- **TStripBuilder** (T448): collects up to 64 vertices per block and writes
  the block exactly as section 6 (GIF tag V4-32, strip table, positions,
  normals, masked UVs, colours, the ones present); `finish(mode,
  scaleFlags)` writes the VifModel header after the blocks. Users:
  TirePacket (T462: `packetOut` lit tread / shoulder / sidewall, PRIM 0x3C;
  `packetIn` face disc + inside in colour 0x80000000; `packetShadow`, alpha
  strip under the tyre), RaceTirePacket (T449: brake disc and caliper, heat
  glow, the hexagon wheel of LOD 2), the race meters' dial (T160).
- **Positions sent separately**: RaceTirePacket's disc and caliper quads are
  built without positions; `setDiscVertices` / `setCaliperVertices` put 4
  xyz per frame into the VIF1 list with `vu1_unpack_xyz` (UNPACK V3-32 to
  0x000) right before `model_draw`. The decoder takes them as
  `DecodeOptions::preXyz`.
- **Templates in the program**: flare.c's sun models (`g_002F5BD0`,
  `g_002F5D48`), the disc and rays built at start-up (`flare_build_disc`,
  `flare_build_rays`), T462's unit prism `g_00340CE0`.
- **Immediate primitives** use the same programs without a VifModel:
  `pgl_draw_strip_vu1` and the strip batcher (unlit entry, colours / STROW
  constant colour, strip table as above), `tri_draw16` (memory card icons:
  V4-16 xyzw + V2-16 ST, `ITOP 0x48` = `VU1L_DEFAULT` format 3, lit entry;
  PRIM TRIANGLE). Their data is on the EE; the immediate-primitive renderer
  can feed it straight to the same shaders (format 3: positions as
  integers, ST / 4096).

## 9. The decoder

`src/engine/pgl/native/vif_model.h / .cpp`, namespace `gt3vif`, C++17 with
no Windows or game dependency.

```cpp
gt3vif::Mesh mesh;
gt3vif::DecodeOptions opt;            // STMASK 0xC0C0C0C0, STCYCL 4/4, STMOD 0; preXyz
gt3vif::decodeModel(vifModel, mesh, opt);             // 32-bit: data words are pointers
gt3vif::decodeModel(vifModel, resolveFn, user, opt, mesh);   // any build
gt3vif::decodePart(data, qwc, tex, partIndex, mode, opt, mesh);  // one VifPart (appends)
```

`Mesh`: `mode`, `scaleFlags`, the header's `nVerts` / `nPrims`, `attribs`
(`ATTR_POS / MORPH / NORMAL / ST / COLOR / EXT_POS`), `vertices` (72-byte
`Vertex`: pos, pos2, nrm, nrm2, st, st2, rgba, rgba2), `indices` (triangle
list, absolute vertex numbers, each triangle starting with its kicking
vertex, strip winding normalised), `strips` (lengths), `batches` (one per
part: vertex / index / strip ranges, PRIM, `tex` id, NLOOP, REGS, flags)
and `warnings`.

It interprets the VIF codes (UNPACK S / V2 / V3 / V4 x 32 / 16 / 8 and
V4-5 with USN, STCYCL skipping and filling writes, STMASK with STROW /
STCOL, STMOD offset / difference; NOP, BASE, OFFSET, ITOP, FLUSH*, MARK,
MSKPATH3; MSCAL inside a part ends a batch; MPG / DIRECT are skipped) into
an image of a VU1 data buffer, then reads the image as the programs do: the
GIF tag, the strip table, the streams by format and lighting mode. The
triangles follow the ADC rule of the transform programs (first two
vertices of each strip: no kick) and the GS's PRIM types (strip, list, fan).
`Batch::flags` reports what does not look like a model packet (no GIF tag,
REGS not ST RGBAQ XYZF2, strip table not adding up to NLOOP, more than 64 /
32 vertices, a mode / format pair the microcode gets wrong, a stream the
mode needs not written, an MSCAL inside the data).

## 10. Integration

Where the decoder plugs in (the EE code stays the decomp's, its packet
writing replaced):

- **`model_draw`** (render.c 0x0024D048) → native: look up / decode the
  VifModel (cache), then per part, in order: call `partTexFunc(part.tex)`
  if `tex != 0` (it changes the texture / material shadows), redo the
  lighting step of `vifmodel_send_lighting` (it recomputes the block when
  `lightFlags & 7`, e.g. after `material_load`) into the lights constant
  buffer, take the GS register shadows as the render state, and draw the
  part's index range with the shader permutation (lighting program × format
  × flat / smooth × fog × texture). Statistics: `nVerts`, `nPrims`, `nParts`
  as today. `vif_model_draw` (flare.c) is the same with the flare program
  and its own constants (colour, occlusion texel).
- **PRIM**: from the batch, with `vifmodel_put_part`'s override when
  `g_002F72C0.enable` (`GsLibState`): `prim = (prim & ~primMask) |
  (primAttr & primMask)` (primMask default FGE | ABE).
- **Program state** at the draw: `vu1Prog` (0 = nothing; the cull mode of
  section 4.6; 0x320 vs 0x33E: the geometry shader `gs_vu1` is the
  microcode's clipper, always on (a no-op where the game picked 0x320):
  Sutherland-Hodgman in clip space on the raw ST and colours against
  |x|, |y| <= 2w and |z| <= w, then the projection of section 3.3 per
  vertex. Direct3D's own clipping cannot stand in: the attributes it would
  clip are already x Q and screen-linear, meaningless for a vertex behind
  the camera (the road under the bumper camera got garbage texture
  coordinates),
  `vu1LightProg` (the lighting permutation, set by `pgl_send_*`),
  `vifmodel_setup_lighting`'s colour / alpha scale (`scaleFlags`) and env
  matrices (modes 2 / 3), `morphWeight` (formats 1 / 2), the matrix block
  (`pgl_sync_matrices` / `pgl_send_matrix`: C, viewScale, viewCenter,
  limits), FOGCOL.
- **Lighting blocks**: keep `pgl_build_light_list`, `pgl_update_eye_lights`,
  `pgl_update_light_colors`, `pgl_build_env_matrix`, `pgl_build_envmap_matrix`,
  `pgl_envmap_rotate` (their VU0 asm rewritten in C++); make `pgl_send_*`
  write the same qwords into the constant buffer of section 5, and keep
  their `vu1LightProg` selection.
- **Cache**: key = the VifModel pointer (the decoded mesh holds every
  part). File models live as long as their load buffer: flush the entries
  in a buffer's range when the loader frees / reuses it (course unload,
  car model reload, menu model swaps); runtime packets (TStripBuilder) are
  rebuilt into reused work areas (`TirePacket::virtual_1`, the meters):
  invalidate from `TStripBuilder::finish` (mark the packet range), or keep
  a 64-bit hash of the part data with each entry and check it on first use
  per frame. (The PC port does both, the data hash only in turn:
  renderer.md 7.)
- **`vu1_unpack_xyz`**: record the 4 positions; the next `model_draw` passes
  them as `preXyz` (decode those small packets each frame, or cache all but
  the positions and upload 4 vertices).
- **Command streams** (`modelset_run_part`) need nothing: they call
  `model_draw` and the pgl matrix / state functions. The frustum test
  (0x0A) still saves work (it skips parts); its clipping-program switch can
  be ignored.
- **Immediate primitives** (`pgl_draw_strip_vu1`, `strip_batch_*`,
  `tri_draw16`) are the immediate-primitive renderer's; their shading is
  the unlit path (or `VU1L_DEFAULT` format 3 for `tri_draw16`) of section 5.

## 11. Verification

`tools/mdldump` (built standalone as in PORT.md into `build/tools/mdldump`)
on files extracted with `tools/gt3vol.py`:

- Every packet's decoded vertex count (Σ NLOOP) and triangle count equal
  the VifModel header's `nVerts` / `nPrims`, for all 2191 model files
  swept (cars/day, eve, night, menu/cars, courses/US, menu/course, wheel,
  menu/wheel, gadget, race/flare.bin): no mismatch, no warning.
- Rendered with `objrender.py` and looked at: a car body (cars/day/am0006,
  part 0 at LOD 0), the menu car, a wheel, the traffic cone, the flare
  disc, and courses/US/L_laguna1 (from above and in perspective, its
  vertex colours): the shapes are right.
- Winding: the face normals of the emitted triangles agree with the stored
  vertex normals on 100 % of the cone's and the wheel's lit triangles,
  99.2 % of the car's, 99.9 % of Laguna Seca's.
- `mdldump --selftest` runs synthetic packets through the VIF paths the
  files do not use: `pgl_put_const_color`'s filling write (STCYCL 0/4,
  STROW, STMASK 0x55555555), `tri_draw16`'s format 3 (V4-16 / V2-16 signed,
  PRIM TRIANGLE), morph format 1, V4-5 with a skipping write and a fan,
  positions from `vu1_unpack_xyz`.

```
tools\msvc.cmd cmake -S tools/mdldump -B build/tools/mdldump -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
tools\msvc.cmd cmake --build build/tools/mdldump
python tools/gt3vol.py extract cars/day/am0006 am0006
build\tools\mdldump\mdldump.exe am0006 -o out/am0006 --set body --part 0
python tools/mdldump/objrender.py out/am0006.obj am0006.png --yaw 215 --pitch 15 --persp
```

## 12. Open points

- The env programs read `1 / |V|` with `MFP` from an `ERLENG` issued for
  the vertex being lit (pipelined across loop iterations); the math above
  takes the evident intent (the current vertex). Not checked against
  hardware output.
- The flare program takes the occlusion texel's first lane (red) as the
  visibility; flare.c's comments suspect the stored texel is red modulated
  by the reduced alpha. Check with the flare port.
- GS rounding: Z and F are truncated (`FTOI4`), X / Y are 12.4 fixed
  point; the shaders work in floats. The `+3072` culled marker (bit 14 of
  the F word) is assumed ignored by the GS.
- Lupo 2's lighting mode 6 (projected shadow, `ref/enhanced/ported/rendering.md`
  item 6) is not in GT3's microcode; the decoder flags modes above 5.
- Morphing course objects (0x41) blend normals without renormalising, as
  the microcode does.
