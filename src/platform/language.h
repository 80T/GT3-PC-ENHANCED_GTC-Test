/* language.h - the game's language in GT3 PC ([Game] Language: the
 * settings menu, F2, and the launcher's Game tab; docs/languages.md).
 *
 * The US build already speaks the PAL build's languages: its country
 * (Country g_00352FD8, T516: 0 Japanese, 1 English (US), 2 English (UK), 3
 * French, 4 German, 5 Italian, 6 Spanish) picks the menus' folder
 * (menu/<code>/), the race display's (race/<code>/), the real-time texts'
 * block (text/realtime.dat), the courses' region folder and the units, and
 * the US disc carries every language's menus.  Only the build never changes
 * it from English (US): the PAL build asks at its first start
 * (menu/language.mbl, not in the US code), the Japanese one is Japanese.
 * Here the language of a run is taken at its start - the setting's when its
 * menus are installed, else the installed disc's own (the install marker's
 * "language", paths::install_info: the PAL disc English (UK), the Japanese
 * disc Japanese; English (US) for an earlier launcher's install) - and set
 * before the databases are read (game_init, T007); a loaded save keeps it
 * (main, T000: retail took the save's).
 *
 * Each release's code reads its own region's databases (the US build
 * paramdb_us.db, the PAL build _eu, the Japanese build paramdb.db, GT
 * Concept _bs / _gtc_eu / _kr: the marker's "database", "strings",
 * "unistrings").  Here the language's: Japanese the plain ones, English
 * (US) _us, the European languages _eu, when the data has them; else the
 * disc's own.  The product config likewise: product_eu.gcf for the European
 * languages, product_us.gcf for English (US), when there, else product.gcf
 * (the PAL disc's product_eu.gcf: product.gcf without the US demo replay). */
#pragma once

namespace language {

constexpr int kCount = 7;
constexpr int kJapanese = 0, kEnglishUS = 1;

/* The country code of a language ("JP", "US", "BR", "FR", "GR", "IT", "SP":
 * the menus' folder) and its name in the settings menu ("ENGLISH (UK)"). */
const char *code(int lang);
const char *name(int lang);
/* Its menus are in the data folder (menu/<code>/gt_mode301.pmb: the
 * Japanese disc has a few files of the others' folders only). */
bool installed(int lang);
/* The installed disc's own language. */
int disc_language();
/* This run's language, taken at the first call (game_init's). */
int current();
/* The language the setting gives (installed or not: the disc's then) -
 * this run's unless the setting changed since the start. */
int chosen();

/* This run's car database files: kind 0 the parameters (paramdb), 1 the
 * strings (paramstr), 2 the unicode strings (paramunistr). */
const char *car_database(int kind);
/* This run's product config (gameconf/...). */
const char *product_config();

} // namespace language
