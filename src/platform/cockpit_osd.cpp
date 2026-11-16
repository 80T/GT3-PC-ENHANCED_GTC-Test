/* cockpit_osd.cpp - the cockpit camera's calibration panel (cockpit_osd.h). */
#include "platform/cockpit_osd.h"
#include "platform/gamepad.h"                   /* button_names */
#include "platform/gpu.h"                       /* the second monitor's screen (two players) */
#include "platform/hotkey_hint.h"
#include "platform/input_map.h"
#include "platform/overlay.h"
#include "platform/settings.h"                  /* split_screen: the panel over the player's half */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

namespace cockpit_osd {
namespace {

using Clock = std::chrono::steady_clock;

/* A camera frame this recent: the race runs with the cockpit camera's view
 * (a paused one, a menu, or a replay's other cameras leave it older). */
constexpr auto kLive = std::chrono::milliseconds(250);
/* Key steps asked for and not taken yet, at most. */
constexpr int kMaxSteps = 8;

std::atomic<bool> g_active{false};
std::atomic<int> g_mode{kModeCockpit};      /* PC (2026-11-14): whose the mode is (Mode) */
std::atomic<int> g_request{kRequestNone};   /* the custom camera items' request, until the race takes it */
std::atomic<int> g_rowCount{kAxisCount};    /* the rows the race's side gave last (the axes) */
std::atomic<int> g_axis{kAxisSide};
std::atomic<int> g_action{kNone};           /* the last hotkey's, until the race takes it */
std::atomic<int> g_steps{0};                /* the keys' steps, until the race takes them */
std::atomic<int> g_player{0};               /* the port calibrating (player 1 at the start; pad_state) */
std::atomic<long long> g_lastFrame{0};      /* Clock's ticks; 0: none */
std::atomic<long long> g_seen[2] = {{0}, {0}};  /* each port's camera's last tick (player_seen) */

/* The player's own controls as the pad backends report them (pad_state):
 * what was held at the last read, and the Left / Right repeat (a held
 * direction repeats after 0.4 s, every 66 ms, as the settings menu's). */
struct PadFollow {
    unsigned held = 0;
    unsigned repeating = 0;                 /* kPadLeft / kPadRight held, repeating */
    long long repeatAt = 0;                 /* Clock's ticks */
    /* PC (2026-11-09): the next read only takes what is held (the mode just
     * started).  User: "Cockpit cam calibrator sometimes doesn't bring up the
     * popup and start.  Seems to always happen on the first selection": the
     * backends report only while the mode is on, so at its first start
     * `held' was 0 - the Cross that chose START in the F2 menu, still down,
     * read as a new press: Save, and the mode ended at once.  A later start
     * kept the last session's held Cross, and worked. */
    bool sync = true;
};
constexpr unsigned kPadAll = kPadUp | kPadDown | kPadLeft | kPadRight | kPadCross | kPadCircle | kPadTriangle;
std::mutex g_padLock;
PadFollow g_pad[2][2];                      /* [the controller's / the wheel's][port] */

long long ticks_of(std::chrono::milliseconds ms)
{
    return std::chrono::duration_cast<Clock::duration>(ms).count();
}

std::mutex g_lock;                          /* what the panel shows (PC 2026-11-14: the race's side's texts) */
std::string g_title, g_head;
std::vector<Row> g_rows;                    /* none: nothing to adjust (the head line says why) */

int row_count()
{
    return std::clamp(g_rowCount.load(std::memory_order_relaxed), 1, kMaxRows);
}

bool recent(long long last)
{
    return last != 0 && Clock::now().time_since_epoch().count() - last <
                            std::chrono::duration_cast<Clock::duration>(kLive).count();
}

bool live()
{
    return recent(g_lastFrame.load());
}

/* Both players' cameras tick: a two-player race. */
bool two_players()
{
    return recent(g_seen[0].load()) && recent(g_seen[1].load());
}

void ask(int action)
{
    if (g_active.load())
        g_action = action;
}

/* text cut to `width' pixels at scale s ("..." at its end) */
std::string fit(const std::string &text, float width, int s)
{
    if (overlay::text_width(text.c_str(), s) <= width)
        return text;
    const int chars = (int)(width / (6.0f * (float)s)) - 3;
    return chars > 0 ? text.substr(0, (size_t)chars) + "..." : std::string("...");
}

/* Hotkey h's device - its own binding, else the player's pad control that
 * does the same (`pad': "UP", the face button as the controller names it;
 * nullptr none) - and its key ("CROSS (F4)"; "F4" without a device; ""
 * without either). */
std::string control_name(int h, const char *pad)
{
    std::string dev = hotkey_hint::device_text(h);
    if (dev.empty() && pad != nullptr)
        dev = pad;
    const std::string key = hotkey_hint::key_text(h);
    if (dev.empty())
        return key;
    return key.empty() ? dev : dev + " (" + key + ")";
}

/* "UP / DOWN (Up / Down) AXIS": two hotkeys' controls and what they do
 * together. */
std::string pair_text(int a, int b, const char *padA, const char *padB, const char *what)
{
    std::string devA = hotkey_hint::device_text(a), devB = hotkey_hint::device_text(b);
    if (devA.empty() && padA != nullptr)
        devA = padA;
    if (devB.empty() && padB != nullptr)
        devB = padB;
    const std::string keyA = hotkey_hint::key_text(a), keyB = hotkey_hint::key_text(b);
    std::string devs = devA.empty() ? devB : devB.empty() ? devA : devA + " / " + devB;
    std::string keys = keyA.empty() ? keyB : keyB.empty() ? keyA : keyA + " / " + keyB;
    if (devs.empty() && keys.empty())
        return "";
    if (devs.empty())
        return keys + " " + what;
    return devs + (keys.empty() ? "" : " (" + keys + ")") + " " + what;
}

/* The mode on, as `mode' (PC 2026-11-14), or off. */
bool turn(bool on, int mode)
{
    g_action = kNone;
    g_steps = 0;
    g_axis = 0;
    g_player = 0;                           /* (player 1; the other player's pad takes over) */
    if (on) {
        /* PC (2026-11-09): every pad's first read takes what is held (the
         * buttons that chose START are no presses: PadFollow::sync) */
        std::lock_guard<std::mutex> hold(g_padLock);
        for (auto &which : g_pad)
            for (PadFollow &f : which) {
                f.sync = true;
                f.repeating = 0;
            }
        /* (the rows until the race's side gives its own: the cockpit
         * camera's, the fewest - five since 2026-11-15) */
        g_rowCount = kAxisCount;
        g_mode = mode;
    }
    g_active = on;
    return on;
}

} // namespace

bool toggle()
{
    /* (the custom cameras' editor on: the calibration takes the mode over;
     * the editor's side sees the mode is not its own and puts its camera
     * back) */
    g_request = kRequestNone;
    return turn(!(g_active.load() && g_mode.load() == kModeCockpit), kModeCockpit);
}

bool toggle_custom(int request)
{
    const bool on = !(g_active.load() && g_mode.load() == kModeCustom);
    g_request = on ? request : (int)kRequestNone;
    return turn(on, kModeCustom);
}

void request(int r)
{
    if (!g_active.load())
        g_player = 0;                       /* (player 1's camera takes it) */
    g_request = r;
}

int mode()
{
    return g_mode.load(std::memory_order_relaxed);
}

int take_request()
{
    if (g_request.load(std::memory_order_relaxed) == kRequestNone)
        return kRequestNone;
    return g_request.exchange(kRequestNone);
}

void stop()
{
    g_active = false;
    g_action = kNone;
    g_steps = 0;
}

bool active()
{
    return g_active.load(std::memory_order_relaxed);
}

int axis()
{
    return std::clamp(g_axis.load(std::memory_order_relaxed), 0, row_count() - 1);
}

void axis_key(int dir)
{
    if (!g_active.load())
        return;
    const int rows = row_count();
    g_axis = (axis() + (dir > 0 ? 1 : rows - 1)) % rows;
    g_steps = 0;                            /* (steps asked of the axis before stay with it) */
}

void adjust_key(int dir)
{
    if (!g_active.load())
        return;
    const int s = g_steps.load();
    g_steps = std::clamp(s + (dir > 0 ? 1 : -1), -kMaxSteps, kMaxSteps);
}

void save_key() { ask(kSave); }
void cancel_key() { ask(kCancel); }
void reset_key() { ask(kReset); }

bool pad_state(int which, int port, unsigned held)
{
    if (which < 0 || which > 1 || port < 0 || port > 1)
        return false;
    std::lock_guard<std::mutex> hold(g_padLock);
    PadFollow &f = g_pad[which][port];
    const unsigned pressed = f.sync ? 0u : held & ~f.held;     /* PC (2026-11-09): sync - no presses */
    f.held = held;
    f.sync = false;
    if (!g_active.load()) {
        f.repeating = 0;
        return false;
    }
    if (port != g_player.load()) {
        /* the other player's pad: a press takes the calibration over (that
         * press does nothing else); until then its controls are the game's */
        if ((pressed & kPadAll) == 0)
            return false;
        g_player = port;
        g_steps = 0;
        f.repeating = 0;
        return true;
    }
    if (pressed & kPadUp)
        axis_key(-1);
    if (pressed & kPadDown)
        axis_key(1);
    if (pressed & kPadLeft)
        adjust_key(-1);
    if (pressed & kPadRight)
        adjust_key(1);
    if (pressed & kPadCross)
        save_key();
    if (pressed & kPadCircle)
        cancel_key();
    if (pressed & kPadTriangle)
        reset_key();
    const long long now = Clock::now().time_since_epoch().count();
    if (pressed & (kPadLeft | kPadRight)) {
        f.repeating = pressed & (kPadLeft | kPadRight);
        f.repeatAt = now + ticks_of(std::chrono::milliseconds(400));
    } else if (f.repeating != 0 && (held & f.repeating) == f.repeating) {
        if (now >= f.repeatAt) {
            adjust_key((f.repeating & kPadRight) ? 1 : -1);
            f.repeatAt = now + ticks_of(std::chrono::milliseconds(66));
        }
    } else {
        f.repeating = 0;
    }
    return true;
}

int player()
{
    return std::clamp(g_player.load(std::memory_order_relaxed), 0, 1);
}

void player_seen(int port)
{
    if (port >= 0 && port <= 1)
        g_seen[port] = Clock::now().time_since_epoch().count();
}

int take_steps()
{
    if (!g_active.load())
        return 0;
    return g_steps.exchange(0);
}

int race_frame(const char *title, const char *head, const Row *rows, int count)
{
    g_lastFrame = Clock::now().time_since_epoch().count();
    count = rows != nullptr ? std::clamp(count, 0, kMaxRows) : 0;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        g_title = title != nullptr ? title : "";
        g_head = head != nullptr ? head : "";
        g_rows.assign(rows, rows + count);
    }
    if (count > 0)
        g_rowCount = count;
    if (!g_active.load())
        return kNone;
    return g_action.exchange(kNone);
}

