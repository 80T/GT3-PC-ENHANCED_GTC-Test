# GT3 PC graphics: textures, render targets and screen effects

The texture and render-target part of the Direct3D 11 renderer: what the
game uploads and samples, how VRAM addresses resolve to native resources,
how the texture unit's registers map to D3D11, and a native recipe for every
effect that renders off screen or reads back a frame buffer.  The pgl state /
primitive API and the VU1 models have their own notes; this one stops where
a texture or a render target is chosen.

Sources: `decomp/core_gt3enhanced` (addresses are GT3 US retail), the GS
User's Manual for register and memory-layout semantics (PCSX2's GS code was
read as a cross-check, nothing copied).  Everything below about file data was
verified by decoding the disc's files with `tools/texdump`.

Contents: 1 texture sources, 2 formats in use, 3 upload / sample PSM
mismatches, 4 the decoder library, 5 the VRAM address registry, 6 sampling
(TFX, TCC, TEXA, filters, CLAMP), 7 render targets, 8 effects, 9 frame flow
and presentation, 10 open questions.

## 1. Texture sources

Almost everything textured is a **'Tex1' set** (`Tex1Image`, G001_flare.h;
`engine/pgl/tex1.cpp`): `nTex` register blocks (`GsTexRegs`: TEX0, TEX1,
MIPTBP1, MIPTBP2, CLAMP), `nXfer` upload records (`Tex1Xfer`: data,
BITBLTBUF DBP / DBW / DPSM, w, h), an optional variation table and an
optional CLUT animation table.  A set is made resident with
`VramAllocator::loadTex1` (T513, 17 callers) or `TexSet::load` (T515):
`tex1_set_vram_base` relinks TBP0 / CBP / MIPTBPn / DBP for the block it
got, `tex1_upload_packet` (or `tex1_upload_to_vram`) queues one image
transfer per record, then `gfx_set_texture_regs` (0x0024CD48) loads a
register block.

| Source | Code | Files | Texture formats seen |
|---|---|---|---|
| menu layouts | T243 `relocate_archive`, T226 `MenuImageHolder` (gunzips `ITEM_GZIPPED` entries into the menu heap), T257 `Image::virtual_2` (`loadTex1` + `tex[index]`) | `menu/<lang>/*.pmb`: texture table `{size, data}` at header +0x0C, most entries gzipped Tex1 | T4 / T8 with CT32 or CT16 CLUT, CT32 direct (9 textures); region clamp to the picture |
| image packs | T506 `image_pack_*`, then T257 / the HUD | `menu/<lang>/arcade*.imgs`, `race/<lang>/display.imgs`, `race/disp.imgs`, `menu/disp*.imgs`, `menu/opening.imgs` ("IMGS": count, first image, `{name[0x3C], index}`; images chained by `Tex1Image.size`) | T4 (CT32 / CT16 CLUT), T8 |
| loose images | callers of `loadTex1` | `advertise/startup*.img`, `menu/license_bg.img`, `menu/prize_get.img`, `race/<lang>/meter_*.img`, `race/glare.img`, `smoke.img`, `particle.img`, `reflection.img`, `tex_flare.img` | T4 (CSA 0..6, TBP 0xFF), T8, CT24 (`particle.img`, TCC 0) |
| race HUD | T154 / T157 / T158 `race_draw_image(_rect)`: UV = (CLAMP.MAXU + 1) / next power of two | `display.imgs`, `meter_*.img`, `font/menu.fnt` | T4, T8 |
| bitmap fonts | T496 / T498 `Font::begin` (`loadTex1(face->tex)`, `font_set(face, &tex[palette])`), font.c `font_begin` (TEX0_1 / TEX1_1, CLAMP_1 = 0) | `font/system.fnt`, `font/menu.fnt` ("FT01" FontSet: faces at +0x14, Tex1 per face at +0x18) | T4; palette 0 = CT16 CLUT (outlined glyphs), palette 1 = CT32 CLUT |
| kanji | font.c `font_put_kanji` 0x0024A6B0 | `font/system.kf` ("KNJI": 22x22 4-bit glyphs, 256 bytes each, 16-colour CT32 CLUTs) | T4 glyph cell, see 8.22 |
| models (GTM1) | T451 `ModelSet`: `tex[nTex]`, `texSets[LOD][nTex]`; command 0x2B / 0x2C `loadTex1` + `texsel_set_table`; `texmat_select` per VIF part | `cars/{day,eve,night}/*`, `menu/cars/*`, `courses/<lang>/*`, `wheel/*`, `menu/wheel/*`, `tire/*`, `menu/tire/*`, `race/*.prm`, `race/brake.bin`, `gadget/*.mdl` | T4 with CT32 / CT16 CLUT and CSA 0..7, T8, T4HL / T4HH, CT24 (prize env maps, TCC 0); mipmaps on a few course / wheel textures; variations (car colours); CLUT animations (course signs) |
| sprite models | T454 `SpriteModel` (one Tex1) | course files | as models |
| flare | flare.c `vif_model_draw` (`FlareWork.tex[]`), T159 | `race/flare.bin`, `glare.img`, `tex_flare.img`, `reflection.img` | T8, T4 |
| embedded env-map model | T276 `g_00339540` (a GTM1 with its own Tex1) | .rodata | its texture is overridden by the colour buffer (8.12) |
| memory card icons | T077 `icon_model_draw`: `VramAllocator::loadImage(PSMCT16, 128, 128)` | `menu/icon/*.ico`, save files | CT16, A bit forced to 1 (raw or run-length packed texels) |
| car icons | T184 `CarIconMaker` renders and downloads 64 x 32 CT32; T200 `loadImage(PSMCT32, 64, 32)` | runtime | CT32 |
| IPU pictures, menu movie | T249 `MpegImage::drawIpic`, T253 `MenuBgMovie::upload*` | IPU / MPEG output | CT32, one `gs_upload_image` per 16 x 16 macroblock (DSAX / DSAY set) |
| colour filter internals | color_filter.c | generated | CT24 16x16 curve CLUT, CT32 identity CLUT, T8 8x32 index texture |
| boot image | T002 `boot_screen_fade` | the CT24 640x448 picture the bootstrap left at VRAM 0x50000 | (no bootstrap in the port: skip the fade) |
| logger (debug) | T087 / T088 | `logger/*` | Tex1 |

Raw (non-Tex1) uploads go through `gs_upload_image` (0x00254B28: queued on
the GIF list) or `gs_upload_image_sync`; downloads through
`gs_download_image` (0x00254CB0): `GSBuffer::download` (car icons) and
`flare_readback`.

## 2. GS formats as GT3 uses them

Counted over the files extracted for this survey (every US menu layout and
image pack, the HUD, fonts, loose images, two courses, three cars, wheels,
tyres, prize models; gzipped sets included):

- **PSM**: T4 ~8,770 textures, T8 ~770, T4HL / T4HH 33, CT24 11, CT32 9.
  No CT16 / CT16S / T8H texture in the data; CT16 and CT32 appear as
  render targets sampled as textures (env map, frame buffers), T8H only in
  the afterimage (8.6).
- **CLUT**: CPSM CT32 (most) or CT16; **CSM1 only** (no CSM2, TEXCLUT unused);
  CLD 1 on every set (pgl reloads the CLUT with every texture).
