/* pc_race_list.h - PC (ROADMAP 35, no original): the pre-race menu's list
 * of every car of a race of more than 6.
 *
 * The short menu's pre-race pages (qm_gt_event: "RACE MENU", and
 * qm_gt_event_champ: "CHAMPIONSHIP MENU"; the pages that show the laps,
 * hook "qm_laps") list 6 rows: position number boxes, the car names
 * (qm_list_car_name1..6) and the times (qm_list_time1..6) or points
 * (qm_pt1..6) of ShortMenuArgs::list.  A race of up to PC_RACE_CARS_MAX cars
 * keeps those rows as they are; while ShortMenuArgs::allCount > 6 an opaque
 * box of our own covers them and shows 6 lines of ShortMenuArgs::all, which
 * Up / Down (not used by these pages) scroll.  The box is an object of the
 * page's tree, made by the row hooks (T113): the page's own row items
 * (their layout items: the number boxes, the strips, the texts' fonts,
 * colours and anchors) are drawn again in it with our texts, in the box's
 * place in the z order (a popup page covers it), fading in with the rows.
 * The race results (result_list_by_rank, allCount 0) and the licence
 * menus (one car) never show it. */
#ifndef GAME_MENU_RACE_MENU_PC_RACE_LIST_H
#define GAME_MENU_RACE_MENU_PC_RACE_LIST_H

#include "game/game.h"

namespace gt2k_menu {
class PageLayout;                           /* T229 (pointers only) */
class Object;                               /* T220 */
class MenuControl;                          /* T214 */
struct MenuLayoutItem;                      /* T229 */
}

/* The column of a hooked row item. */
enum {
    PC_RACE_LIST_NAME   = 0,                /* qm_list_car_name: the car's name */
    PC_RACE_LIST_TIME   = 1,                /* qm_list_time: its time */
    PC_RACE_LIST_POINTS = 2                 /* qm_pt: its points */
};

/* From the row hooks (T113), after their text is made: item is the hooked
 * layout item of row `row' (0..5, the hook's p8), parent the object the
 * page is built under.  The first row of a pre-race page whose race has
 * more than 6 cars makes the box (attached to parent); every row gives it
 * its place and look. */
void pc_race_list_row(gt2k_menu::PageLayout *layout, gt2k_menu::Object *parent,
                      gt2k_menu::MenuLayoutItem *item, int column, int row);
/* Once a frame of the short menu (ShortmenuControl::mainLoop, after the
 * page's input): Up / Down (the pad's auto-repeated directions) scroll the
 * box by one car while it is shown on the base page (popupLevel 0). */
void pc_race_list_input(const gt2k_menu::MenuControl *pad, int popupLevel);

#endif /* GAME_MENU_RACE_MENU_PC_RACE_LIST_H */
