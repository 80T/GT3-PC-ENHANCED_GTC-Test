/* fpu.h - the FPU mode of native code called from game threads.
 *
 * Game threads run in the EE's FPU mode (scheduler.cpp set_game_fpu_mode:
 * round toward zero, denormals flushed), which keeps the game's float
 * results closest to the PS2's.  Driver code (Direct3D 11, DXGI, the shader
 * compiler) expects the default mode: round to nearest, denormals kept.
 * Every block of such calls made on a game thread is wrapped in a
 * DefaultFpuMode guard, which switches to the default mode and restores the
 * caller's mode when it goes out of scope.  The guards nest. */
#pragma once

#include <float.h>

namespace fpu {

class DefaultFpuMode {
public:
    DefaultFpuMode()
    {
        unsigned int cw = 0;
        _controlfp_s(&cw, 0, 0);
        saved_ = cw & (_MCW_RC | _MCW_DN);
        _controlfp_s(&cw, _RC_NEAR | _DN_SAVE, _MCW_RC | _MCW_DN);
    }
    ~DefaultFpuMode()
    {
        unsigned int cw = 0;
        _controlfp_s(&cw, saved_, _MCW_RC | _MCW_DN);
    }
    DefaultFpuMode(const DefaultFpuMode &) = delete;
    DefaultFpuMode &operator=(const DefaultFpuMode &) = delete;

    /* The caller's mode (rounding and denormal bits). */
    unsigned int saved() const { return saved_; }

private:
    unsigned int saved_ = 0;
};

/* Inside a DefaultFpuMode block: a call back into game code (a readback
 * callback) runs in the game's mode again, given as DefaultFpuMode::saved()
 * returned it. */
class ScopedFpuMode {
public:
    explicit ScopedFpuMode(unsigned int mode)
    {
        unsigned int cw = 0;
        _controlfp_s(&cw, 0, 0);
        saved_ = cw & (_MCW_RC | _MCW_DN);
        _controlfp_s(&cw, mode & (_MCW_RC | _MCW_DN), _MCW_RC | _MCW_DN);
    }
    ~ScopedFpuMode()
    {
        unsigned int cw = 0;
        _controlfp_s(&cw, saved_, _MCW_RC | _MCW_DN);
    }
    ScopedFpuMode(const ScopedFpuMode &) = delete;
    ScopedFpuMode &operator=(const ScopedFpuMode &) = delete;

private:
    unsigned int saved_ = 0;
};

} // namespace fpu
