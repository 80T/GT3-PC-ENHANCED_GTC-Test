/* pc_headcam.cpp - PC: the cockpit camera in the driver's head
 * (pc_headcam.h).  No original: new PC code. */
#include "libc.h"
#include "game/app/pc_headcam.h"
#include "game/app/pc_headcam_find.h"
#include "game/race/core/T121_race_entry.h"         /* RaceEntryCar */
#include "game/car/model/T180_race_car_model.h"     /* RaceCarModel */
#include "game/car/model/T456_car_model_file.h"     /* CarModelFile */
#include "game/car/model/T455_car_model_info.h"     /* CarModelInfo */
#include "game/car/model/T446_car_model.h"          /* PC (2026-11-08): car_list_name (the offsets' key) */
#include "game/camera/onboard_camera_internal.h"    /* CameraMount */
#include "engine/gfx/T451_model_set.h"              /* ModelSet (the body's packets) */
#include "engine/pgl/G001_flare.h"                  /* vifmodel_pc_hide */
#include "platform/cockpit_osd.h"                   /* PC (2026-11-08): the calibration mode and its panel */
#include "platform/settings.h"

#include <math.h>
#include <string.h>
#include <unordered_map>

namespace {

/* The hide box: the head's grown by kRound all round (a little play; more
 * took the roof lining and the pillar beside a rally car's co-driver: user
 * report) and by kDown under it, where the neck and the shoulders' top are
 * (their card reaches up beside the eye). */
constexpr f32 kRound = 0.03f;
constexpr f32 kDown = 0.15f;

/* The car files loaded: the heads found in each (RaceCarModel::setData), by
 * the file.  Kept for good: a file loaded again at an address overwrites
 * what was there (setData runs for every load), and a ring of recent loads
 * lost a race car's entry when the menus had loaded many car models after
 * it (the car select's icons), leaving it the Driver eye and no hiding. */
struct Entry {
    f32 eye[3];                             /* the Driver mount's eye */
    headcam::Heads heads;
    /* PC (2026-11-08): the model's calibrated offsets by slot (x, y, z of the
     * eye's shift in the car's frame, the tilt in degrees), read from
     * gt3pc.ini [CockpitOffsets] on the first use (the key is the car's
     * model name, known from the RaceEntryCar, not the file); while
     * calibrating, the values being tried */
    f32 off[2][4];
    u8 offLoaded[2];
    /* PC (2026-11-15): the model's menu model on / off (gt3pc.ini
     * [CockpitMenuModels], read on the first use as the offsets; both
     * cockpits); while calibrating, the value being tried */
    u8 menu;
    u8 menuLoaded;
};
std::unordered_map<const CarModelFile *, Entry> g_models;
const CarModelFile *s_missed;               /* (the last file said to be missing) */

Entry *lookup(const CarModelFile *file)
{
    if (file == NULL)
        return NULL;
    auto it = g_models.find(file);
    if (it != g_models.end())
        return &it->second;
    if (s_missed != file) {
        s_missed = file;
        printf("PC: cockpit camera: no record of this car's model (loaded without setData?): the Driver eye, nothing hidden\n");
    }
    return NULL;
}

/* The "Driver" mount's eye (minus its point; the built-in mount's without a
 * mount table: T378 g_0033E370): the bumper view's, at the driver's head
 * height on the centre line.  Returns whether the file has the table (24 of
 * the 287 US cars - some colour variants' files - have none: the head is
 * then looked for round the cars' mean, 0.90 m up and 0.15 m back, with
 * wider windows). */
int driver_eye(const CarModelFile *file, f32 eye[3])
{
    eye[0] = 0.0f;
    eye[1] = 0.8f;
    eye[2] = 0.0f;
    const CarModelInfo *info = file != NULL ? file->info : NULL;
    if (info != NULL && info->mountCount >= 2 && info->mounts != NULL) {
        const CameraMount *mt = &info->mounts[0];
        eye[0] = -mt->x;
        eye[1] = -mt->y;
        eye[2] = -mt->z;
        return 1;
    }
    return 0;
}

VifPartRef s_glass[headcam::kMaxGlass];     /* what vifmodel_pc_hide holds while hiding */

const headcam::HeadBox *head_of(const RaceEntryCar *car, int slot)
{
    const Entry *e = lookup(car->model.data);
    if (e == NULL || slot < 1 || slot > e->heads.count)
        return NULL;
    return &e->heads.head[slot - 1];
}

/* ---- PC (2026-11-08): the calibrated offsets and the calibration mode ---- */

/* The camera controls' rates at full deflection (the stick's curve makes the
 * middle fine: pc_freecam.cpp), a tick 1/60 s; the offsets' bounds. */
constexpr f32 kTick = 1.0f / 60.0f;
constexpr f32 kMoveRate = 0.15f;            /* m / s */
constexpr f32 kTiltRate = 20.0f;            /* degrees / s */
constexpr f32 kMoveStep = 0.005f;           /* m a key step (the key's repeat: ~30 a second held) */
constexpr f32 kTiltStep = 0.5f;             /* degrees a key step */
constexpr f32 kMoveMax = 1.0f;              /* m each way */
constexpr f32 kTiltMax = 45.0f;             /* degrees each way */

/* The car's model name, the offsets' key ("r_gt_00": the model file's). */
const char *model_name(const RaceEntryCar *car)
{
    return car_list_name(const_cast<RaceEntryCar *>(car)->getCarCode());
}

/* The entry's offsets of slot (1, 2), read on the first use; NULL for a car
 * without an entry. */
f32 *offsets_of(const RaceEntryCar *car, int slot)
{
    Entry *e = lookup(car->model.data);
    if (e == NULL || slot < 1 || slot > 2)
        return NULL;
    if (!e->offLoaded[slot - 1]) {
        settings::cockpit_offset(model_name(car), slot, e->off[slot - 1]);
        e->offLoaded[slot - 1] = 1;
    }
    return e->off[slot - 1];
}

/* PC (2026-11-15): the entry's menu model setting, read on the first use;
 * NULL for a car without an entry. */
u8 *menu_of(const RaceEntryCar *car)
{
    Entry *e = lookup(car->model.data);
    if (e == NULL)
        return NULL;
    if (!e->menuLoaded) {
        e->menu = settings::cockpit_menu_model(model_name(car)) ? 1 : 0;
        e->menuLoaded = 1;
    }
    return &e->menu;
}

/* The calibration under way: whose offsets are being tried, and the saved
 * ones to go back to (cancel, or the mode stopped from the menu). */
struct Calibration {
    int on;
    const CarModelFile *file;
    int slot;
    char name[64];                          /* the model's (the offsets' key) */
    f32 saved[4];
    u8 savedMenu;                           /* PC (2026-11-15): the menu model's (the car's, not the slot's) */
};
Calibration s_calib;

void calibration_restore()
{
    if (!s_calib.on)
        return;
    auto it = g_models.find(s_calib.file);
    if (it != g_models.end()) {
        memcpy(it->second.off[s_calib.slot - 1], s_calib.saved, sizeof(s_calib.saved));
        it->second.menu = s_calib.savedMenu;    /* PC (2026-11-15) */
    }
    s_calib.on = 0;
}

/* The offsets tried written for the model (as written: mm and tenths of a
 * degree), the mode over. */
void calibration_save()
{
    if (!s_calib.on)
        return;
    auto it = g_models.find(s_calib.file);
    if (it != g_models.end()) {
        f32 *off = it->second.off[s_calib.slot - 1];
        for (int k = 0; k < 3; k++)
            off[k] = roundf(off[k] * 1000.0f) / 1000.0f;
        off[3] = roundf(off[3] * 10.0f) / 10.0f;
        settings::set_cockpit_offset(s_calib.name, s_calib.slot, off);
        /* PC (2026-11-15): and the car's menu model (user: "Cockpit menu
         * model needs to be per-car.  The option needs to be part of the
         * calibrator") */
        settings::set_cockpit_menu_model(s_calib.name, it->second.menu != 0);
        printf("PC: cockpit camera: %s cockpit %d calibrated: eye %+.3f %+.3f %+.3f m, tilt %+.1f degrees, menu "
               "model %s\n",
               s_calib.name, s_calib.slot, off[0], off[1], off[2], off[3], it->second.menu ? "on" : "off");
    }
    s_calib.on = 0;
}

f32 clamp_f(f32 v, f32 lo, f32 hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

} // namespace

void pc_headcam_model(CarModelFile *file)
{
    if (file == NULL)
        return;
    Entry *e = &g_models[file];
    const int mounts = driver_eye(file, e->eye);
    e->heads.count = 0;
    /* PC (2026-11-08): the offsets read again for whatever car this load is
     * (another car can come to the same address; a calibration of the old
     * one is over) */
    memset(e->off, 0, sizeof(e->off));
    e->offLoaded[0] = e->offLoaded[1] = 0;
    e->menu = 0;                            /* PC (2026-11-15): read again too */
    e->menuLoaded = 0;
    if (s_calib.file == file)
        s_calib.on = 0;
    const ModelSet *body = file->body;
    if (body != NULL && body->packets != NULL && body->nPackets != 0)
        headcam::find_heads((const void *const *)body->packets, body->nPackets, mounts ? e->eye : NULL, &e->heads);
    char text[256];
    int n = snprintf(text, sizeof(text), "PC: cockpit camera:");
    for (int i = 0; i < e->heads.count; i++) {
        const headcam::HeadBox &h = e->heads.head[i];
        n += snprintf(text + n, sizeof(text) - n, "%s head %d at (%.2f, %.2f, %.2f), %.2f x %.2f x %.2f m%s",
                      i ? ";" : "", i + 1, h.eye[0], h.eye[1], h.eye[2], h.bmax[0] - h.bmin[0],
                      h.bmax[1] - h.bmin[1], h.bmax[2] - h.bmin[2], h.merged ? " (in a batch with the seats)" : "");
    }
    if (e->heads.count == 0)
        n += snprintf(text + n, sizeof(text) - n, " no driver's head in this car's model");
    if (e->heads.glassCount != 0)
        n += snprintf(text + n, sizeof(text) - n, "; %d glass shells facing the eye", e->heads.glassCount);
    snprintf(text + n, sizeof(text) - n, " (the Driver eye %.2f, %.2f, %.2f%s)", e->eye[0], e->eye[1], e->eye[2],
             mounts ? "" : ", no mount table");
    printf("%s\n", text);
}

void pc_headcam_menu_file(CarModelFile *file)
{
    if (file == NULL)
        return;
    Entry *e = &g_models[file];
    const int mounts = driver_eye(file, e->eye);
    e->heads.count = 0;
    e->heads.glassCount = 0;
    memset(e->off, 0, sizeof(e->off));
    e->offLoaded[0] = e->offLoaded[1] = 0;
    e->menu = 0;                            /* PC (2026-11-15) */
    e->menuLoaded = 0;
    const ModelSet *body = file->body;
    if (body != NULL && body->packets != NULL && body->nPackets != 0)
        headcam::find_heads((const void *const *)body->packets, body->nPackets, mounts ? e->eye : NULL, &e->heads);
    e->heads.count = 0;                     /* (the seats: a menu model has no driver) */
    if (e->heads.glassCount != 0)
        printf("PC: cockpit menu model: %d glass shells facing the eye\n", e->heads.glassCount);
}

int pc_headcam_on(void)
{
    return settings::cockpit_camera() ? 1 : 0;
}

int pc_headcam_menu_model(const RaceEntryCar *car)
{
    /* PC (2026-11-15): the car model's own setting (the calibrator's) */
    if (car == NULL || !settings::cockpit_camera())
        return 0;
    const u8 *m = menu_of(car);
    return m != NULL && *m != 0 ? 1 : 0;
}

void pc_headcam_hide_file(const CarModelFile *file)
{
    const Entry *e = lookup(file);
    const int n = e != NULL ? e->heads.glassCount : 0;
    for (int i = 0; i < n; i++) {
        s_glass[i].model = (const VifModel *)e->heads.glass[i].model;
        s_glass[i].part = e->heads.glass[i].part;
    }
    vifmodel_pc_hide(NULL, NULL, s_glass, n);
}

f32 pc_headcam_fov(void)
{
    return (f32)settings::cockpit_fov();
}

int pc_headcam_count(const RaceEntryCar *car)
{
    const Entry *e = lookup(car->model.data);
    return e != NULL && e->heads.count >= 2 ? 2 : 1;
}

void pc_headcam_eye(const RaceEntryCar *car, int slot, f32 eye[3])
{
    const headcam::HeadBox *h = head_of(car, slot);
    if (h == NULL) {
        driver_eye(car->model.data, eye);
    } else {
        eye[0] = h->eye[0];
        eye[1] = h->eye[1];
        eye[2] = h->eye[2];
    }
    /* PC (2026-11-08): the model's calibrated shift (the head's box, which is
     * what is hidden, stays where it is) */
    const f32 *off = offsets_of(car, slot);
    if (off != NULL)
        for (int k = 0; k < 3; k++)
            eye[k] += off[k];
}

f32 pc_headcam_tilt(const RaceEntryCar *car, int slot)
{
    const f32 *off = offsets_of(car, slot);
    return off != NULL ? off[3] : 0.0f;
}

int pc_headcam_calibrating(void)
{
    /* PC (2026-11-14): the mode is the calibration's (not the custom
     * cameras' editor's, which shares the panel: cockpit_osd.h) */
    const int on = cockpit_osd::active() && cockpit_osd::mode() == cockpit_osd::kModeCockpit ? 1 : 0;
    if (!on)
        calibration_restore();              /* (stopped from the menu: the offsets tried go back) */
    return on;
}

/* PC (2026-11-14): the panel's texts are the race's side's (cockpit_osd.h):
 * the title, the car and the cockpit, the four axes with their values - or,
 * out of the cockpit view, what to do. */
static const char kCalibTitle[] = "COCKPIT CAMERA CALIBRATION";

static int calibration_frame(const char *name, int slot, const f32 *values, const char *menuText)
{
    if (name == NULL)
        return cockpit_osd::race_frame(kCalibTitle, "SWITCH TO THE COCKPIT VIEW", NULL, 0);
    static const char *const kNames[cockpit_osd::kAxisCount] = {"SIDE", "HEIGHT", "FORWARD", "TILT", "MENU MODEL"};
    cockpit_osd::Row rows[cockpit_osd::kAxisCount];
    memset(rows, 0, sizeof(rows));
    for (int a = 0; a < cockpit_osd::kAxisCount; a++) {
        snprintf(rows[a].name, sizeof(rows[a].name), "%s", kNames[a]);
        if (a == cockpit_osd::kAxisMenuModel)
            snprintf(rows[a].value, sizeof(rows[a].value), "%7s", menuText);    /* PC (2026-11-15) */
        else if (a == cockpit_osd::kAxisTilt)
            snprintf(rows[a].value, sizeof(rows[a].value), "%+7.1f DEG", values[a]);
        else
            snprintf(rows[a].value, sizeof(rows[a].value), "%+7.3f M", values[a]);
    }
    char head[96];
    snprintf(head, sizeof(head), "%s   COCKPIT%s", name, slot == 2 ? " 2" : "");
    return cockpit_osd::race_frame(kCalibTitle, head, rows, cockpit_osd::kAxisCount);
}

int pc_headcam_calibrate(const RaceEntryCar *car, int slot, int port, f32 stickX, f32 stickY)
{
    /* the calibrating player's camera alone (a two-player race has two: each
     * fed one calibration, and the panel flickered between the two cars -
     * user report); a replay's camera, without a player, is player 1's */
    const int who = port < 0 ? 0 : port;
    cockpit_osd::player_seen(who);
    if (who != cockpit_osd::player())
        return 0;
    f32 *off = car != NULL ? offsets_of(car, slot) : NULL;
    if (off == NULL) {
        /* not the cockpit view (or a car without a record): the panel says
         * so; save / cancel still end the mode, for the car last adjusted */
        cockpit_osd::take_steps();          /* (steps asked for out of the view go nowhere) */
        const int action = calibration_frame(NULL, 0, NULL, NULL);
        if (action == cockpit_osd::kCancel) {
            calibration_restore();
            cockpit_osd::stop();
        } else if (action == cockpit_osd::kSave) {
            calibration_save();
            cockpit_osd::stop();
        }
        return 1;
    }
    u8 *menu = menu_of(car);                /* PC (2026-11-15): the car's menu model (an entry: as off) */
    if (!s_calib.on || s_calib.file != car->model.data || s_calib.slot != slot) {
        /* (the other cockpit of the same car: the menu model's saved value
         * stays the one from before the calibration) */
        const int sameCar = s_calib.on && s_calib.file == car->model.data;
        s_calib.on = 1;
        s_calib.file = car->model.data;
        s_calib.slot = slot;
        snprintf(s_calib.name, sizeof(s_calib.name), "%s", model_name(car));
        memcpy(s_calib.saved, off, sizeof(s_calib.saved));
        if (!sameCar)
            s_calib.savedMenu = *menu;
    }
    /* the axis chosen on the panel (one at a time: user), by the keys' steps
     * (Left / Right: 5 mm, half a degree) and the camera controls on its
     * natural direction - side: left / right (pc_freecam_stick gives right
     * negative), the rest up / down, the tilt as the look controls turn
     * (stick up looks down: user, "the Y axis should match the chase cam
     * look controls").  The panel's axes are the offsets' in order; its
     * forward is -z. */
    const int axis = cockpit_osd::axis() < cockpit_osd::kAxisCount ? cockpit_osd::axis() : 0;
    const int steps = cockpit_osd::take_steps();
    if (axis == cockpit_osd::kAxisMenuModel) {
        /* PC (2026-11-15): the menu model - Right on, Left off (held, it
         * stays: no flicker); the camera controls nothing.  Turned on, the
         * menu file is read at the next frame (RacePS2Base::virtual_3) */
        if (steps != 0)
            *menu = steps > 0 ? 1 : 0;
    } else {
        const f32 stick = axis == cockpit_osd::kAxisSide ? -stickX : axis == cockpit_osd::kAxisTilt ? -stickY : stickY;
        const f32 rate = axis == cockpit_osd::kAxisTilt ? kTiltRate : kMoveRate;
        const f32 step = axis == cockpit_osd::kAxisTilt ? kTiltStep : kMoveStep;
        const f32 sign = axis == cockpit_osd::kAxisForward ? -1.0f : 1.0f;
        off[axis] += sign * (stick * rate * kTick + (f32)steps * step);
        for (int k = 0; k < 3; k++)
            off[k] = clamp_f(off[k], -kMoveMax, kMoveMax);
        off[3] = clamp_f(off[3], -kTiltMax, kTiltMax);
    }

    const f32 values[cockpit_osd::kAxisCount] = {off[0], off[1], -off[2] + 0.0f, off[3], 0.0f};   /* (+ 0: no "-0.000") */
    /* (on, but the car has no menu model to draw: its file missing or without
     * a body - the race model stays) */
    const char *menuText = *menu == 0 ? "OFF" : car->model.pcMenuState == 2 ? "ON (NOT FOUND)" : "ON";
    switch (calibration_frame(s_calib.name, slot, values, menuText)) {
    case cockpit_osd::kSave:
        calibration_save();
        cockpit_osd::stop();
        break;
    case cockpit_osd::kCancel:
        calibration_restore();
        cockpit_osd::stop();
        break;
    case cockpit_osd::kReset:
        if (axis == cockpit_osd::kAxisMenuModel)
            *menu = 0;                      /* PC (2026-11-15): the race model */
        else
            off[axis] = 0.0f;               /* (the chosen axis alone: user) */
        break;
    default:
        break;
    }
    return 1;
}

/* PC (2026-11-14): the custom cameras' side (pc_headcam.h). */
void pc_headcam_from(const RaceEntryCar *car, int which, f32 eye[3])
{
    eye[0] = eye[1] = eye[2] = 0.0f;
    const CarModelFile *file = car->model.data;
    switch (which) {
    case 1:
        driver_eye(file, eye);
        break;
    case 2: {
        /* the chase mount's eye (minus its point; the built-in mount's
         * without a mount table: T378 g_0033E370) */
        const CarModelInfo *info = file != NULL ? file->info : NULL;
        if (info != NULL && info->mountCount >= 2 && info->mounts != NULL) {
            eye[0] = -info->mounts[1].x;
            eye[1] = -info->mounts[1].y;
            eye[2] = -info->mounts[1].z;
        } else {
            eye[1] = 1.5f;
            eye[2] = 6.0f;
        }
        break;
    }
    case 3:
        pc_headcam_eye(car, 1, eye);
        break;
    default:
        break;
    }
}

void pc_headcam_hide_at(const RaceEntryCar *car, const f32 eye[3], int on)
{
    const Entry *e = on ? lookup(car->model.data) : NULL;
    if (e == NULL) {
        vifmodel_pc_hide(NULL, NULL, NULL, 0);
        return;
    }
    /* the head the eye is in: its box as pc_headcam_hide cuts it */
    const headcam::HeadBox *in = NULL;
    f32 lo[3], hi[3], centre[3], half[3];
    for (int i = 0; i < e->heads.count && in == NULL; i++) {
        const headcam::HeadBox &h = e->heads.head[i];
        int inside = 1;
        for (int k = 0; k < 3; k++) {
            lo[k] = h.bmin[k] - kRound;
            hi[k] = h.bmax[k] + kRound;
        }
        lo[1] -= kDown;
        for (int k = 0; k < 3; k++)
            inside &= eye[k] >= lo[k] && eye[k] <= hi[k];
        if (inside)
            in = &h;
    }
    if (in != NULL)
        for (int k = 0; k < 3; k++) {
            centre[k] = 0.5f * (lo[k] + hi[k]);
            half[k] = 0.5f * (hi[k] - lo[k]);
        }
    /* the glass shells facing inward whose box holds the eye: from there
     * they are front faces, as from the driver's seat */
    int n = 0;
    for (int i = 0; i < e->heads.glassCount; i++) {
        const headcam::Ref &g = e->heads.glass[i];
        int inside = 1;
        for (int k = 0; k < 3; k++)
            inside &= eye[k] >= g.lo[k] && eye[k] <= g.hi[k];
        if (!inside)
            continue;
        s_glass[n].model = (const VifModel *)g.model;
        s_glass[n].part = g.part;
        n++;
    }
    if (in == NULL && n == 0) {
        vifmodel_pc_hide(NULL, NULL, NULL, 0);
        return;
    }
    vifmodel_pc_hide(in != NULL ? centre : NULL, in != NULL ? half : NULL, s_glass, n);
}

void pc_headcam_hide(const RaceEntryCar *car, int slot, int on)
{
    const headcam::HeadBox *h = on ? head_of(car, slot) : NULL;
    if (h == NULL) {
        vifmodel_pc_hide(NULL, NULL, NULL, 0);
        return;
    }
    f32 lo[3], hi[3], centre[3], half[3];
    for (int k = 0; k < 3; k++) {
        lo[k] = h->bmin[k] - kRound;
        hi[k] = h->bmax[k] + kRound;
    }
    lo[1] -= kDown;
    for (int k = 0; k < 3; k++) {
        centre[k] = 0.5f * (lo[k] + hi[k]);
        half[k] = 0.5f * (hi[k] - lo[k]);
    }
    /* (and the glass shells wound to face the eye, not drawn at all) */
    const Entry *e = lookup(car->model.data);
    for (int i = 0; i < e->heads.glassCount; i++) {
        s_glass[i].model = (const VifModel *)e->heads.glass[i].model;
        s_glass[i].part = e->heads.glass[i].part;
    }
    vifmodel_pc_hide(centre, half, s_glass, e->heads.glassCount);
}
