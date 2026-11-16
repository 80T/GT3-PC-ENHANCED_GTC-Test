/* challenge_osd.cpp - the challenge modes' panel (challenge_osd.h).  No
 * original: new PC code. */
#include "platform/challenge_osd.h"
#include "platform/overlay.h"
#include "platform/settings.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>

namespace challenge_osd {
namespace {

const char *const kNames[settings::kChallengeCount] = {"MONEY DECAY", "CAR DECAY", "COURSE SHRINKAGE"};

std::mutex g_lock;
std::string g_title;                                            /* (under g_lock) */
std::string g_status[settings::kChallengeCount][kStatusLines];  /* (under g_lock) */
std::atomic<bool> g_shown{false};

} // namespace

void set_title(const char *title)
{
    std::lock_guard<std::mutex> hold(g_lock);
    g_title = title != nullptr ? title : "";
}

void set_status(int c, const char *const lines[kStatusLines])
{
    if (c < 0 || c >= settings::kChallengeCount)
        return;
    std::lock_guard<std::mutex> hold(g_lock);
    for (int i = 0; i < kStatusLines; i++)
        g_status[c][i] = lines != nullptr && lines[i] != nullptr ? lines[i] : "";
}

void set_shown(bool shown)
{
    g_shown = shown;
}

void present(ID3D11RenderTargetView *bb)
{
    using overlay::rgba;
    if (bb == nullptr || !g_shown.load())
        return;
    bool on[settings::kChallengeCount];
    bool any = false;
    for (int c = 0; c < settings::kChallengeCount; c++) {
        on[c] = settings::challenge((settings::Challenge)c);
        any = any || on[c];
    }
    if (!any || !overlay::begin())
        return;
    struct Line {
        std::string text;
        uint32_t colour;
        bool apart;                         /* a little apart from the line above (a challenge's name) */
    };
    const uint32_t amber = rgba(255, 200, 80, 255), title = rgba(255, 200, 80, 200), text = rgba(230, 230, 230, 255);
    Line lines[1 + settings::kChallengeCount * (1 + kStatusLines)];
    int n = 0;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        lines[n++] = {g_title.empty() ? std::string("CHALLENGE MODES") : g_title, title, false};
        for (int c = 0; c < settings::kChallengeCount; c++) {
            if (!on[c])
                continue;
            lines[n++] = {std::string(kNames[c]) + " ACTIVE", amber, true};
            for (int i = 0; i < kStatusLines; i++)
                if (!g_status[c][i].empty())
                    lines[n++] = {"  " + g_status[c][i], text, false};
        }
    }
    int bw, bh;
    overlay::size(&bw, &bh);
    /* (the panels' size: osd.cpp's unit, song_osd's small text) */
    const int unit = std::max(1, bh / 540), s = unit;
    const float pad = 6.0f * (float)unit, gap = 3.0f * (float)unit;
    float textW = 0.0f, h = 2.0f * pad;
    for (int i = 0; i < n; i++) {
        textW = std::max(textW, overlay::text_width(lines[i].text.c_str(), s));
        h += 7.0f * (float)s + (i > 0 ? gap : 0.0f) + (lines[i].apart ? gap : 0.0f);
    }
    const float w = textW + 2.0f * pad;
    const float x = 8.0f * (float)unit, y = (float)bh - h - 8.0f * (float)unit;
    overlay::rect(x, y, w, h, rgba(0, 0, 0, 160));
    overlay::rect(x, y, 2.0f * (float)unit, h, amber);
    float ty = y + pad;
    for (int i = 0; i < n; i++) {
        if (lines[i].apart)
            ty += gap;
        overlay::text(x + pad, ty, s, lines[i].text.c_str(), lines[i].colour);
        ty += 7.0f * (float)s + gap;
    }
    overlay::end(bb);
}

} // namespace challenge_osd
