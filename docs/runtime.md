# GT3 PC: the C and C++ runtime

What the decompiled program takes from its C and C++ runtime, where the
MSVC CRT behaves differently, and every GCC / egcs-only construct in
`src/game`, `src/engine` and `include` with its MSVC replacement. The
system side (kernel, files, pads, cards) is in [system.md](system.md).

Paths are relative to `decomp/core_gt3enhanced`. "Sites" are call sites
found by `ref/funcs.json` (callers) or by the scripts in
`build/scratch/system/` (`gnu.py`, `sdkcalls.txt`, `ctors.txt`). The C++
sources were syntax-checked with GCC 14 `-std=gnu++14 -fpermissive
-Wpedantic` (`permissive.sh`): **no diagnostic needed `-fpermissive`**; the
only pedantic findings are the statement expressions, case ranges and the
VLA listed in section 8. The game's C++ is otherwise standard; what MSVC
needs is the GCC extension syntax and the runtime semantics below.

## 1. What the PS2 build links, and what the port uses

| PS2 piece | Files | Port |
|---|---|---|
| crt0, collect2 tables | `src/runtime/startup/crt0.S`, `collect2_ctors.c` | not compiled: the platform entry point; MSVC runs the static constructors (7.4) |
| libgcc: 64-bit integer helpers, soft double (`dp_bit.c`), soft float (`fp_bit.c`), DWARF frames (`frame.c`), EH (`T542_eh.c`) | `src/runtime/libgcc/` | not compiled (MSVC does it in hardware / `_alldiv` etc.); keep the semantics of `fptoui` (5.3) |
| libm (`sin`, `cos`, `sqrt`, double) | `src/runtime/libm/libm.c` | MSVC CRT |
| newlib (string, stdlib, stdio, `malloc_r`, `strtod`, locale) | `src/runtime/newlib/` | MSVC CRT, **except `rand` and `qsort`** (2.2): compile those two from `stdlib.c` under new names |
| C++ runtime: `operator new/delete`, `type_info` (`T548`, `T549`), exceptions (`T543`) | `src/runtime/cxx/` | not compiled: MSVC's RTTI and EH; global `operator new/delete` replaced (3.2) |
| Sony libraries (libkernl, libcdvd, libpad, libmc, libscf, libgraph) | `src/sdk/` | not compiled; native replacements (system.md 3) except libscf's clock arithmetic |
| pdistd: heap, allocator, printf family, random, inflate, crc, hash, bignum, cipher, bit stream, fast float math | `src/engine/pdistd/` | **kept** (they are the game's C library): rename the symbols that collide with the CRT (2.1); `strlen.S` (MMI) replaced by the CRT `strlen` |

## 2. C library functions the game and engine call

### 2.1 Names that collide with the CRT

The game defines its own `malloc`, `free`, `realloc`, `calloc`,
`memalign` (`src/engine/pdistd/T532_heap.cpp`), `printf`, `vprintf`
(`T533_printf.cpp`), `sprintf` (`T534`), `vsprintf` (`T535`), `strlen`
(`strlen.S`), `atan2f`, `atanf`, `cosf`, `expf`, `logf`, `powf`, `sincosf`,
`sinf`, `tanf` (`fastmath.c`), and declares `errno` as `(*__errno())`
(`include/libc.h:70`). Linked with the static CRT these clash (LNK2005, or
the CRT's inline `<math.h>` / `<stdio.h>` definitions), and the platform
code must keep using the real CRT ones.

Replacement: a header forced into the game and engine translation units
(not into platform code) that renames them, for example
`#define malloc gt3_malloc`, `#define printf gt3_printf`,
`#define cosf gt3_cosf`, ... `strlen` maps to the CRT. `errno`,
`__errno`, the newlib syscall glue (`write`, `read`, `open`, `close`,
`lseek`, `sbrk`, `fstat`, `isatty`, `getpid`, `kill`, `link`, `unlink`) are
in `src/sdk` and `include/libc.h` only: not compiled. The decomp headers
never include the toolchain's `<string.h>` / `<stdlib.h>` / `<stdio.h>`
(libc.h's rule); platform headers must not be included by game files
after the rename header, or before it, in a way that sees both names.

### 2.2 The functions and their semantics

| Function | Sites | Callers (examples) | MSVC | Difference / action |
|---|---|---|---|---|
| `memcpy`, `memset`, `memmove`, `memcmp` | 54, 74, 2, 2 | everywhere | CRT | none |
| `strcpy`, `strcat`, `strcmp`, `strncpy`, `strncmp`, `strchr`, `strstr` | 215, 48, 43, 12, 2, 3, 1 | everywhere | CRT | none (MSVC deprecation warnings: `_CRT_SECURE_NO_WARNINGS`) |
| `strlen` | 61 | everywhere | CRT | the PS2 one is MMI (reads aligned qwords past the end); CRT is fine |
| `strupr` | 1 | `IOPLoadModuleBase::makePath` (`T524:46`) | `_strupr` | the IOP script is not compiled |
| `strcasecmp` | 1 | `iso9660_find_in_dir` (`T526:139`) | `_stricmp` | ISO 9660 code not compiled |
| `atoi` | 1 | `Font::setStyle` (`src/engine/text/T496_font.cpp:218`) | CRT | none (small decimal numbers) |
| `sscanf` | 1 | `EventStatusObject` (`src/game/menu/gtmode/T041_event_status.cpp:78`, `"%02d"`) | CRT | none |
| `atexit` | 1 | `RaceBase::virtual_25` (`T129_race_base.cpp:613`, the function-local static `RaceInput`) | CRT | runs at process exit, which the PS2 never reached: end the process without exit handlers (7.5) |
| **`rand`** | 3 | `TirePacket::TirePacket` (`T462_tire_packet.cpp:45`: `rand() * 2^-31`), `music_play` (`T463:230`), `ads_play` (`T466:95`) | **not equivalent** | newlib: one global LCG `x = x * 1103515245 + 12345`, `x & 0x7FFFFFFF`, seed 1 (`src/runtime/newlib/stdlib.c:296`). MSVC: `RAND_MAX` 0x7FFF and per-thread state. `TirePacket` needs the 31-bit range. Keep newlib's as `gt3_rand`. |
| **`qsort`** | 5 | car list (`T446:69`), course table (`T179:497`), menu draw order by z (`T220_object.cpp:359`), menu lists (`T495:254`), garage sort (`T219:493`) | **not equivalent** | both unstable but in different ways: equal keys (equal z of menu objects, equal garage sort keys) come out in another order, so overlapping menu items would draw in another order. Keep newlib's as `gt3_qsort`. |
| `_ctype_` (data) | 1 | `CTYPE_TOLOWER` (`src/game/menu/arcade/T025_arcade_course_select.cpp:85`) | none | replace with `(c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c` (newlib's `_U` test); do not use CRT `tolower` (debug CRT asserts on negative chars) |
| `sin`, `cos` (double) | 1 each | `flare_build_disc` (`src/engine/pgl/flare.c:197`) | CRT | last-ulp differences from newlib's fdlibm; graphics only |
| `sqrt` (double) | 3 | `matrix_slerp_rotation` (`T461:20`), `color_similarity` (`T193:33`, `:63`) | CRT | correctly rounded both |
| `abort`, `__assert` | libscf, runtime | | | log + break |

Not used by game or engine: stdio streams (`FILE`), `strtol` (newlib's is
64-bit `long`), `strtod`, locale.

## 3. Allocation

### 3.1 The heap

`malloc`, `memalign`, `realloc`, `calloc`, `free` are pdistd's
(`src/engine/pdistd/T532_heap.cpp`): one best-fit `pdistd::Allocator`
(`T530_allocator.cpp`) over `[align16(heap_start), heap_limit)`, created on
the first allocation by `heap_init_once`, which zero-fills the whole arena
(`calloc` relies on that and does not clear). Every entry point runs
under `intr_suspend`. Natively:

- `heap_start` / `heap_limit` = the platform's arena (system.md 8.5).
- `memalign(align, size)` (36 sites; mostly 0x40) is released with `free`:
  the pair only works inside the game heap. MSVC has no `memalign`, and
  `_aligned_malloc` needs `_aligned_free`: game memory must never be
  freed by the CRT or the other way round.
- The interrupt lock makes the allocator safe for game threads; native
  service threads (audio, file streaming) must not call it.
- `realloc` (2 sites) and `calloc` (unreferenced) as they are.

### 3.2 `operator new` / `delete`

| Operator | PS2 (file) | Port |
|---|---|---|
| `operator new(size_t)` | `T545_op_new.cpp`: `malloc`, size 0 -> 1; on failure `__new_handler` (never set) or throw `bad_alloc` | replace the global one: `gt3_malloc`, size 0 -> 1; on failure log the size and abort (nothing in the game catches) |
| `operator new[](size_t)` | `T547` | same |
| `operator delete(void *)`, `delete[]` | `T544`, `T546`: `if (p) free(p)` | same with `gt3_free`; also replace the **sized** `operator delete(void *, size_t)` and `delete[](void *, size_t)` (MSVC C++14+ calls them; GCC 14 built with `-fno-sized-deallocation`) |
| placement new | `<new>` | MSVC `<new>`; the races are built with placement new in `alloca`'d memory (`RaceReplayInformation::createRace` from `T000_main.cpp:340`, `T053_gtmode_races.cpp:697`, `:855`) |

Replacing the global operators affects every `new` in the executable,
platform code included. Either platform code allocates with the CRT
explicitly (no `new` of its own), or the game arena accepts platform
allocations (they would then take the interrupt lock). The first is
cleaner.

Class allocators (sizes chosen for egcs objects, see 9.2):

| Class | file:line | Allocates from |
|---|---|---|
| `gt2k_menu::Object` | `T220_object.cpp:49` | the current menu heap (`T204`), 16-aligned; **retries forever** with `yield_low_priority` (system.md R6) |
| `gt2k_menu::Transitive` | `include/game/menu/framework/T216_menu_transitive.h:40` | menu heap; `Allocator::debug()` on failure |
| `gt2k_menu::Operator` | `T217_operator.h:67` | menu heap |
| `MemorycardMenuItem` | `T073_memorycard_menu.h:125` | menu heap |
| `RaceCarModel` | `T180_race_car_model.cpp:128`, sized delete `:135` | the race pool `g_002B3BF0` (0x22000 bytes) |

Other allocation hooks: `menu_heap_push` / `pop` (`T204`) switch the
current menu heap; `loader_defer` (`T508:151`) frees menu car buffers on
the loader thread.

## 4. The printf family

Two formatters, both kept (the CRT cannot replace them):

- **pdistd** `printf` / `vprintf` / `sprintf` / `vsprintf`
  (`src/engine/pdistd/T533..T535`) over `pdistd::PrintFormat`
  (`T539_print_format.cpp`). `printf` goes to `printf_to_deci2` (the
  log). Conversions: `d i u o x X p c s e f g E G`, `%t` (an int printed as
  fixed point with `precision` decimals), `%F` (a **raw IEEE single passed
  as its bits in an int**, `T539:529`; no caller uses it), length `h`, `l`
  and `L` / `ll`. **`l` is 64-bit** (EE `long`): `format_number`
  (`src/game/common/T494_text_utils.cpp:57`, `:69`) passes an `s64` to
  `"%ld"`. Floats are formatted from the bit pattern with the
  `s_pow10_double` table (`T530a_printf_tables.cpp`), not by the CRT:
  `"%.1f"` in the garage, the speedometer and the short menu
  (`T036:314`, `T113:231`, `T154:1686`, `T158:368`) keep the PS2 rounding.
- **pgl** `fmt_sprintf` / `font_printf` / `fmt_vprintf`
  (`src/engine/pgl/fontprint.c:132`, `format_part2.c:163`): `%d %u %t %o
  %i(octal) %x %X %c %s`; floats consume their slot and print nothing.
  50 callers, mostly menus with `%t` (36 uses), `%s`, `%d`.

Specifiers used in the program's format strings: `%d` 195, `%s` 147, `%t`
36 (all through `fmt_sprintf`), `%x` 10, `%X` 6, `%f` 5, `%u` 2, `%c` 2,
`%ld` 1, `%%` 5. `%t` means something else to the CRT (a C99 length
modifier), another reason to keep both formatters.

**Both read the EE `va_list` layout and must be rewritten for x86:**

- On the EE every argument takes an 8-byte slot. `PrintFormat`'s `int`
  path reads the slot twice: `va_arg(args, s32)` then
  `((u32 *)args)[-2]` (`T539_print_format.cpp:354..355`), which on x86
  reads the previous argument. Use `uval = (u32)sval`.
- `fmt_vprintf` takes the `va_list` as `const u8 *` and steps 8 bytes per
  argument (`FMT_NEXT` / `FMT_ARG`, `format_part2.c:152..153`). On x86
  `int` and pointers take 4 bytes and `double` / `long long` 8: rewrite
  with `va_arg` of the exact type per conversion.
- On the EE a 64-bit value passed to `%d` / `%x` was harmless (the low word
  of its slot); on x86 it shifts every later argument. Annotate the
  formatters (`_Printf_format_string_`) and run `/analyze`, or check the
  calls with a script, before trusting their output.

## 5. Floating point

### 5.1 EE FPU against SSE

The game was compiled by egcs with `-ffast-math` (`tools/build.py:88`) for
the R5900 FPU, which is single precision only and not IEEE:

- it rounds **toward zero** (PCSX2's default EE round mode is "chop" for
  the same reason);
- denormals are flushed to zero;
- there is no Inf / NaN: overflow gives +-`FLT_MAX`;
- `cvt.w.s` truncates and saturates;
- `sqrt.s`, `rsqrt.s`, `min.s` / `max.s` are single instructions (the
  source reaches them through `boot_sqrtf` / `boot_fabsf` in
  `include/types.h`, `__builtin_*`, and inline asm in game code, 8.8).

MSVC x86 with SSE2 is IEEE round-to-nearest. Physics, AI and replays are
recomputed frame by frame from recorded inputs (`AutomobileControlRecord`):
the demo replays read from the volume (`T079_demo_file_manager.cpp`, the
enhanced engine's attract replays, `T000_main.cpp:445`) and best-lap
ghosts made on a PS2 will only replay correctly if the arithmetic matches
closely. Recommended for every game thread: MXCSR rounding toward zero,
FTZ and DAZ (`_mm_setcsr`), `/fp:precise` (no reassociation, no
contraction), `/arch:SSE2` (no FMA). Test with the disc's demo replays.
Exact equality cannot be promised (Inf/NaN clamping, the accumulator
instructions); the replays will tell.

Decimal float literals in the source are written so that a
round-to-nearest compiler gives the egcs bits (`fastmath.c` header,
`gs_driver_internal.h`): MSVC gives the same constants.

### 5.2 Float math library

`fastmath.c` (`src/engine/pdistd/fastmath.c`) is Polyphony's own
single-precision `atan2f`, `atanf`, `cosf`, `expf`, `logf`, `powf`,
`sincosf`, `sinf`, `tanf` (`sinf` / `cosf` 94 game sites, the others 80):
polynomial approximations, not the CRT's results. Keep the C branch (the
one GCC 14 builds, `#else` of `FASTMATH_RETAIL_ASM`) under new names
(2.1); using the CRT here would change physics and camera motion.
`fmod_wrap` (`T538_fmod_wrap.cpp:20`): portable.

### 5.3 Conversions

- **float to unsigned** (`fptoui`, `src/runtime/libgcc/fp_bit.c:681`):
  0 for negative, NaN or < 1; 0xFFFFFFFF from 2^32 up. 8 game callers:
  `machine_test_new_record` (`T053:163`), `logger_record_thread`
  (`T055:492`), `Color8::setClamped` (`T188:15`),
  `Rectangle::drawShadow` (`T256:203`), `renderWhen` (`T141:161`,
  `T146:560`, `:618`), `MusicPlayList::orderShuffle` (`T430:114`); engine:
  `anim_frame_reset/step` (`T452:78`, `:94`), `flare_build_disc`,
  `font_set_*`. MSVC's `(unsigned)f` is undefined for negative values
  (x86 wraps): use a helper with the fp-bit rules at these sites.
- float to int: truncation; out of range the EE saturates, x86 gives
  0x80000000.
- `__fixsfdi` / `__floatdisf` (float <-> s64, `T108:1313`, `T099:80`,
  `T157:444`, `T160:152`, `T521:212`): MSVC equivalents.
- Doubles were soft-float (`dp_bit.c`, IEEE round-to-nearest). With MXCSR
  set to round-toward-zero, SSE doubles round toward zero too: only the
  float formatting, libm and a few colour / interpolation helpers use
  doubles.

## 6. Integers and types

- 64-bit arithmetic (`__divdi3`, `__moddi3`, `__muldi3`: `T253:207`,
  `T099:80`, `T108:1277`): truncating division both sides.
- `long` is **64-bit** under ee-gcc, 32-bit under MSVC. Uses: the GCF /
  menu-link / PMB relocations (`(off) = (long)(int)(field)`,
  `src/game/data/gcf_internal.h:66`, `T436..T445`, `T242`, `T243`,
  `src/game/menu/framework/menu_links_internal.h:92`), relocation deltas
  (`music_inf_relocate`, `T463:52`; `T466`), `io_getstat`'s size
  (`hostfs_io.c`), and the `%ld` above. All are offsets within the 32-bit
  address space: same results with a 32-bit `long`.
- `int` / `u32` / `size_t` / pointers are 32-bit both sides; `char` is
  signed both sides; little-endian both sides.
- `u128` / `s128` (`include/types.h:97..98`, `mode(TI)`): see 8.6.
- Shifts by 32 or more: both mask the count to 5 bits for 32-bit values.
- Multi-character constants (`'FR'`, `T278_showroom_car.cpp`): same value
  in MSVC.

## 7. C++ runtime

### 7.1 Exceptions

No `try`, `catch` or `throw` in game or engine code. The calls of
`__throw` (from 574 functions) and `std::terminate` (566) are
compiler-generated landing pads
(destructors during unwinding, `throw()` specifications). Only `operator
new` could throw, and the port's does not (3.2). Build with `/EHsc` (or
`/EHs-c-` once nothing needs unwinding); EH frame registration
(`reg_frame`, `__register_frame_info_table`) disappears.

### 7.2 RTTI

52 `dynamic_cast`s (e.g. `T000_main.cpp:344`, `T055`, `T087`, `T081`,
`T263`, `T264`): standard C++, `/GR`.

The tree also carries an **emulation of egcs's typeinfo functions for GCC
14**: about 1,160 lines of `__tf<class>()` functions, `__ti<class>`
storage arrays, `__rtti_si` / `__rtti_user` / `__rtti_class` calls and
`__class_type_info::base_info` tables, all under `#ifndef BOOT_OLD_GCC`
(e.g. `T002_render_thread.cpp:272..281`, `T009:162..175`, `T519:380..404`),
some declared `__attribute__((weak))` (`T053:1046`, `T080:296`, ...), with
the egcs `type_info` classes of `src/runtime/cxx/T548_tinfo.h` /
`T549_tinfo2.h`. They are never called by game logic and would clash with
MSVC's own `type_info`. Exclude them: a third mode macro (e.g.
`GT3PC`) and a mechanical change of those guards to
`#if !defined(BOOT_OLD_GCC) && !defined(GT3PC)`, or a filter that drops
every `#ifndef BOOT_OLD_GCC` block that defines `__tf*` / `__ti*` or calls
`__rtti_*`.

`__pure_virtual` (`T543`) becomes MSVC's `_purecall`.

### 7.3 Pointer-to-member functions

GCC 14 built the C++ with `-Wno-pmf-conversions`. The only conversions are
the three casts of a derived-class member pointer to the base type in the
IOP script, `(IOPLoadCmd)&RaceIOPLoadModules::cmdSetMediaMode`,
`::cmdInitClients`, `::cmdBindClients`
(`src/game/app/T009_race_iop_load_modules.cpp:48`, `:84`, `:91`), stored
with string pointers cast to `int` in `IOPLoadCommand`
(`include/engine/pdi/T524_iop_load_module_base.h:14..19`). The script is
not compiled natively (system.md 7.3). If it were: a `static_cast` of the
member pointer is valid C++ (single inheritance); with MSVC declare
`IOPLoadModuleBase` complete before the `IOPLoadCmd` typedef (or use
`/vmg`) so all member pointers share one representation.

The other member-pointer tables are standard: `Operator` handlers
(`g_002C8850`, `T217_operator.cpp:581`, called at `:630`),
`ArcadeControl::NameFn / ChangeFn / CheckFn`
(`include/game/menu/arcade/T010_arcade_control.h:114..116`, called in
`T020:322`, `T021:130..148`). MSVC member pointers are 4 bytes (egcs 8):
no data file holds them.

### 7.4 Static constructors

110 entries (109 constructors + frame registration), run on the PS2 by
`__main` from `main` (`T000_main.cpp:111`), **in reverse link order**: the
pdistd heap first (`global_ctor_allocator`, `T532:156`), then random,
the five pdi RPC objects (`T519..T523`: they create semaphores), T518,
T516, render context, Serialize, loader ring, `vol_device_object`
(`T503`: its constructor runs `intr_suspend`), ... T001's packet buffers
last (`build/scratch/system/ctors.txt` has the full order). MSVC runs them
before `main`, per translation unit in link order. Dependencies:

- The heap object must be constructed before any allocation; no static
  constructor allocates today, but put `T532_heap.cpp` (and the platform
  kernel) in `#pragma init_seg(lib)` so it is so whatever the link order.
- Kernel objects and the interrupt lock must work before `main`
  (constant-initialised tables): `VolDevice::VolDevice` takes the
  interrupt lock, `create_mutex_sema` is called by RPC constructors (the
  replaced ones may not need it).
- `Serialize::ClassInfo` objects link themselves into `g_00352FC0`
  (constant-initialised to NULL, `T509:237`): order-independent.

### 7.5 Static destructors, `atexit`, function-local statics

- 48 static destructors exist; the PS2 never ran them (`__do_global_dtors`
  has no caller). At process exit MSVC would run them (and the `atexit`
  of 2.2) after the game threads stopped: they free game memory and delete
  kernel objects. End the process without them (`TerminateProcess` or
  `quick_exit` after flushing the log and closing card files).
- Function-local statics: `RaceBase::virtual_25` spells the guard out by
  hand in the GCC 14 branch (`T129:604..615`). Build with
  `/Zc:threadSafeInit-`: MSVC's thread-safe statics wait on a Win32
  primitive that the game scheduler does not know, which can deadlock when
  one game thread blocks inside an initialiser and another enters it.

## 8. GCC / egcs-only constructs and their replacements

Counts are lines outside `#ifdef BOOT_OLD_GCC` branches (the port takes the
other branch, as GCC 14 does).

### 8.1 Statement expressions `({ ... })`

| file:line | Use | Replacement |
|---|---|---|
| `src/engine/gfx/model_set_internal.h:93..95` | `U102_GET_F32/U32/U16(p)`: read and advance (39 expansions in `T451`, `T452`, `T454`) | inline functions taking `const u8 *&p` |
| `src/engine/pdistd/T537_gzip_inflate.cpp:226` | bit-buffer macro (11 expansions) | inline function or `do { } while (0)` with an out variable |
| `src/game/menu/arcade/T025_arcade_course_select.cpp:85` | `CTYPE_TOLOWER` (newlib `_ctype_`) | inline function (2.2) |
| `src/engine/pgl/vu1_lighting.c:91` | `U115_QMTC2_PAIR` (VU0 asm) | graphics rewrite |
| pgl headers `GS_SPR_CTX` (`gs_driver_internal.h:91` and 5 more) | scratchpad context pointer | graphics rewrite |
| `include/types.h:61`, `:78` (`boot_fabsf`, `boot_sqrtf_v`) | egcs branch only | the `#else` (`fabsf`, `sqrtf`) |

### 8.2 `__typeof__`

All in C++ files: `model_set_internal.h:125`, `:127` (`U102_RELOC`),
`menu_links_internal.h:92`, `prize_model_internal.h:86`, `tex1.cpp:41`
(`REBASE`), `T508_loader_thread.cpp:27` (`LOADER_VOLATILE`). Replace with
`std::remove_reference_t<decltype(x)>` (plain `decltype` of a parenthesised
lvalue is a reference type).

### 8.3 `__builtin_*`

| Builtin | Lines | Replacement |
|---|---|---|
| `__builtin_alloca` | 40 in 22 files: `T000:334`, `:472`, `T053:697`, `:855`, `T055:1120`, `T462:144`, `T213:562`, `T220:356`, `T495:248`, `T219:487`, `T108:525`, `:853`, `T102`, `T124`, `T138`, `T415..T417`, `T420`, `T503:382`, `T519:143`, `T521`, `T523:53`, `:88`, `:126`, `T432` (8), `T496` | `_alloca` (`<malloc.h>`); the main thread's stack must be large (system.md 8.5) |
| `__builtin_sqrtf`, `__builtin_fabsf` | 8, 7 (mostly through `boot_sqrtf` / `boot_fabsf`, `include/types.h`) | `sqrtf`, `fabsf` (intrinsics) |
| `__builtin_prefetch` | 3 (`T453_vision_list.cpp:100`, `:102`, `T451`) | `_mm_prefetch` or nothing |
| `__builtin_abs` | 4 (`T479:353`, `:368`, `:381`, `T539:711`) | `abs` |
| `__builtin_memcpy` | 2 (`T431_ranking_code.cpp:44`, `:51`) | `memcpy` |

### 8.4 Case ranges

`src/game/data/T482_car_equipments.cpp:1307..1341`, 13 labels
`case PART_X ... PART_Y:`. Replace with explicit case lists or a range test
before the switch.

### 8.5 Variable-length arrays

One: `u8 windowRaw[windowSize + 0x40];`
(`src/engine/io/T503_vol_thread.cpp:158`). Replace with
`_alloca(windowSize + 0x40)` or a fixed 0x2040-byte array. (The other
"VLA-like" stack arrays in the tree are `alloca` calls, 8.3.) No
zero-length arrays, no flexible array members.

### 8.6 `u128` / `s128` (`__attribute__((mode(TI)))`)

`include/types.h:97..98`. Uses outside VU0 / MMI asm: the pad DMA areas
(`T477_ps_port.cpp:23`, ignored natively), `scePadPortOpen`'s parameter
(`include/libpad.h`), `RaceCourse::getGridPosition` returning a 16-byte
grid vector by value (`T153_race_course.cpp:198..200`), quadword
temporaries in `T173_race_smoke.cpp:583` and
`T317_dynamics_conductor.cpp:513` (MMI code), `REG128`, and pgl / media
copies. Replacement: `struct alignas(16) u128 { u64 lo, hi; };` with copy
only; the arithmetic users are inline asm being rewritten anyway.

### 8.7 Attributes

| Attribute | Lines | Replacement |
|---|---|---|
| `aligned(n)` | 102 in 48 files (game: `T167` 9, `T159` 5, `T173` 4, `race_car_model_internal.h` 3, `T446`, `T447`, `tire_model_internal.h`, `T469`, `T411:45`, `T245:179`, `T462:658`, `T165`, `T276`, `T397_variable_camera_base.h`; engine: pgl, `pdi1394_rpc.c` 5, `T525:18..20`, `T477:23`, `T508:19`, `T463:46`, usbd, pjoy, ipu) | `alignas(n)` (C++) / `_Alignas(n)` or `__declspec(align(n))` (C), **moved in front** of the declarator (GCC accepts it after; MSVC does not). `typedef f32 MenuCameraVector[4] __attribute__((aligned(16)))` (`T411:45`) becomes a struct or per-variable `alignas`. |
| `packed` | 10 (`T077_memorycard_icon.cpp:38`, `model_set_internal.h:85..86`, `pdi1394_rpc_internal.h:64`, `gs_driver_internal.h:68`, `pgl_font_internal.h:115`, `pgl_vu1_internal.h:82`, `system_drivers_internal.h:286`, `include/engine/io/T526_iso9660.h:25`, `G002_mpeg_frameq.h:107`) | `#pragma pack(push, 1)` / `#pragma pack(pop)`; unaligned reads are fine on x86 |
| `may_alias` | 26 (pgl headers, `BOOT_MAY_ALIAS` in `types.h:46`, `G002`) | nothing (MSVC does no type-based alias analysis) |
| `mode(TI)` | 2 | 8.6 |
| `weak` | on the `__tf` functions | excluded with them (7.2) |
| `used` (`BOOT_USED`) | `types.h:45` | nothing |
| `noreturn` | `include/libc.h:55` | `__declspec(noreturn)` |

MSVC x86 rejects **by-value parameters of a type aligned above 4**
(C2719). After adding `alignas(16)` to vector structs, any function taking
one by value must take it by `const &` (return values are fine).

### 8.8 Inline assembly

367 lines outside the egcs-only branches (277 more inside) in 51 files;
in game code (`src/game`, `include/game`), by kind:

| Kind | Where | Replacement |
|---|---|---|
| `ei` closing an `intr_suspend` section | `T055` (4), `T078`, `T397`, `T405`, `T406`, and as `EI()` `T212` (5), `T122` (2), `T216`, `T264`; engine: `T501` 18, `T510` 3, `T530` 7, `T532`, `T477` 3, `sound_seq.c` 3 (as `EI()` or raw asm) | `intr_resume(was)` (system.md 1.7) |
| `mfc0 $1` (COP0 Random) | `T414_race_random.cpp:26` | platform random 0..47 (system.md 2.5) |
| `mfpc` / `mtpc` / `mtps` | pgl | graphics / platform counter |
| `pref` | `T453_vision_list.cpp`, `T173` | `_mm_prefetch` or nothing |
| FPU: `rsqrt.s`, `min.s`, `max.s`, `mula.s`, `madda.s`, `madd.s` | `race_car_model_internal.h:92`, `T167_rigid_body.cpp` (33 lines), `T159_flare.cpp`, `T160`, `T158`, `prize_model_internal.h`, `T328_surface_tables.h`, `T218`, `T176`, `T177`, `setting_sliders_internal.h`, `car_box_internal.h` | plain float C++ (same operation order) |
| VU0 macro mode (`vnop`, `qmtc2`, `qmfc2`, `lqc2`, `vmini`, ...) | `T180:510`, `T447:242..278`, `tire_model_internal.h`, `race_car_model_internal.h:135..191`, `T167`, `T173`, `T165`, `replay_effects_internal.h`, `T130` | the VU0 rewrite (graphics owner), with the interrupt-off rules of system.md 1.1 |
| MMI (`pmulth`, `pextlw`, `pcpyld`, `pcpyud`, `pcpyh`, `pmflo`, `ppach`, `pmaddh`) and a register variable `register u64 v7 __asm__("$9")` | `T077_memorycard_icon.cpp` (46 lines), `T173`, `T317` | plain C++ |
| `busy_wait_delay` | `thread_util.c:184` | no-op |

### 8.9 Other GNU spellings

- `__inline__` (`eekernel.h`, `G002_mpeg_frameq.h:614`, `ipu_internal.h`,
  ...) -> `__inline` (C) / `inline` (C++); `__volatile__` only appears with
  `__asm__`.
- `#pragma implementation` / `interface` (22 files, under
  `#if defined(__GNUC__) && __GNUC__ < 3`) and
  `#pragma GCC diagnostic` (`T278:10`): MSVC ignores unknown pragmas
  (C4068; disable the warning).
- Anonymous-namespace anchors `extern const char x[] __asm__(".._.._pdicdvd...")`
  (`T519:30`, `T523:20`, `T533`): egcs branch only.
- `_Static_assert` in C headers (`eekernel.h`, `sif.h`, ...): MSVC C11
  (`/std:c11` or later) accepts it.
- Function pointer to `void *` casts (`usbd.c`, `pjoy.c`): accepted.
- Not present: labels as values, nested functions, zero-length arrays,
  designated initialisers in C++, implicit `int`, old `for`-scope rules
  (GCC 14 would have reported them).

## 9. Layout differences

### 9.1 What changes between egcs and MSVC

- **vptr**: egcs puts it after the data members of the first polymorphic
  class (`Control` +0xC0, `IOPLoadModuleBase` +0x4020); GCC 14 and MSVC at
  +0. The headers document the egcs offsets; the GCC 14 branch is already
  self-consistent. The one hand-written offset into a polymorphic object,
  `PSPort::translate`'s `(const u8 *)this + 0x18` (`T477:437`), is the
  same under MSVC (vptr 0, `status` 4, `port` 8, `st` 0xC, data 0x14).
- **`bool`** is 4 bytes in egcs, 1 in MSVC. Data records shared with files
  use `int` in the non-egcs branch (`ref/MATCHING.md`, "Both compilers");
  internal classes (`McSlot::format`, `T501`) just change size.
- **Member pointers**: 8 bytes in egcs, 4 in MSVC (7.3).
- **Bit-fields**: MSVC starts a new unit when the declared type's size
  changes; the game's bit-fields use one type per struct (`RaceDate`
  `u64`, `T121_race_entry.h:124`; `LoggerSample` `u16`; T194 `u32`/`s32`;
  cameras `unsigned int`): same layout. The mixed-type ones are GS register
  formats in pgl (graphics).
- **Alignment**: `u64` / `double` members align to 8 in both; `alignas`
  replaces `aligned` (8.7).

### 9.2 Consequences

- Fixed pools sized for egcs objects (system.md 8.2: race pool 0x22000,
  menu heaps, card work area, car / course arenas) must still hold the MSVC
  objects; log `sizeof` of the pooled classes at start-up and grow a pool
  where needed.
- Structures overlaid on file data (models, textures, GCF, menu layouts,
  sound banks, save data) must keep their egcs layout. Turn `EGCS_SIZE`
  (`include/game/game.h:20`) into a real check for non-polymorphic types
  under MSVC, e.g.
  `static_assert(std::is_polymorphic<T>::value || sizeof(T) == (n), #T)`:
  it catches every data record whose size moved.
- Saves written by the PS2 load on PC only if the serialised layouts match
  (`Serialize`, `T509`).

## 10. Build settings (summary)

- C++ files: `/std:c++17 /GR /EHsc /Zc:threadSafeInit- /fp:precise`;
  C files: `/std:c11`; 32-bit, `/arch:SSE2`.
- A forced-include header for game and engine sources: the CRT renames
  (2.1), `__inline__`, `BOOT_USED` / `BOOT_MAY_ALIAS` empty, the `u128`
  struct, `GT3PC` defined (7.2), `NON_MATCHING` defined (the C branches of
  hand-written assembly, e.g. `fastmath.c`), `EGCS_SIZE` as in 9.2.
- `_CRT_SECURE_NO_WARNINGS`; disable C4068 (unknown pragma).
- Per game thread: MXCSR round-toward-zero + FTZ + DAZ (5.1).
- Exit without static destructors (7.5).
