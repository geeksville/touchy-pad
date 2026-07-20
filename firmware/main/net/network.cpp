// SPDX-License-Identifier: GPL-3.0-or-later
//
// Touchy-Pad WiFi + mDNS bring-up (Stage lb8). See network.h.

#include "sdkconfig.h"

#if CONFIG_TOUCHY_WIFI

#include "network.h"
#include "http_api.h"
#include "platform.h"
#include "tc_tag.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "fs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include <string>

static const char *TAG = TOUCHY_TAG("network");

namespace {

// Bring-up-once guards + last-applied config so network_apply() is a cheap
// no-op when nothing changed.
bool s_stack_inited = false;   // netif/event-loop/wifi driver up
bool s_started      = false;   // esp_wifi_start() called
bool s_connected    = false;   // got an IP

std::string s_ssid;
std::string s_psk;
std::string s_hostname;
bool        s_no_tls = false;  // Stage lb14: override — serve HTTP even if certs present

esp_netif_t *s_netif = nullptr;

// Stage lb9 — mTLS is enabled iff the full cert set is present on flash
// (uploaded by `touchy pref provision-mtls`). When present we serve
// HTTPS-with-mTLS on 443; otherwise plaintext HTTP on 80.
bool mtls_provisioned()
{
    static const char *paths[] = {
        "F:tls/server.crt", "F:tls/server.key", "F:tls/client_ca.crt"};
    for (const char *p : paths) {
        std::string rest;
        Fs *fs = fs_resolve(p, &rest);
        size_t len = 0;
        if (!fs) return false;
        const uint8_t *peeked = fs->peek(rest, &len);
        if (peeked) continue;   // present (zero-copy)
        // FlashFs can't peek; fall back to a real read to test existence.
        uint8_t *raw = fs->readBinary(rest, &len);
        if (!raw) return false;
        delete[] raw;
    }
    return true;
}

// Derive the default mDNS/hostname from the board serial:
// "touchypad_<last-6-of-serial>", lowercased and stripped of non-alnum.
std::string default_hostname()
{
    const char *serial = platform_serial();
    std::string suffix;
    for (const char *p = serial; *p && suffix.size() < 12; ++p) {
        char c = *p;
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')) {
            suffix.push_back(c);
        } else if (c >= 'A' && c <= 'Z') {
            suffix.push_back((char)(c - 'A' + 'a'));
        }
    }
    if (suffix.size() > 6) suffix = suffix.substr(suffix.size() - 6);
    if (suffix.empty()) suffix = "0000";
    return std::string("touchypad_") + suffix;
}

void start_servers()
{
    bool mtls = !s_no_tls && mtls_provisioned();

    // (Re)start mDNS under the configured hostname.
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(s_hostname.c_str());
        mdns_instance_name_set("Touchy-Pad");
        mdns_service_add(nullptr, "_touchy", "_tcp", mtls ? 443 : 80, nullptr, 0);
        ESP_LOGI(TAG, "mDNS up as %s.local", s_hostname.c_str());
    } else {
        ESP_LOGW(TAG, "mdns_init failed");
    }

    if (http_api_start(mtls) != 0) {
        ESP_LOGE(TAG, "command API failed to start (mtls=%d)", (int)mtls);
    }
}

void stop_servers()
{
    http_api_stop();
    mdns_free();
}

void on_wifi_event(void *, esp_event_base_t base, int32_t id, void *)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_connected) {
            s_connected = false;
            stop_servers();
        }
        // Keep retrying while a network is configured.
        if (!s_ssid.empty()) {
            esp_wifi_connect();
        }
    }
}

void on_ip_event(void *, esp_event_base_t, int32_t id, void *data)
{
    if (id == IP_EVENT_STA_GOT_IP) {
        auto *evt = static_cast<ip_event_got_ip_t *>(data);
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&evt->ip_info.ip));
        s_connected = true;
        start_servers();
    }
}

// Lazy one-time bring-up of the netif + event loop + WiFi driver.
bool ensure_stack()
{
    if (s_stack_inited) return true;

    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    if (esp_netif_init() != ESP_OK) return false;
    // The default event loop may already exist (created by another
    // subsystem); ESP_ERR_INVALID_STATE means "already created" — fine.
    esp_err_t loop = esp_event_loop_create_default();
    if (loop != ESP_OK && loop != ESP_ERR_INVALID_STATE) return false;

    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) return false;

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &on_wifi_event, nullptr, nullptr);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &on_ip_event, nullptr, nullptr);

    esp_wifi_set_mode(WIFI_MODE_STA);
    s_stack_inited = true;
    return true;
}

}  // namespace

void network_apply(const touchy_NetworkConfig &cfg)
{
    std::string ssid     = cfg.has_wifi_ssid   ? cfg.wifi_ssid   : "";
    std::string psk      = cfg.has_wifi_psk    ? cfg.wifi_psk    : "";
    std::string hostname = (cfg.has_hostname && cfg.hostname[0]) ? cfg.hostname
                                                                 : default_hostname();
    bool        no_tls   = cfg.no_tls;

    // No credentials → ensure everything is torn down.
    if (ssid.empty()) {
        if (s_started) {
            ESP_LOGI(TAG, "WiFi config cleared — disconnecting");
            stop_servers();
            esp_wifi_disconnect();
            esp_wifi_stop();
            s_started = s_connected = false;
        }
        s_ssid.clear();
        return;
    }

    // Nothing changed and we're already up → no-op. (mTLS cert changes
    // arrive via FileWrite, not this path, and take effect on the next
    // bring-up / reboot; WiFi credential changes re-run everything below.)
    if (s_started && ssid == s_ssid && psk == s_psk && hostname == s_hostname
                  && no_tls == s_no_tls) {
        return;
    }

    if (!ensure_stack()) {
        ESP_LOGE(TAG, "network stack bring-up failed");
        return;
    }

    // A change while already running: tear the session down before
    // reconnecting with the new parameters.
    if (s_started) {
        stop_servers();
        esp_wifi_disconnect();
        esp_wifi_stop();
        s_started = s_connected = false;
    }

    s_ssid     = ssid;
    s_psk      = psk;
    s_hostname = hostname;
    s_no_tls   = no_tls;

    wifi_config_t wc = {};
    strncpy((char *)wc.sta.ssid, s_ssid.c_str(), sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, s_psk.c_str(), sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = s_psk.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    esp_netif_set_hostname(s_netif, s_hostname.c_str());
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_start();   // fires WIFI_EVENT_STA_START → esp_wifi_connect()
    s_started = true;

    ESP_LOGI(TAG, "joining '%s' (host=%s)", s_ssid.c_str(), s_hostname.c_str());
}

#endif // CONFIG_TOUCHY_WIFI
