// SPDX-License-Identifier: GPL-3.0-or-later
//
// Touchy-Pad per-device preferences (stage 19).

#include "prefs.h"

#include "fs.h"
#include "protobuf.h"
#include "preferences.pb.h"

#include "esp_log.h"

#include <cstdio>

static const char *TAG = "prefs";

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
        m_timeout_ms = pf->screen_timeout_ms;
        m_current_screen = pf->current_screen;  // fixed-size buf, NUL-terminated
        ESP_LOGI(TAG, "Loaded prefs: screen_timeout_ms=%" PRIu32
                      " current_screen='%s'",
                 m_timeout_ms, m_current_screen.c_str());
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

void Prefs::save()
{
    PbMessage<touchy_PreferencesFile> pf(touchy_PreferencesFile_fields);
    pf->file_version      = touchy_PreferencesFile_Version_CURRENT;
    pf->screen_timeout_ms = m_timeout_ms;
    // current_screen is a fixed-size char[N] in the generated struct;
    // snprintf truncates safely if the source ever exceeds the bound
    // (which it shouldn't — the bound matches ScreenLoadCmd.path).
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
