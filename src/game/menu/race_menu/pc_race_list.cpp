/* pc_race_list.cpp - PC (ROADMAP 35, no original): the pre-race menu's list
 * of every car of a race of more than 6 (pc_race_list.h).
 *
 * PcRaceListPanel is an Item of the page's tree (its own res: the opaque
 * box), made by the first row hook of a pre-race page whose session has
 * ShortMenuArgs::allCount > 6.  Each row hook gives it the hooked layout
 * item (the name, or the time / points) of its row; the first one of a row
 * also gives it the page's plain items in that row's band (the number box
 * "noN", the dark strips): their layout items are drawn again in the box
 * (Items that are not in the tree: "stamps"), with the place, name and time
 * / points of car offset + row, the player's line in SHORT_MENU_HIGHLIGHT
 * as the hooks do.  Its z is above every row item's (the page's popups come
 * above it), its fade in / out the rows' (the first hooked item's flags).
 * A slim scroll bar (track, and a thumb in the number boxes' yellow) is at
 * its right.  The input (control thread) only moves `offset'; the texts are
 * set again by the box's update (the render thread, which draws it). */
#include "game/menu/framework/T213_menu_item.h"            /* gt2k_menu::Item (base, the stamps) */
#include "game/menu/framework/T214_menu_control.h"         /* gt2k_menu::MenuControl (up / down) */
#include "game/menu/framework/T220_object.h"               /* gt2k_menu::Object (operator new: the menu heap) */
#include "game/menu/framework/T229_page_layout.h"          /* PageLayout, MenuLayer, MenuLayoutItem, ITEM_* */
#include "game/menu/framework/T233_menu_sound.h"           /* menu_play_se, MENU_SE_CURSOR */
#include "game/data/messages/T114_msg_shortmenu.h"        /* g_002B1B30, the SHORTMENU texts ("pt" / "pts") */
#include "game/data/T489_car_database.h"         /* db_car_name */
#include "game/common/T494_text_utils.h"           /* format_time */
#include "engine/pdistd/T534_sprintf.h"              /* sprintf */
#include "libc.h"                           /* strcmp */
#include "game/menu/race_menu/T102_race_menu_helpers.h"    /* ShortMenuArgs (all, allCount) */
#include "game/menu/race_menu/pc_race_list.h"
#include "game/menu/race_menu/shortmenu_internal.h"                           /* g_003514F0, SHORT_MENU_HIGHLIGHT */

namespace gt2k_menu {

enum {
    PC_LIST_ROWS   = 6,                     /* the page's rows (list[6]) */
    PC_LIST_PIECES = 6                      /* a row's plain items (number box, strips) at most */
};

/* The box's margin around the rows and the scroll bar's width (menu
 * units). */
static const f32 kListPad = 3.0f;
static const f32 kListBarW = 4.0f;

/* The box: opaque black (the page's opaque bars' colour); the scroll
 * track dark grey; the thumb the number boxes' colour (this until a number
 * box is seen: their yellow). */
static const u8 kBoxRGBA[4]   = { 0x00, 0x00, 0x00, 0xFF };
static const u8 kTrackRGBA[4] = { 0x40, 0x40, 0x40, 0xFF };
static const u8 kThumbRGBA[4] = { 0xFF, 0xD1, 0x00, 0xFF };

class PcRaceListPanel;
/* The box of the page being shown (NULL: none); cleared by its
 * destructor. */
static PcRaceListPanel *s_panel = NULL;

class PcRaceListPanel : public Item {
public:
    PcRaceListPanel(PageLayout *layout, MenuLayoutItem *first);
    virtual ~PcRaceListPanel();
    virtual void virtual_1();               /* the fade; the texts after a scroll */
    virtual void virtual_2();               /* the box, the rows, the scroll bar */

    void addRow(PageLayout *layout, MenuLayoutItem *item, int column, int row);
    void scanRow(PageLayout *layout, MenuLayoutItem *item, int row);
    void extend(const MenuLayoutItem *item);
    void place();                           /* the box and the scroll bar from the bounds */
    void refresh();                         /* every row's texts and the thumb */
    void refreshRow(int row);

