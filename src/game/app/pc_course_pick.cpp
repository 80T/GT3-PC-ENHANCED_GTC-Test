/* pc_course_pick.cpp - the course randomizer and the course forcer
 * (pc_course_pick.h).  No original: new PC code. */
#include "game/app/pc_course_pick.h"
#include "game/course/T179_course_data.h"               /* CourseData, course_path, course_table_name */
#include "game/course/T457_runway.h"                    /* Runway (its grid count) */
#include "game/data/T481_car_data_base.h"               /* g_002EBD88, nameToCode */
#include "game/data/T482_car_equipments.h"              /* CARDB_COURSE, CARDB_RACE */
#include "game/data/T484_race_database.h"               /* CourseRecord, RaceEvent */
#include "game/data/T489_car_database.h"                /* race_name_split, RACE_KIND_* */
#include "game/race/core/T124_race_information.h"       /* RaceInformation */
#include "game/race/core/T125_race_organization.h"      /* RaceOrganization (raceDB, courseCode) */
#include "game/race/core/T129_race_base.h"              /* RaceInfoBlock */
#include "game/race/core/T149_race_replay_information.h"    /* RaceReplayInformation */
#include "game/race/modes/T135_race_arcade_demo.h"      /* RaceArcadeDemoInformation */
#include "game/race/modes/T148_race_machine_test.h"     /* RaceMachineTestInformation */
#include "game/menu/framework/T213_menu_item.h"         /* gt2k_menu::Item */
#include "game/menu/framework/T213_menu_texture.h"      /* MenuTexture, MenuUV */
#include "game/menu/framework/T229_page_layout.h"       /* PageLayout, MenuLayoutData, ITEM_HAS_TEXTURE */
#include "game/menu/framework/T261_menu_draw_state.h"   /* MENU_SCREEN_H */
#include "engine/io/T502_file_resolve.h"                /* file_locate_n */
#include "engine/io/T503_vol_thread.h"                  /* vol_device_object */
#include "platform/host_file.h"
#include "platform/settings.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

using namespace gt2k_menu;

