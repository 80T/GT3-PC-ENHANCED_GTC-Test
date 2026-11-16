/* song_osd.cpp - the race's song changer's panel (song_osd.h). */
#include "platform/song_osd.h"
#include "platform/hotkey_hint.h"               /* PC (2026-11-08): control_text */
#include "platform/input_map.h"
#include "platform/overlay.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <stdio.h>
#include <string.h>
#include <string>

namespace song_osd {
namespace {

using Clock = std::chrono::steady_clock;

/* A live race's frame this recent: the race runs (a paused one, or none,
 * leaves it older). */
constexpr auto kLive = std::chrono::milliseconds(250);
/* Steps asked for and not taken yet, at most. */
constexpr int kMaxSteps = 4;

std::atomic<bool> g_open{false};
std::atomic<long long> g_lastFrame{0};      /* Clock's ticks; 0: none */
std::atomic<int> g_steps{0};

std::mutex g_lock;                          /* the song's text */
std::string g_title, g_artist;              /* folded (fold); title "": none */
int g_number = 0, g_count = 0;

bool live()
{
    const long long last = g_lastFrame.load();
    return last != 0 && Clock::now().time_since_epoch().count() - last <
                            std::chrono::duration_cast<Clock::duration>(kLive).count();
}

/* The game's text (the ANSI code page) in the panel font's characters: the
 * accents left off (Unicode's decomposition: e from e-acute), a few letters
 * and signs spelled out, anything else '?'. */
std::string fold(const char *text)
{
    std::string out;
    if (text == nullptr)
        return out;
    const int n = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (n <= 1)
        return out;
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_ACP, 0, text, -1, &w[0], n);
    const int m = FoldStringW(MAP_COMPOSITE | MAP_FOLDCZONE, w.c_str(), -1, nullptr, 0);
    if (m > 1) {
        std::wstring d((size_t)m, L'\0');
        FoldStringW(MAP_COMPOSITE | MAP_FOLDCZONE, w.c_str(), -1, &d[0], m);
        w.swap(d);
    }
    for (wchar_t c : w) {
        if (c == 0)
            break;
        if (c >= 0x300 && c <= 0x36F)
            continue;                       /* (a combining accent) */
        if (c >= 0x20 && c < 0x7F) {
            out += (char)c;
            continue;
        }
        switch (c) {
        case 0xC6: out += "AE"; break;
        case 0xE6: out += "ae"; break;
        case 0x152: out += "OE"; break;
        case 0x153: out += "oe"; break;
        case 0xDF: out += "ss"; break;
        case 0xD8: case 0xF8: out += 'O'; break;
        case 0x110: case 0x111: case 0xD0: case 0xF0: out += 'D'; break;
        case 0x141: case 0x142: out += 'L'; break;
        case 0xDE: case 0xFE: out += "TH"; break;
        case 0x2018: case 0x2019: case 0xB4: out += '\''; break;
        case 0x201C: case 0x201D: out += '"'; break;
        case 0x2013: case 0x2014: out += '-'; break;
        case 0x2026: out += "..."; break;
        case 0xD7: out += 'X'; break;
        case 0xA0: case 0x3000: out += ' '; break;
        default: out += '?'; break;
        }
    }
    return out;
}

/* "UP (,) PREVIOUS": hotkey h's device and key, then what it does (PC
 * 2026-11-08: platform/hotkey_hint.h, shared with the cockpit camera's
 * calibration panel). */
std::string control_text(int h, const char *what)
{
    return hotkey_hint::control_text(h, what);
}

/* text cut to `width' pixels at scale s ("..." at its end) */
std::string fit(const std::string &text, float width, int s)
{
    if (overlay::text_width(text.c_str(), s) <= width)
        return text;
    const int chars = (int)(width / (6.0f * (float)s)) - 3;
    return chars > 0 ? text.substr(0, (size_t)chars) + "..." : std::string("...");
}

} // namespace

void toggle()
{
    if (!live())
        return;
    g_open = !g_open.load();
    g_steps = 0;
}

