/* pc_clutch.cpp - the realistic gear shift's clutch mode (pc_clutch.h).  No
 * original: new PC code. */
#include "game/app/pc_clutch.h"
#include "game/app/pc_shift.h"                          /* the race's flags */
#include "game/car/physics/T302_car_state.h"            /* RaceCarState, DynCarState, CarEngine, GearCalc */
#include "game/car/physics/T296_car_drivetrain.h"       /* car_engine_speed_from_wheels */
#include "game/car/physics/T342_car_input.h"            /* CarInput, CarCtrlOut */
#include "game/car/physics/T343_gear_select.h"          /* car_select_gear, car_set_gear */
#include "platform/clutch_input.h"

#include <math.h>
#include <stdio.h>

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace {

constexpr f32 kRpmToRad = 0.10471976f;

/* A car's clutch: this step's frame (a clutch held: have; its travel; the
 * H-shifter's gate, -1 none), neutral, a mis-shift's grind left (s), the
 * engine's peak torque (N m; < 0 not found yet).  The lock: the map's (the
 * cars' steps run on the physics workers: a car's entry is its own job's,
 * found or made under it, as pc_rev_cut's / pc_shift's). */
struct Clutch {
    bool have = false;
    f32 pedal = 0.0f;
    int gate = -1;
    bool neutral = false;
    f32 grind = 0.0f;
    f32 peak = -1.0f;
    bool holds = false;                     /* the step's drive within what the clutch holds: it locks */
    bool logged = false;
};
std::mutex g_lock;
std::unordered_map<const RaceCarState *, Clutch> g_cars;
std::atomic<int> g_logLeft{200};            /* (the mis-shifts' lines) */

Clutch *entry(const RaceCarState *car)
{
    std::lock_guard<std::mutex> hold(g_lock);
    return &g_cars[car];
}

Clutch *find(const RaceCarState *car)
{
    std::lock_guard<std::mutex> hold(g_lock);
    auto it = g_cars.find(car);
    return it != g_cars.end() ? &it->second : NULL;
}

/* car is one the clutch mode works, when its frame holds a clutch
 * (pc_clutch.h "Whose"); held: its gears also while it is held at the start
 * (DynCarState.stopped: the countdown) */
bool whose(const RaceCarState *car, bool held = false)
{
    const DynCarState *st = &car->st;
    if (st->driver != DRIVER_PAD || st->ctrlMode != CTRL_DRIVER)
        return false;
    /* (PC 2026-11-09: GT3's MT is transmission mode 1, TRANS_SEMI_AUTO - the
     * entry's transmission = gearAuto ^ 1; the manual modes 4 / 5 no menu
     * sets, kept) */
    if (st->transMode != TRANS_SEMI_AUTO && st->transMode != TRANS_MANUAL && st->transMode != TRANS_SEQUENTIAL)
        return false;
    if ((st->stopped != 0 && !held) || st->pitTimer != 0)
        return false;
    if (car->params.gear.gearboxType != GEARBOX_STOCK || car->hybrid)
        return false;
    return (pc_shift_race_flags(car->org) & PC_SHIFT_RACE_CLUTCH) != 0;
}

/* car's clutch is its player's this step (held: see whose) */
Clutch *active(RaceCarState *car, bool held = false)
{
    if (!whose(car, held))
        return NULL;
    Clutch *k = find(car);
    return k != NULL && k->have ? k : NULL;
}

/* how far the clutch grips: 0 out (from PC_CLUTCH_DISENGAGED on) .. 1 in */
f32 engagement(const Clutch &k)
{
    const f32 e = (PC_CLUTCH_DISENGAGED - k.pedal) / PC_CLUTCH_DISENGAGED;
    return e < 0.0f ? 0.0f : e > 1.0f ? 1.0f : e;
}

/* the most it passes now (N m) */
f32 capacity(const RaceCarState *car, Clutch &k)
{
    if (k.peak < 0.0f) {
        const CarEngine *e = &car->params.engine;
        f32 peak = 0.0f;
        for (int i = 0; i < e->torqueCurve.count && i < 16; i++)
            peak = e->torque[i] > peak ? e->torque[i] : peak;
        k.peak = peak;
    }
    return engagement(k) * PC_CLUTCH_CAPACITY * k.peak;
}

f32 idle_speed(const RaceCarState *car)
{
    return (f32)car->params.engine.idle * kRpmToRad;
}

const char *ordinal(int gear)
{
    static const char *const names[] = {"reverse", "1st", "2nd", "3rd", "4th", "5th", "6th", "7th", "8th", "9th"};
    return gear >= 0 && gear <= 9 ? names[gear] : "?";
}

} // namespace

