# Languages and discs

GT3 PC runs the US build's code (the decomp of SCUS-97102's CORE.GT3, with
the enhanced engine's ports) on the files of the USA, Europe and Japan
discs, in any of the seven languages that code knows; other Gran Turismo 3
engine discs are extracted as data only.

## The languages

The US build's code already speaks the PAL build's languages. Its country
(`Country g_00352FD8`, T516; set by `set_country`, T050) picks:

| | Japanese | English (US) | English (UK) | French | German | Italian | Spanish |
|---|---|---|---|---|---|---|---|
| country (`Country::code`) | 0 `JP` | 1 `US` | 2 `BR` | 3 `FR` | 4 `GR` | 5 `IT` | 6 `SP` |
| menus (`menu/<code>/`, T197 / T241) and race display (`race/<code>/display.imgs`, T154) | JP | US | BR | FR | GR | IT | SP |
| real-time texts (`text/realtime.dat`, block = country) | 0 | 1 | 2 | 3 | 4 | 5 | 6 |
| courses' region (`course_set_region`) | JP | US | UK | EU | EU | EU | EU |
| units (`UnitSettings::setPreset`) | 1 | 2 | 3 | 4 | 5 | 6 | 7 |

and a few rules of its own (Opel shown as Vauxhall in Britain, T044 / T435;
the Japanese name tables, T482). The US build simply never changes it from
English (US); the PAL build asks at its first start (`menu/language.mbl`,
code the US build does not have) and keeps the answer in the save; the
Japanese build is Japanese.

`[Game] Language` (gt3pc.ini; the launcher's Game tab, the settings menu's
GAME > LANGUAGE) chooses it: `auto` (each version's own: USA English (US),
Europe English (UK), Japan Japanese) or a language. The game takes it at its start (`platform/language.h`
`language::current`): in `game_init` (T007), right after the real-time texts
are read and before the databases, the product config and the menus are,
`set_country` gets it; after a game is loaded (`main`, T000) it is set
again instead of the save's own country (the save is then written with it).
A language whose menus are not installed (`menu/<code>/gt_mode301.pmb`) is
not offered, and one set anyway falls back to the disc's. A change in the
settings menu applies at the next start (its value says `NEXT START` until
then).

Each release's code reads its own region's car database and product
config. Here the language's are read, when the data has them, else the
disc's own (the install marker's `database`, `strings`, `unistrings`), else
the US build's:

| language | car database (`paramdb`, `paramstr`, `paramunistr`) | product config |
|---|---|---|
| Japanese | `database/paramdb.db` ... | `gameconf/product.gcf` |
| English (US) | `database/paramdb_us.db` ... | `gameconf/product_us.gcf`, else `product.gcf` |
| the European ones | `database/paramdb_eu.db` ... | `gameconf/product_eu.gcf`, else `product.gcf` |

