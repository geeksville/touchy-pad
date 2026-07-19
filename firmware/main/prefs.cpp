// SPDX-License-Identifier: GPL-3.0-or-later
//
// Touchy-Pad per-device preferences (stage 19).

#include "prefs.h"
#include "tc_tag.h"

#include "backlight.h"
#include "fs.h"
#include "led_display.h"
#include "log_proto.h"
#include "protobuf.h"
#include "preferences.pb.h"
#include "screens.h"

#include "esp_log.h"

#if CONFIG_TOUCHY_WIFI
#include "network.h"
#endif

#include <cstdio>

static const char *TAG = TOUCHY_TAG("prefs");

// Filesystem path (relative to the LittleFS mount root). Stays on flash
// regardless of the new R:/F: drive-letter scheme — device-private
// preferences are never addressed by host paths.
static constexpr const char *PREFS_PATH = "prefs/prefs.pb";

// Encode buffer. `PreferencesFile` carries the scalars, one fixed-size
// string (current_screen, max 96 bytes), and the Stage lb6 board_config
// (one Display of one Panel). 512 bytes is comfortably above the
// worst-case encoding.
static constexpr size_t PREFS_BUF_SIZE = 512;

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
        if (pf->has_board_config) m_board_config = pf->board_config;
        if (pf->has_network) m_network = pf->network;
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
    if (p.has_board_config) {
        // Stage lb6 — no live side effect; used at the next display_init().
        m_board_config = p.board_config;
    }
    if (p.has_network) {
        // Stage lb8 — merge the NetworkConfig sub-fields individually (so a
        // host that sets only wifi_ssid doesn't wipe a previously-set psk),
        // then apply live: (re)join WiFi / restart the API.
        const touchy_NetworkConfig &n = p.network;
        if (n.has_wifi_ssid) {
            m_network.has_wifi_ssid = true;
            strncpy(m_network.wifi_ssid, n.wifi_ssid, sizeof(m_network.wifi_ssid) - 1);
            m_network.wifi_ssid[sizeof(m_network.wifi_ssid) - 1] = '\0';
        }
        if (n.has_wifi_psk) {
            m_network.has_wifi_psk = true;
            strncpy(m_network.wifi_psk, n.wifi_psk, sizeof(m_network.wifi_psk) - 1);
            m_network.wifi_psk[sizeof(m_network.wifi_psk) - 1] = '\0';
        }
        if (n.has_hostname) {
            m_network.has_hostname = true;
            strncpy(m_network.hostname, n.hostname, sizeof(m_network.hostname) - 1);
            m_network.hostname[sizeof(m_network.hostname) - 1] = '\0';
        }
#if CONFIG_TOUCHY_WIFI
        network_apply(m_network);
#endif
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
    *pf.get() = to_proto();

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

touchy_PreferencesFile Prefs::to_proto() const
{
    touchy_PreferencesFile pf = touchy_PreferencesFile_init_zero;
    pf.file_version      = touchy_PreferencesFile_Version_CURRENT;
    // Stage 82 — fields are `optional` (explicit presence); set the
    // has_* flags so nanopb serialises / the host sees them.
    pf.has_screen_timeout_ms = true;
    pf.screen_timeout_ms = m_timeout_ms;
    pf.has_min_log_level = true;
    pf.min_log_level = m_min_log_level;
    pf.has_boot_delay_s = true;
    pf.boot_delay_s = m_boot_delay_s;
    pf.has_backlight_level = true;
    pf.backlight_level = m_backlight_level;
    // Stage lb6 — board_config is only serialised when something was
    // programmed (at least one display); an empty config stays absent so
    // a never-configured device round-trips as "no board_config".
    if (m_board_config.displays_count > 0) {
        pf.has_board_config = true;
        pf.board_config = m_board_config;
    }
    // Stage lb8 — network config is only serialised when something was
    // programmed (any field present); an all-absent config stays out so a
    // never-configured device round-trips as "no network".
    if (m_network.has_wifi_ssid || m_network.has_wifi_psk ||
        m_network.has_hostname) {
        pf.has_network = true;
        pf.network = m_network;
    }
    // current_screen is a fixed-size char[N] in the generated struct;
    // snprintf truncates safely if the source ever exceeds the bound
    // (which it shouldn't — the bound matches FileOpenWriteCmd.path).
    pf.has_current_screen = true;
    snprintf(pf.current_screen, sizeof(pf.current_screen), "%s",
             m_current_screen.c_str());
    return pf;
}

// Stage lb6/lb10 — proto-free accessor for the board-compiled LED display
// driver (declared in led_display.h). Keeps the protobuf types out of the
// board component. Reports the single configured LED panel chain, if any,
// resolving cols_snaked's UNSET-means-true presence here.
bool led_chain_config(LedChainDesc *out)
{
    const touchy_BoardConfig &cfg = Prefs::instance().board_config();
    if (cfg.displays_count == 0 || cfg.displays[0].chains_count == 0) {
        return false;
    }
    const touchy_PanelChain &chain = cfg.displays[0].chains[0];
    if (chain.panels_count == 0) {
        return false;
    }

    out->gpio        = (int)chain.gpio;
    out->tile_by_row = chain.tile_by_row;

    int n = chain.panels_count;
    if (n > LED_CHAIN_MAX_PANELS) n = LED_CHAIN_MAX_PANELS;
    out->panel_count = n;

    for (int i = 0; i < n; ++i) {
        const touchy_Panel &p = chain.panels[i];
        out->panels[i].width        = (int)p.width;
        out->panels[i].height       = (int)p.height;
        out->panels[i].rows_snaked  = p.rows_snaked;
        // proto3 presence: UNSET ⇒ true (see led_display.h / preferences.proto).
        out->panels[i].cols_snaked  = p.has_cols_snaked ? p.cols_snaked : true;
        out->panels[i].row_major    = p.row_major;
        out->panels[i].cols_flipped = p.cols_flipped;
        out->panels[i].rows_flipped = p.rows_flipped;
    }
    return true;
}