void present(ID3D11RenderTargetView *bb)
{
    using overlay::rgba;
    if (bb == nullptr || !g_active.load() || !live())
        return;
    /* where: the game window's back buffer; in a two-player race on two
     * monitors (the split screen's second monitor open: screen 1, player 2's)
     * player 2's own screen */
    const bool two = two_players();
    const int who = player();
    const settings::SplitScreen split = settings::split_screen();
    const bool ownScreens = two && split == settings::SplitScreen::monitors && gpu::screen_count() > 1 &&
                            !gpu::span_active();
    ID3D11RenderTargetView *target = bb;
    bool began;
    if (ownScreens && who == 1) {
        target = gpu::screen_backbuffer(1);
        int sw = 0, sh = 0;
        gpu::screen_size(1, &sw, &sh);
        began = target != nullptr && overlay::begin_sized(sw, sh);
    } else {
        began = overlay::begin();
    }
    if (!began)
        return;
    /* PC (2026-11-14): the title, the head line and the rows are the race's
     * side's (the cockpit camera's calibration, the custom cameras' editor) */
    std::string title, headText;
    std::vector<Row> rows;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        title = g_title;
        headText = g_head;
        rows = g_rows;
    }
    const int rowCount = (int)rows.size();
    const int cur = rowCount > 0 ? std::min(axis(), rowCount - 1) : -1;
    /* (the hotkeys are read once, at the game's start; the pad's names as
     * the controller connected then names them) */
    static const gamepad::ButtonNames names = gamepad::button_names();
    static const std::string controls1 = [] {
        std::string h;
        const std::string parts[] = {
            pair_text(input_map::kHotCalibrationUp, input_map::kHotCalibrationDown, "UP", "DOWN", "AXIS"),
            pair_text(input_map::kHotCalibrationLeft, input_map::kHotCalibrationRight, "LEFT", "RIGHT", "ADJUST"),
            "CAMERA CONTROLS TOO"};
        for (const std::string &p : parts)
            if (!p.empty())
                h += (h.empty() ? "" : "   ") + p;
        return h;
    }();
    const std::string controls = two ? controls1 + "   THE OTHER PAD: TAKE OVER" : controls1;
    static const std::string saveKey = control_name(input_map::kHotCalibrationSave, names.south);
    static const std::string cancelKey = control_name(input_map::kHotCalibrationCancel, names.east);
    static const std::string resetKey = control_name(input_map::kHotCalibrationReset, names.north);
    std::string keys = saveKey + " SAVE";
    if (!cancelKey.empty())
        keys += "   " + cancelKey + " CANCEL";
    if (!resetKey.empty() && cur >= 0)
        keys += "   " + resetKey + " RESET " + rows[cur].name;

    int bw, bh;
    overlay::size(&bw, &bh);
    const int unit = std::max(1, bh / 540), big = std::max(1, (bh * 3 + 540) / 1080), fine = unit;
    const float pad = 6.0f * (float)unit, gap = 4.0f * (float)unit;
    const float maxText = (float)bw * 0.6f - 2.0f * pad;
    /* (the car's model name as the file's, "r_gt_00", in the font's upper
     * case) */
    const std::string head = std::string(two ? (who == 1 ? "PLAYER 2   " : "PLAYER 1   ") : "") + headText;
    /* the lines: the title, the head, the rows (a caption above the ones
     * that have one), the controls, the keys */
    struct Line {
        std::string text;
        int scale;
        uint32_t colour;
        int row;                            /* the row it is (-1: none) */
        bool apart;                         /* a little apart from the line above */
    };
    std::vector<Line> lines;
    const uint32_t amber = rgba(255, 200, 80, 255), grey = rgba(150, 150, 150, 255),
                   white = rgba(255, 255, 255, 255), dim = rgba(200, 200, 200, 255);
    if (!title.empty())
        lines.push_back({title, fine, rgba(255, 200, 80, 200), -1, false});
    lines.push_back({fit(head, maxText, big), big, rowCount > 0 ? amber : grey, -1, false});
    size_t nameW = 0;
    for (const Row &r : rows)
        nameW = std::max(nameW, strlen(r.name));
    for (int a = 0; a < rowCount; a++) {
        if (rows[a].caption[0] != '\0')
            lines.push_back({rows[a].caption, fine, grey, -1, true});
        char row[80];
        snprintf(row, sizeof(row), "%-*s  %s", (int)nameW, rows[a].name, rows[a].value);
        lines.push_back({row, fine, a == cur ? white : dim, a, false});
    }
    lines.push_back({fit(controls, maxText, fine), fine, grey, -1, true});
    lines.push_back({fit(keys, maxText, fine), fine, grey, -1, false});

    float textW = 0.0f, h = 2.0f * pad;
    int shown = 0;
    for (const Line &l : lines) {
        if (l.text.empty())
            continue;
        textW = std::max(textW, overlay::text_width(l.text.c_str(), l.scale));
        h += 7.0f * (float)l.scale + (shown > 0 ? gap : 0.0f) + (l.apart ? gap : 0.0f);
        shown++;
    }
    const float w = textW + 2.0f * pad;
    /* the top right corner - of the calibrating player's half of a split
     * screen in a two-player race (horizontal: player 1 above; vertical, or
     * two monitors before the second one opens: player 1 left; two monitors:
     * the player's own screen, above) */
    float right = (float)bw, top = 0.0f;
    if (two && !ownScreens) {
        if (split == settings::SplitScreen::horizontal && who == 1)
            top = (float)bh * 0.5f;
        else if (split != settings::SplitScreen::horizontal && who == 0)
            right = (float)bw * 0.5f;
    }
    const float x = right - w - 8.0f * (float)unit, y = top + 8.0f * (float)unit;
    overlay::rect(x, y, w, h, rgba(0, 0, 0, 190));
    overlay::rect(x + w - 2.0f * (float)unit, y, 2.0f * (float)unit, h, amber);
    float ty = y + pad;
    for (const Line &l : lines) {
        if (l.text.empty())
            continue;
        if (l.apart)
            ty += gap;
        if (l.row >= 0 && l.row == cur) {
            /* the chosen axis: a band behind its row and a mark before it
             * (the settings menu's highlight) */
            const float s = (float)l.scale;
            overlay::rect(x + pad - 3.0f * s, ty - 2.0f * s, textW + 6.0f * s, 7.0f * s + 4.0f * s,
                          rgba(255, 255, 255, 30));
            overlay::rect(x + pad - 5.0f * s, ty, 2.0f * s, 7.0f * s, amber);
        }
        overlay::text(x + pad, ty, l.scale, l.text.c_str(), l.colour);
        ty += 7.0f * (float)l.scale + gap;
    }
    overlay::end(target);
}

} // namespace cockpit_osd
