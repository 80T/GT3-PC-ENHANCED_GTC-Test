# A new game, a loaded one, and the saves

What GT3 PC adds to the start of a game (`src/game/app/pc_new_game.*`) and
how the versions keep their saves apart.

## The starting credits

A new game is `GameStatus::init` (T429) on the game's own status
(`createGameStatus` at boot; a loaded save replaces it). Its money is the
product config's `initial` service price (`ProductConfig::servicePrice`),
which the code multiplies by 100 in Japanese, yen, as it does every price
(`CarInformation::getPrice` too). The discs' `initial` (2026-10-24):

| product config | `initial` | a new game |
|---|---|---|
| US disc `product.gcf` | 18,000 | 18,000 Cr. |
| PAL disc `product_eu.gcf` (and its `product.gcf`) | 18,000 | 18,000 Cr. |
| Japanese disc `product.gcf` | 15,000 | 1,500,000 yen |

`[Game] StartingCredits` (the launcher's Game tab, F2 GAME > STARTING
CREDITS; from the next new game) replaces it: `auto` (the version's own,
above), `15000` or `18000` - x 100 in Japanese (1,500,000 / 1,800,000 yen).
The money is 64-bit (`GameStatus::money`, `format_number`).

## The debug start (the launcher's Debug tab, `[Debug]`; read at start)

- **FillGarage** (a new game): one of every car of the car database
  (`db_car_count` / `db_car_code`: the parameter database the run reads)
  whose model is in the car list (`car_list_has`: the files of
  `cars/day`), in each of its colours (`CarColorDataBase::getNumColors` of
  its model; one when it has none) - so the cars added to or taken out of
  the data come and go with it. The garage grows in blocks (ROADMAP 18); the
  log says how many: `PC: new game: the garage filled: ...`. A car whose
  stock parts (the preset's, else the car record's) name a part that is not
  in its table is left out, with a line `PC: new game: car <name> (<code>)
  left out: its <part> part <code> is not in the database`: the game reads
  those records without a test, and a modded database's broken car crashed
  the new game (2026-10-25). A tuning part (lightweight, port polish ...
  wheel) with code 0 is no part, not a missing one.
- **FillMoney** (a new game): 100,000,000; in Japanese 10,000,000,000 yen.
- **UnlockArcade**: every course everywhere. The product config's arcade
  mode lists (`g_002944A8`: SINGLE_RACE, TIME_ATTACK, TWO_PLAYER, I_LINK,
  POD_RACE when there) are merged when it loads (`pc_merge_course_lists`,
  T435): the longest list's courses in its order, then the ones another
  list adds, every list then that one, each course available - the US
  disc's 36 courses in every mode (single race gains `slalom80r` both ways,
  two players Cote d'Azur, `montecarlo`). And a new game's and a loaded
  game's arcade progress level (`GameStatus::arcadeLevel`, which opens a
  course of its level or below) is raised to the highest course level of
  those lists: every course open. The cars' levels (`arcadeClear`), the
  rewards of a full clear (`arcadeLevel >= 6`, T030) and the region locks
  (which cars a version has) stay as they are. Off, the product config's
  lists stand (an editor for them: later).
- **UnlockLicenses** (a new game and a loaded one): every test of the six
  licences (B, A, IB, IA, S, R) at least bronze (`LicenseRecords::passAll`,
  which retail has and never calls; a better medal is kept): every licence
  held. The licence trophies (`largeTrophy`, won at the licence ceremony)
  stay as they were.

A loaded game with an unlock on is saved with it.

## The saves

Each release saved under its own product code (`mcGetNamePrefix`, T108:
the region letters and the code), and so does each version here, on the
same memory card folders (`memcard1`, `memcard2`):

| version | saves |
|---|---|
| USA (and the bundle: the US code) | `BASCUS-97102...` |
| Europe | `BESCES-50294...` |
| Japan | `BISCPS-15009...` |

The version is the install marker's boot file (`paths::install_info`; an
earlier launcher's install is the US disc). A version sees and loads its own
saves only.