(the PAL disc's `product_eu.gcf` is its `product.gcf` without the US demo
replay `replay_demo01`; its `product.gcf` and `product_us.gcf` are the US
disc's.) The log says what a run took: `language: FRENCH (FR),
database/paramdb_eu.db, ...`.

## The versions and their files

Three versions are played, each from a data folder of its own next to the
game, which the launcher's Play tab starts (`gt3pc.exe --data <folder>`;
docs/launcher.md):

| version | discs | data folder |
|---|---|---|
| Gran Turismo 3 A-Spec (USA) | SCUS-97102, PBPX-95503 (the bundle: the same files) | `data_US` |
| Gran Turismo 3 A-Spec (Europe) | SCES-50294 | `data_EU` |
| Gran Turismo 3 A-Spec (Japan) | SCPS-15009 | `data_JP` |

Any other disc with `CORE.GT3` (GT Concept, a demo) is extracted as data
only, into `data_<boot file>` (`data_SCES_508.58`): GT Concept's code is
not GT3's, and the US code does not boot on its files (user, 2026-10-24).
The game started without `--data` takes `[Game] Version` (the last played),
else the first of `data_US`, `data_EU`, `data_JP` there.

The installed files: one unified structure, the volume's own where the
discs agree, so that the game reads every version the same way:

- the courses are always in a region's folder, `courses\<US|JP|UK|EU>\`:
  the US, PAL and bundle discs have all four; the Japanese disc's volume has
  its 80 courses in `courses\` itself (its code lists `courses` and reads
  `courses/<name>`), and the launcher puts them into `courses\JP\`
  (`unified_path`, src/launcher/install.cpp). The game's course table and
  paths take the language's region folder, else the first of US, JP, UK,
  EU there (`course_table_init`, T179: retail listed `courses/US` always and
  read the region's).

## The Japanese version

The US code on the Japanese disc's files (version 1.01; a static comparison
of the two discs, 2026-10-24, then the user's runs):

- Its courses lie flat (above): `course_table_init` looked up `courses/US`,
  found nothing and read the empty TOC's word at address 4 - the crash at
  boot (user report).
- It has no `stream/course.ipic` (the arcade menus' course and class
  stills; its own code shows the course movies alone): the US code's
  `ArcadeControl::load` waited for the file forever. Now loaded when there,
  the stills skipped otherwise (T010, T022, T025).
- The rest checked and compatible: `paramdb.db` (the US disc's very file),
  `racedetail.db` (fewer records), `product.gcf` (4 US cars fewer:
  `s2000_*_99_ext`, the Vauxhalls), `menu/gt_mode.mbl` (the US-only cars'
  items and `layer16..18` / `tip16..18` fewer, which the code does not name;
  TCS / ASM items more), `race/JP/display.imgs` (the mph speedometers
  `speed_120mp` / `speed_160mp` fewer: a Japanese run shows km/h),
  `sound/se.inf` (the Pod's voices fewer: the enhanced engine's, off without
  them), `text/realtime.dat` (67 texts of 193: the others stay the built-in
  English). Its `race/US/display.imgs` is another, older set - but the
  Japanese files have the Japanese menus only, so only Japanese is offered.
- 115 cars of the US disc are not on it (its product config does not have
  them), nor 38 wheel files.

## The discs

What the discs hold (their GT3.VOL compared with the US disc's 5,333
files, 2026-10-24):

| disc | boot file | its code reads | own language | languages installed | against the US disc |
|---|---|---|---|---|---|
| Gran Turismo 3 A-Spec (USA) | SCUS_971.02 | `_us` | English (US) | all seven | - |
| Gran Turismo 3 A-Spec (USA, bundle) | PBPX_955.03 | `_us` | English (US) | all seven | the same files (2 sizes differ) |
| Gran Turismo 3 A-Spec (Europe) | SCES_502.94 | `_eu` | English (UK) | all seven | the PAL BGM set (`bgm/ads.inf` lists it), the `_p` demo replays (the PAL code's `demo/%s%s_p`; the US code reads the plain ones, there too), 15 more wheels, 2 tyres; 702 files of other sizes |
| Gran Turismo 3 A-Spec (Japan) | SCPS_150.09 | plain | Japanese | Japanese | no `_us` / `_eu` databases, no other language's menus; 320 fewer course files, other BGM |
| GT Concept 2001 Tokyo (Japan) | SCPS_150.10 | `_bs` | Japanese | all seven | GT3's files nearly all (37 of its movies, `stream/`, and 66 BGM tracks not), plus GT Concept's: `concept*.gcf`, `menu/concept*.mbl`, 194 more car files; 1,360 files of other sizes |
| GT Concept 2002 Tokyo-Geneva (Europe) | SCES_508.58 | `_gtc_eu` | English (UK) | all seven (and `cn`, `kr`, `pt`, `tw`, not offered) | likewise, 3,080 files more (bikes too) |
| GT Concept 2002 Tokyo-Seoul (Korea) | SCPS_560.05 | `_kr` | English (US) | all seven (and `cn`, `kr`, `tw`, not offered) | likewise, 1,804 files more |

What it means:

- The US disc's European languages: their menus are there (an earlier
  translation than the PAL disc's: 89 to 120 of each language's 143 to 146
  menu and race display files differ), but its `text/realtime.dat` has the French, German, Italian
  and Spanish blocks empty and English (UK)'s partly (135 of 193): those
  texts stay the built-in English. The PAL disc's `realtime.dat` has all
  seven languages full (194 texts each) - a transplant fixes it.
- The Japanese disc: Japanese only.
- GT Concept: data only (it does not boot on the US code, as expected: its
  front end, `concept.gcf` and `menu/concept*.mbl`, is not ported - the
  enhanced engine ports Lupo 2's game logic - nor are its extra languages:
  the US code has seven countries, `Country::code` has no code past
  Spanish). Its discs carry GT3's files too, `paramdb_us.db` / `_eu.db` of
  their own (not the same as GT3's: the US one 1,947,504 bytes against
  1,931,944).
- The PAL version boots (user's run 2026-10-24): English (UK), the `_eu`
  databases, `product_eu.gcf`, its 640 x 512 movies - at 25 frames a second,
  which ran 6/5 too fast at the port's 59.94 Hz until `MenuBgMovie::update`
  paced them by their own rate (docs/movies.md 4, "The PAL movies").
- Not checked yet, by playing: the PAL / Japanese BGM lists, the Japanese
  texts in the US code's menus.
- The saves: each version its own on the same memory card folders, under
  its release's product code (`BASCUS-97102`, `BESCES-50294`,
  `BISCPS-15009`: docs/game_start.md "The saves").
- The starting credits: the Japanese product config's 15,000 x 100 =
  1,500,000 yen, the US and PAL ones' 18,000 Cr.; a setting changes them
  (docs/game_start.md).