namespace {

/* A course the run can load. */
struct Course {
    u64 code;
    std::string label;                                  /* its file's name */
    int places;                                         /* start places; -1 not known */
    f32 lap;                                            /* its lap length (m); 0 not known */
    bool row;                                           /* the database has it */
};

/* What an organization's race runs other than its record (0: the
 * record's): its laps, its time limit in minutes, the course whose file it
 * reads (pc_race_run_bind; every organization is made by
 * race_organization_create, which sets or clears its entry). */
struct RaceRun {
    int laps = 0;
    int minutes = 0;
    u64 file = 0;
    int mode = 0;                                       /* PC (2026-11-07): the course mode forcer's (settings::CourseMode) */
    bool modeLogged = false;
};
std::unordered_map<const RaceOrganization *, RaceRun> g_runs;

/* A RaceInfoBlock's in its spare bytes (unk_080, after the course scale's
 * 10 bytes): a tag, then the value (u16; the file a u64), each when not the
 * record's.  (After the car multipliers' parts, pc_car_tuning.cpp.)  PC
 * (2026-11-07): the mode in the six bytes between the scale and the laps.
 * PC (2026-11-08): the two bytes after the laps' (+0x16) are the rev
 * limiter's mark (pc_rev_cut.cpp), the two after the time's (+0x1E) the
 * gear shifts' (pc_shift.cpp). */
constexpr u32 kModeTag = 0x45444F4D;                    /* "MODE" */
constexpr int kModeAt = 0x0A;
constexpr u32 kLapsTag = 0x5350414C;                    /* "LAPS" */
constexpr int kLapsAt = 0x10;
constexpr u32 kTimeTag = 0x454D4954;                    /* "TIME" */
constexpr int kTimeAt = 0x18;
constexpr u32 kFileTag = 0x454C4946;                    /* "FILE" */
constexpr int kFileAt = 0x74;
static_assert(kModeAt + 4 + 2 <= kLapsAt, "the mode's six bytes");
static_assert(kFileAt + 4 + sizeof(u64) <= sizeof(((RaceInfoBlock *)0)->unk_080), "the spare bytes");

/* PC (2026-11-07): the course mode forcer's line modes' lines, m along
 * the lap: the machine tests' (T311 g_002D75F0). */
constexpr f32 kModeLines[3] = {400.0f, 1000.0f, 6845.0f};
const char *const kModeLabels[] = {"", "the course's own finish line", "the 400 m line", "the 1000 m line",
                                   "the 6845 m line (max speed)"};

/* The file pc_course_force gave the race being made (0: none). */
u64 g_forcedFile;

std::vector<Course> g_courses;                          /* by label */
/* The randomizer's courses (indexes of g_courses): 0 the leagues' and the
 * endurance events', 1 the rally events'; PC (2026-11-07) 2 the extended
 * setting's: every course of the database with a file and a start grid but
 * the licence sections (the "L_" files), whatever its surface. */
constexpr int kPoolExtended = 2;
std::vector<int> g_pool[3];

const Course *find_course(u64 code)
{
    for (const Course &c : g_courses)
        if (c.code == code)
            return &c;
    return nullptr;
}

/* A string of the car database (paramstr); "" past the table. */
const char *db_string(u16 index)
{
    CarDataBase &db = g_002EBD88;
    if (db.strings == NULL || index >= (u32)db.strings->numOfElements)
        return "";
    return db.getString(index);
}

/* The start places of course `code': its runway's grid count, read from
 * the head of its file (CourseData +0x70 the runway, Runway +0x28 the
 * count; offsets from the address word 0 names).  -1 when the file is not
 * a plain host file (packed, on the disc) or the values look wrong. */
static_assert(offsetof(CourseData, runway) == 0x70, "the course file's runway word");
static_assert(offsetof(Runway, unk_28) == 0x28, "the runway's grid count");
static_assert(offsetof(Runway, lapLength) == 0x18, "the runway's lap length");

/* ... and its lap length (Runway +0x18, m; 0 when not known). */
void read_head(u64 code, int *places, f32 *lap)
{
    char path[0x100], host[0x200];
    int lsn = 0;
    u32 size = 0;
    *places = -1;
    *lap = 0.0f;
    course_path(code, path);
    host_file_quiet(1);
    int fd = -1;
    if (file_locate_n(path, host, sizeof(host), &lsn, &size, &vol_device_object) != -1 && host[0] != '\0')
        fd = host_file_open(host);
    host_file_quiet(0);
    if (fd < 0)
        return;
    u32 base = 0, runway = 0, grid = 0;
    f32 length = 0.0f;
    if (host_file_read_at(fd, 0, &base, 4) == 4 && (base & 0xFFFF) != 0x8B1F &&     /* (not gzipped) */
        host_file_read_at(fd, offsetof(CourseData, runway), &runway, 4) == 4) {
        if (runway == 0) {
            *places = 0;
        } else if (runway > base) {
            const uint64_t at = (uint64_t)(runway - base);
            if (host_file_read_at(fd, at + offsetof(Runway, unk_28), &grid, 4) == 4 && grid <= 64)
                *places = (int)grid;
            if (host_file_read_at(fd, at + offsetof(Runway, lapLength), &length, 4) == 4 && length > 1.0f &&
                length < 1.0e6f)
                *lap = length;
        }
    }
    host_file_close(fd);
}

/* The randomizer's discipline of a race name: 0 a league or endurance
 * event, 1 a rally event, -1 none (a licence, a test, the arcade...). */
int discipline(const char *race)
{
    if (race == NULL || strlen(race) < 4)           /* (race_name_split reads a G name's last four) */
        return -1;
    int kind = 0, index = 0;
    race_name_split(race, &kind, &index);
    if (kind == RACE_KIND_GT_SINGLE || kind == RACE_KIND_GT_CHAMPIONSHIP || kind == RACE_KIND_GT_ENDURANCE)
        return 0;
    if (kind == RACE_KIND_GT_DIRT)
        return 1;
    return -1;
}

const char *race_label(RaceInformation *info)
{
    return info->db.event != NULL ? db_string(info->db.event->name) : "?";
}

/* The course label of a code (its file's name), "?" when not a course. */
const char *course_label(u64 code)
{
    const Course *c = find_course(code);
    return c != nullptr ? c->label.c_str() : "?";
}

std::mt19937 &rng()
{
    static std::mt19937 r{std::random_device{}()};
    return r;
}

/* The course table's "?" texture (its gzip name: hoge_T_localize_izumi4_r_
 * texture_<language>_q_min.img). */
bool question_texture(const MenuTexture &t)
{
    const u8 *p = (const u8 *)t.data;
    if (p == NULL || p[0] != 0x1F || p[1] != 0x8B || (p[3] & 0x08) == 0)  /* gzip with a name (FNAME) */
        return false;
    size_t at = 10;
    if (p[3] & 0x04)                                                        /* FEXTRA */
        at += 2 + (size_t)(p[10] | p[11] << 8);
    const char *name = (const char *)p + at;
    static const char kPrefix[] = "hoge_T_localize_izumi4_r_texture_";
    const size_t len = strnlen(name, 96);
    if (len >= 96 || strncmp(name, kPrefix, sizeof(kPrefix) - 1) != 0)
        return false;
    return strstr(name, "_q_min.") != NULL || (len >= 6 && strcmp(name + len - 6, "_q_min") == 0);
}

/* The layout's item showing the "?" texture (any page, any layer): the
 * leagues' "event_hide"; that item by name when no texture matches. */
MenuLayoutItem *question_face(PageLayout *layout)
{
    MenuLayoutData *d = layout->data;
    if (d == NULL)
        return NULL;
    int tex = -1;
    for (int t = 0; t < d->textureCount && tex < 0; t++)
        if (question_texture(d->textures[t]))
            tex = t;
    if (tex >= 0) {
        auto shows = [tex](MenuLayoutItem *it) {
            return it != NULL && (it->flags & ITEM_HAS_TEXTURE) != 0 && it->uv != NULL && it->uv->texture == tex;
        };
        for (int p = 0; p < d->pageCount; p++) {
            MenuPage *page = d->pages[p];
            for (int i = 0; i < page->itemCount; i++)
                if (shows(page->items[i]))
                    return page->items[i];
            for (int l = 0; l < page->layerCount; l++) {
                MenuLayer *layer = page->layers[l];
                for (int i = 0; i < layer->itemCount; i++)
                    if (shows(layer->items[i]))
                        return layer->items[i];
                for (int i = 0; i < layer->objectCount; i++)
                    if (shows(layer->objects[i]))
                        return layer->objects[i];
            }
        }
    }
    return layout->page != NULL ? layout->findAnyItem("event_hide") : NULL;
}

} // namespace

