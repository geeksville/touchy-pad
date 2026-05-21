// SPDX-License-Identifier: GPL-3.0-or-later
//
// Touchy-Pad per-device preferences singleton (stage 19).
//
// Persists device-wide settings — currently just the backlight sleep timeout
// — to `/prefs/prefs.pb` (a `touchy.PreferencesFile` protobuf) so they
// survive reboots. The host updates them via normal Command messages
// (ScreenSleepTimeoutCmd); the firmware applies them on boot.

#pragma once

#include <cstdint>

class Prefs {
public:
    // Load preferences from `/prefs/prefs.pb`. Falls back to built-in
    // defaults if the file is absent or corrupt. Always returns true so
    // callers don't need to handle a "prefs unavailable" path.
    bool begin();

    // Backlight auto-sleep timeout in milliseconds. 0 = disabled.
    uint32_t screen_timeout_ms() const { return m_timeout_ms; }

    // Update the timeout and persist it immediately to flash.
    void set_screen_timeout_ms(uint32_t ms);

    // Singleton accessor.
    static Prefs &instance();

private:
    Prefs() = default;

    void save();

    uint32_t m_timeout_ms = 0;  // default: backlight always on
};