- **CSA**: 0..7, mostly even.  pgl's TEX2 trick (`gfx_set_texture_regs`):
  TEX0 goes out without CLD, TEX2 = TEX0 with CLD loads the CLUT, and when
  TEX0.CSA != 0 TEX2 gets PSM T8 and CSA 0: the whole 256-entry CLUT at CBP
  is loaded and the 4-bit texture reads entries CSA x 16 .. + 15, which in
  the CSM1 16 x 16 image is the 8 x 2 block at ((CSA & 1) x 8, (CSA >> 1)
  x 2).  The even CSAs keep a set's palettes in one 8-wide column of 8 x 2
  blocks: four 16-colour palettes in one 8 x 8 CT32 block.  The kanji font
  loads its own way (CLD 1 and CSA 1 on a T4 TEX0: the 8 x 2 CLUT lands at
  entries 16..31 and is read there).
- **TEXA**: constant TA0 0, AEM 0, TA1 0x80 (`gfx_reset_texture_state`;
  nothing changes it).  So CT24 texels have alpha 0, CT16 texels alpha 0x80
  (A = 1) or 0.
- **TCC**: 1 everywhere except the CT24 textures (TCC 0); `gs_fix_tex0_tcc`
  forces TCC 0 for indexed textures when `gs_set_clut` was given a CT24
  CLUT (unused path) and TCC 1 otherwise.
- **TFX**: 0 (modulate) in every set; the code sets decal for the colour
  filter and modulate elsewhere.
- **Filters**: MMAG / MMIN linear (nearly all) or nearest (a few dozen
  model textures); LCM 0 in every set; mipmapped
  textures (MXL 1..3) use MMIN 5 (linear-mipmap-linear) with K = -1.0 or
  -2.0, L 0, MTBA 0.
- **CLAMP**: REPEAT or REGION_CLAMP (to the picture) in the data; CLAMP in
  `GSBuffer::setTexture`; REGION_REPEAT only in the colour filter.
- **Alpha**: GS alpha, 0x80 = 1.0 (`pack_rgba_floats_to_u32` scales alpha
  by 128).  Textured primitives use `GsContext.colorTex` = colour bytes x
  129 / 256, so a vertex colour 0xFF modulates by 1.0.

## 3. Upload / sample PSM mismatches

Yes, there are, and they are the rule rather than the exception: **most
'Tex1' sets upload their 4-bit and 8-bit textures and their CLUTs as PSMCT32
rectangles of buffer width 64 (DBW 1)**, laid out so that the GS memory they
fill reads back as the T4 / T8 textures (TBW 2..10) and the CSM1 CLUTs the
register blocks describe.  Examples:

| File | Upload records | Textures read from them |
|---|---|---|
| `race/glare.img` | CT32 64x1024 DBW 1 at 0; CT32 16x16 at 0x400 | T8 512x512 TBW 8 at 0, CLUT CT32 at 0x400 |
| `menu/license_bg.img` | CT32 64x1120 at 0; CT32 16x16 at 0x460 | T8 1024x512 TBW 10 (region 640x448) |
| `advertise/startupa.img` | CT32 64x864 at 0; CT32 32x32, 32x16, 16x16, 16x8, 8x8 filling 0x360..0x37F | T4 1024x512 TBW 10 **at TBP 0xFF** (not page aligned), T4 256x256, two T4 1024x64; one CLUT at CBP 5 inside the first rectangle, CSA 0/2/4/6 |
| car body set (`cars/day/ni0157` @0x55800) | CT32 64x480; CT32 32x16 at 0x1E0 | 57 T4 textures at arbitrary TBPs, TBW 2..6, CSA 0..6, CT32 and CT16 CLUTs |
| prize model (`race/g_normal_gold.prm`) | CT32 64x256; CT32 8x8 at 0x100 | a **CT24** 128x128 texture at 0 **and** T4HH / T4HL textures in the top byte of the same words |
| `font/*.fnt` | CT32 rectangles | T4 128xN TBW 2, CLUTs CT16 (palette 0) and CT32 (palette 1) |

Other sets upload in the texture's own format (`meter_*.img`: T4 160x160
DBW 4; `arcade*.imgs`: T8 WxH DBW 4..8 with CT16 8x2 CLUTs; `gt_mode*.pmb`
backgrounds: T8 640x448 / 512x512), and raw uploads (`gs_upload_image`) are
always in the sampled format, except the colour filter's generated data.

The conversion they need is the GS's local-memory layout: each format
stores a pixel at an address given by its page / block / column tables (CT32
pages 64x32, CT16 64x64, T8 128x64, T4 128x128, the block order inside a
page and the column order inside a block differ per format; T8H / T4HL /
T4HH use the CT32 layout and its top byte / nibble; the Z formats their own
block order).  `gstex::TexSpace` (4.3) does exactly that for one set: it writes
each upload into a byte image of the set's blocks with the upload's layout
and reads every texture with the texture's layout.  The space is local to
the set (its `vramBase .. vramBase + vramSize` blocks, a few hundred KB at
most); it is a format conversion of the set's own data, not a copy of the
game's VRAM.  TBP / CBP values that are not page aligned are handled the
GS's way: the block number is TBP + page x 32 + the block table entry, so a
texture may start inside a page and run into the next (startupa's TBP 0xFF
decodes correctly).

Render targets are sampled in other formats too; those cases are effects,
listed in 8 (afterimage: CT32 alpha bytes read as T8H; colour filter: CT32
read as T8 byte planes; depth copies: Z24 read as a texture; the unused
`ReplayBlur::invertDepth`: Z24 drawn as CT24).

## 4. The decoder library

`src/engine/pgl/native/`, plain C++17, namespace `gstex`, no Windows or game
dependency (tools/texdump links it alone; `tools/texdump/CMakeLists.txt`
lists the sources).  Output pixels are `uint32_t` 0xAABBGGRR (bytes R, G,
B, A: a PSMCT32 pixel, `DXGI_FORMAT_R8G8B8A8_UNORM`), alpha the GS value
(0x80 = 1.0).

### 4.1 gs_tex_format.h: formats, registers, sizes

- `enum Psm` (PSMCT32 .. PSMZ16S), `psm_name`, `psm_valid`, `psm_indexed`,
  `psm_depth`, `psm_transfer_bits` (CT24 24, T8H 8, T4HL 4 ...),
  `psm_geom` (page / block size).
