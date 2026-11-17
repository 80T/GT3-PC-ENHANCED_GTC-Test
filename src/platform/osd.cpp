/* osd.cpp - a short message over the picture (osd.h). */
#include "platform/osd.h"
#include "platform/overlay.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <stdio.h>

namespace osd {
namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kShownFor = std::chrono::seconds(2);

std::mutex g_lock;
char g_text[256];                       /* (under g_lock; PC 2026-11-16: 64 before - UTF-8 translations) */
/* It shows until then (Clock's ticks). */
std::atomic<long long> g_shownUntil{0};

} // namespace

void show(const char *text)
{
    {
        std::lock_guard<std::mutex> hold(g_lock);
        snprintf(g_text, sizeof(g_text), "%s", text);
    }
    g_shownUntil = (Clock::now() + kShownFor).time_since_epoch().count();
}

void present(ID3D11RenderTargetView *bb)
{
    using overlay::rgba;
    if (bb == nullptr || Clock::now().time_since_epoch().count() >= g_shownUntil.load() || !overlay::begin())
        return;
    char text[sizeof(g_text)];
    {
        std::lock_guard<std::mutex> hold(g_lock);
        snprintf(text, sizeof(text), "%s", g_text);
    }
    int bw, bh;
    overlay::size(&bw, &bh);
    /* (twice the panels' size: read at a glance, not studied) */
    const int unit = std::max(1, bh / 540), s = unit * 2;
    const float pad = 6.0f * unit;
    const float w = overlay::text_width(text, s) + 2.0f * pad, h = 7.0f * s + 2.0f * pad;
    const float x = ((float)bw - w) * 0.5f, y = 8.0f * unit;
    overlay::rect(x, y, w, h, rgba(0, 0, 0, 190));
    overlay::rect(x, y, 2.0f * unit, h, rgba(255, 200, 80, 255));
    overlay::text(x + pad, y + pad, s, text, rgba(255, 200, 80, 255));
    overlay::end(bb);
}

} // namespace osd
