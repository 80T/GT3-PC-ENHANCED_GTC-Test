/* hotkey_hint.h - PC (2026-11-08): a hotkey's controls named for a panel
 * drawn with platform/overlay.h ("UP (,) PREVIOUS": the device's button, the
 * key, what it does), as the race's song changer shows them (song_osd.cpp,
 * where this came from; the cockpit camera's calibration panel too,
 * cockpit_osd.cpp).  No Windows headers. */
#pragma once

#include <string>

namespace hotkey_hint {

/* "UP (,) PREVIOUS": hotkey h's device and key, then what it does; "" when
 * it has neither.  The face buttons and the Guide button as the connected
 * controller names them (gamepad::button_names). */
std::string control_text(int hotkey, const char *what);
/* The parts: the device's binding alone ("UP", "SELECT", "A"; "" none) and
 * the key alone ("F4", "Ctrl+Esc"; "" none). */
std::string device_text(int hotkey);
std::string key_text(int hotkey);

} // namespace hotkey_hint