- Register structs with `decode(u64)` / `encode()`: `Tex0` (also TEX2),
  `Tex1` (K signed 7.4), `Clamp`, `Texa` (defaults = pgl's), `Miptbp`,
  `TexClut`; `Transfer` (BITBLTBUF DBP / DBW / DPSM, TRXPOS, TRXREG);
  `TexRegs` (the 0x28-byte `GsTexRegs`).
- `image_bytes(psm, w, h)`, `transfer_qwc` (as
  `gs_build_image_transfer_packet`, including its quirk),
  `vram_words(psm, w, h, &alignedW)` (= `tex_vram_size`).
- Footprints: `image_blocks(psm, bp, bw, x, y, w, h)` -> `BlockRange`
  (exact min / max block), `image_block_mask(...)` (the exact block set),
  `texture_blocks(tex0)`, `clut_blocks(tex0, t8Load)`, `mip_level(tex0,
  miptbp1, miptbp2, level)`.

### 4.2 gs_tex_layout.h: the memory layout

`block_number(psm, bp, bw, x, y)`, `pixel_bit_address(psm, bp, bw, x, y)`,
the column functions `column_index32 / 16 / 8 / 4`,
`clut_csm1_position(i)` (bits 3 and 4 of the index swapped), and
`layout_self_test()` (block tables against their bit rules, column
functions are permutations, reference points of the T8 / T4 columns, every
page covers its 32 blocks).

### 4.3 gs_tex_space.h: TexSpace

`TexSpace(firstBlock, blockCount)`; `upload(Transfer, data, bytes)` (a
host -> local transfer: row-major pixels, CT24 3 bytes, 4-bit formats low
nibble first, TRXPOS offsets, short data stops it); `read(psm, bp, bw, x,
y)` (the raw field: CT24 low 24 bits, T8H top byte, T4HL / T4HH a nibble;
0 outside); `write`; `written(block)`.  A CT24 / T8H / T4Hx write keeps the
other bits of the word, as the GS does.

### 4.4 gs_tex_decode.h: texels -> RGBA8

- `expand_ct16 / expand_ct24 / expand_direct(psm, raw, texa)`: CT16 RGB
  shifted left by 3 (the GS does not replicate the high bits: white is
  0xF8), A from TEXA (TA1 for A = 1, else TA0 or 0 with AEM and RGB = 0);
  CT24 A = TA0 (0 with AEM and RGB = 0).  Z formats read as their CT
  counterparts.
- `ClutBuffer`: the GS CLUT buffer.  `load(space, tex0)` does what a write
  with CLD != 0 does (CSM1: 256 entries from the 16 x 16 image or 16 from
  the 8 x 2 image at CBP, stored at CSA x 16; CSM2: a CT16 line);
  `lookup(psm, csa, index, texa)`.  `pgl_clut_load(tex0)` gives the TEX2 pgl
  writes (the T8 trick above).
- `decode_rect(space, psm, tbp, tbw, x0, y0, w, h, clut, csa, texa, out,
  stride)`, `decode_tex0(...)`.
- `decode_region(tex0, clamp)`: the rectangle worth decoding (the clamp
  region when both axes are REGION_CLAMP, else the whole 2^TW x 2^TH).
- `decode_linear(psm, data, bytes, w, h, palette, texa, out, stride)`: an
  upload's data as is, for an upload whose format and buffer width are the
  texture's (no layout change).
- `alpha_to_unorm(rgba)`: GS alpha -> 0..255, for viewing.

### 4.5 gs_tex1.h: Tex1 sets

- `tex1_parse(file, size, offset, set)`: the file form (pointer fields are
  offsets from `offset - self`: 0 for a lone image, the GTM1 / font-set
  start for embedded sets); `tex1_from_memory(img, set)`: a set the game
  has relocated (pointers are addresses; 32-bit builds); `tex1_scan(file,
  size)`: every set in a buffer.
- `Tex1Set`: `vramBase`, `vramSize`, `tex[]` (`TexRegs`), `uploads[]`
  (`Tex1Upload`: data, bytes, `Transfer`), `variations[]` (the TEX0
  rewrites), `anims[]` (CLUT animation channels: id, mode, colours, dest,
  key palettes).
- `tex1_layout(set, space)`: all uploads laid out in a space of the set's
  blocks.  `tex1_texture_sources(set, i)`: which upload records hold
  texels (any mip level) or the CLUT of register block i (exact block sets).
  `tex1_select_variation(set, k)` (= `tex1_select_variation` 0x00248EC8).
  `tex1_decode(set, space, i, level, texa, out, full)`: one mip level of one
  texture, its CLUT loaded the pgl way, the clamp region (or `full`).

Verified: `texdump selftest` (layout rules, upload / read round trips in
every format), `texdump memtest` (the relocated in-memory form gives the
same textures as the file form: 2,700+ textures of a course, a car, the HUD,
the fonts), and by eye on decoded PNGs: menu backgrounds and logos, course
names, the HUD meters and digits, car liveries (CT32 and CT16 CLUTs with
CSA), course textures and their mip chains, the fonts (both palettes), the
kanji sheet, the flare / smoke / particle textures, the prize models' CT24
+ T4HL / T4HH packing, memory card icons.  A full course decodes in about
0.2 s; all 134 US `.pmb` layouts decode without an error.

## 5. The VRAM address registry

The renderer keeps GS VRAM addresses as handles: nothing stores GS memory
contents, but every native object that the game would have put into VRAM
is registered under the blocks it covers, so that a later TEX0 / FRAME /
ZBUF value finds it.

### 5.1 Objects

- **Upload group**: one Tex1 set upload (`tex1_upload_packet` /
  `tex1_upload_to_vram`: blocks `vramBase .. vramBase + vramSize - 1`),
  holding the live `Tex1Image *` (`tex1_from_memory`).
- **Image upload**: one `gs_upload_image(_sync)` (Transfer, data pointer,
  bytes, a hash of the data at upload time).
- **Colour target**: a native render target registered when a draw buffer
  is drawn into or cleared (FRAME: base, FBW, PSM, GS width / height, the
  field mode, the native texture).
- **Depth target**: the same for ZBUF (and for FRAME with a Z PSM, 8.3).

### 5.2 The block owner map

One entry per GS block (0x4000 entries, 256-byte granularity): the object
that last wrote the block and a serial number.  An upload marks the blocks
of its exact block set (`image_block_mask`); a draw or clear marks the
blocks of the target's FRAME rectangle (or SCISSOR); a release
(`VramAllocator::dealloc`) changes nothing (the GS keeps the bytes).  Two
layers: the **RGB / Z layer** and the **top-byte layer** (bits 24..31 of
32-bit words).  A CT32 write with FBMSK 0 and an upload in CT32, CT16, T8 or
T4 own both; a CT24 / Z24 target and a CT24 upload own only the RGB layer; a
write with FBMSK 0x00FFFFFF (alpha only), a T8H / T4HL / T4HH upload own
only the top-byte layer.  This is what keeps the afterimage planes (which
live in the unused top bytes of the CT24 frame buffers and the Z24 buffers,
8.6) and the prize models' CT24 + T4HH packing apart.

### 5.3 Resolving TEX0

At each draw with texturing on (PRIM.TME), from TEX0 / TEX1 / MIPTBPn /
CLAMP / TEXA and the CLUT state (5.4):

1. Look up the owners of the texture's footprint (TBP0's block first; the
   decode region's block set when the first owner does not cover it), in
   the top-byte layer for T8H / T4HL / T4HH, in the RGB / Z layer for the
   other formats (T8 / T4 / CT32 read both: the owner of the RGB layer,
   which a 32-bit or 4/8-bit upload owns together with the top byte; a CT32
   texture read by a draw that writes no RGB takes the top byte's owner
   when that is a colour target: only its alpha is used, renderer.md 6).
2. **Owner is a colour target** whose base is TBP0 (and TBW = FBW): sample
   the native target.  PSM CT32 / CT24 / CT16 of a target of the same
   family: the target as is, with the format's alpha (CT24: TEXA.TA0;
   CT16: TA1 / TA0 by the stored alpha's bit 7; the 5-bit colour
   quantisation is not reproduced).  T8H / T4HL / T4HH of a CT32 target:
   the target's alpha byte (or nibble) through the CLUT, in the shader
   (8.6).  Anything else: a logged reinterpretation, resolved by the effect
   recipes in 8 (none left in the code paths the game uses).
