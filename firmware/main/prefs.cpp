// SPDX-License-Identifier: GPL-3.0-or-later
//
// Touchy-Pad per-device preferences (stage 19).

#include "prefs.h"
#include "tc_tag.h"

#include "backlight.h"
#include "fs.h"
#include "log_proto.h"
#include "protobuf.h"
#include "preferences.pb.h"
#include "screens.h"

#include "esp_log.h"

#include <cstdio>

static const char *TAG = TOUCHY_TAG("prefs");

// Filesystem path (relative to the LittleFS mount root). Stays on flash
// regardless of the new R:/F: drive-letter scheme — device-private
// preferences are never addressed by host paths.
static constexpr const char *PREFS_PATH = "prefs/prefs.pb";

// Encode buffer. `PreferencesFile` carries the scalars plus one
// fixed-size string (current_screen, max 96 bytes via
// preferences.options post-Stage 51). 256 bytes is comfortably above
// the worst-case encoding.
static constexpr size_t PREFS_BUF_SIZE = 256;

Prefs &Prefs::instance()
{
    static Prefs s;
    return s;
}

bool Prefs::begin()
{
    auto &fs = FlashFs::instance();
    size_t len = 0;
    uint8_t *data = fs.readBinary(PREFS_PATH, &len);
    if (!data) {
        ESP_LOGI(TAG, "No prefs file found — using defaults "
                       "(screen_timeout_ms=%" PRIu32 ")",
                 m_timeout_ms);
        return true;
    }

    PbMessage<touchy_PreferencesFile> pf(touchy_PreferencesFile_fields);
    bool ok = pf.decode(data, len);
    delete[] data;

    if (ok) {
        if (pf->has_screen_timeout_ms) m_timeout_ms = pf->screen_timeout_ms;
        if (pf->has_current_screen) m_current_screen = pf->current_screen;
        if (pf->has_min_log_level) m_min_log_level = pf->min_log_level;
        if (pf->has_boot_delay_s) m_boot_delay_s = pf->boot_delay_s;
        if (pf->has_backlight_level) m_backlight_level = (uint8_t)pf->backlight_level;
        ESP_LOGI(TAG, "Loaded prefs: screen_timeout_ms=%" PRIu32
                      " current_screen='%s' min_log_level=%" PRIu32
                      " boot_delay_s=%" PRIu32 " backlight_level=%u",
                 m_timeout_ms, m_current_screen.c_str(), m_min_log_level,
                 m_boot_delay_s, (unsigned)m_backlight_level);
    } else {
        ESP_LOGW(TAG, "Prefs file corrupt — using defaults");
    }
    return true;  // never hard-fail on prefs
}

void Prefs::set_current_screen(const std::string &path)
{
    if (path == m_current_screen) return;   // no churn for repeat loads
    m_current_screen = path;
    save();
}

void Prefs::set_screen_timeout_ms(uint32_t ms)
{
    m_timeout_ms = ms;
    save();
}

void Prefs::set_backlight_level(uint8_t level)
{
    if (level > 100) level = 100;
    m_backlight_level = level;
    save();
}

bool Prefs::apply_partial(const touchy_PreferencesFile &p)
{
    // Stage 82 — merge only the fields the host actually set. file_version
    // is device-owned and intentionally ignored. We fire each field's side
    // effect, then persist once at the end.
    bool screen_ok = true;

    if (p.has_screen_timeout_ms) {
        m_timeout_ms = p.screen_timeout_ms;
        backlight_set_timeout(m_timeout_ms);
    }
    if (p.has_min_log_level) {
        m_min_log_level = p.min_log_level;
        log_proto_set_min_level((touchy_LogPriority)m_min_log_level);
    }
    if (p.has_boot_delay_s) {
        m_boot_delay_s = p.boot_delay_s;  // applied at next boot only
    }
    if (p.has_backlight_level) {
        m_backlight_level = (uint8_t)p.backlight_level;
        backlight_set_level(m_backlight_level);  // applies + persists
    }
    if (p.has_current_screen) {
        // screens_load() updates g_current_path and calls back into
        // set_current_screen(), but we also mirror it here so the value
        // persists even if the load path changes.
        if (screens_load(p.current_screen)) {
            m_current_screen = p.current_screen;
        } else {
            screen_ok = false;
        }
    }

    save();
    return screen_ok;
}

void Prefs::save()
{
    PbMessage<touchy_PreferencesFile> pf(touchy_PreferencesFile_fields);
    pf->file_version      = touchy_PreferencesFile_Version_CURRENT;
    // Stage 82 — fields are now `optional` (explicit presence); set the
    // has_* flags so nanopb serialises them.
    pf->has_screen_timeout_ms = true;
    pf->screen_timeout_ms = m_timeout_ms;
    pf->has_min_log_level = true;
    pf->min_log_level = m_min_log_level;
    pf->has_boot_delay_s = true;
    pf->boot_delay_s = m_boot_delay_s;
    pf->has_backlight_level = true;
    pf->backlight_level = m_backlight_level;
    // current_screen is a fixed-size char[N] in the generated struct;
    // snprintf truncates safely if the source ever exceeds the bound
    // (which it shouldn't — the bound matches FileOpenWriteCmd.path).
    pf->has_current_screen = true;
    snprintf(pf->current_screen, sizeof(pf->current_screen), "%s",
             m_current_screen.c_str());

    uint8_t buf[PREFS_BUF_SIZE];
    size_t  n = 0;
    if (!pf.encode(buf, sizeof(buf), &n)) {
        ESP_LOGE(TAG, "Failed to encode prefs");
        return;
    }
    if (!FlashFs::instance().writeFile(PREFS_PATH, buf, n)) {
        ESP_LOGE(TAG, "Failed to write prefs to %s", PREFS_PATH);
        return;
    }
    ESP_LOGI(TAG, "Saved prefs (screen_timeout_ms=%" PRIu32
                  " current_screen='%s')",
             m_timeout_ms, m_current_screen.c_str());
}