void pc_course_pick_init(void)
{
    CarDataBase &db = g_002EBD88;
    g_courses.clear();
    g_pool[0].clear();
    g_pool[1].clear();
    g_pool[kPoolExtended].clear();
    if (db.db == NULL)
        return;
    /* every file of the course folder (the table lists the folder and the
     * mods'), the database's or not */
    const int rows = db.getDataBlock(CARDB_COURSE)->numElements;
    const int files = course_table_size();
    int unknown = 0, noRow = 0;
    for (int i = 0; i < files; i++) {
        const char *name = course_table_at(i);
        const u64 code = nameToCode(name);
        const bool row = db.getRecord(CARDB_COURSE, code) != NULL;
        int places;
        f32 lap;
        read_head(code, &places, &lap);
        if (!row && places < 0) {
            printf("PC: courses: %s is not a course file: left out\n", name);
            continue;
        }
        unknown += places < 0;
        noRow += !row;
        g_courses.push_back({code, name, places, lap, row});
    }
    std::sort(g_courses.begin(), g_courses.end(),
              [](const Course &a, const Course &b) { return _stricmp(a.label.c_str(), b.label.c_str()) < 0; });

    /* the randomizer's courses: those the events of each discipline race on */
    DataBlock *races = db.getDataBlock(CARDB_RACE);
    for (int i = 0; i < races->numElements; i++) {
        const RaceEvent *ev = (const RaceEvent *)races->getElement(i);
        const int d = discipline(db_string(ev->name));
        if (d < 0)
            continue;
        const Course *c = find_course(ev->course);
        if (c == nullptr)
            continue;
        const int index = (int)(c - g_courses.data());
        if (std::find(g_pool[d].begin(), g_pool[d].end(), index) == g_pool[d].end())
            g_pool[d].push_back(index);
    }
    /* PC (2026-11-07): the extended pool - the database's courses with a
     * start grid (a file without a row cannot be a race's course), not the
     * licence sections */
    for (size_t i = 0; i < g_courses.size(); i++) {
        const Course &c = g_courses[i];
        if (c.row && c.places != 0 && _strnicmp(c.label.c_str(), "L_", 2) != 0)
            g_pool[kPoolExtended].push_back((int)i);
    }

    std::vector<settings::CourseChoice> choices(g_courses.size());
    for (size_t i = 0; i < g_courses.size(); i++) {
        snprintf(choices[i].label, sizeof(choices[i].label), "%s", g_courses[i].label.c_str());
        choices[i].places = g_courses[i].places;
    }
    settings::set_course_choices(choices.data(), (int)choices.size());
    printf("PC: courses: %d files, %d of them not in the database (its %d rows; %d start places not known); "
           "the randomizer's: %d for the leagues and endurance, %d for the rally events, %d extended (any surface)\n",
           (int)g_courses.size(), noRow, rows, unknown, (int)g_pool[0].size(), (int)g_pool[1].size(),
           (int)g_pool[kPoolExtended].size());
}

