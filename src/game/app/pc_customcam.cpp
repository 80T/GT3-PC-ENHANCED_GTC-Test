/* pc_customcam.cpp - PC: the user's own cameras on the view button
 * (pc_customcam.h).  No original: new PC code. */
#include "libc.h"
#include "game/app/pc_customcam.h"
#include "game/app/pc_headcam.h"                    /* pc_headcam_from, pc_headcam_hide_at */
#include "game/race/core/T121_race_entry.h"         /* RaceEntryCar */
#include "game/car/model/T446_car_model.h"          /* car_list_name (the model's scope) */
#include "game/camera/T378_onboard_camera.h"        /* CameraShake */
#include "platform/cockpit_osd.h"                   /* the editor's mode, panel and requests */
#include "platform/settings.h"                      /* [CustomCameras] */
#include "platform/settings_menu.h"                 /* notice */

#include <ctype.h>
#include <math.h>
#include <string.h>
#include <string>
#include <unordered_map>

namespace {

using settings::CustomCamera;

/* ---- the cameras ----------------------------------------------------- */

/* A scope's cameras: "global", or a car model's.  Read from gt3pc.ini the
 * first time the scope is asked for and kept; the editor changes them here
 * and writes the scope back on a save or a delete. */
struct List {
    int count;
    CustomCamera cam[settings::kCustomCameraMax];
};
std::unordered_map<std::string, List> g_lists;
const char kGlobal[] = "global";

List &list_of(const char *scope)
{
    auto it = g_lists.find(scope);
    if (it != g_lists.end())
        return it->second;
    List &l = g_lists[scope];
    l.count = settings::custom_cameras(scope, l.cam);
    if (l.count != 0)
        printf("PC: custom cameras: %d of %s\n", l.count, scope);
    return l;
}

/* The car's model name, its scope ("r_gt_00": the model file's, as the
 * cockpit camera's offsets). */
const char *model_name(const RaceEntryCar *car)
{
    const char *name = car_list_name(const_cast<RaceEntryCar *>(car)->getCarCode());
    return name != NULL ? name : "";
}

/* Camera `index' (from 1) of the car - the global ones, then the model's -
 * with its scope and its place in it; NULL without. */
CustomCamera *find(const RaceEntryCar *car, int index, std::string *scope, int *at)
{
    if (car == NULL || index < 1)
        return NULL;
    List &g = list_of(kGlobal);
    if (index <= g.count) {
        if (scope != NULL)
            *scope = kGlobal;
        if (at != NULL)
            *at = index - 1;
        return &g.cam[index - 1];
    }
    const char *name = model_name(car);
    List &m = list_of(name);
    const int i = index - g.count - 1;
    if (i >= m.count)
        return NULL;
    if (scope != NULL)
        *scope = name;
    if (at != NULL)
        *at = i;
    return &m.cam[i];
}

/* ---- what is kept per onboard camera ---------------------------------- */

struct CamState {
    const void *key;
    CameraShake sx, sy;                     /* the cockpit camera's shake (the wheels' slip) */
    f32 eye[3];                             /* the eye as shown, in the car's frame */
    int shown;
};
CamState s_state[8];

CamState *state_of(const void *key)
{
    int free = -1;
    for (int i = 0; i < 8; i++) {
        if (s_state[i].key == key)
            return &s_state[i];
        if (s_state[i].key == NULL && free < 0)
            free = i;
    }
    if (free < 0)
        free = 0;
    memset(&s_state[free], 0, sizeof(s_state[free]));
    s_state[free].key = key;
    return &s_state[free];
}

/* ---- the editor ------------------------------------------------------- */

/* The panel's rows (cockpit_osd's axes), in order. */
enum {
    R_FROM, R_TYPE, R_SIDE, R_HEIGHT, R_FORWARD, R_PAN, R_TILT, R_ANGLE, R_FOV, R_RIGID,
    R_COUNT = R_RIGID + PC_RIGID_COUNT
};
const char kTitle[] = "CUSTOM CAMERA";
const char *const kFromNames[settings::kCustomFromCount] = {"CAR CENTER", "BUMPER CAM", "CHASE CAM", "COCKPIT CAM"};
const char *const kTypeNames[settings::kCustomTypeCount] = {"CHASE", "ONBOARD"};
const char *const kRowNames[R_RIGID] = {"FROM", "TYPE", "SIDE", "HEIGHT", "FORWARD", "PAN", "TILT", "ANGLE", "FOV"};
const char *const kRigidNames[PC_RIGID_COUNT] = {"TURN",       "PITCH",      "ROLL",  "SPEED SHAKE",
                                                 "SLIP SHAKE", "SIDE FORCE", "BUMPS", "SWAY"};

/* A tick 1/60 s.  The keys' steps (the key's repeat: ~30 a second held, a
 * pad's 15) and the camera controls' rates at full deflection (the stick's
 * curve makes the middle fine: pc_freecam.cpp).  The position has no limits,
 * so a held key's steps grow: 1 cm, 5 cm from the 20th in a row, 25 cm from
 * the 60th (a pause of two thirds of a second, or the other direction,
 * starts over). */
constexpr f32 kTick = 1.0f / 60.0f;
constexpr f32 kMoveStep = 0.01f;            /* m */
constexpr f32 kMoveRate = 3.0f;             /* m / s */
constexpr f32 kTurnStep = 0.5f;             /* degrees: pan, tilt */
constexpr f32 kWholeStep = 1.0f;            /* degrees: angle, field of view */
constexpr f32 kTurnRate = 30.0f;            /* degrees / s */
constexpr int kRigidStep = 5;               /* percent */
constexpr f32 kRigidRate = 50.0f;           /* percent / s */
constexpr int kRunGap = 40;                 /* ticks */
constexpr f32 kFovDefault = 60.0f;          /* the fixed views' (OnBoardCamera::fov) */

struct Edit {
    int on;                                 /* a camera is being edited */
    const void *cam;                        /* by this onboard camera (the editing player's) */
    std::string scope;                      /* its scope and its place in it */
    int at;
    int isNew;                              /* made by this edit: not in gt3pc.ini until saved */
    CustomCamera saved;                     /* what cancel puts back */
    int row;                                /* the row adjusted last (another one starts the two below over) */
    int run, runDir, idle;                  /* the key's steps in a row, their direction, the ticks since */
    f32 frac;                               /* the part of a percent the camera controls have moved a rigidity */
};
Edit s_edit;

void note(const char *text)
{
    char upper[64];
    snprintf(upper, sizeof(upper), "%s", text);
    for (char *c = upper; *c != '\0'; c++)
        *c = (char)toupper((unsigned char)*c);
    settings_menu::notice(upper, 2.0);
}

f32 clamp_f(f32 v, f32 lo, f32 hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* The number among the car's cameras of the one being edited (0: it is not
 * one of this car's). */
int edited_index(const RaceEntryCar *car)
{
    if (!s_edit.on)
        return 0;
    const int globals = list_of(kGlobal).count;
    if (s_edit.scope == kGlobal)
        return s_edit.at + 1;
    if (s_edit.scope != model_name(car))
        return 0;
    return globals + s_edit.at + 1;
}

void remove_at(List &l, int at)
{
    if (at < 0 || at >= l.count)
        return;
    for (int i = at; i + 1 < l.count; i++)
        l.cam[i] = l.cam[i + 1];
    l.count--;
}

/* The edit given up: the saved camera back, a new one gone (*index, when
 * given: the editing camera's view, 0 after its new camera went). */
void edit_cancel(int *index)
{
    if (!s_edit.on)
        return;
    List &l = list_of(s_edit.scope.c_str());
    if (s_edit.at < l.count) {
        if (s_edit.isNew) {
            remove_at(l, s_edit.at);
            if (index != NULL)
                *index = 0;
        } else {
            l.cam[s_edit.at] = s_edit.saved;
        }
    }
    s_edit.on = 0;
}

/* The camera edited written with its scope (as written: mm, tenths of a
 * degree). */
void edit_save(void)
{
    if (!s_edit.on)
        return;
    List &l = list_of(s_edit.scope.c_str());
    if (s_edit.at < l.count) {
        CustomCamera &c = l.cam[s_edit.at];
        for (int k = 0; k < 3; k++)
            c.pos[k] = roundf(c.pos[k] * 1000.0f) / 1000.0f;
        c.pan = roundf(c.pan * 10.0f) / 10.0f;
        c.tilt = roundf(c.tilt * 10.0f) / 10.0f;
        c.angle = roundf(c.angle * 10.0f) / 10.0f;
        c.fov = roundf(c.fov * 10.0f) / 10.0f;
        settings::set_custom_cameras(s_edit.scope.c_str(), l.cam, l.count);
        printf("PC: custom camera: %s.%d \"%s\" saved: from %s, %s, offset %+.3f %+.3f %+.3f m, pan %+.1f, tilt %+.1f, "
               "angle %+.1f, field of view %.1f, rigidity %d %d %d %d %d %d %d %d\n",
               s_edit.scope.c_str(), s_edit.at + 1, c.name, kFromNames[c.from], kTypeNames[c.type], c.pos[0], c.pos[1],
               c.pos[2], c.pan, c.tilt, c.angle, c.fov, c.rigid[0], c.rigid[1], c.rigid[2], c.rigid[3], c.rigid[4],
               c.rigid[5], c.rigid[6], c.rigid[7]);
    }
    s_edit.on = 0;
}

void edit_begin(const void *cam, const std::string &scope, int at, int isNew, const CustomCamera &saved)
{
    s_edit.on = 1;
    s_edit.cam = cam;
    s_edit.scope = scope;
    s_edit.at = at;
    s_edit.isNew = isNew;
    s_edit.saved = saved;
    s_edit.row = -1;
    s_edit.run = 0;
    s_edit.runDir = 0;
    s_edit.idle = 0;
    s_edit.frac = 0.0f;
}

/* A new camera of `scope', the copy of the view shown (src; NULL: of the
 * custom camera in view), put in view and edited. */
void begin_new(const void *cam, const RaceEntryCar *car, const std::string &scope, const PcCustomSource *src,
               int *index)
{
    if (scope.empty())
        return;
    List &l = list_of(scope.c_str());
    if (l.count >= settings::kCustomCameraMax) {
        char text[64];
        snprintf(text, sizeof(text), "%d cameras already: delete one", settings::kCustomCameraMax);
        note(text);
        cockpit_osd::stop();
        return;
    }
    CustomCamera c = {};
    const CustomCamera *shown = src == NULL ? find(car, *index, NULL, NULL) : NULL;
    if (shown != NULL) {
        c = *shown;
    } else if (src != NULL) {
        c.from = src->from;
        c.type = src->onboard ? settings::kCustomTypeOnboard : settings::kCustomTypeChase;
        for (int k = 0; k < 3; k++)
            c.pos[k] = src->pos[k];
        c.pan = src->pan;
        c.tilt = src->tilt;
        c.angle = clamp_f(src->angle, -settings::kCustomAngleMax, settings::kCustomAngleMax);
        c.fov = clamp_f(src->fov, settings::kCustomFovMin, settings::kCustomFovMax);
        for (int i = 0; i < PC_RIGID_COUNT; i++)
            c.rigid[i] = src->rigid[i];
    } else {
        c.from = settings::kCustomFromChase;
        c.type = settings::kCustomTypeChase;
        c.fov = kFovDefault;
        for (int i = PC_RIGID_SPEED_SHAKE; i < PC_RIGID_COUNT; i++)
            c.rigid[i] = 100;
    }
    /* its name: the first of "Custom 1", "Custom 2" .. (a model's: "Car
     * custom 1" ..) the scope does not have yet; gt3pc.ini renames it */
    const char *prefix = scope == kGlobal ? "Custom" : "Car custom";
    for (int number = 1; number <= settings::kCustomCameraMax + 1; number++) {
        snprintf(c.name, sizeof(c.name), "%s %d", prefix, number);
        int used = 0;
        for (int i = 0; i < l.count; i++)
            used |= _stricmp(l.cam[i].name, c.name) == 0;
        if (!used)
            break;
    }
    l.cam[l.count++] = c;
    *index = scope == kGlobal ? l.count : list_of(kGlobal).count + l.count;
    edit_begin(cam, scope, l.count - 1, 1, c);
}

/* The custom camera in view deleted (from gt3pc.ini too). */
void do_delete(const RaceEntryCar *car, int *index)
{
    std::string scope;
    int at = 0;
    const CustomCamera *c = find(car, *index, &scope, &at);
    if (c == NULL) {
        note("No custom camera in view");
        return;
    }
    char text[64];
    snprintf(text, sizeof(text), "Camera deleted: %s", c->name);
    if (s_edit.on) {
        /* (the one being edited: nothing to put back; another: its edit is
         * given up first - the cameras after the deleted one move up) */
        if (s_edit.scope == scope && s_edit.at == at)
            s_edit.on = 0;
        else
            edit_cancel(NULL);
        cockpit_osd::stop();
    }
    List &l = list_of(scope.c_str());
    remove_at(l, at);
    settings::set_custom_cameras(scope.c_str(), l.cam, l.count);
    *index = 0;
    note(text);
}

/* The chosen row moved: by the keys' steps and, on its natural direction, by
 * the camera controls (as pc_freecam_stick gives them: x right negative, y
 * up positive; a tilt as the look controls turn: stick up looks down). */
void adjust(CustomCamera &c, const RaceEntryCar *car, int row, int steps, f32 sx, f32 sy, int *flags)
{
    f32 grow = 1.0f;
    if (row != s_edit.row) {
        s_edit.row = row;
        s_edit.run = 0;
        s_edit.frac = 0.0f;
    }
    if (steps != 0) {
        const int dir = steps > 0 ? 1 : -1;
        if (dir != s_edit.runDir || s_edit.idle > kRunGap)
            s_edit.run = 0;
        s_edit.runDir = dir;
        s_edit.run += steps > 0 ? steps : -steps;
        s_edit.idle = 0;
        grow = s_edit.run > 60 ? 25.0f : s_edit.run > 20 ? 5.0f : 1.0f;
    } else if (s_edit.idle <= kRunGap) {
        s_edit.idle++;
    }
    const f32 move = (f32)steps * kMoveStep * grow;
    switch (row) {
    case R_FROM:
        if (steps != 0) {
            /* (the eye stays where it is: the offset is taken from the new
             * starting point) */
            const int to = (c.from + (steps > 0 ? 1 : settings::kCustomFromCount - 1)) % settings::kCustomFromCount;
            f32 a[3], b[3];
            pc_headcam_from(car, c.from, a);
            pc_headcam_from(car, to, b);
            for (int k = 0; k < 3; k++)
                c.pos[k] += a[k] - b[k];
            c.from = to;
        }
        break;
    case R_TYPE:
        if (steps != 0) {
            c.type = c.type == settings::kCustomTypeChase ? settings::kCustomTypeOnboard : settings::kCustomTypeChase;
            *flags |= PC_CUSTOM_TYPE;
        }
        break;
    case R_SIDE:
        c.pos[0] += move - sx * kMoveRate * kTick;
        break;
    case R_HEIGHT:
        c.pos[1] += move + sy * kMoveRate * kTick;
        break;
    case R_FORWARD:                         /* (forward is -z) */
        c.pos[2] -= move + sy * kMoveRate * kTick;
        break;
    case R_PAN:
        c.pan += (f32)steps * kTurnStep - sx * kTurnRate * kTick;
        if (c.pan > 180.0f)
            c.pan -= 360.0f;
        if (c.pan < -180.0f)
            c.pan += 360.0f;
        break;
    case R_TILT:
        c.tilt = clamp_f(c.tilt + (f32)steps * kTurnStep - sy * kTurnRate * kTick, -90.0f, 90.0f);
        break;
    case R_ANGLE:
        c.angle = clamp_f(c.angle + (f32)steps * kWholeStep + sy * kTurnRate * kTick, -settings::kCustomAngleMax,
                          settings::kCustomAngleMax);
        break;
    case R_FOV:
        c.fov = clamp_f(c.fov + (f32)steps * kWholeStep + sy * kTurnRate * kTick, settings::kCustomFovMin,
                        settings::kCustomFovMax);
        break;
    default: {
        const int i = row - R_RIGID;
        if (i < 0 || i >= PC_RIGID_COUNT)
            break;
        s_edit.frac += sy * kRigidRate * kTick;
        const int whole = (int)s_edit.frac;
        s_edit.frac -= (f32)whole;
        const int v = c.rigid[i] + steps * kRigidStep + whole;
        c.rigid[i] = v < 0 ? 0 : v > 100 ? 100 : v;
        break;
    }
    }
}

/* The chosen row back to its plain value: no offset, no turn, the fixed
 * views' field of view, fixed to the car. */
void reset_row(CustomCamera &c, int row, int *flags)
{
    switch (row) {
    case R_FROM:
        break;                              /* (a choice, not an amount) */
    case R_TYPE:
        break;
    case R_SIDE:
    case R_HEIGHT:
    case R_FORWARD:
        c.pos[row - R_SIDE] = 0.0f;
        break;
    case R_PAN:
        c.pan = 0.0f;
        break;
    case R_TILT:
        c.tilt = 0.0f;
        break;
    case R_ANGLE:
        c.angle = 0.0f;
        break;
    case R_FOV:
        c.fov = kFovDefault;
        break;
    default:
        if (row - R_RIGID >= 0 && row - R_RIGID < PC_RIGID_COUNT)
            c.rigid[row - R_RIGID] = 100;
        break;
    }
    (void)flags;
}

void upper_copy(char *out, size_t n, const char *text)
{
    snprintf(out, n, "%s", text);
    for (char *c = out; *c != '\0'; c++)
        *c = (char)toupper((unsigned char)*c);
}

/* The panel's texts for the camera edited; returns the action asked for. */
int edit_frame(const CustomCamera &c)
{
    cockpit_osd::Row rows[R_COUNT];
    memset(rows, 0, sizeof(rows));
    for (int r = 0; r < R_RIGID; r++)
        snprintf(rows[r].name, sizeof(rows[r].name), "%s", kRowNames[r]);
    snprintf(rows[R_FROM].value, sizeof(rows[R_FROM].value), "%s", kFromNames[c.from]);
    snprintf(rows[R_TYPE].value, sizeof(rows[R_TYPE].value), "%s", kTypeNames[c.type]);
    snprintf(rows[R_SIDE].value, sizeof(rows[R_SIDE].value), "%+8.3f M", c.pos[0]);
    snprintf(rows[R_HEIGHT].value, sizeof(rows[R_HEIGHT].value), "%+8.3f M", c.pos[1]);
    snprintf(rows[R_FORWARD].value, sizeof(rows[R_FORWARD].value), "%+8.3f M", -c.pos[2] + 0.0f);
    snprintf(rows[R_PAN].value, sizeof(rows[R_PAN].value), "%+8.1f DEG", c.pan);
    snprintf(rows[R_TILT].value, sizeof(rows[R_TILT].value), "%+8.1f DEG", c.tilt);
    snprintf(rows[R_ANGLE].value, sizeof(rows[R_ANGLE].value), "%+8.1f DEG", c.angle);
    snprintf(rows[R_FOV].value, sizeof(rows[R_FOV].value), "%8.1f DEG", c.fov);
    snprintf(rows[R_RIGID].caption, sizeof(rows[R_RIGID].caption), "RIGIDITY (100: FIXED TO THE CAR)");
    for (int i = 0; i < PC_RIGID_COUNT; i++) {
        snprintf(rows[R_RIGID + i].name, sizeof(rows[R_RIGID + i].name), "%s", kRigidNames[i]);
        snprintf(rows[R_RIGID + i].value, sizeof(rows[R_RIGID + i].value), "%8d %%", c.rigid[i]);
    }
    char name[32], scope[64], head[128];
    upper_copy(name, sizeof(name), c.name);
    upper_copy(scope, sizeof(scope), s_edit.scope == kGlobal ? "every car" : s_edit.scope.c_str());
    snprintf(head, sizeof(head), "%s   %s%s", name, scope, s_edit.isNew ? "   NEW" : "");
    return cockpit_osd::race_frame(kTitle, head, rows, R_COUNT);
}

} // namespace

int pc_customcam_count(const RaceEntryCar *car)
{
    if (car == NULL)
        return 0;
    return list_of(kGlobal).count + list_of(model_name(car)).count;
}

int pc_customcam_view(const RaceEntryCar *car, int index, PcCustomView *out)
{
    const CustomCamera *c = find(car, index, NULL, NULL);
    if (c == NULL)
        return 0;
    pc_headcam_from(car, c->from, out->eye);
    for (int k = 0; k < 3; k++)
        out->eye[k] += c->pos[k];
    out->pan = c->pan;
    out->tilt = c->tilt;
    out->angle = c->angle;
    out->fov = c->fov;
    out->onboard = c->type == settings::kCustomTypeOnboard ? 1 : 0;
    for (int i = 0; i < PC_RIGID_COUNT; i++)
        out->loose[i] = (f32)(100 - c->rigid[i]) / 100.0f;
    return 1;
}

int pc_customcam_onboard(const RaceEntryCar *car, int index)
{
    const CustomCamera *c = find(car, index, NULL, NULL);
    return c != NULL && c->type == settings::kCustomTypeOnboard ? 1 : 0;
}

const char *pc_customcam_name(const RaceEntryCar *car, int index)
{
    const CustomCamera *c = find(car, index, NULL, NULL);
    return c != NULL ? c->name : "";
}

void pc_customcam_shake(const void *cam, Random *rng, f32 gain)
{
    CamState *s = state_of(cam);
    s->sx.update(rng, gain);
    s->sy.update(rng, gain);
}

void pc_customcam_shake_get(const void *cam, f32 *x, f32 *y)
{
    CamState *s = state_of(cam);
    *x = s->sx.getValue();
    *y = s->sy.getValue();
}

void pc_customcam_shake_reset(const void *cam)
{
    CamState *s = state_of(cam);
    s->sx.reset();
    s->sy.reset();
}

void pc_customcam_shown(const void *cam, const f32 *m, const f32 *frame)
{
    CamState *s = state_of(cam);
    s->shown = 0;
    /* the eye in the world: the view's translation is minus the eye along
     * its axes (the rotation's columns), as pc_freecam.cpp reads it */
    f32 world[3] = {0.0f, 0.0f, 0.0f};
    for (int k = 0; k < 3; k++) {
        const f32 ax = m[k], ay = m[4 + k], az = m[8 + k];
        const f32 len2 = ax * ax + ay * ay + az * az;
        if (!(len2 > 0.0f))
            return;
        const f32 along = -m[12 + k] / len2;
        world[0] += ax * along;
        world[1] += ay * along;
        world[2] += az * along;
    }
    for (int k = 0; k < 3; k++)
        s->eye[k] = world[0] * frame[k] + world[1] * frame[4 + k] + world[2] * frame[8 + k] + frame[12 + k];
    s->shown = 1;
}

int pc_customcam_eye(const void *cam, f32 eye[3])
{
    CamState *s = state_of(cam);
    for (int k = 0; k < 3; k++)
        eye[k] = s->eye[k];
    return s->shown;
}

void pc_customcam_hide(const void *cam, const RaceEntryCar *car, int on)
{
    CamState *s = state_of(cam);
    pc_headcam_hide_at(car, s->eye, on && s->shown);
}

int pc_customcam_editing(void)
{
    return cockpit_osd::active() && cockpit_osd::mode() == cockpit_osd::kModeCustom ? 1 : 0;
}

int pc_customcam_edit(const void *cam, const RaceEntryCar *car, int port, int *index, const PcCustomSource *src,
                      f32 stickX, f32 stickY)
{
    const int who = port < 0 ? 0 : port;
    int flags = 0;
    /* the editor stopped from the menu (or the cockpit camera's calibration
     * took the panel): what was being edited goes back, by the camera that
     * edited it (its type with it) */
    if (!pc_customcam_editing() && s_edit.on && s_edit.cam == cam) {
        const CustomCamera *was = find(car, *index, NULL, NULL);
        const int type = was != NULL ? was->type : -1;
        edit_cancel(index);
        const CustomCamera *now = find(car, *index, NULL, NULL);
        if (now != NULL && now->type != type)
            flags |= PC_CUSTOM_TYPE;
    }
    /* the camera's view is no longer the camera edited (another race: the
     * view starts over): the edit is given up, the mode with it */
    if (s_edit.on && s_edit.cam == cam && edited_index(car) != *index) {
        edit_cancel(NULL);
        if (pc_customcam_editing())
            cockpit_osd::stop();
    }
    if (pc_customcam_editing())
        cockpit_osd::player_seen(who);
    if (who != cockpit_osd::player())
        return flags;

    /* the F2 menu's requests */
    const int asked = cockpit_osd::take_request();
    if (asked == cockpit_osd::kRequestDelete) {
        do_delete(car, index);
    } else if (asked != cockpit_osd::kRequestNone && pc_customcam_editing()) {
        /* (an edit left on by a mode stopped and started again between two
         * ticks: given up first) */
        if (s_edit.on)
            edit_cancel(s_edit.cam == cam ? index : NULL);
        if (asked == cockpit_osd::kRequestNewGlobal)
            begin_new(cam, car, kGlobal, src, index);
        else if (asked == cockpit_osd::kRequestNewCar)
            begin_new(cam, car, model_name(car), src, index);
        /* (kRequestEdit: the camera in view, below) */
    }
    if (!pc_customcam_editing())
        return flags;

    /* the other player's pad took the editor over (cockpit_osd::pad_state):
     * the first player's edit is given up, this player's camera in view is
     * the one edited */
    if (s_edit.on && s_edit.cam != cam)
        edit_cancel(NULL);
    if (!s_edit.on) {
        std::string scope;
        int at = 0;
        const CustomCamera *c = find(car, *index, &scope, &at);
        if (c == NULL) {
            /* not a custom camera's view: the panel says so; save / cancel
             * end the mode */
            cockpit_osd::take_steps();      /* (steps asked for out of the view go nowhere) */
            const int action = cockpit_osd::race_frame(kTitle, "SWITCH TO A CUSTOM CAMERA", NULL, 0);
            if (action == cockpit_osd::kCancel || action == cockpit_osd::kSave)
                cockpit_osd::stop();
            return flags;
        }
        edit_begin(cam, scope, at, 0, *c);
    }

    List &l = list_of(s_edit.scope.c_str());
    if (s_edit.at >= l.count) {             /* (cannot be: the list only changes through the editor) */
        s_edit.on = 0;
        cockpit_osd::stop();
        return flags;
    }
    flags |= PC_CUSTOM_STICK | PC_CUSTOM_LOCK;
    CustomCamera &c = l.cam[s_edit.at];
    const int row = cockpit_osd::axis() < R_COUNT ? cockpit_osd::axis() : 0;
    adjust(c, car, row, cockpit_osd::take_steps(), stickX, stickY, &flags);
    switch (edit_frame(c)) {
    case cockpit_osd::kSave: {
        char text[64];
        snprintf(text, sizeof(text), "Camera saved: %s", c.name);
        edit_save();
        cockpit_osd::stop();
        note(text);
        break;
    }
    case cockpit_osd::kCancel: {
        const int type = c.type;
        const int wasNew = s_edit.isNew;
        edit_cancel(index);
        cockpit_osd::stop();
        if (!wasNew && l.cam[s_edit.at].type != type)
            flags |= PC_CUSTOM_TYPE;        /* (the saved camera's type is back) */
        break;
    }
    case cockpit_osd::kReset:
        reset_row(c, row, &flags);
        break;
    default:
        break;
    }
    return flags;
}
