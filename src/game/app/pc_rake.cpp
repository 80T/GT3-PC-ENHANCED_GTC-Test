/* pc_rake.cpp - PC: the body's rest pitch kept out of the direction of the
 * car's forces (pc_rake.h).  No original: new PC code. */
#include "game/app/pc_rake.h"
#include "game/car/physics/T302_car_state.h"            /* RaceCarState, DynCarState, CarDynamics */
#include "game/common/T319_angle_math.h"                /* eulerToAxes */
#include "game/enhanced/physics.h"                      /* ENH_PHYSICS_REAR_RIDE_HEIGHT */
#include "engine/media/G002_mpeg_frameq.h"              /* atan2f (the game's: DynCar_place's) */

#include <stdlib.h>                                     /* getenv (GT3PC_RAKE_FIX) */

#include <atomic>

#if ENH_PHYSICS_REAR_RIDE_HEIGHT
/* GT3PC_RAKE_FIX=0: off (read once; the workers may race to the same answer). */
static int rake_fix_on()
{
    static std::atomic<int> on{-1};
    int v = on.load(std::memory_order_relaxed);
    if (v < 0) {
        const char *e = getenv("GT3PC_RAKE_FIX");
        v = e != NULL && e[0] == '0' ? 0 : 1;
        on.store(v, std::memory_order_relaxed);
    }
    return v;
}
#endif

f32 pc_rake_angle(RaceCarState *car)
{
#if ENH_PHYSICS_REAR_RIDE_HEIGHT
    CarDynamics *p = &car->params;
    if (p->axleHeight[0] == p->axleHeight[1])
        return 0.0f;
    /* (DynCar_place's own expression, T330) */
    return atan2f(p->axleHeight[0] - p->axleHeight[1], p->wheelbase);
#else
    (void)car;
    return 0.0f;
#endif
}

int pc_rake_axes(RaceCarState *car, f32 *forward, f32 *lateral, f32 *up)
{
#if ENH_PHYSICS_REAR_RIDE_HEIGHT
    CarDynamics *p = &car->params;
    DynCarState *st = &car->st;
    if (p->axleHeight[0] == p->axleHeight[1] || !rake_fix_on())
        return 0;
    eulerToAxes(forward, lateral, up, st->pitch - pc_rake_angle(car), st->roll, st->yaw);
    return 1;
#else
    (void)car;
    (void)forward;
    (void)lateral;
    (void)up;
    return 0;
#endif
}