int pc_course_force(RaceInformation *info)
{
    course_read_file_of(0, 0);                          /* (a race's own, each) */
    g_forcedFile = 0;
    if (!settings::course_forcer() || info == NULL)
        return 0;
    if (dynamic_cast<RaceReplayInformation *>(info) != NULL || dynamic_cast<RaceArcadeDemoInformation *>(info) != NULL)
        return 0;                                       /* a saved race plays where it was driven */
    char label[64];
    settings::forced_course(label, sizeof(label));
    if (label[0] == '\0')
        return 0;
    const Course *c = find_course(nameToCode(label));
    if (c == nullptr) {
        printf("PC: course forcer: %s is not a course of this data; %s keeps its own\n", label, race_label(info));
        return 0;
    }
    if (c->places == 0) {
        printf("PC: course forcer: %s has no start places: %s keeps its own (%s)\n", c->label.c_str(),
               race_label(info), course_label(info->getCourseCode()));
        return 0;
    }
    /* (more cars than places start along its racing line: pc_grid_start) */
    if (!c->row) {
        /* not in the database: the race stays on its own course, its name,
         * kind and tyres, and reads this file */
        printf("PC: course forcer: %s reads the file %s (not in the database) for its own course %s\n",
               race_label(info), c->label.c_str(), course_label(info->getCourseCode()));
        course_read_file_of(info->getCourseCode(), c->code);
        g_forcedFile = c->code;
        return 1;
    }
    if (info->getCourseCode() != c->code) {
        printf("PC: course forcer: %s on %s (its own: %s)\n", race_label(info), c->label.c_str(),
               course_label(info->getCourseCode()));
        info->setCourseCode(c->code);
    }
    return 1;
}

void pc_course_career(RaceInformation *info)
{
    if (info == NULL || pc_course_force(info) != 0 || !settings::course_randomizer())
        return;
    const int d = discipline(race_label(info));
    if (d < 0)
        return;
    /* PC (2026-11-07): extended - any surface: the extended pool, and a grid
     * short of the cars does (the cars past it start along the racing line,
     * pc_race_cars.h, as the forcer's); on: the discipline's pool, a grid
     * that holds the cars */
    const bool extended = settings::course_randomizer_mode() == settings::CourseRandomizer::extended;
    const int cars = info->getEntryCount();
    std::vector<int> fit;
    for (int index : g_pool[extended ? kPoolExtended : d])
        if (g_courses[index].places < 0 || (extended ? g_courses[index].places != 0 : g_courses[index].places >= cars))
            fit.push_back(index);
    if (fit.empty()) {
        printf("PC: course randomizer: no course holds %s's %d cars; its own\n", race_label(info), cars);
        return;
    }
    const Course &c = g_courses[fit[std::uniform_int_distribution<int>(0, (int)fit.size() - 1)(rng())]];
    printf("PC: course randomizer: %s (%d cars) on %s (its own: %s; one of %d%s)\n", race_label(info), cars,
           c.label.c_str(), course_label(info->getCourseCode()), (int)fit.size(), extended ? ", any surface" : "");
    info->setCourseCode(c.code);
}

int pc_course_hidden(const char *event)
{
    return settings::course_randomizer() && discipline(event) >= 0;
}

Item *pc_course_question(PageLayout *layout, const char *course, MenuLayoutItem *pos)
{
    MenuLayoutItem *q = question_face(layout);
    MenuLayoutItem *face = course != NULL ? layout->findHiddenItem(course) : NULL;
    MenuLayoutItem *place = pos != NULL ? pos : face;
    if (q == NULL || place == NULL)
        return NULL;
    /* The "?" with its own size over the course's picture: a course mini
     * (55 x 42) holds the picture in its lower part (2..53 x 12..40, the top
     * 12 rows clear: the US minis' PNGs), the size of the "?" (51 x 27): so
     * centred across, its top 12/42 of the mini's height down (centring it
     * on the mini put it 4.5 lines high, on the button's top edge).  y is
     * down here (Item::setRect). */
    f32 down = 0.0f;
    if (face != NULL)
        down = face->h * (12.0f / 42.0f) + q->h * 0.5f - face->h * 0.5f;
    Item *item = new Item(q, layout);
    item->x.set(place->x + place->w * 0.5f);
    item->y.set((f32)MENU_SCREEN_H - (place->y + place->h * 0.5f) + down);
    item->z = face != NULL ? face->z : place->z;
    return item;
}

