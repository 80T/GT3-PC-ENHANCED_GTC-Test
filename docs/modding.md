# Adding events and courses

How to add events and courses to GT3 PC: what the game reads, in what
order to add things, what the PC build no longer limits, and what still
does. The code side is in detail; the game assets side is brief.

Everything here lives in the game's `data\` folder (next to `gt3pc.exe`).
The game lists that folder when it starts, so restart it after adding files.
`.gz` files are not listed: add files unpacked.

**Tool:** the database (`database\paramdb_us.db` with `paramstr_us.db` and
`paramunistr_us.db`) is edited with **GT ParamDB Editor** (part of
GTDataSQLiteConverter). It keeps
the rows sorted by label hash, which the game's lookups depend on. The
tables used here are EVENT, COURSE, ENEMY_CARS, REGULATIONS and ARCADE_CAR.
`gameconf\product.gcf` has no editor yet (see [product.gcf](#productgcf)).

## What the PC build changed

| What | PS2 / before | Now |
|---|---|---|
| Event results in the save | 382 slots, by position in product.gcf's sorted event list (adding an event shifted every later event's results) | as many as product.gcf has events, saved **by event name** |
| Course best times / free run records | 44 slots each, by position | as many as the lists have, saved by course name |
| Arcade records | 144 slots (36 courses x 4 difficulties) | 4 x the SINGLE_RACE courses, saved by course name |
| Prize-set flags | 200, by position | as many as product.gcf has prize sets, saved by name |
| Music play list | 64 tracks | every BGM track, saved by song |
| Garage | 200 cars / 200 wheels, buying refused at 200 | grows as needed (limit 100 000) |
| Course file size | 13 MB, a bigger file cut off silently (then garbage pointers) | read whole, the memory grows to fit |
| Course gadgets | 100, the rest silently missing | 1024 (more are logged) |
| Course flares | 100 lamps + 100 sources | 255 + 255 |
| Course textures | a texture set bigger than the free VRAM drew the wrong texture | drawn from the set in memory |
| Course / car sounds | banks overflowing into their neighbours | each bank gets room for its size |
| Debug logger course map | 128 KB | by size |

The save keeps the PS2's other records as they were. Saves written by this
build are GameStatus **8.0**; PS2 saves (7.1) still load. Results of events,
courses and prize sets the current data does not have are kept and written
back, so a mod removed for a while loses nothing. Renaming an event or a
course, though, cuts it from its saved results: the save knows it by name.

## Adding a course

1. **The course file.** Copy it to `data\courses\US\<name>`. The game
   builds its course list from this folder when it starts, and loads
   courses from the region folder in use (US in this build). One file holds
   everything per course: models, vision lists, the road (runway), the AI
   lines, the environment, the mini map, replay cameras and the ambient
   sound. It must contain AI lines (enemy lines): the game uses them without
   checking.
2. **The COURSE row** (ParamDB Editor, table COURSE):
   - label (`Course`) = the course file's name exactly: a race finds its
     course by this label;
   - `SurfaceType` = 2 for a dirt course, else as the tarmac courses;
   - `Filename` = the base name of the course's menu pictures and of the
     logger map (`logger\<Filename>`);
   - `DisplayName` = the name shown in the menus.
3. **Make it raceable:**
   - GT mode: an event whose EVENT row has this course as `CourseID` (see
     [Adding an event](#adding-an-event));
   - arcade: entries in product.gcf's course lists (next step).
4. **product.gcf course lists** (section type 0x8). Each list holds
   `{name, available, unk5, level, length}` per course; a course's place in
   the list is its index:

   | List | Used by |
   |---|---|
   | `SINGLE_RACE` | arcade single race, arcade records, arcade level progress |
   | `TIME_ATTACK` | arcade free run, free run records |
   | `TWO_PLAYER` | 2-player battle |
   | `I_LINK` | i.LINK battle |
   | `COMPULSORY` | the time trial menu |

   Set `available` = 1 to show it, `level` = the arcade level that unlocks
   it (**0 to 5 only**: higher values read past the menus' tables), `length`
   = the length shown in the menu.
5. **Menu assets** (brief):
   - `menu\arcade.mbl`: layout items `<course>`, `<course>_logo`,
     `<course>_doc`, `<course>_mini`;
   - `stream\course.ipic`: a still named after the course;
   - `stream\<name in lower case>_full.pss`: the preview movie (keep the
     name under about 45 characters), or `..._full.mp4` (H.264 and AAC,
     movies.md "MP4 movies");
   - `menu\gt_mode.mbl`: items `<Filename>` and `<Filename>_doc` (the
     course logo and text on GT mode's event pages).

A reverse layout is not a flag: it is another course with a file of its
own.

## Adding an event

1. **The EVENT row** (ParamDB Editor, table EVENT). The label (`EventID`)
   is the race code; its letters decide what kind of race it is (next
   section). Fill in:
   - `CourseID`: a COURSE row's label;
   - `Opponent1..32`: ENEMY_CARS rows, the first unused one "-"; with
     `OpponentCarColour1..32` (an opponent missing from ENEMY_CARS is left
     out of the race);
   - `Regulation`: a REGULATIONS row (the cars allowed), or "-" for any
     car;
   - `Laps`, `NumEntries`, the enemy and tyre-wear fields, the gold /
     silver / bronze times, `PrizeMoney1st..6th`;
   - `NeedLicense`, `NeedTire`, `NeedTuning`, `NeedAspiration`,
     `NeedDrivetrain`: the entry conditions;
   - `Difficulty`: the league (it decides which league's completion the
     event counts toward), `TrophyType`, `SeriesChampBonus` for a
     championship.

   `RaceModeCode` is not read: a new event needs nothing in racemode.db or
   racedetail.db.
2. **The event in product.gcf** (section type 0x4): `{name, value}` with
   `name` = the race code and `value` = the requirement (the event opens
   when `GameStatus::isLicenseSPassed` reaches it: 0 = open at once, 1 =
   once every S licence test has a medal). **Required** for any event a menu button points at: a button
   whose code is missing here crashes the menu. Without it the race still
   runs, but its result is not saved.
3. **Prize cars** (product.gcf section type 0xF, optional):
   `{name, count, cars[] {code, colour, weight}}`, at most **4 cars** (a
   set of more gives no prize at all). What a set is won for comes from its
   name:
   - `<series>0000`: every race of a series won;
   - `<series>NN00`: championship NN;
   - `<series>000N`: endurance race N;
   - fixed names: `arcade_*_complete`, `gtmode_*_complete`,
     `achieve_half`, `achieve_3_quoter`, `achieve_complete`.
4. **The GT mode button** (`menu\gt_mode.mbl`, brief): a button whose
   record name is the race code, with one of the actions
   - `set_race_code` (a single race), `set_race_code_spot` (a spot race),
     `set_race_code_champ` (a championship: see below);
   - `go_free_run` (GT mode free run: the button name is the course).

   The event pages show up to **11** status buttons each.

### Race codes

The race code's letters decide its kind (`race_name_split`,
src/game/data/T489_car_database.cpp):

| Code | Kind |
|---|---|
| `G` ... ending `00d` | GT mode single race |
| other `G` ... | championship race (the last two digits the round) |
| `G` with `_e_` / `_n_` / `_h_` | the league (beginner / amateur / professional) |
| `E` ... | endurance |
| `D` ... | dirt |
| `ATdd` | time trial dd (01..10 only) |
| `ATA` ... | free run |
| `A2` ... | 2-player battle |
| other `A` ... | arcade |
| `L0B`, `L0A`, `LIB`, `LIA`, `L0S`, `L0R` | licence tests |
| `M4`, `M1`, `MM` | machine tests |

**Championships:** `set_race_code_champ` adds 1 to the code's last
character; the number of rounds is the two digits 4 and 3 places from the
end; each next round rewrites the last two digits. Keep the series numbering
to two digits (event numbers per series stay below 0x100). Race codes are at
most 63 characters, championship codes at most 31.

### How a race finds its course

- GT mode, time trial, licences: the EVENT row's `CourseID`.
- Arcade single race, free run, 2-player, i.LINK: the course chosen in the
  menu (from the product.gcf list), with fixed race codes (`A?..T0001` /
  `A?..D0001`, `ATAF0001`, `A2P0001`, `ALNK0001`).
- A course code the course list does not have loads the **first course** in
  the list, silently.

## Order of work

1. The course file (`data\courses\US\`).
2. The COURSE row, its strings.
3. ENEMY_CARS / REGULATIONS rows if the event needs new ones, then the
   EVENT row.
4. product.gcf: the event (0x4), the course in the lists (0x8), prize sets
   (0xF).
5. The menu assets: the `gt_mode.mbl` button, the course pictures, the
   still, the movie.

## If a piece is missing

| Missing | What happens |
|---|---|
| the course file (or not in `courses\US`) | the first course in the list loads instead |
| the COURSE row of a course the arcade lists | crash in the arcade course select (no check) |
| the EVENT row of a button's code | the race is built from nothing: unpredictable |
| the event in product.gcf, with a button for it | crash on the event page; no result saved (logged) |
| an ENEMY_CARS row | that opponent left out |
| menu pictures / the still / the movie | missing pictures (not traced further) |
| a product.gcf table not sorted by name (byte order) | lookups miss existing entries (binary search) |

## What still limits events and courses

| Limit | Where | Why it stays |
|---|---|---|
| Arcade records page: 34 courses | its menu hooks (`course_logo_01..34`, `course_name_..`, `mark_..`), T030 | fixed menu items; courses 35+ race but do not show there |
| Arcade course levels 0..5 | T025, T020 tables | menu tables (a higher level reads past them) |
| Time trial: 10 races (AT01..AT10) | T417 | fixed race and preset tables |
| Licences: 6 x 8 tests | T420 | fixed tables |
| 11 status buttons per GT mode page | T036 | menu hooks |
| 4 cars per prize set | T061 | the prize screen |
| 32 opponents per event, 8 cars on track, 6 places | EVENT row, race code | data format / race engine |
| 65 535 rows per database table | paramdb | file format (16-bit counts) |
| Championships: 8 entries, 2-digit rounds | T151, T422 | save / code format |
| In the course file: 4 + 4 lights, 16 smoke types, 8 flare types, 4 drive parameter sets, 8 contact points per gadget shape | T178, T179 | course file format |
| Ranking codes past 256 courses / 512 cars | T431 | the code's 8 / 9 bits (become ambiguous, nothing breaks) |

## product.gcf

`gameconf\product.gcf` is binary: the game reads it whole and turns its
offsets into pointers in place, so entries cannot be appended without
rebuilding the offsets. The records are described in
`src/game/data/gcf_internal.h` (sections 0x0 general, 0x4 events, 0x6 the
best-time table, 0x8 course lists, 0xA service prices, 0xC attract demos,
0xD memory-card demos, 0xE car categories, 0xF prize sets). Sections 0x4,
0x6 and 0xF are searched by binary search with signed-char compares: keep
them sorted by byte order. If a section type appears twice, the last one
counts.

## In the log

`build\bin\gt3pc.log` (lines start `PC: `):

- `EventResults: event ... is not in product.gcf` - a race ran whose code
  is not in section 0x4 (no result saved);
- `FreeRunRecords: course ... is not in product.gcf's TIME_ATTACK list`,
  `ArcadeRecords: ...`, `CourseBestTimes: ...`, `PrizeSetFlags: ...`;
- `course <path>: N bytes, the course arena's unit grown ...` - a course
  bigger than retail's 13 MB;
- `course gadgets: X of Y left out`, `course gadget shape i: n contact
  points, 8 kept`, `course flare source of type t left out`;
- `GameStatus load: N <list> not in the current data: kept, written back by
  the next save` - saved results of events / courses a mod removed.