3. **Owner is a depth target** and PSM is a Z format: the depth resource's
   SRV (8.3).
4. **Owners are uploads**:
   - all of them one upload group: the group's decoded texture, keyed by
     (group identity, the TEX0 fields TBP0 TBW PSM TW TH CBP CPSM CSM CSA,
     the CLUT source, TEXA, the content generation, 5.5);
   - one image upload in the texture's own format with DBP = TBP0, DBW =
     TBW, DSAX = DSAY = 0: `decode_linear` (kanji glyphs, icons, CLUT-less
     CT32 images);
   - otherwise (macroblock uploads of IPU pictures, mixed owners): a
     `TexSpace` of the footprint's blocks, every owning upload replayed into
     it in serial order, decoded.  Cached by the owners' serials.
5. Nothing registered: a 1x1 transparent texture and a log line.

The cached native texture holds the decode region (`decode_region`) and
records which GS texels it holds (x0, y0, w, h) for the UV remap (6.6).

### 5.4 CLUT state

The renderer keeps the GS CLUT buffer's state the way `ClutBuffer` models
it: every TEX0 / TEX2 value written with CLD != 0 (pgl's TEX2 from
`gfx_set_texture_regs`, `gs_set_clut`, the kanji TEX0_2, the colour filter's
TEX0_1) records a CLUT load: (CBP, CPSM, CSM, CSA, T8- or T4-shaped, the
owner and serial of CBP's block).  CLD 2..5 (CBP0 / CBP1 caching) are
treated as loads.  A texture's decode key includes the palette it reads;
the palette is decoded from the CLUT's owner (`ClutBuffer::load` on its
space) into the buffer's 512 halfwords (CT32 entries split low / high,
CT16 entries one each), and read back in the texture's own CPSM
(docs/renderer.md section 6).

### 5.5 Caches and invalidation

- Decoded textures are cached across frames.  `VramAllocator` gives every
  released block back at the end of each frame (`unlock`, T002), so the
  game re-uploads every set it draws every frame: the registry must make an
  upload of an unchanged set cheap (identity check, no decode).
- Upload group identity: the `Tex1Image *` plus a load stamp.  Proposal: at
  `tex1_relocate` (called once per loaded set) the port writes a fresh
  64-bit stamp into the header's unused `unk_28[8]` (zero in the files), so
  a set loaded at an address an older set used is never confused with it.