void pc_race_run_bind(RaceOrganization *org, RaceInformation *info)
{
    g_runs.erase(org);
    if (org == NULL || info == NULL || org->raceDB == NULL || org->raceDB->event == NULL)
        return;
    if (dynamic_cast<RaceReplayInformation *>(info) != NULL || dynamic_cast<RaceArcadeDemoInformation *>(info) != NULL)
        return;                                         /* (a saved race's: pc_race_run_load) */
    /* a machine test's record laps are the test, its time none */
    const bool test = dynamic_cast<RaceMachineTestInformation *>(info) != NULL;
    const RaceEvent *ev = org->raceDB->event;
    const char *race = db_string(ev->name);
    RaceRun run;
    run.file = g_forcedFile;
    if (settings::race_time_forcer() && !test) {
        run.minutes = settings::forced_race_time();
        if (ev->limitTime != 0)
            printf("PC: race time: %s runs %d minutes (the race time forcer; its own %d)\n", race, run.minutes,
                   ev->limitTime);
    }
    /* PC (2026-11-07): the course mode forcer (not the tests: they have
     * modes of their own) */
    const int mode = (int)settings::course_mode_forcer();
    if (mode != (int)settings::CourseMode::off && !test && mode > 0 && mode < settings::kCourseModeCount) {
        run.mode = mode;
        printf("PC: course mode: %s runs to %s (the course mode forcer)\n", race, kModeLabels[mode]);
    }
    if (settings::lap_forcer() && !test) {
        run.laps = settings::forced_laps();
        printf("PC: laps: %s runs %d laps (the lap forcer; its own %d)\n", race, run.laps, ev->numberOfLaps);
    } else if (settings::course_randomizer() && discipline(race) >= 0 && ev->numberOfLaps != 0) {
        const Course *own = find_course(ev->course), *drawn = find_course(org->courseCode);
        if (own != nullptr && drawn != nullptr && own != drawn && own->lap > 0.0f && drawn->lap > 0.0f) {
            int laps = (int)floorf(ev->numberOfLaps * own->lap / drawn->lap + 0.5f);
            laps = laps < 1 ? 1 : laps > 255 ? 255 : laps;
            if (laps != ev->numberOfLaps) {
                run.laps = laps;
                printf("PC: laps: %s runs %d laps of %s (%.0f m; its own %d of %s, %.0f m: %.1f km -> %.1f km)\n",
                       race, laps, drawn->label.c_str(), drawn->lap, ev->numberOfLaps, own->label.c_str(), own->lap,
                       ev->numberOfLaps * own->lap / 1000.0f, laps * drawn->lap / 1000.0f);
            }
        }
    }
    if (run.laps != 0 || run.minutes != 0 || run.file != 0 || run.mode != 0)
        g_runs[org] = run;
}

/* PC (2026-11-07): the course mode forcer's mode of a race */
int pc_race_mode(const RaceOrganization *org)
{
    auto it = g_runs.find(org);
    return it != g_runs.end() ? it->second.mode : 0;
}

int pc_race_mode_lines(const RaceOrganization *org)
{
    const int m = pc_race_mode(org);
    return m >= (int)settings::CourseMode::m400 ? m - (int)settings::CourseMode::m400 + 1 : 0;
}

f32 pc_race_mode_line(const RaceOrganization *org, int i, f32 lapLength)
{
    const int lines = pc_race_mode_lines(org);
    if (i < 0 || i >= lines)
        return -1.0f;
    f32 d = kModeLines[i];
    /* a line past the lap: 10 m before its end (the lap distance wraps there) */
    if (lapLength > 20.0f && d > lapLength - 10.0f)
        d = lapLength - 10.0f;
    return d;
}

