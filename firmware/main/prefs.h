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
#include <string>

#include "touchy.pb.h"

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

    // Stage 82 — minimum log priority queued for the host (a
    // touchy_LogPriority value). Lines below this are dropped device-side.
    // Default LOG_PRIORITY_ERROR.
    uint32_t min_log_level() const { return m_min_log_level; }

    // Stage 82 — early-boot delay in seconds (0 = disabled). Read at boot
    // to give a host debug-log connection time to attach.
    uint32_t boot_delay_s() const { return m_boot_delay_s; }

    // Stage 94 — display backlight brightness as a percentage (0 = off …
    // 100 = max). Default 100. Applied on boot by backlight_init().
    uint8_t backlight_level() const { return m_backlight_level; }

    // Update the backlight brightness and persist it immediately to flash.
    void set_backlight_level(uint8_t level);

    // Stage lb6 — hardware/board description (LED panel geometry, etc.).
    // Persisted like any other pref but with no live side effect: it is
    // read once at boot by display_init(). Returns the merged config (all
    // arrays default-empty when nothing was ever programmed).
    const touchy_BoardConfig &board_config() const { return m_board_config; }

    // Stage lb8 — WiFi + network-API configuration. Persisted like any
    // other pref; applying it (via apply_partial or boot) joins/leaves the
    // WiFi network and starts/stops the HTTP(S) command API. Returns the
    // merged config (all fields default-absent when never programmed).
    const touchy_NetworkConfig &network() const { return m_network; }

    // Stage LB4 — snapshot the current preferences as a fully-populated
    // `touchy_PreferencesFile` (file_version + every value field, all
    // has_* presence flags set). This is exactly what `save()` writes to
    // flash, so the returned message mirrors the persisted file. Used by
    // the GetPreferencesCmd handler to reply with the live settings.
    touchy_PreferencesFile to_proto() const;

    // Stage 82 — apply a partial PreferencesFile from a SetPreferencesCmd.
    // Only fields with explicit presence (has_*) are merged; the host's
    // file_version (if any) is ignored. Each applied field fires its side
    // effect (backlight timeout, screen load, log threshold) and the merged
    // result is persisted once. Returns false only if a requested
    // current_screen could not be loaded.
    bool apply_partial(const touchy_PreferencesFile &p);

    // Full drive-prefixed path of the screen the firmware was most
    // recently asked to show (set by screens.cpp after every successful
    // screens_load) — e.g. `F:host/s/home.pb`. Empty string
    // means "no preference": boot uses the registry's first entry,
    // falling back to the built-in default screen.
    const std::string &current_screen() const { return m_current_screen; }

    // Record the most-recently-loaded screen path; written through to
    // flash so a reboot can restore it. No-op when the value would not
    // change, to avoid pointless flash wear on rapid screen switches.
    void set_current_screen(const std::string &path);

    // Singleton accessor.
    static Prefs &instance();

private:
    Prefs() = default;

    void save();

    // Default applies only when no prefs file exists yet (first boot or
    // after a flash erase). Once the user calls ScreenSleepTimeoutCmd —
    // even with 0 to mean "disable" — that value is persisted and used
    // verbatim. 5 minutes keeps the LCD off long enough to mitigate
    // image-retention without surprising someone who briefly steps away.
    uint32_t    m_timeout_ms     = 5u * 60u * 1000u;   // 5 minutes
    std::string m_current_screen;       // default: empty (no preference)

    // Stage 82 — see accessors above. Default min log level is ERROR so the
    // host log tunnel is quiet unless explicitly turned up.
    uint32_t    m_min_log_level  = touchy_LogPriority_ERROR;
    uint32_t    m_boot_delay_s   = 0;

    // Stage 94 — backlight brightness percent (0..100). Default full.
    uint8_t     m_backlight_level = 100;

    // Stage lb6 — board hardware description (LED panel geometry). Empty
    // (no displays) until the host programs it via SetPreferencesCmd.
    touchy_BoardConfig m_board_config = touchy_BoardConfig_init_zero;

    // Stage lb8 — WiFi + network-API config. Empty until the host
    // programs it via SetPreferencesCmd.
    touchy_NetworkConfig m_network = touchy_NetworkConfig_init_zero;
};