void pc_clutch_live_input(int player, CarInput *in, int wheel)
{
    const int flags = pc_shift_live_flags();
    if (!(flags & PC_SHIFT_RACE_CLUTCH))
        return;
    /* the device's clutch, else the other one's (a clutch on the keyboard
     * of the controller's profile with a wheel...); -1 none */
    const f32 a = clutch_input::clutch(player, wheel != 0), b = clutch_input::clutch(player, wheel == 0);
    const f32 v = a > b ? a : b;
    if (v < 0.0f)
        return;                             /* (no clutch: the realistic mode's; the shift buttons kept) */
    in->unk_3 = (u8)(1 + lroundf((v > 1.0f ? 1.0f : v) * 254.0f));
    if (flags & PC_SHIFT_RACE_HSHIFTER) {
        const int g = clutch_input::gate(player);
        if (g >= 0)
            in->shift = (s8)(PC_CLUTCH_GATE_CODE + g);
    }
}

void pc_clutch_input(RaceCarState *car, const CarInput *in, CarCtrlOut *out)
{
    int gate = -1;
    if (in->shift >= PC_CLUTCH_GATE_CODE && in->shift <= PC_CLUTCH_GATE_CODE + PC_CLUTCH_GATE_REVERSE) {
        gate = in->shift - PC_CLUTCH_GATE_CODE;
        out->shift = 0;                     /* (a gate, never a shift) */
    }
    const int flags = pc_shift_race_flags(car->org);
    if (!(flags & PC_SHIFT_RACE_CLUTCH))
        return;
    Clutch *k = entry(car);
    k->have = in->unk_3 != 0;
    k->pedal = k->have ? (f32)(in->unk_3 - 1) / 254.0f : 0.0f;
    k->gate = (flags & PC_SHIFT_RACE_HSHIFTER) ? gate : -1;
    if (k->have && !k->logged && car->st.driver == DRIVER_PAD) {
        k->logged = true;
        printf("PC: clutch: car %p - the player's clutch%s (out from %d %% travel, %d x the engine's peak torque all"
               " the way in)\n", (void *)car, k->gate >= 0 ? " and H-shifter" : "",
               (int)(PC_CLUTCH_DISENGAGED * 100.0f + 0.5f), (int)(PC_CLUTCH_CAPACITY + 0.5f));
    }
}

int pc_clutch_trans_mode(RaceCarState *car, const CarInput *in, int transMode)
{
    /* (this frame's clutch: pc_clutch_input reads it after the throttle; the
     * countdown's too, as its gears: pc_clutch_gear_select) */
    if (transMode == TRANS_SEMI_AUTO && in->unk_3 != 0 && whose(car, true))
        return TRANS_MANUAL;
    return transMode;
}

bool pc_clutch_from_input(RaceCarState *car)
{
    Clutch *k = active(car);
    if (k == NULL)
        return false;
    car->st.clutch = engagement(*k);
    return true;
}

bool pc_clutch_state(RaceCarState *car)
{
    Clutch *k = active(car);
    if (k == NULL)
        return false;
    DynCarState *st = &car->st;
    const f32 dt = car->dt;
    if (k->grind > 0.0f) {
        if (k->pedal >= PC_CLUTCH_DISENGAGED)
            k->grind = 0.0f;                /* (the clutch pressed: the shift the player's again) */
        else if (dt > 0.0f)
            k->grind = k->grind > dt ? k->grind - dt : 0.0f;
    }
    st->engineWPrev = st->engineSpeed;
    const f32 wheel = car_engine_speed_from_wheels(car);
    st->wheelEngineW = wheel;
    if (k->neutral || k->grind > 0.0f || engagement(*k) <= 0.0f) {
        st->clutchState = CLUTCH_FREE;
    } else if (st->clutchState == CLUTCH_LOCKED) {
        /* (the last step's drive past what the clutch holds now, or the
         * wheels below the idle: slipping) */
        if (st->clutchTorqueAbs > capacity(car, *k) || wheel < idle_speed(car))
            st->clutchState = CLUTCH_SLIP;
    } else {
        st->clutchState = CLUTCH_SLIP;
    }
    return true;
}