void pc_race_mode_course(const RaceOrganization *org, f32 ownLine, f32 lapLength)
{
    auto it = g_runs.find(org);
    if (it == g_runs.end() || it->second.mode == 0 || it->second.modeLogged)
        return;
    it->second.modeLogged = true;
    const int lines = pc_race_mode_lines(org);
    if (lines == 0) {
        if (ownLine != 0.0f)
            printf("PC: course mode: the finish line at %.0f m of the %.0f m lap (the course's own)\n", ownLine,
                   lapLength);
        else
            printf("PC: course mode: the course has no finish line of its own: the race's laps\n");
        return;
    }
    char text[96] = "";
    for (int i = 0; i < lines; i++) {
        const f32 d = pc_race_mode_line(org, i, lapLength);
        snprintf(text + strlen(text), sizeof(text) - strlen(text), "%s%.0f m%s", i > 0 ? ", " : "", d,
                 d != kModeLines[i] ? " (held short of the lap)" : "");
    }
    printf("PC: course mode: the finish line and the checkpoints at %s of the %.0f m lap, no lap count\n", text,
           lapLength);
}

int pc_race_laps(const RaceOrganization *org, int own)
{
    if (own <= 0)
        return own;
    auto it = g_runs.find(org);
    return it != g_runs.end() && it->second.laps != 0 ? it->second.laps : own;
}

int pc_race_minutes(const RaceOrganization *org, int own)
{
    if (own <= 0)
        return own;
    auto it = g_runs.find(org);
    return it != g_runs.end() && it->second.minutes != 0 ? it->second.minutes : own;
}

void pc_race_run_store(RaceInfoBlock *blk, const RaceOrganization *org)
{
    auto it = g_runs.find(org);
    if (it == g_runs.end())
        return;
    const u16 laps = (u16)it->second.laps, minutes = (u16)it->second.minutes;
    if (laps != 0) {
        memcpy(blk->unk_080 + kLapsAt, &kLapsTag, 4);
        memcpy(blk->unk_080 + kLapsAt + 4, &laps, 2);
    }
    if (minutes != 0) {
        memcpy(blk->unk_080 + kTimeAt, &kTimeTag, 4);
        memcpy(blk->unk_080 + kTimeAt + 4, &minutes, 2);
    }
    if (it->second.file != 0) {
        memcpy(blk->unk_080 + kFileAt, &kFileTag, 4);
        memcpy(blk->unk_080 + kFileAt + 4, &it->second.file, sizeof(u64));
    }
    if (it->second.mode != 0) {                         /* PC (2026-11-07) */
        const u16 mode = (u16)it->second.mode;
        memcpy(blk->unk_080 + kModeAt, &kModeTag, 4);
        memcpy(blk->unk_080 + kModeAt + 4, &mode, 2);
    }
}

void pc_race_run_load(RaceOrganization *org, const RaceInfoBlock *blk)
{
    g_runs.erase(org);
    u32 tag;
    u16 value;
    RaceRun run;
    memcpy(&tag, blk->unk_080 + kLapsAt, 4);
    memcpy(&value, blk->unk_080 + kLapsAt + 4, 2);
    if (tag == kLapsTag)
        run.laps = value;
    memcpy(&tag, blk->unk_080 + kTimeAt, 4);
    memcpy(&value, blk->unk_080 + kTimeAt + 4, 2);
    if (tag == kTimeTag)
        run.minutes = value;
    memcpy(&tag, blk->unk_080 + kFileAt, 4);
    if (tag == kFileTag) {
        u64 file;
        memcpy(&file, blk->unk_080 + kFileAt + 4, sizeof(u64));
        if (course_table_name(file) != NULL) {
            run.file = file;
            course_read_file_of(org->courseCode, file);
            printf("PC: the saved race reads the file %s\n", course_table_name(file));
        } else {
            printf("PC: the saved race's course file is not in the course folder: its own course\n");
        }
    }
    memcpy(&tag, blk->unk_080 + kModeAt, 4);           /* PC (2026-11-07): the course mode */
    memcpy(&value, blk->unk_080 + kModeAt + 4, 2);
    if (tag == kModeTag && value > 0 && value < settings::kCourseModeCount)
        run.mode = value;
    if (run.laps == 0 && run.minutes == 0 && run.file == 0 && run.mode == 0)
        return;
    g_runs[org] = run;
    printf("PC: the saved race runs %d laps, %d minutes (0: the record's)%s%s\n", run.laps, run.minutes,
           run.mode != 0 ? ", to " : "", run.mode != 0 ? kModeLabels[run.mode] : "");
}
