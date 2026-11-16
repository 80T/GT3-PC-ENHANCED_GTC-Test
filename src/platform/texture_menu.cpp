/* texture_menu.cpp - PC (2026-11-13): the texture interface (texture_menu.h). */
#include "platform/texture_menu.h"
#include "platform/input_map.h"
#include "platform/osd.h"
#include "platform/overlay.h"
#include "platform/settings.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <stdio.h>
#include <string.h>
#include <string>

namespace texture_menu {
namespace {

using Clock = std::chrono::steady_clock;

enum Line { kReplace, kDump, kPrecache, kCompress, kRefresh, kTexture, kLines };

std::atomic<const Backend *> g_backend{nullptr};
std::atomic<bool> g_shown{false};
std::atomic<int> g_line{kReplace};
std::mutex g_lock;                          /* the note */
std::string g_note;
Clock::time_point g_noteUntil;

bool select_mode()
{
    return settings::texture_dump() == settings::TextureDump::select;
}

int lines()
{
    return select_mode() ? kLines : kTexture;
}

void note(const char *text)
{
    std::lock_guard<std::mutex> hold(g_lock);
    g_note = text;
    g_noteUntil = Clock::now() + std::chrono::seconds(3);
}

void changed()
{
    const Backend *b = g_backend.load();
    if (b != nullptr)
        b->settings_changed();
}

/* The refresh asked for (the panel's line, the hotkey): what was said. */
const char *ask_refresh()
{
    const Backend *b = g_backend.load();
    if (b == nullptr)
        return "TEXTURES: NOT READY";
    if (!b->refresh())
        return "TEXTURE REFRESH: TURN THE PRE-CACHE OFF FIRST";
    return "TEXTURES: LOOKING FOR CHANGED FILES";
}

/* The line's next (dir 1, 0) or previous (-1) value. */
void change(int line, int dir)
{
    const Backend *b = g_backend.load();
    switch (line) {
    case kReplace:
        settings::set_texture_replace(!settings::texture_replace());
        changed();
        break;
    case kDump: {
        const int n = settings::kTextureDumpCount;
        const int v = ((int)settings::texture_dump() + (dir < 0 ? n - 1 : 1)) % n;
        settings::set_texture_dump((settings::TextureDump)v);
        changed();
        if (g_line.load() >= lines())
            g_line = kDump;
        break;
    }
    case kPrecache:
        settings::set_texture_precache(!settings::texture_precache());
        changed();
        break;
    case kCompress:
        settings::set_texture_compress(!settings::texture_compress());
        changed();
        note("COMPRESSION: FOR THE FILES LOADED FROM NOW ON");
        break;
    case kRefresh:
        if (dir >= 0)
            note(ask_refresh());
        break;
    case kTexture:
        if (b == nullptr)
            break;
        if (dir == 0)
            note(b->dump_selected() ? "WRITTEN TO TEX_REPLACEMENTS/DUMPS_SELECTED"
                                    : "NO TEXTURE CHOSEN, OR NOT DRAWN YET");
        else
            b->select_step(dir);
        break;
    }
}

} // namespace

void set_backend(const Backend *backend)
{
    g_backend = backend;
}

void toggle()
{
    const bool open = !g_shown.load();
    g_shown = open;
    if (open && g_line.load() >= lines())
        g_line = kReplace;
    const Backend *b = g_backend.load();
    if (b != nullptr)
        b->menu_open(open);
}

void hide()
{
    if (g_shown.load())
        toggle();
}

void replace_key()
{
    const bool on = !settings::texture_replace();
    settings::set_texture_replace(on);
    changed();
    osd::show(on ? "REPLACEMENT TEXTURES: ON" : "REPLACEMENT TEXTURES: OFF");
}

void refresh_key()
{
    const char *said = ask_refresh();
    if (g_shown.load())
        note(said);
    else
        osd::show(said);
}

bool shown()
{
    return g_shown.load(std::memory_order_relaxed);
}

bool key(int vk, bool repeat)
{
    if (!g_shown.load())
        return false;
    const int n = lines();
    int line = g_line.load();
    switch (vk) {
    case VK_UP:
        g_line = (line + n - 1) % n;
        return true;
    case VK_DOWN:
        g_line = (line + 1) % n;
        return true;
    case VK_LEFT:
    case VK_RIGHT:
        /* (a switch does not flicker under a held key; the texture list
         * runs on) */
        if (!repeat || line == kTexture)
            change(line, vk == VK_RIGHT ? 1 : -1);
        return true;
    case VK_RETURN:
    case VK_SPACE:
        if (!repeat)
            change(line, 0);
        return true;
    case VK_PRIOR:
    case VK_NEXT:
        if (select_mode()) {
            const Backend *b = g_backend.load();
            for (int i = 0; i < 10 && b != nullptr; i++)
                b->select_step(vk == VK_NEXT ? 1 : -1);
        }
        return true;
    case VK_ESCAPE:
    case VK_BACK:
        if (!repeat)
            hide();
        return true;
    }
    return false;
}

void present(ID3D11RenderTargetView *bb)
{
    using overlay::rgba;
    if (bb == nullptr)
        return;
    /* a refresh done: what it found, over the picture (the panel shown or
     * not) */
    const Backend *b = g_backend.load();
    static uint32_t refreshSeen = 0;
    if (b != nullptr && b->refresh_serial() != refreshSeen) {
        refreshSeen = b->refresh_serial();
        Status done;
        b->status(&done);
        char said[96];
        if (done.refChanged + done.refNew + done.refGone == 0)
            snprintf(said, sizeof(said), "TEXTURES: NO FILE CHANGED");
        else
            snprintf(said, sizeof(said), "TEXTURES: %u CHANGED, %u NEW, %u GONE", done.refChanged, done.refNew,
                     done.refGone);
        osd::show(said);
    }
    if (!g_shown.load() || !overlay::begin())
        return;
    Status st;
    if (b != nullptr)
        b->status(&st);
    const int line = g_line.load();
    const bool select = select_mode();
    static const char *const kDumpNames[settings::kTextureDumpCount] = {"OFF", "SELECT", "ALL"};

    /* the lines: name, value */
    std::string names[kLines], values[kLines];
    names[kReplace] = "REPLACEMENTS";
    values[kReplace] = settings::texture_replace() ? "ON" : "OFF";
    names[kDump] = "DUMPING";
    values[kDump] = kDumpNames[(int)settings::texture_dump()];
    names[kPrecache] = "PRE-CACHE";
    values[kPrecache] = settings::texture_precache() ? "ON" : "OFF";
    names[kCompress] = "COMPRESSION";
    values[kCompress] = settings::texture_compress() ? "ON" : "OFF";
    names[kRefresh] = "REFRESH";
    values[kRefresh] = settings::texture_precache() ? "PRE-CACHE IS ON" : st.refreshing ? "LOOKING..." : "ENTER";
    names[kTexture] = "TEXTURE";
    char text[160];
    if (st.seen == 0)
        snprintf(text, sizeof(text), "NONE DRAWN");
    else if (st.selHash == 0)
        snprintf(text, sizeof(text), "- / %u", st.seen);
    else if (st.selIndex >= 0)
        snprintf(text, sizeof(text), "%d / %u", st.selIndex + 1, st.seen);
    else
        snprintf(text, sizeof(text), "(NOT DRAWN NOW) / %u", st.seen);
    values[kTexture] = text;

    /* under them: the chosen texture, what the replacements are doing, the keys */
    std::string info[7];
    int nInfo = 0;
    if (select) {
        if (st.selHash != 0) {
            snprintf(text, sizeof(text), "%08X%08X", (unsigned)(st.selHash >> 32), (unsigned)st.selHash);
            std::string s = text;
            if (st.selW != 0) {
                snprintf(text, sizeof(text), "  %ux%u", st.selW, st.selH);
                s += text;
            }
            if (st.selReplaced)
                s += "  REPLACED";
            if (st.selDumped)
                s += "  DUMPED";
            info[nInfo++] = s;
        } else {
            info[nInfo++] = "LEFT / RIGHT ON TEXTURE: CHOOSE ONE (DRAWN GREEN)";
        }
    }
    if (!st.listed)
        snprintf(text, sizeof(text), "FOLDERS: BEING LISTED");
    else
        snprintf(text, sizeof(text), "%u FOLDER%s, %u FILE%s: %u LOADED, %u WAITING, %u REFUSED", st.folders,
                 st.folders == 1 ? "" : "S", st.files, st.files == 1 ? "" : "S", st.loaded, st.waiting, st.failed);
    info[nInfo++] = text;
    if (st.dumpsWaiting != 0)
        snprintf(text, sizeof(text), "DUMPS: %u WRITTEN, %u WAITING", st.dumped, st.dumpsWaiting);
    else
        snprintf(text, sizeof(text), "DUMPS: %u WRITTEN", st.dumped);
    info[nInfo++] = text;
    if (settings::texture_precache()) {
        if (st.cacheTodo != 0)
            snprintf(text, sizeof(text), "PRE-CACHE: %u MADE, %u TO LOOK AT", st.cacheMade, st.cacheTodo);
        else
            snprintf(text, sizeof(text), "PRE-CACHE: UP TO DATE (%u MADE)", st.cacheMade);
        info[nInfo++] = text;
    }
    if (st.refreshes != 0) {
        snprintf(text, sizeof(text), "LAST REFRESH: %u CHANGED, %u NEW, %u GONE", st.refChanged, st.refNew, st.refGone);
        info[nInfo++] = text;
    }
    {
        std::lock_guard<std::mutex> hold(g_lock);
        if (!g_note.empty() && Clock::now() < g_noteUntil)
            info[nInfo++] = g_note;
    }
    char hot[32];
    input_map::hotkey_text(input_map::kHotTextureMenu, hot, sizeof(hot));
    snprintf(text, sizeof(text), "UP / DOWN  LEFT / RIGHT  %s%s%sESC CLOSE",
             select ? "ENTER DUMP  PAGE UP / DOWN 10  " : "", hot, hot[0] != '\0' ? " / " : "");
    const std::string keys = text;

    int bw, bh;
    overlay::size(&bw, &bh);
    const int s = std::max(1, bh / 540);
    const float fs = (float)s, pad = 6.0f * fs, row = 10.0f * fs, gap = 4.0f * fs;
    const int nLines = select ? kLines : kTexture;
    float nameW = 0.0f, valueW = 0.0f, infoW = overlay::text_width(keys.c_str(), s);
    for (int i = 0; i < nLines; i++) {
        nameW = std::max(nameW, overlay::text_width(names[i].c_str(), s));
        valueW = std::max(valueW, overlay::text_width(values[i].c_str(), s));
    }
    for (int i = 0; i < nInfo; i++)
        infoW = std::max(infoW, overlay::text_width(info[i].c_str(), s));
    const float title = overlay::text_width("TEXTURES", s);
    const float cols = 12.0f * fs + nameW + 18.0f * fs + valueW + 12.0f * fs;
    const float w = std::max(std::max(cols, infoW), title) + 2.0f * pad;
    const float h = 2.0f * pad + row + gap + (float)nLines * row + gap + (float)(nInfo + 1) * row;
    const float x = 8.0f * fs, y = 8.0f * fs;
    const uint32_t amber = rgba(255, 200, 80, 255), white = rgba(230, 230, 230, 255), grey = rgba(150, 150, 150, 255);
    overlay::rect(x, y, w, h, rgba(0, 0, 0, 200));
    overlay::rect(x, y, 2.0f * fs, h, amber);
    float ty = y + pad;
    overlay::text(x + pad, ty, s, "TEXTURES", rgba(255, 200, 80, 200));
    ty += row + gap;
    for (int i = 0; i < nLines; i++) {
        const bool cur = i == line;
        if (cur)
            overlay::rect(x + 2.0f * fs, ty - 1.5f * fs, w - 2.0f * fs, row, rgba(255, 200, 80, 40));
        const float vx = x + pad + 12.0f * fs + nameW + 18.0f * fs;
        overlay::text(x + pad + 12.0f * fs, ty, s, names[i].c_str(), cur ? amber : white);
        if (cur) {
            overlay::text(x + pad, ty, s, ">", amber);
            overlay::text(vx - 9.0f * fs, ty, s, "<", amber);
            overlay::text(vx + overlay::text_width(values[i].c_str(), s) + 3.0f * fs, ty, s, ">", amber);
        }
        overlay::text(vx, ty, s, values[i].c_str(), cur ? amber : white);
        ty += row;
    }
    ty += gap;
    for (int i = 0; i < nInfo; i++) {
        /* (the chosen texture's line in its own colour: the highlight's) */
        const bool chosen = select && i == 0 && st.selHash != 0;
        overlay::text(x + pad, ty, s, info[i].c_str(), chosen ? rgba(0, 255, 0, 255) : white);
        ty += row;
    }
    overlay::text(x + pad, ty, s, keys.c_str(), grey);
    overlay::end(bb);
}

} // namespace texture_menu