    MenuLayoutItem boxRes;                  /* this Item's res: the box */
    MenuLayoutItem trackRes;
    MenuLayoutItem thumbRes;
    u8   boxColors[16];
    u8   trackColors[16];
    u8   thumbColors[16];
    Item track;
    Item thumb;
    Item piece[PC_LIST_ROWS][PC_LIST_PIECES];   /* the row's plain items, by z */
    int  pieceCount[PC_LIST_ROWS];
    int  pieceNumber[PC_LIST_ROWS][PC_LIST_PIECES]; /* it shows the place (a number box) */
    int  scanned[PC_LIST_ROWS];
    Item text[PC_LIST_ROWS][2];             /* [0] the name, [1] the time / points */
    int  textUsed[PC_LIST_ROWS][2];
    int  rightKind[PC_LIST_ROWS];           /* PC_RACE_LIST_TIME / _POINTS */
    int  thumbColorSet;
    f32  minX, minY, maxX, maxY;            /* the rows' bounds (layout units, y up) */
    f32  topZ;                              /* the highest z of the rows' items */
    int  level;                             /* the layout's level (z + 100 a level) */
    int  offset;                            /* the first car shown (the input moves it) */
    int  shown;                             /* the offset the texts are of (-1: none yet) */
};

static void fill_colors(u8 *dst, const u8 *rgba)
{
    for (int i = 0; i < 4; i++) {
        dst[i * 4 + 0] = rgba[0];
        dst[i * 4 + 1] = rgba[1];
        dst[i * 4 + 2] = rgba[2];
        dst[i * 4 + 3] = rgba[3];
    }
}

static void clear_res(MenuLayoutItem *res, const u8 *colors, u32 fade)
{
    memset(res, 0, sizeof(*res));
    res->flags = ITEM_HAS_COLORS | fade;
    res->colors = colors;
}

/* The box of `first''s page: hidden bounds (place() sets them), the
 * offset that shows the player's line (the player starts last: the last 6
 * then), the fade of the row items. */
PcRaceListPanel::PcRaceListPanel(PageLayout *layout, MenuLayoutItem *first)
{
    u32 fade = first->flags & (ITEM_FADE_IN_MASK | ITEM_FADE_OUT_MASK);

    fill_colors(boxColors, kBoxRGBA);
    fill_colors(trackColors, kTrackRGBA);
    fill_colors(thumbColors, kThumbRGBA);
    clear_res(&boxRes, boxColors, fade);
    clear_res(&trackRes, trackColors, 0);
    clear_res(&thumbRes, thumbColors, 0);
    for (int r = 0; r < PC_LIST_ROWS; r++) {
        pieceCount[r] = 0;
        scanned[r] = 0;
        textUsed[r][0] = 0;
        textUsed[r][1] = 0;
        rightKind[r] = PC_RACE_LIST_TIME;
    }
    thumbColorSet = 0;
    minX = minY = 1.0e9f;
    maxX = maxY = -1.0e9f;
    topZ = first->z;
    level = layout->level;
    shown = -1;

    ShortMenuArgs *args = g_003514F0;
    int count = args->allCount;
    int player = -1;
    for (int i = 0; i < count; i++) {
        if (args->all[i].highlight != 0) {
            player = i;
            break;
        }
    }
    offset = player - (PC_LIST_ROWS - 1);
    if (offset > count - PC_LIST_ROWS)
        offset = count - PC_LIST_ROWS;
    if (offset < 0)
        offset = 0;

    setResource(&boxRes, NULL);
    appear(&boxRes);                        /* the rows' fade in, if they have one */
}

PcRaceListPanel::~PcRaceListPanel()
{
    if (s_panel == this)
        s_panel = NULL;
}

/* The bounds take a layout item's rectangle. */
void PcRaceListPanel::extend(const MenuLayoutItem *item)
{
    if (item->x < minX)
        minX = item->x;
    if (item->y < minY)
        minY = item->y;
    if (item->x + item->w > maxX)
        maxX = item->x + item->w;
    if (item->y + item->h > maxY)
        maxY = item->y + item->h;
    if (item->z > topZ)
        topZ = item->z;
}

/* The page's plain items in the band of row `row' (its hooked item's
 * vertical centre, no taller than half again its height): the number box
 * (a text of digits: it shows the place) and the strips, drawn by z. */
void PcRaceListPanel::scanRow(PageLayout *layout, MenuLayoutItem *item, int row)
{
    MenuLayer *l = layout->layer;
    MenuLayoutItem *found[PC_LIST_PIECES];
    int n = 0;
    f32 cy = item->y + item->h * 0.5f;

    scanned[row] = 1;
    if (l == NULL)
        return;
    for (int i = 0; i < l->itemCount && n < PC_LIST_PIECES; i++) {
        MenuLayoutItem *it = l->items[i];
        f32 d = it->y + it->h * 0.5f - cy;
        if (d < -3.0f || d > 3.0f || it->h > item->h * 1.5f)
            continue;
        /* by z (the page's draw order) */
        int k = n++;
        while (k > 0 && found[k - 1]->z > it->z) {
            found[k] = found[k - 1];
            k--;
        }
        found[k] = it;
    }
    for (int k = 0; k < n; k++) {
        MenuLayoutItem *it = found[k];
        int number = 0;
        if ((it->flags & ITEM_HAS_TEXT) && it->text != NULL && it->text->text != NULL &&
            it->text->text[0] != '\0') {
            number = 1;
            for (const char *s = it->text->text; *s != '\0'; s++) {
                if (*s < '0' || *s > '9')
                    number = 0;
            }
        }
        piece[row][k].setResource(it, layout->getTexture(it));
        pieceNumber[row][k] = number;
        extend(it);
        if (number && thumbColorSet == 0 && (it->flags & ITEM_HAS_COLORS) && it->colors != NULL) {
            /* the thumb in the number boxes' colour */
            thumbColorSet = 1;
            fill_colors(thumbColors, it->colors);
            thumb.setVertexColors(&thumbRes);
        }
    }
    pieceCount[row] = n;
}

/* Row `row''s hooked item (column: PC_RACE_LIST_*): its text stamp, the
 * row's plain items (the first time), the bounds, the texts of the car
 * shown there. */
void PcRaceListPanel::addRow(PageLayout *layout, MenuLayoutItem *item, int column, int row)
{
    int c = column == PC_RACE_LIST_NAME ? 0 : 1;

    text[row][c].setResource(item, layout->getTexture(item));
    textUsed[row][c] = 1;
    if (c == 1)
        rightKind[row] = column;
    extend(item);
    if (scanned[row] == 0)
        scanRow(layout, item, row);
    place();
    if (shown != offset) {
        /* no row has texts yet */
        shown = offset;
        for (int r = 0; r < PC_LIST_ROWS; r++)
            refreshRow(r);
    } else {
        refreshRow(row);
    }
    refresh();                              /* the thumb */
}

/* The box around the bounds, the scroll bar at its right (inside it), all
 * above every row item. */
void PcRaceListPanel::place()
{
    f32 z = topZ + 0.5f + (f32)level * 100.0f;

    boxRes.x = minX - kListPad;
    boxRes.y = minY - kListPad;
    boxRes.w = maxX - minX + kListPad * 3.0f + kListBarW;
    boxRes.h = maxY - minY + kListPad * 2.0f;
    boxRes.z = z;
    setRect(&boxRes);
    trackRes.x = maxX + kListPad;
    trackRes.y = minY;
    trackRes.w = kListBarW;
    trackRes.h = maxY - minY;
    trackRes.z = z;
    if (track.res != &trackRes)
        track.setResource(&trackRes, NULL);
    else
        track.setRect(&trackRes);
}

/* The texts of row `row': car offset + row's place, name and time /
 * points (the hooks' formats; the player's line highlighted). */
void PcRaceListPanel::refreshRow(int row)
{
    ShortMenuArgs *args = g_003514F0;
    char buf[0x80];

    if (args == NULL)
        return;
    int e = offset + row;
    if (e < 0 || e >= args->allCount)
        return;
    ShortMenuResultEntry *line = &args->all[e];

    sprintf(buf, "%d", e + 1);
    for (int k = 0; k < pieceCount[row]; k++) {
        if (pieceNumber[row][k] != 0)
            piece[row][k].setText(buf);
    }
    if (textUsed[row][0] != 0) {
        Item *name = &text[row][0];
        const char *car = line->car != 0 ? db_car_name(line->car) : NULL;
        if (car == NULL) {
            name->setText("---");
            name->textColor = 0;
        } else {
            sprintf(buf, "%s", car);
            name->setText(buf);
            name->textColor = line->highlight != 0 ? SHORT_MENU_HIGHLIGHT : 0;
        }
    }
    if (textUsed[row][1] != 0) {
        Item *right = &text[row][1];
        if (rightKind[row] == PC_RACE_LIST_POINTS) {
            if (line->points >= 2)
                sprintf(buf, "%d %s", line->points, g_002B1B30[SHORTMENU_TEXT_POINT_PLURAL]);
            else
                sprintf(buf, "%d %s", line->points, g_002B1B30[SHORTMENU_TEXT_POINT_SINGLE]);
        } else {
            format_time(buf, line->time);
        }
        right->setText(buf);
        right->textColor = line->highlight != 0 ? SHORT_MENU_HIGHLIGHT : 0;
    }
}

/* Every row's texts and the thumb for `offset'. */
void PcRaceListPanel::refresh()
{
    ShortMenuArgs *args = g_003514F0;

    if (args == NULL)
        return;
    if (shown != offset) {
        shown = offset;
        for (int r = 0; r < PC_LIST_ROWS; r++)
            refreshRow(r);
    }
    int range = args->allCount - PC_LIST_ROWS;
    f32 trackH = maxY - minY;
    f32 thumbH = trackH * (f32)PC_LIST_ROWS / (f32)args->allCount;
    if (thumbH < 6.0f)
        thumbH = 6.0f;
    f32 travel = trackH - thumbH;
    f32 at = range > 0 ? travel * (f32)offset / (f32)range : 0.0f;
    thumbRes.x = trackRes.x;
    thumbRes.w = trackRes.w;
    thumbRes.h = thumbH;
    thumbRes.y = maxY - at - thumbH;        /* from the top (y up) */
    thumbRes.z = trackRes.z;
    if (thumb.res != &thumbRes)
        thumb.setResource(&thumbRes, NULL);
    else
        thumb.setRect(&thumbRes);
}

void PcRaceListPanel::virtual_1()
{
    Item::virtual_1();
    if (shown != offset)
        refresh();
}

/* The box (this Item), each row's items in their order, then the scroll
 * bar; all at the box's alpha (its fade). */
void PcRaceListPanel::virtual_2()
{
    if (g_003514F0 == NULL || visible == 0)
        return;
    Item::virtual_2();
    for (int r = 0; r < PC_LIST_ROWS; r++) {
        for (int k = 0; k < pieceCount[r]; k++) {
            piece[r][k].alphaRatio = alphaRatio;
            piece[r][k].virtual_2();
        }
        for (int c = 0; c < 2; c++) {
            if (textUsed[r][c] != 0) {
                text[r][c].alphaRatio = alphaRatio;
                text[r][c].virtual_2();
            }
        }
    }
    track.alphaRatio = alphaRatio;
    track.virtual_2();
    thumb.alphaRatio = alphaRatio;
    thumb.virtual_2();
}

/* The page shows the laps (an object of hook `qm_laps'): a pre-race page
 * (the results pages have none). */
static int page_is_pre_race(PageLayout *layout)
{
    MenuLayer *l = layout->layer;

    if (l == NULL)
        return 0;
    for (int i = 0; i < l->objectCount; i++) {
        MenuLayoutItem *o = l->objects[i];
        if ((o->flags & ITEM_TYPE_MASK) == ITEM_HAS_TYPE && o->type != NULL && o->type->name != NULL &&
            strcmp(o->type->name, "qm_laps") == 0)
            return 1;
    }
    return 0;
}

} /* namespace gt2k_menu */

