# Struct layouts: egcs against MSVC

Which of the game's types MSVC lays out differently from the PS2 compiler
(SCE ee-gcc 2.9-ee-991111b, "egcs"), which of those differences matter, and
how the ones that matter are fixed. The rules behind the differences are in
[runtime.md](runtime.md) section 9.

## The check

`python tools/msvc_layout.py` reads every header of `include/**` and
`src/{game,engine}/**` (not the native folders): the PS2 offsets the
comments document (`u32 x;  /* 0x14 */`), the sizes (`EGCS_SIZE`,
`EGCS_SIZE_ABI`, `static_assert(sizeof)`, a "Size 0xNN" in the comment
before the class) and the base classes (egcs: declaration order, the first at
0, or the offset the comment gives, "Bases: Drawable at +0x00, List<Button>
at +0xB0"). It compiles one probe per header with the game's options
(`pc_prelude.h`, `/permissive`, C++17), links them into
`build/scratch/layout/probe/probe.exe`, which prints `offsetof`, `sizeof`,
the base offsets and `std::is_polymorphic` as MSVC sees them, and writes
`build/scratch/layout/report.txt` (the differing types, the types MSVC makes
larger, what could not be checked) and `layout.json`. About 15 seconds.

Coverage (2026-10-18): 440 headers, 1,093 types, 7,040 documented members,
392 base subobjects, 1,091 sizes. Not checked: `src/engine/pdi/
pdi1394_rpc_internal.h` (does not compile alone; the i.LINK RPC is not
compiled natively), members without an offset comment, classes defined in
`.cpp` files (none of them is polymorphic data or holds a `bool` that a file
or a save shares), bit-fields (offsetof cannot take them; no compiled struct
mixes bit-field types).

147 types differ. Three of them matter.

## The differences that matter

| Type | PS2 (egcs) | MSVC | Where it matters | Fix |
|---|---|---|---|---|
| `DataBlock`, `StringsDataBase` (`include/game/data/T481_car_data_base.h`) | data from +0, vptr +0x14 | vptr +0, data from +4 | overlays on `database/paramdb_us.db`, `paramstr_us.db`, `paramunistr_us.db`, `racemode.db`, `racedetail.db` (`CarDataBase::readFile`, `RaceModeDatabase`): every field read 4 bytes off, `RaceDataBase::select` failed | (earlier fix) not polymorphic on PC: the vptr is a plain word `vptrSlot` at +0x14, the destructor not virtual (nothing deletes a block through a pointer) |
| `FlareClass` (`include/game/effects/T159_flare.h`) | `unk_000` (G001's `FlareWork`, 0x7C bytes) at +0, vptr +0x23FC | vptr +0, `FlareWork` members from +4 | `flare_work(this)` handed `this` to the flare library as its `FlareWork`: `flare_init` overwrote the vptr with `occVram`, and `FlareClass::flush` read `queries` (+0xC) where the library keeps `screenVram`, so the occlusion results read back were the wrong count | `flare_work` returns `fc->unk_000` (`src/game/effects/T159_flare.cpp`), with `static_assert`s that `queries` / `glareImg` sit at +0x08 / +0x7C of the work area |
| `CarEquipments` (`include/game/data/T482_car_equipments.h`, bases `CarSaveEquipments`, `CarDataBase`) | `CarSaveEquipments` +0, `CarDataBase` +0x1D0 (vptr +0x1DC), `presetCar` +0x1E0 | `CarDataBase` +0 (MSVC moves the polymorphic base first), `CarSaveEquipments` +0x10, `presetCar` +0x1E0 | `RaceBase::virtual_7` (the info block of every replay and ghost save) assigned a whole `CarEquipments` over each `RaceInfoSlot` through `static_cast<CarEquipments &>(slot->info)`: under MSVC the downcast starts 0x10 bytes before the slot, and the 0x1E8-byte copy overwrote `RaceInfoBlock::carCount` (slot 0) with the `strings` pointer, so the loop ran far past the 8 slots | only the `CarSaveEquipments` part is copied (`src/game/race/core/T129_race_base.cpp`); the bytes the block gets are the same (on the PS2 the copy's tail, 0x1D0..0x1E8, was overwritten by the condition right after) |
| `gt2k_menu::ControlBase` (`include/game/menu/framework/T247_control_base.h`) | `control1` (a `MenuControl`) +0, vptr +0x288 | vptr +0, `control1` +4 | found by a crash after a free run (the memory-card comment entry), not by the probe: the controllers pass `this` as the page hooks' `void *ctx`, and `create_text_box` (T231) took that `ctx` as the page's `MenuControl`, which on the PS2 is the controller's address. Under MSVC the `TextBox`'s `control` pointed at the controller's vptr, and `getButtonsOn` (`TextBox::virtual_1`, on the render thread) called slot 5 of `NameEntryControl`'s 2-slot vtable: the string `"menu/name_entry.mbl"` after it | `create_text_box` names the pad, `&static_cast<ControlBase *>(ctx)->control1` (`src/game/menu/framework/T231_text_box.cpp`); its users (`NameEntryControl`, `MemorycardControl`) derive from `ControlBase` alone. No other hook reads a controller `ctx` as anything but its own controller type |

Everything else that reaches a `CarEquipments` through its bases uses
`static_cast` to the base (`CarEquipments::serialize` / `deserialize`,
`CarInformation`'s, `race_entry_load_slot`), which MSVC adjusts. The other
classes with two bases (`CarInformation`, `GarageCar`, `gt2k_menu::Button`,
`ButtonList`, `MemorycardMenu`) keep egcs's order; `OperatorHolder` does not
(its `Cell<OperatorHolder>` base moves from +0x28 to +0) but is a menu object
only reached through `static_cast`.

## What was checked and does not matter

Data the game shares with files, the memory card and replays, and how each
reaches its C++ types:

- **Files relocated in place.** From the types of every relocate function
  (`modelset_relocate`, `tex1_relocate`, `vif_model_relocate`,
  `fontset_relocate`, `image_pack_relocate`, `course_data_relocate`,
  `vision_relocate`, `runway_relocate`, the GCF tables, the menu layouts and
  links, `music_inf_relocate`, `se_inf_relocate`, the sound banks, the car
  and wheel files, course cameras, flare sources, the minimap, the logger's
  course map...) and the database overlays, through every member by value
  and pointer: 148 types, none polymorphic, none with a differing offset;
  the only `bool` is `SplitBattleOption::tireDamage` (below). No polymorphic
  type of the program is cast from a loaded buffer (the casts to polymorphic
  types are of program objects: menu contexts, `findChild`, races).
- **The memory card** (`Serialize`, T509). The serialisable classes are
  polymorphic but save plain blocks addressed by member:
  `GameStatus` the 0x130 bytes from `day` (ints, s64, chars),
  `GarageCar` its `CarInformation` save block (`CarSaveEquipments` and
  `CarSaveInformation` by `static_cast`) then `valid` / `index`,
  `GarageWheel` `valid` / `name`, `SettingSerialize` its `CarSaveEquipments`,
  `ChampionshipSave` its head and per entry the car's save block and the
  points; the record blocks (`TimeTrialRecords`, `LicenseRecords`,
  `GameOptions`, ...) are not polymorphic and keep their layout. The card's
  own records (`MemorycardIconSys`, `McPs1Header`, `MemorycardDir`) are
  plain; icons are unpacked by the program into `McIconModel` (plain, its
  `bool` followed by an `int`).
  `GameOptions::tireDamage` / `SplitBattleOption::tireDamage` are `bool`
  (4 bytes in egcs): the next member is an `int`, so the offsets agree and
  a PS2 save's word (0 / 1) reads back as its low byte. MSVC stores only
  that byte; the 3 padding bytes keep what the zeroed `GameStatus` or the
  loaded save put there.
- **Replays and ghosts.** `RaceInfoBlock` / `RaceInfoSlot` are plain (the
  `virtual_7` copy above was the one problem); `OneLapGhostInfo` holds the
  car as GT3's 0x16DC raw bytes of `RaceCarState`, which is not polymorphic
  and holds no polymorphic member (`car_restore` keeps the live links);
  the control records are bit streams in plain buffers.
- **Raw copies and hard-coded offsets.** `memset` / `memcpy` of `this`:
  `CarDataBase::clear` (0x10 bytes: it clears the vptr under both compilers,
  as retail does; `CarEquipments` stores its own after), `CarEquipments::
  operator=` (whole object to whole object, vptr included, as retail),
  `CarCondition`, `RaceRuleOptions`, the save records (not polymorphic).
  `PSPort::translate`'s `(const u8 *)this + 0x18` is the same offset in
  both layouts (`port` +0x8, `st` +0xC agree). No other `this` + constant,
  no cast of a global or a table to a class.
- **Compiled tables with member pointers** (`EGCS_SIZE_ABI`:
  `IOPLoadCommand` 0xC -> 0x18, `RaceMeterLayout` 0x10 -> 0x18,
  `PageHistory`, `MemorycardSlot`, `MemorycardSlotSet` with their `bool`s):
  built by the compiler from initialisers and read through the same type.

## Internal differences (no fix)

Objects the program builds itself and reaches only through their C++ type:
the vptr moved from after the data to +0 (members +4, or +8 when the class
has a `u64`), `bool` members 1 byte instead of 4, or both. Sizes are the
same or smaller except where listed in the pools section.

| Group | Types | PS2 -> MSVC |
|---|---|---|
| input | `Control`, `PortBase` | members +4 |
| engine | `mpegif`, `IOPLoadModuleBase`, `PDIClientRpc`, `pdi1394`, `_UnitArenaBase`, `BitStreamBuffer`, `GZIP::InflateBase` (its plain base `InflateState` at +8), `GT2KRealTimeText`, `pdistd::Allocator`, the egcs `type_info` classes (not compiled) | members +4 / +8 |
| cameras | `CameraRotDamper` (0x450 -> 0x458), `CameraGrooveMeter`, `DesignerCamera` (its dampers move; size kept by its 64-byte alignment) | +4 / +8 |
| car models, physics, effects | `RaceCarModel`, `TireModelBase`, `TStripBuilder`, `TirePacket`, `RigidBodyManager`, `ParticleManager`, `FlareClass` (but see above), `PrizeModelSetContext`, `PrizeBase` | +4 |
| data | `CarDataBase` (program object holding the file pointers; see `clear` above), `RaceModeDatabase` | +4 |
| race core | `RaceEntryBase`, `RaceEntryCar`, `RaceEventQueue`, `RaceLapTime`, `RaceCourse`, `RaceOrganization` (0x8C8 -> 0x8D0), `RaceInformation` and every `Race*Information` (+8 each), `DynamicsConductor`, `AutomobileControlRecord::Manager` / `Recorder`, `PauseBase`, `RaceBGMBase`, `GTSOUNDINSTRUMENT` | +4 / +8 |
| race modes and displays | `RaceSinglePlayer`, `RaceArcade`, `RaceArcadeDemo`, `RaceFreePractice`, `RaceSplitBattle`, `RaceLinkBattle` (0x35680 -> 0x35640), `RaceSolitaire`, `RaceTimeTrial`, `RaceFreeRun`, `RaceLicense`, `RaceMachineTest`, `RaceChampionship` (0x34C80 -> 0x34C40), `RaceDisplay`, `RaceTimeTrialDisplay`, `RaceLicenseDisplay`, `RaceSplitDisplay`, `RaceDisplayObjectBase`, `RaceDisplayEventBase`, `ErrorEventView`, `ExaminationDisplayer`, `RaceResultBase`, `ResultArcade` / `ResultChampionship` / `ResultLinkBattle` / `Result2PBattle` (bools: -0x2C), `RaceMenuButton`, `RaceMenuLayer`, `RegurationResult` | shifts inside, sizes equal or smaller |
| menus | `gt2k_menu::Object`, `ControlBase`, `PageManager`, `pCell`, `pList`, `Ipic`, `IpicArchive`, `Car`, `TheCar`, `CarsItem`, `PrizeCounter`, the smart indexes, every `*Control` screen, the button lists, `TextBox`, `ColorBox`, `Slider`, `GearChart`, `KeyConfigItem`, `MemorycardSlotMenu`, `WorkingMenu`, `OperatorHolder`, the logger windows | shifts inside, sizes equal or smaller |

## Pools and buffers sized for egcs objects

MSVC makes 18 documented types larger (report section "larger under MSVC"):
`IOPLoadCommand` and `RaceMeterLayout` (compiled tables), `CameraRotDamper`
(+8, inside `DesignerCamera`, whose size does not change), `RaceOrganization`
(+8) and `RaceInformation` with the 13 `Race*Information` classes (+8).
Every other documented type is the same size (837) or smaller (56); all 247
`gt2k_menu` types included.

| Pool | Holds | PS2 -> MSVC | Verdict |
|---|---|---|---|
| race object pool `g_002B3BF0`, 0x22000 bytes (`RACE_POOL_SIZE`, `src/game/app/app_internal.h`), 16-byte units | `RaceCourse` 0x14, `RaceOrganization` 0x8C8 -> **0x8D0**, `RaceEntryBase` 0x20 / `RaceEntryPS2` 0x3C, `RaceEntryCar` 0x2190, `RaceSolitaireEntry` 0x60, `RaceCarModel` 0x6E4, `Recorder` 0x44, `Manager` 0x60, `RaceBGMBase` 8 / `RaceBGMPS2` 0x9B8; the race classes' own `operator new` (no caller reaches it: `race_create_by_name` always gets a buffer, the races are built in `alloca`'d memory of their `sizeof`) | only `RaceOrganization` grows, and 0x8C8 already took a 0x8D0 block (the allocator rounds to 16) | no change |
| menu heaps (`memalign` blocks of `T015`, `T038`, `T051`, `T056`..`T061`, `T080`, `T097`, `T105`...; `Object`, `Transitive`, `Operator`, `MemorycardMenuItem` allocators) | menu objects | all equal or smaller | no change |
| memory card file pool (`MemorycardFileList`, cell = largest file class by `sizeof`) | `Memorycard*File` | equal (0x68..0x298) | no change |
| race / replay memory `RaceMemPool` (`T174`), `UnitArena<64>` (course 0xD00000, car models 0xA8000), icon heaps, card work area | raw file data, control records, `OneLapGhostInfo` (0x1B00, equal), `McIconModel` (0x50, equal) | equal | no change |
| stack and `alloca` objects (`Race*Information` +8, controls, races) | sized by `sizeof` | - | no change |

No pool needed a larger size.

## Uncertain

- Members without an offset comment and classes defined in `.cpp` files
  are not probed; a raw copy into or out of such a class would not show
  here. The greps above (`this` + constant, casts of buffers and globals,
  `memcpy` / `memset` of objects, downcasts from a plain base) found none
  besides the three fixed.
- The lens flares: the fix makes the occlusion query count and the vptr
  right; whether the flares looked wrong before (and look right now) needs
  a race with lamps or the sun on screen.
- Replays and ghosts saved by the port before the `virtual_7` fix were
  written by a loop that ran past the block; they should not be trusted.
