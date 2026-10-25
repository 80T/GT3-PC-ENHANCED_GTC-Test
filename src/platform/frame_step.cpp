/* frame_step.cpp - the tick step of GT3 PC (frame_step.h). */
#include "platform/frame_step.h"
#include "platform/input_map.h"
#include "platform/log.h"
#include "platform/overlay.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <stdio.h>

namespace frame_step {
namespace {

using Clock = std::chrono::steady_clock;

std::atomic<bool> g_paused{false};
std::atomic<int> g_pending{0};          /* steps asked for, not run yet */
std::atomic<unsigned> g_steps{0};       /* ticks stepped since the pause */
constexpr int kMaxPending = 4;          /* (a held key does not queue a burst) */
/* The indicator shows until then (Clock's ticks): kShownFor after each F5 */
std::atomic<long long> g_shownUntil{0};
constexpr auto kShownFor = std::chrono::seconds(3);

void show_indicator()
{
    g_shownUntil = (Clock::now() + kShownFor).time_since_epoch().count();
}

} // namespace

void key_step()
{
    show_indicator();
    if (!g_paused.load()) {
        g_pending = 0;
        g_steps = 0;
        g_paused = true;
        char step[32], run[32];
        input_map::hotkey_text(input_map::kHotPause, step, sizeof(step));
        input_map::hotkey_text(input_map::kHotResume, run, sizeof(run));
        pclog::write("frame step: paused (%s one tick, %s run)", step, run[0] != '\0' ? run : "no key to");
        return;
    }
    if (g_pending.load() < kMaxPending)
        g_pending++;
}

void key_resume()
{
    if (!g_paused.load())
        return;
    g_paused = false;
    g_pending = 0;
    pclog::write("frame step: running again after %u stepped tick(s)", g_steps.load());
}

bool paused()
{
    return g_paused.load(std::memory_order_relaxed);
}

bool take_step()
{
    int n = g_pending.load();
    while (n > 0) {
        if (g_pending.compare_exchange_weak(n, n - 1)) {
            g_steps++;
            return true;
        }
    }
    return false;
}

void present(ID3D11RenderTargetView *bb)
{
    using overlay::rgba;
    if (!paused() || bb == nullptr || Clock::now().time_since_epoch().count() >= g_shownUntil.load() ||
        !overlay::begin())
        return;
    int bw, bh;
    overlay::size(&bw, &bh);
    char l1[48], step[32], run[32], l2[96];
    snprintf(l1, sizeof(l1), "PAUSED  TICK +%u", g_steps.load());
    input_map::hotkey_text(input_map::kHotPause, step, sizeof(step));
    input_map::hotkey_text(input_map::kHotResume, run, sizeof(run));
    if (run[0] != '\0')
        snprintf(l2, sizeof(l2), "%s NEXT TICK   %s RESUME", step, run);
    else
        snprintf(l2, sizeof(l2), "%s NEXT TICK", step);
    const int s = std::max(1, bh / 540);
    const float pad = 6.0f * s, lineH = 12.0f * s;
    const float w = std::max(overlay::text_width(l1, s), overlay::text_width(l2, s)) + 2.0f * pad;
    const float h = 2.0f * lineH + 2.0f * pad - 3.0f * s;
    const float x = (float)bw - w - 8.0f * s, y = 8.0f * s;
    overlay::rect(x, y, w, h, rgba(0, 0, 0, 190));
    overlay::rect(x, y, 2.0f * s, h, rgba(255, 200, 80, 255));
    overlay::text(x + pad, y + pad, s, l1, rgba(255, 200, 80, 255));
    overlay::text(x + pad, y + pad + lineH, s, l2, rgba(200, 200, 200, 255));
    overlay::end(bb);
}

} // namespace frame_step