using namespace gt2k_menu;

void pc_race_list_row(PageLayout *layout, Object *parent, MenuLayoutItem *item, int column, int row)
{
    ShortMenuArgs *args = g_003514F0;

    if (args == NULL || args->allCount <= PC_LIST_ROWS || row < 0 || row >= PC_LIST_ROWS ||
        layout == NULL || parent == NULL || item == NULL)
        return;
    if (page_is_pre_race(layout) == 0)
        return;
    PcRaceListPanel *panel = s_panel;
    if (panel == NULL || panel->parent != parent || panel->isAlive() == 0) {
        panel = new PcRaceListPanel(layout, item);  /* Object::operator new: the menu heap */
        panel->attachTo(parent);
        s_panel = panel;
    }
    panel->addRow(layout, item, column, row);
}

void pc_race_list_input(const MenuControl *pad, int popupLevel)
{
    PcRaceListPanel *panel = s_panel;
    ShortMenuArgs *args = g_003514F0;

    if (panel == NULL || args == NULL || popupLevel != 0 || panel->isAlive() == 0)
        return;
    int last = args->allCount - PC_LIST_ROWS;
    int at = panel->offset;
    if (pad->up != 0 && at > 0)
        at--;
    else if (pad->down != 0 && at < last)
        at++;
    if (at != panel->offset) {
        panel->offset = at;                 /* the box's update sets the texts */
        menu_play_se(MENU_SE_CURSOR);
    }
}
