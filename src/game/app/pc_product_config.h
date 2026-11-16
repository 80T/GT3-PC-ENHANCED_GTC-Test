/* pc_product_config.h - PC: the product config from its SQLite database
 * (platform/gcf_tables.h; docs/modding.md "product.gcf").  No original: new
 * PC code.
 *
 * gameconf/<config>.gcf (the run's: platform/language.h product_config)
 * read as gameconf/<config>.sqlite3 when that is there (the launcher deletes
 * the .gcf once it has written and checked its database) - its tables
 * general, cars, courses, events, best_times, services, demos, card_demos,
 * prizes - built into the .gcf's layout, which ProductConfig (T435)
 * relocates as ever.  A table the database lacks: the .gcf's (when it is
 * still there).  The log: "PC: product config from
 * gameconf/<config>.sqlite3: ..." and a line for each row skipped. */
#ifndef GAME_APP_PC_PRODUCT_CONFIG_H
#define GAME_APP_PC_PRODUCT_CONFIG_H

/* The config of `gcfPath' from its database, in the .gcf's layout on the
 * game heap (ProductConfig frees it as the file's buffer); NULL when there
 * is none, or it cannot be read (said; the .gcf is read, and without one
 * the game stops with the reason). */
void *pc_product_config_load(const char *gcfPath);

#endif /* GAME_APP_PC_PRODUCT_CONFIG_H */