- Content generation: the hash of the set's small uploads (<= 4 KB: the
  CLUT rectangles) recomputed at each upload.  CLUT animations
  (`clut_anim_apply_all` writing `ClutAnimChannel.dest`, inside a CLUT
  upload's data) and nothing else change texel data in place; variations
  (`tex1_select_variation`) rewrite TEX0, which is in the key.  A changed
  hash re-decodes only the textures whose CLUT lies in the changed upload.
- Image uploads: keyed by their data hash (they are small or one-off;
  IPU pictures and movie frames change every upload).
- A draw into blocks owned by an upload makes those textures unreachable
  (the owner map now points at the target); cached decodes stay for a
  later identical upload.  An upload over a target's blocks detaches the
  target (its contents are no longer what the GS would read there).
- Order: uploads and draws are processed in program order.  The PS2 runs
  uploads on the GIF list and draws on the VIF1 list, synchronised by
  SIGNAL / WAIT markers (`gfx_put_irq_marker`, T513's fences) so that each
  draw sees the uploads queued before it; executing the pgl calls in order
  gives the same result.  `VramAllocator::slotFor` reuses a released block
  for the next set within the same frame; the owner map handles that.  A
  set moved within the frame (evicted and reloaded at another block,
  `TexSet::load` per wheel) is uploaded at the base it had when the upload
  was recorded (the PS2 wrote the transfer's BITBLTBUF into the packet then),
  and each block remembers the base of the copy that wrote it: draws
  recorded before the move read the earlier copy (renderer.md 2 and 6).
- Downloads (`gs_download_image`): the native target is resolved to its GS
  size (7.2) and copied back through a staging texture; the bytes are
  converted to the requested PSM (CT32 in both callers).

### 5.6 CLUT animations and variations

`clut_anim_step` / `clut_anim_apply_all` (models: T446, T179 every frame;
sprite models) blend key palettes into the CLUT's bytes in RAM; the next
`loadTex1` re-uploads them (every frame, 5.5).  The registry sees a new
content generation and re-decodes the textures that use that CLUT.  If that
turns out costly (courses: 2..4 channels of 16 colours per animated set),
the alternative is indexed native textures (R8_UINT indices + a 256 x 1
palette texture, the lookup and bilinear filtering in the shader); the
RGBA path is the pass-1 choice.

## 6. Sampling

### 6.1 Alpha

Textures and render targets keep **GS alpha bytes** (0x80 = 1.0) in
R8G8B8A8_UNORM.  The shader converts: `a_gs = tex.a * 255 / 128` wherever
alpha is a weight (texture function, alpha test reference in GS units,
blend factor As / Ad).  Destination alpha written by a draw is the GS byte /
255 again.  The blend agent decides how As > 1.0 (bytes 0x81..0xFF) is
honoured (they occur: the flare queries write alpha 2.0).

### 6.2 Texture function (TEX0.TFX, TCC)

With Ct the texel and Cf the vertex colour (`GsContext.colorTex` for
textured primitives), all in GS bytes, results clamped to 0..255:

| TFX | RGB | A, TCC 1 | A, TCC 0 |
|---|---|---|---|
| 0 modulate | Ct x Cf / 128 | At x Af / 128 | Af |
| 1 decal | Ct | At | Af |
| 2 highlight | Ct x Cf / 128 + Af | At + Af | Af |
| 3 highlight2 | Ct x Cf / 128 + Af | At | Af |

GT3 uses modulate (all data, the menus, the effects) and decal (colour
filter internals).  TCC 0 occurs with the CT24 textures; TCC 1 with a CT24
render target as texture means alpha = TA0 = 0 (e.g. `PGL_FX_ALPHA_A_INTO_B`
on a CT24 buffer changes nothing; unused).

### 6.3 TEXA

Applied at decode time for uploads (`expand_ct16 / ct24`) and in the shader
for render targets of CT24 / CT16 format; constant in GT3 (TA0 0, AEM 0,
TA1 0x80), so the renderer may bake it.

### 6.4 Filtering (TEX1)

- MMAG / MMIN 0 nearest, 1 linear: point / linear D3D11 samplers.
- Mipmaps (MXL > 0): MMIN 2..5 = nearest-mip-nearest, nearest-mip-linear,
  linear-mip-nearest, linear-mip-linear.  The levels are separate GS
  textures (MIPTBP1 / MIPTBP2); the decoder produces each level
  (`tex1_decode(level)`), uploaded as the native mip chain.
- LOD: the GS does not use screen-space derivatives.  LOD = K (LCM 1, and
  the 2D sprites with Q = 1), else LOD = (log2(1 / Q) << L) + K with Q = 1
  / w per pixel; the magnification filter applies at LOD <= 0, the
  minification filter above; levels clamp to MXL.  `gfx_set_texture_regs`
  adds `GsContext.lodBias << L` to K.  The faithful shader computes this
  LOD from the interpolated Q and samples with `SampleLevel`; a
  derivative-based LOD (with K as `MipLODBias`) is the enhancement option
  for the later texture-filtering pass.

### 6.5 Wrap modes (CLAMP)

| WMS / WMT | GS | D3D11 |
|---|---|---|
| 0 REPEAT | wrap at 2^TW | the native texture holds the whole 2^TW x 2^TH space; `WRAP` |
| 1 CLAMP | clamp to 0 .. 2^TW - 1 | whole space; `CLAMP` |
| 2 REGION_CLAMP | clamp to MINU .. MAXU | the native texture holds only the region; `CLAMP`, UV remapped (6.6); bilinear taps clamp at the region edge on both |
| 3 REGION_REPEAT | u = (u & MINU) \| MAXU | integer addressing in the shader (`Load`); only the colour filter, which has its own recipe |

One axis REGION_CLAMP and the other not: the native texture holds the
region on that axis and the whole space on the other.

### 6.6 Texture coordinates

ST (STQ, perspective) and UV (FST, 12.4 texels) are in texels of the GS
space 2^TW x 2^TH (ST normalised to it).  A native texture holding GS texels
(x0, y0, w, h) of that space is sampled at `((u_texels - x0) / w, (v_texels
- y0) / h)`; for a render target, (x0, y0, w, h) = (0, 0, GS width, GS
height) of the target, whatever its native size (so 640 x 224 field
buffers drawn at output resolution are sampled correctly).  Texel centres:
the GS samples texel i at i + 0.5 like D3D; GS pixel centres are integer
coordinates (the primitives agent's XYOFFSET mapping shifts them).  Effects
that copy 1:1 must land texel centres on pixel centres (or use `Load`) to
stay sharp.

## 7. Render targets

### 7.1 VRAM layout (RenderContext::init, T512)

| Buffer | PSM | GS size | Word address | Role |
|---|---|---|---|---|
| `frame.buf[0]` | CT24 | 640 x 224, field mode HALF_CUR | 0 | draw / display buffer (one field) |
| `frame.buf[1]` | CT24 | 640 x 224 | 0x46000 | the other one |
| `unkAC4.buf[0]` | Z24 | 640 x 448, FRAME_SHIFT | 0 | depth of the 3D views (races, menu cars) |
| `unkAC4.buf[1]` | Z24 | 640 x 448 | 0x23000 | the other one |
| `bufB38` | Z24 | 640 x 224 | 0x23000 | depth of sub views / menu car renders; the replay blur's level chain as CT24 |
| `unkB70` | - | 0x800 words | 0x69000 | the kanji glyph cell |

All of 0..0x69000 is one allocation; the two pairs flip together every
frame (T002), so a frame's depth buffer overlaps the colour buffer being
drawn (the scene is drawn into a separate `screen` and only copied into the
frame buffer once the depth is no longer needed).  Everything else is
allocated per frame from `VramAllocator` (GSBuffer(psm, w, h),
`allocVram`) and freed at the end of the effect.  Native: one resource per
(base, family, size) target; the overlaps are the owner map's business
(5.2), not aliasing of native memory.

### 7.2 Native sizes and formats

- Every target is drawn at the output scale s = output height / 448: a GS
  target of W x H is (W x s) x (H' x s) with H' = 2H for the half-height
  field buffers (the 448-line frame).  The race `screen` (640 x 448) and the
  field buffer (640 x 224 field) are then the same native size.
- Targets the CPU reads back (car icons 64 x 32, the flare result strip
  64 x 8) are resolved to their GS size before the download.
- CT32 / CT24: R8G8B8A8_UNORM (CT24's alpha is never written: FBMSK, and
  reads as TA0).  CT16 (env maps 256 x 256 / 64 x 64): R8G8B8A8_UNORM too;
  writes keep alpha bit 7 semantics when sampled (6.3).  Z24 / Z32:
  D24_UNORM_S8_UINT (or D32_FLOAT with the depth agent's mapping), with an
  SRV for the depth reads of 8.3.
- FRAME.FBMSK: the game only uses byte masks (0xFF000000, 0x00FFFFFF, 0xFF,
  0xFF00, 0xFF0000, 0, all), which map to `RenderTargetWriteMask`.
- SCANMSK (`gs_set_scanmask_mode`): ghost cars (T131, rows of the other
  field) and the wheel blur (T449, even rows).  Native: `discard` on the
  parity of the GS row (`floor(SV_Position.y / s)`); the ghost cars no
  longer use it (half strength, renderer.md "Fields").  The wheel blur's buffer (at
  most 64 wide) is switched to GS resolution at its first scan-masked draw
  (renderer.md 5): its rows are averaged by a 2:1 shrink afterwards, which
  only gives the mean of a still and a turned row at s = 1.
- Sampling the bound target (`write_alpha_depth`, the reflection blur,
  the weather drops, Blur2D, `pfx_color_correction`): D3D11 cannot bind a
  resource as SRV and RTV at once.  When colour writes are off, unbind the
  RTV; otherwise copy the target (or the read rectangle) to a scratch
  texture first.  The GS's in-place read-modify-write per 64-pixel strip
  approximates "read the old contents" for these effects, so the copy is the
  faithful choice.

### 7.3 GSBuffer operations (T514)

`setDrawBuffer` (FRAME, field mode, scan mask, rectangle as scissor /
viewport), `setTargetB` (tgtB for gfx_frame_effect / gs_clear_buffers bit
2), `setZBuffer`, `setTexture` (TBW = width rounded up to 64, TW / TH the
size's next powers of two, CLAMP both axes, modulate, no mipmaps),
`makeTexRegs` (the same as a GsTexRegs), `blit(src, mag, min)` (stretched
copy, no blend / tests), `blitField` (8.2), `download`, `upload` (no
callers), `loadTex1` (no callers).

## 8. Effects

Every effect below draws into an off-screen target, samples a frame buffer
or reads VRAM in another format.  "Native" is the D3D11 recipe; FIX blends
use the blend factor (`OMSetBlendState(..., blendFactor)`).

### 8.1 gfx_frame_effect (0x00253828)

Full-screen copies between the draw buffer A (tgtA) and target B (tgtB) in
64-pixel sprite strips (`gs_copy_target`: TEX0 = the source buffer as a
1024 x 1024 texture, REPEAT, LCM nearest, UVs scaled + 6/16 texel), FIX =
amount x 128:

| Kind | GS | Used by |
|---|---|---|
| 0 ADD_A_TO_B | B += A x f (f < 0: B -= A x -f) | - |
| 1 COPY_A_TO_B | B = A x f | - |
| 2 ADD_WHITE | B += white x f | - |
| 3 SCALE_B | B = B x f | - |
| 4 COPY_B_TO_A | A = B x f (integer XYOFFSET) | T181 env-map glows (f 1.0) |
| 5 MIX_A_INTO_B | B = lerp(B, A, f) | T131 camera trail, T245 / T058 / T145 accumulation |
| 6 ALPHA_A_INTO_B | B = lerp(B, A, A.alpha) | - |
| 7 / 8 FIELD_BLEND(_INV) | deflicker: lines r, r + 1 of A averaged, then lerp by f | - |

Native: a full-screen triangle sampling the source target (point filter,
1:1) into the destination with the blend state (B += A x f: src
BLEND_FACTOR, dest ONE, op ADD; B -= A x f: op REV_SUBTRACT; B = A x f:
src BLEND_FACTOR, dest ZERO; lerp(B, A, f): src BLEND_FACTOR, dest
INV_BLEND_FACTOR; lerp by A's alpha: SRC_ALPHA scaled to GS units, 6.1),
FBMSK as the write mask (T131's trail writes RGB only).  Kinds 7 / 8
reduce to kind 5 in progressive output.

### 8.2 blitField and the field modes (T514 0x00244680)

The race / menu 3D `screen` (640 x 448, FRAME_SHIFT: shifted by half a
pixel on odd fields) is drawn into the 640 x 224 field buffer, 2:1 in v,
bilinear: on the even field (`gs_get_field_mode() == HALF_EVEN`) as two
passes v-shifted by +-(1 - t) texels, the second blended 50 / 50; otherwise
(`half` = `RenderContext.unk18` = 1) as one pass shifted by half a texel;
t = `unk14` = 0.5.  A field-phase-aware vertical filter.  Native: the
field buffer and `screen` have the same native size, so blitField is one
unshifted copy (1:1, roadmap item 3); the half-line field offsets
(`gfx_set_field_mode`, `XYOFFSET.OFY + field x 8`) are dropped
(renderer.md, "Fields").

### 8.3 Depth copies (Z24 as a texture)

`GSBuffer depth(buf, PSMZ24); depth.blit(unkAC4.cur)` (T131): the scene's
640 x 448 Z buffer sampled as a PSMZ24 texture and drawn into a 640 x 224
buffer whose FRAME.PSM is Z24, i.e. the depth copied (nearest) into a
second depth buffer used by the rain, smoke and flare passes.  T173 does the
same into a smaller Z buffer (smoke at reduced resolution), T107 / T058 /
T245 / T077 use bufB38 or their own Z24.  Native: a full-screen pass that
reads the source depth SRV and writes SV_Depth into the destination DSV
(colour off); at equal native sizes it is a `CopyResource` (or the same
resource reused).  Z values are copied as they are (the depth agent owns the
Z mapping).

### 8.4 Replay depth of field (T182 ReplayBlur, replays with course flag 0x100)

`buildLevels`: the screen downsampled into a chain of 5 levels laid out in
a 640 x 224 CT24 buffer (bufB38's VRAM: 320x224, 320x112, 160x112, 160x56,
160x56), bilinear, region-clamped per level.  `draw`: for the near side,
from the blurriest level down, each level stretched over the screen as a
sprite at the depth of its circle of confusion, depth-tested (LEQUAL, no
depth writes) against the scene depth, alpha = fraction of the octave;
then the sharp screen (blitField) at the last near depth; then the far side
levels at their distances.  Native: the same passes: a mip-like chain of
render targets (or one target with the same sub-rectangles) and full-screen
sprites with a constant depth (the sprite Z from `pgl_set_sprite_depth`),
depth test against the copied depth, alpha blending.  `drawZ` /
`invertDepth` (Z24 inverted by drawing it as CT24 with a subtractive
blend) and `drawFrameDepth` have no callers.

### 8.5 Colour correction (color_filter.c `pfx_color_correction`)

GS: a per-channel tone curve (a 256-entry CT24 CLUT: for input byte v,
B(t) = t^2 + 2c t (1 - t) per channel with t = clamp((v / 255 - lo) / (hi -
lo), 0, 1), entry 0 unclamped) and a saturation matrix (keep = saturation
x 128, rest = 128 - keep, luma weights 0.3 / 0.59 / 0.11).  The frame's CT32
pages are read as PSMT8 (128 x 64 bytes per page) so that three sprite
passes can pick the R, B and G bytes of every pixel as CLUT indices; each
pass's CLUT colour is modulated by one matrix column and added into a work
page; context-2 sprites then use the work page as a CLUT for an 8 x 32 T8
index texture (`g_00345590`) to write the pixels back in frame order; a VU1
micro program (0x763) repeats it per page.  Native: one full-screen pass
from a copy of the frame: `out_c = clamp(sum_k M[c][k] x curve_c(in_k) /
128)` for c, k in {R, G, B} (each output channel's curve applied to every
input channel, then the matrix; M = keep x I + rest x luma rows), RGB
written, alpha kept.  The curve as a 256 x 1 texture (or computed
in the shader); the GS clamps after each additive pass, so clamp the three
partial sums the same way (R pass, then + B, then + G) for exactness.

### 8.6 Afterimage (T131 captureAfterimage / drawAfterimage)

GS: `pfx_split_rgb_planes` (micro program 0x78D) reads the last displayed
frame buffer's bytes as T8 through an identity CLUT and writes R, G and B
into the **alpha bytes** of three 640 x 224 planes at `vramB74` + 0,
0x23000, 0x46000, i.e. the unused top bytes of the CT24 frame buffers and
Z24 buffers.  `drawAfterimage` reads each plane as a PSMT8H texture (the
top byte as index) through an identity CT32 CLUT, writes it into one colour
channel of the frame (write masks 0xFF / 0xFF00 / 0xFF0000: FBMSK
0xFFFFFF00 / 0xFFFF00FF / 0xFF00FFFF) with blend (Cs - Cd) x FIX + Cd, FIX
= afterimage^3 (afterimage = 1 - (1/60) / time, decaying).  Native (the
generic path): the split copies the frame's native pixels into a capture
and registers its R, G, B as three plane objects owning the top-byte layer
of the planes' blocks (renderer.md 6); the three PSMT8H draws stay the
game's, each plane read through the CLUT (a palette pass) and written into
its channel with the write mask and the FIX blend.  The enhanced engine's
half-line field correction is removed (progressive).

### 8.7 Camera frame effect (T131, `cm->frameEffect`)

B = the frame being drawn (`buf->setTargetB()`), A = the other frame buffer
(the previous frame, still on screen), `gfx_frame_effect(MIX_A_INTO_B,
min(level, 0.8))` with RGB writes: a motion trail.  Native: requires the
previous frame's target to survive (keep both frame targets, present from
them, 9); a full-screen lerp with blend factor.

### 8.8 Vignette (T183, replays with course flag 0x40)

A 32-segment ring (strip) between two radii, colour0 inside, colour1
outside, alpha blended over the screen in an ortho -320..320 x -224..224
view.  No off-screen work: ordinary geometry.  PC: not drawn (removed at the
user's request, 2026-10-20: T131 no longer calls `race_draw_vignette`; T183
stays, unused).

### 8.9 Menu blur (T254 Blur2D)

The screen (field buffer) drawn 8 times into a CT32 copy at 1/8 each
(Cs x FIX + Cd, FIX 0.125) with sub-pixel offsets g_003372B0 x (w, h),
region-clamped to the screen, nearest; the copy drawn back.  Native: an 8-tap
blur shader with the same offsets in GS pixels (scaled by s), from a copy
of the target, written back.

### 8.10 Road reflections (T131 drawReflections)

Model34 writes the screen's alpha (the reflecting areas); the reflected
course, mirrored cars and lamp reflections go into a reflW x reflH CT32
target with its own Z24; blurred `reflBlur` times by drawing the target
over itself shifted +-0.5 texel, bilinear (in-place feedback); then
stretched over the screen with the blend `CourseEnv.reflBlend` (course data,
FIX 0; a destination-alpha mode such as 0x58 Cs x Ad + Cd would use the
alpha model34 wrote).  Native: the target at s x size; each blur
pass ping-pongs between two targets (copy-then-draw, 7.2); the final draw
as given.

### 8.11 Rear-view mirror (T131 drawMirror)

A sub-rectangle of `screen` (GSBuffer copy + setRect): the mirrored view
drawn with its own projection into that viewport, black frame lines, depth
cleared to the nearest afterwards.  Native: viewport + scissor on the screen
target; nothing off screen except the blank 64 x 64 CT16 env map.

### 8.12 Dynamic environment map (T276 envmapRender, T277)

Three 120-degree views of the sky and nearby course parts are rendered into
a 128 x 256 CT32 colour + Z24 target (placed at `screen.addr` before the
screen is drawn: VRAM reuse), each drawn into the 256 x 256 CT16 env map
through the embedded GTM1 model (its texture replaced by the colour target:
`color.setTexture()` + `modelset_set_env_tex(NULL)`).  The cars sample the
env map (`modelset_set_env_tex(makeTexRegs)`).  Native: the same render-to-
texture passes (the env map at s x 256 or a fixed higher size); the CT16
target reads alpha as 0x80 / 0 by bit 7 (it is cleared to 0x80000000).

### 8.13 Car lamp glows in the env map (T181 drawGlows)

When the runway has lights: envBuf copied into a car's own env texture
(`gfx_frame_effect(COPY_B_TO_A, 1.0)`), glows (fans with alpha) drawn into
it, RGB only.  Native: CopyResource + draws.

### 8.14 Lens flare occlusion (flare.c, T159 FlareClass)

GS: per light, `flare_occlusion_query` clears the alpha of a box of the
screen, draws the light's square or octagon with alpha 2.0 (0xFF)
depth-tested at the light's depth (alpha writes only), copies the box into
the 64 x 64 work target, reduces it to one texel by repeated bilinear
halving (prebuilt packets for 2..17), and stores `red(255) x c x avgAlpha`
(modulate, blend Cs x As; c = scale x w x h / 4, scale = the caller's scale
/ size^2, x 4 / pi for octagons) into texel q of the 64 x 8 result strip.
`flare_readback` downloads the strip within the frame (the VIF1 / GIF lists
stall for it) and the VU1 flare models read the results (UNPACK V4-8) as
their intensity.  Native: draw each query shape with a D3D11 occlusion
query (depth test against the scene depth, colour writes off), visible
fraction f = samples passed / samples of the box; `flare_readback` fills
`results[q]` = the texel the GS would have stored: R = min(255, min(255,
255 x c8 / 128) x a8 / 128) with c8 = (round(c x 255) x 129) >> 8 (the
textured-primitive colour of `pgl_color4f(c, c, c, 1)`) and a8 = 255 x f
(the box's average alpha), G = B = 0, A = a8 (the work texel is the red
flare_init drew, its alpha the reduced visibility).  Waiting for the queries in-frame
matches the PS2; a one-frame-late fallback is wrong (texel q is the frame's
q-th query, and the lights queried change between frames: the port shipped
it and the glows took other lights' visibility, renderer.md 8).  The work /
strip targets then need no native counterpart.  (The port keeps the generic
path, exact on GS pixels: each halving reads the previous one through a
TEXFLUSH, which ends the executor's batch; the work buffer and the strip
are drawn at GS resolution, so the halvings' taps fall midway between two GS
pixels as on the GS and the copy takes the screen at the GS pixel centres;
the store's Cs As keeps As = 2.0 (the shader multiplies).  renderer.md 3, 5,
8.)

### 8.15 Smoke at reduced resolution (T173 RaceSmoke::render)

When `CourseEnv.smokeDivX / Y` > 1: the depth copied into a (W / divX) x
(H / divY) Z24 (8.3), the particles drawn into a CT32 target of that size,
then added over the frame (Cs x FIX + Cd, FIX 1.0).  Native: the same at s x
the reduced size (an intentional low-resolution pass: keep its ratio).  Only
`route5` / `r_route5` (wet) and the four `L_circle*` licence tests have
divisors 2 / 2 (US course files); every other course, the dirt ones too,
draws the smoke straight into the frame buffer against the 640 x 224 depth
copy (renderer.md 5 and 8: the depth geometry rule that fixed its flicker).

### 8.16 Rain / weather refraction (particles.c weather_draw)

Each drop is a 6-vertex strip textured with **the draw buffer itself**
(tgtA as TEX0, region-clamped to its size), UVs a slightly smaller footprint
around the drop's screen position: a refraction look, blended 50 % (FIX
0.5).  Each drop's packet starts with a TEXFLUSH, so each reads what the
drops before it drew.  Native: the drops sample the target's copy, which a
TEXFLUSH brings up to date before the next drop (pgl_exec's batch ends);
the copy is refreshed only where the target was written since (each draw's
vertex bounds within its scissor, `target_written_rect`; a CopySubresource
box, or with MSAA a resolve pass scissored to it, `ps_color_resolve`).
Copied whole at each drop, the screen at 1080p with MSAA 8x a few hundred
times a frame, the replays crawled (user report).  Depth copies and targets
written without a known place (`gen++` elsewhere) are still refreshed whole.
`GT3PC_PGL_STATS=N` counts the whole and partial refreshes.
Still, a refresh and a draw of its own per drop - a GPU write-then-read
dependency each - kept replays at ~10 ms of GPU a frame (user report, the
second).  So a TEXFLUSH no longer ends such a batch by itself: it records
what the next primitives must see, the target's part written since the copy
and the batch's vertices so far (`g_fbRect`), and a primitive joins the batch
when its texture coordinates (in the target's native pixels, 2 around) read
none of it - the copy holds what it reads - else the batch is drawn and the
copy refreshed there first (`feedback_check`).  The drops rarely overlap: the
weather pass is one copy refresh and one draw; overlapping drops, the flare
query's halvings and blurs over themselves still see what was drawn before.
Measured then (`GT3PC_GPU_SECTIONS`, user, the third report): the drops 6.3
ms of GPU a frame, ~150 whole copies a frame.  Two faults: `resample` (a
target re-made at a new output scale) made MSAA colour targets without a
texture view, so every refresh resolved the whole 8x target; and the written
part was one bounding box, the drops spread over the screen, so a third of
the ~380 drops met it.  Now resampled targets are readable as created ones,
and `g_fbRects` keeps each run between two TEXFLUSHes as its own rectangle.

### 8.17 Menu 3D accumulation (T245 CarRender, T058 trophy ring, T145 mini prize, T107 cars item)

The model is drawn N times (2..7) into a CT32 copy of the screen with
sub-pixel jitter (`gfx_offset_xy`) or a turning eye (depth of field), each
pass mixed into an accumulation target with weight 1 / (i + 1)
(MIX_A_INTO_B: a running mean), the mean blended over the screen.  T107
draws the cars into a 640 x 448 copy of the screen (`back`, the depth in
`unkAC4.cur`) and copies it back with `blitField(back, 1, 0.8)` (8.2).
Native: the same passes (they are
supersampling; MSAA can replace them later).  The HUD needles and the
steering sign (T160, T157) use the same jitter with alpha blending straight
into the frame.

### 8.18 Tyre motion blur (T449)

Tread: the tread texture rendered into a 128 x 64 CT32 target, then drawn
over itself shifted along s with 50 % blend, halving the shift per pass.
Wheel face: the face drawn still on every row and rotated on the odd rows
(SCANMSK even) of a w x 3w target at double height, shrunk back to w x w
with bilinear filtering (the average of both), repeated with half the angle.
Native: render-to-texture; the scan-mask interleave + 2:1 shrink equals a
50 / 50 blend of the two draws, which is the simpler recipe (or keep the
interleave with GS-row discard, 7.2).  Done: the game's passes as they are,
the face buffer at GS resolution (7.2): at the output scale the shrink's
bilinear taps (two native rows) sat inside one GS row and the face came out
striped.  The shrink samples at v = w + 1 + 2y texels (the call's + 0.5 and
the sprite's + 8 / 16), i.e. on the boundary of rows w + 2y and w + 2y + 1:
the GS takes their mean.  The still face is the wheel model's
`texSets[wheel_file_lod * 3 + level][1]` (`tex[1]`), a `TexSet` per wheel;
the blur renders from that set's second register block (the same texels,
CSA 2: a palette that differs only in the colour of its transparent entry -
wheel/mi0099: 92 / 95 / 97 instead of 68 grey - so the blur's bilinear edges
fade towards the spokes' colour).

### 8.19 write_alpha_depth (T131)

The screen sampled as a texture into itself with no colour writes (FBMSK
all), alpha test NOTEQUAL 0, depth test ALWAYS at sprite Z 1: depth only
where model58 wrote alpha.  Native: RTV unbound, the screen's SRV sampled,
`discard` on alpha 0, depth written.

### 8.20 Memory card icon renderer (T077 icon_model_render)

The icon mesh (CT16 128 x 128 texture, A forced 1) drawn into a 256 x 256
CT32 + Z24 target, shrunk bilinear to 128 x 128, that drawn as a quad
(alpha blended, colour x 2).  Native: as given (supersampling).

### 8.21 Car icons (T184 CarIconMaker)

Each car drawn at 128 x 64 into its target, reduced into a 64 x 32 CT32
target, `GSBuffer::download` (CT32) into the icon's pixels, which T200 later
uploads with `loadImage(PSMCT32, 64, 32)`.  Native: render, resolve to
64 x 32, staging copy, the bytes in CT32 order with GS alpha.  All icons
are made in one frame; the allocator puts car i's 64 x 32 buffer at the
base of car i - 1's 128 x 128 reflection buffer, so the download reads the
block's owner, not the target used there last; the mask is drawn alpha-only
(FBMSK 0x00FFFFFF) and its CT32 read by the alpha-only halving resolves
through the owner of its top bytes (renderer.md 5, 6 and 8).

### 8.22 Kanji glyphs (font.c font_put_kanji)

Per character: the glyph (22 x 22 4-bit) uploaded as PSMT4 into the cell
at `unkB70` (DBW 2), the 16-colour CT32 CLUT once per packet as an 8 x 2
image 0x1F blocks above, then a context-2 sprite with TEX0_2 = T4 128 x 128
TBW 2, CBP = cell + 0x1F, CSA 1, CLD 1, TCC 1, TEX1_2 = 0x61 (LCM, linear),
CLAMP_2 region 0..w-1 / 0..h-1, alpha blend.  Native: the registry's linear
path (5.3) keyed by the glyph's data pointer and the palette: in effect a
per-glyph cache; a kanji atlas is a later optimisation.

### 8.23 IPU pictures and the menu movie (T249, T253)

The decoded picture is uploaded into a CT32 GSBuffer one 16 x 16 macroblock
at a time (`gs_upload_image` with DSAX / DSAY; the picture rows of blocks,
the movie columns of blocks), interlaced movies only the shown field's half.
Native: the media layer should register one image upload for the whole
picture (the registry's composition path handles the per-macroblock form,
at a cost).

### 8.24 Transitions and screen masks (T005, T246)

Fades, comb-out, line-out: flat sprites with blending over the field buffer
(and the menu mask colour); no off-screen work.  The bootstrap's boot-image
fade (T002) has no source image in the port: skip it.

## 9. Frame flow and presentation

- The render thread (T002) per frame: `gfx_begin_packet`, `vram.lock()`,
  `object->render(frame.buf[cur])`, `vram.unlock()` (every released block
  freed), `gfx_end_packet`; then `gfx_sync_pipeline` (waits for the previous
  packet), `gfx_vsync`, `frame.cur ^= 1`, `frame.buf[cur].setDisplay()`
  (DISPFB2 / DISPLAY2: the buffer the previous packet finished), kick the
  new packet.  So the PS2 shows a frame one frame after it was built.
- Menus draw straight into the 640 x 224 field buffer (HALF_CUR: y in
  12.3, the field's half-line offset); races draw the 3D view into
  `screen` (640 x 448), copy it with blitField, then the HUD into the field
  buffer.
- Display: interlaced NTSC, FFMD 1 (field mode): each field buffer is shown
  as one 224-line field (DISPLAY2 MAGV 1, DH 224); `gfx_set_display_offset`
  (screen-adjust options) moves DISPLAY2 DX / DY; `gfx_set_display_enable(0)`
  blanks.
- Native: two persistent frame targets mirroring `frame.buf[0/1]` (at
  output scale, 448-line logical height), the draw buffer chosen as the
  game chooses it.  At the end of `render` present the target just drawn
  (no extra frame of latency), keeping the other one intact for the
  afterimage (8.6) and the camera trail (8.7).  Present: the target scaled
  into the largest 4:3 rectangle of the window's client area (bars black),
  linear filtering, through a final full-screen pass into the swap chain's
  back buffer; display enable off = black; the display offset ignored (or a
  pixel shift, later).
- Clears: `gs_clear_buffers` draws 64-pixel-wide sprites with TEST / ZBUF
  set for the clear (colour, depth, or depth with alpha test NEVER and AFAIL
  ZB_ONLY); native `ClearRenderTargetView` / `ClearDepthStencilView` within
  the scissor (or a full-screen quad when FBMSK is not all bits).

## 10. Open questions

- As > 0x80 in blending (the flare query writes alpha 2.0, alpha-only and
  only into the box that is then averaged): exact only if the blend path
  keeps GS alpha above 1.0; the flare recipe (8.14) sidesteps it.
- CT16 render targets: 5-bit colour quantisation and dithering (DTHE) are
  not reproduced; only the env maps are CT16 and they feed reflections.
- The GS's LOD from Q versus hardware derivatives (6.4): faithful by
  default, the enhancement later; only a handful of textures have mips.
- The VU1 flare program's use of the result texels (which channel scales
  what) is the VU1 agent's to confirm; 8.14 gives the bytes the GS stored.
- `PGL_FX_FIELD_BLEND` (no callers) and the field offsets are gone in the
  progressive port; the ghost cars are drawn at half strength
  (renderer.md, "Fields").
- The registry's composition path (5.3, mixed owners) covers the IPU
  macroblock uploads; if the media layer registers whole pictures it is
  only a safety net.