void step(int dir)
{
    if (!g_open.load() || !live())
        return;
    const int s = g_steps.load();
    g_steps = std::clamp(s + (dir > 0 ? 1 : -1), -kMaxSteps, kMaxSteps);
}

int race_frame()
{
    g_lastFrame = Clock::now().time_since_epoch().count();
    if (!g_open.load()) {
        g_steps = 0;
        return 0;
    }
    int s = g_steps.load();
    while (s != 0 && !g_steps.compare_exchange_weak(s, s > 0 ? s - 1 : s + 1)) {
    }
    return s > 0 ? 1 : s < 0 ? -1 : 0;
}

void race_end()
{
    g_open = false;
    g_steps = 0;
    g_lastFrame = 0;
}

void set_song(const char *title, const char *artist, int number, int count)
{
    std::string t = fold(title), a = fold(artist);
    std::lock_guard<std::mutex> hold(g_lock);
    g_title = title != nullptr ? (t.empty() ? std::string("?") : t) : std::string();
    g_artist = a;
    g_number = number;
    g_count = count;
}

void present(ID3D11RenderTargetView *bb)
{
    using overlay::rgba;
    if (bb == nullptr || !g_open.load() || !live() || !overlay::begin())
        return;
    std::string title, artist;
    int number, count;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        title = g_title;
        artist = g_artist;
        number = g_number;
        count = g_count;
    }
    /* (the hotkeys are read once, at the game's start) */
    static const std::string hint = [] {
        std::string h;
        const std::string parts[] = {control_text(input_map::kHotPreviousSong, "PREVIOUS"),
                                     control_text(input_map::kHotNextSong, "NEXT"),
                                     control_text(input_map::kHotSongMenu, "CLOSE")};
        for (const std::string &p : parts)
            if (!p.empty())
                h += (h.empty() ? "" : "   ") + p;
        return h;
    }();
    int bw, bh;
    overlay::size(&bw, &bh);
    /* the title half again as large as the panels' text (osd.cpp's unit) */
    const int unit = std::max(1, bh / 540), big = std::max(1, (bh * 3 + 540) / 1080), small = unit;
    const float pad = 6.0f * (float)unit, gap = 4.0f * (float)unit;
    const float maxText = (float)bw * 0.45f - 2.0f * pad;
    char head[48];
    if (count > 0)
        snprintf(head, sizeof(head), "RACE SONGS  %d / %d", number, count);
    else
        snprintf(head, sizeof(head), "RACE SONGS");
    const std::string lines[4] = {fit(head, maxText, small),
                                  fit(title.empty() ? std::string("NO SONG PLAYING") : title, maxText, big),
                                  fit(artist, maxText, small), fit(hint, maxText, small)};
    const int scale[4] = {small, big, small, small};
    float textW = 0.0f, h = 2.0f * pad;
    for (int i = 0; i < 4; i++) {
        textW = std::max(textW, overlay::text_width(lines[i].c_str(), scale[i]));
        h += 7.0f * (float)scale[i] + (i < 3 ? gap : 0.0f);
    }
    h += gap;                               /* (the controls a little apart) */
    const float w = textW + 2.0f * pad;
    const float x = 8.0f * (float)unit, y = (float)bh - h - 8.0f * (float)unit;
    const uint32_t amber = rgba(255, 200, 80, 255);
    overlay::rect(x, y, w, h, rgba(0, 0, 0, 190));
    overlay::rect(x, y, 2.0f * (float)unit, h, amber);
    const uint32_t colour[4] = {rgba(255, 200, 80, 200), title.empty() ? rgba(150, 150, 150, 255) : amber,
                                rgba(230, 230, 230, 255), rgba(150, 150, 150, 255)};
    float ty = y + pad;
    for (int i = 0; i < 4; i++) {
        if (i == 3)
            ty += gap;
        overlay::text(x + pad, ty, scale[i], lines[i].c_str(), colour[i]);
        ty += 7.0f * (float)scale[i] + gap;
    }
    overlay::end(bb);
}

} // namespace song_osd
