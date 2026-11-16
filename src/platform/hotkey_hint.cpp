/* hotkey_hint.cpp - a hotkey's controls named for a panel (hotkey_hint.h). */
#include "platform/hotkey_hint.h"
#include "platform/gamepad.h"
#include "platform/input_map.h"

namespace hotkey_hint {
namespace {

/* A short name for a device's binding in the panel ("UP", "SELECT"; a
 * joystick's: as the launcher names it, without the device).  PC
 * (2026-11-07): the face buttons and the Guide button as the controller
 * names them (gamepad::button_names: A, B, X, Y, XBOX on an Xbox one). */
std::string device_name(const input_map::Binding &b)
{
    using input_map::Source;
    static const char *const kButtons[] = {"CROSS", "CIRCLE", "SQUARE", "TRIANGLE", "SELECT", "GUIDE", "START",
                                           "L3", "R3", "L1", "R1", "UP", "DOWN", "LEFT", "RIGHT"};
    if (b.source == Source::pad_button && b.index < sizeof(kButtons) / sizeof(kButtons[0])) {
        const gamepad::ButtonNames pad = gamepad::button_names();
        const char *const own[] = {pad.south, pad.east, pad.west, pad.north, nullptr, pad.guide};
        if (b.index < sizeof(own) / sizeof(own[0]) && own[b.index] != nullptr)
            return own[b.index];
        return kButtons[b.index];
    }
    if (b.source == Source::pad_axis && b.index == input_map::kPadAxisLeftTrigger)
        return "L2";
    if (b.source == Source::pad_axis && b.index == input_map::kPadAxisRightTrigger)
        return "R2";
    input_map::Binding own = b;
    own.device = 0;
    char d[96];
    input_map::describe(own, d, sizeof(d));
    return d;
}

} // namespace

std::string control_text(int h, const char *what)
{
    std::string s = device_text(h);
    const std::string key = key_text(h);
    if (!key.empty())
        s += s.empty() ? key : " (" + key + ")";
    if (s.empty())
        return "";
    return s + " " + what;
}

std::string device_text(int h)
{
    return device_name(input_map::hotkey_device(h));
}

std::string key_text(int h)
{
    char key[64];
    input_map::describe_combo(input_map::hotkey(h), key, sizeof(key));
    return key;
}

} // namespace hotkey_hint