bool pc_clutch_drive(RaceCarState *car, f32 engineTq, f32 *drive)
{
    Clutch *k = active(car);
    if (k == NULL)
        return false;
    const DynCarState *st = &car->st;
    const f32 cap = capacity(car, *k);
    const f32 inertiaInv = car->params.gear.inertiaInv, dt = car->dt;
    /* the torque that brings the engine to the wheels' speed within the
     * step, at most what the clutch passes */
    f32 need = engineTq;
    if (inertiaInv > 0.0f && dt > 0.0f)
        need += (st->engineSpeed - st->wheelEngineW) / (inertiaInv * dt);
    f32 d = need > cap ? cap : need < -cap ? -cap : need;
    /* (within what it holds the clutch sticks: it locks this step - the
     * wheels gaining on the engine as it closes the gap, the engine never
     * crossed them, so GT3's lock where they cross did not come) */
    k->holds = need <= cap && need >= -cap;
    /* at its idle the engine gives no more than its own torque (it does
     * not stall) */
    if (d > 0.0f && st->engineSpeed <= idle_speed(car) * 1.0001f && d > engineTq)
        d = engineTq > 0.0f ? engineTq : 0.0f;
    *drive = d;
    return true;
}

int pc_clutch_engine(RaceCarState *car, f32 wheelSpeed)
{
    Clutch *k = active(car);
    if (k == NULL)
        return PC_CLUTCH_GT3;
    DynCarState *st = &car->st;
    const f32 idle = idle_speed(car);
    if (st->engineSpeed < idle)
        st->engineSpeed = idle;
    /* (GT3's crossing lock kept otherwise: a gear taken against the car's
     * motion - the reverse rolling forward - can turn the wheels round within
     * a step, and the lock there flares the engine to their spin for a step;
     * without it the clutch turns them back and forth each step, harness
     * 2026-11-09) */
    if (wheelSpeed < idle)
        return PC_CLUTCH_HOLD;
    return k->holds ? PC_CLUTCH_LOCK : PC_CLUTCH_SLIP;
}

bool pc_clutch_gear_select(RaceCarState *car)
{
    /* PC (2026-11-10): also while the car is held at the start (user: "the
     * H-shifter isn't updating the gear during the pre-race countdown
     * period") - GT3's gear choice runs then, the shift buttons' did, the
     * gates went unread */
    Clutch *k = active(car, true);
    if (k == NULL)
        return false;
    DynCarState *st = &car->st;
    const int count = car->params.gear.gearCount;
    int target;
    if (k->gate >= 0) {
        /* the H-shifter: the reverse in its Reverse gate or while the
         * game's reverse control is held, else the gate (one the car has no
         * gear for: neutral) */
        if (k->gate == PC_CLUTCH_GATE_REVERSE || st->ctrl.reverse != 0) {
            target = 0;
        } else if (k->gate == 0 || k->gate > count) {
            k->neutral = true;
            return true;
        } else {
            target = k->gate;
        }
    } else {
        /* the shift buttons; GT3's MT as the manual box (the reverse a gear
         * its control shifts into, 1st the shift up from it: pc_clutch.h) */
        target = st->transMode == TRANS_SEMI_AUTO ? car_shift_manual(car, &st->ctrl) : car_select_gear(car, &st->ctrl);
        if (target < 0)
            return true;
    }
    if (target == st->gear && !k->neutral)
        return true;
    /* (held at the start the game has the clutch out - car_clutch_from_input's
     * 0 - and the grind's time would not run: a gear goes in as it is taken) */
    if (k->pedal < PC_CLUTCH_DISENGAGED && st->stopped == 0) {
        /* a mis-shift: the gear taken with the clutch in - the box grinds */
        k->grind = PC_CLUTCH_GRIND_TIME;
        if (st->driver == DRIVER_PAD && g_logLeft.load(std::memory_order_relaxed) > 0) {
            const int left = g_logLeft.fetch_sub(1, std::memory_order_relaxed) - 1;
            printf("PC: clutch: car %p - %s with the clutch at %d %% travel: the box grinds %d ms%s\n", (void *)car,
                   ordinal(target), (int)(k->pedal * 100.0f + 0.5f), (int)(PC_CLUTCH_GRIND_TIME * 1000.0f + 0.5f),
                   left == 0 ? " (no more of these lines)" : "");
        }
    }
    k->neutral = false;
    car_set_gear(st, target);
    return true;
}

void pc_clutch_reset(RaceCarState *car)
{
    std::lock_guard<std::mutex> hold(g_lock);
    auto it = g_cars.find(car);
    if (it == g_cars.end())
        return;
    const bool logged = it->second.logged;
    it->second = Clutch();
    it->second.logged = logged;             /* (the first frame's line once a car) */
}
