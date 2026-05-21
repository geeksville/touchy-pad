// SPDX-License-Identifier: GPL-3.0-or-later
//
// Touchy-Pad per-device preferences (stage 19).

#include "prefs.h"

#include "fs.h"
#include "protobuf.h"
#include "preferences.pb.h"

#include "esp_log.h"

static const char *TAG = "prefs";

// Filesystem path (relative to the LittleFS mount root).
static constexpr const char *PREFS_PATH = "prefs/prefs.pb";

// Encode buffer — `PreferencesFile` is all scalars, so 64 bytes is ample.
static constexpr size_t PREFS_BUF_SIZE = 64;

Prefs &Prefs::instance()
{
    static Prefs s;
    return s;
}

bool Prefs::begin()
{
    auto &fs = Fs::instance();
    size_t len = 0;
    uint8_t *data = fs.readBinary(PREFS_PATH, &len);
    if (!data) {
        ESP_LOGI(TAG, "No prefs file found — using defaults "
                       "(screen_timeout_ms=0)");
        return true;
    }

    PbMessage<touchy_PreferencesFile> pf(touchy_PreferencesFile_fields);
    bool ok = pf.decode(data, len);
    delete[] data;

    if (ok) {
        m_timeout_ms = pf->screen_timeout_ms;
        ESP_LOGI(TAG, "Loaded prefs: screen_timeout_ms=%" PRIu32,
                 m_timeout_ms);
    } else {
        ESP_LOGW(TAG, "Prefs file corrupt — using defaults");
    }
    return true;  // never hard-fail on prefs
}

void Prefs::set_screen_timeout_ms(uint32_t ms)
{
    m_timeout_ms = ms;
    save();
}

void Prefs::save()
{
    PbMessage<touchy_PreferencesFile> pf(touchy_PreferencesFile_fields);
    pf->file_version     = touchy_PreferencesFile_Version_CURRENT;
    pf->screen_timeout_ms = m_timeout_ms;

    uint8_t buf[PREFS_BUF_SIZE];
    size_t  n = 0;
    if (!pf.encode(buf, sizeof(buf), &n)) {
        ESP_LOGE(TAG, "Failed to encode prefs");
        return;
    }
    if (!Fs::instance().writeFile(PREFS_PATH, buf, n)) {
        ESP_LOGE(TAG, "Failed to write prefs to %s", PREFS_PATH);
        return;
    }
    ESP_LOGI(TAG, "Saved prefs (screen_timeout_ms=%" PRIu32 ")",
             m_timeout_ms);
}
