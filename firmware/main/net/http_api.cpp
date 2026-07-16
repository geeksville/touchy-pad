// SPDX-License-Identifier: GPL-3.0-or-later
//
// Touchy-Pad HTTP(S) command API (Stage lb8). See http_api.h.
//
// Implementation note — HTTPS/TLS-PSK:
//   ESP-IDF v6 does not expose `psk_hint_key` through the public
//   `httpd_ssl_config_t` struct, so PSK-authenticated HTTPS cannot be
//   configured via `esp_https_server` from application code. Until IDF
//   adds that field (or we build a custom TLS server), `http_api_start`
//   returns an error when `https == true`. The plaintext HTTP path is
//   fully implemented and is the normal operating mode.

#include "sdkconfig.h"

#if CONFIG_TOUCHY_WIFI

#include "http_api.h"
#include "host_api.h"
#include "host_api_link.h"   // HOST_API_RX_MAX / HOST_API_TX_MAX
#include "tc_tag.h"

#include "esp_http_server.h"
#include "esp_log.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = TOUCHY_TAG("http_api");

// The one URI we serve.
static constexpr const char *API_URI = "/touchy/api/v1/command";
static constexpr const char *CT      = "application/protobuf";

static httpd_handle_t s_server = nullptr;

static esp_err_t command_post_handler(httpd_req_t *req)
{
    size_t total = req->content_len;
    if (total == 0 || total > HOST_API_RX_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad command length");
        return ESP_FAIL;
    }

    // Heap buffers (kept off the small httpd task stack).
    uint8_t *in  = (uint8_t *)malloc(total);
    uint8_t *out = (uint8_t *)malloc(HOST_API_TX_MAX);
    if (!in || !out) {
        free(in);
        free(out);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    size_t off = 0;
    while (off < total) {
        int r = httpd_req_recv(req, (char *)in + off, total - off);
        if (r <= 0) {
            free(in);
            free(out);
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "recv timeout");
            }
            return ESP_FAIL;
        }
        off += (size_t)r;
    }

    size_t out_len = 0;
    bool ok = host_api_dispatch_serialized(in, total, out, HOST_API_TX_MAX, &out_len);
    free(in);
    if (!ok) {
        free(out);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "encode failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, CT);
    esp_err_t send = httpd_resp_send(req, (const char *)out, out_len);
    free(out);
    return send;
}

static const httpd_uri_t s_uri = {
    .uri      = API_URI,
    .method   = HTTP_POST,
    .handler  = command_post_handler,
    .user_ctx = nullptr,
};

int http_api_start(bool https, const char *psk_hex)
{
    if (s_server) {
        ESP_LOGW(TAG, "already running; stopping first");
        http_api_stop();
    }

    if (https) {
        // HTTPS/TLS-PSK is not yet supported: IDF v6's httpd_ssl_config_t
        // does not expose psk_hint_key, so there is no way to inject a PSK
        // key via the esp_https_server public API. The host-side client
        // already implements the PSK path for when firmware support lands.
        ESP_LOGE(TAG, "HTTPS/TLS-PSK not supported (IDF lacks psk_hint_key "
                      "in httpd_ssl_config_t); falling back to no server");
        return -1;
    }

    (void)psk_hex;   // plaintext path does not use the key

    httpd_config_t conf = HTTPD_DEFAULT_CONFIG();
    conf.server_port      = 80;
    conf.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        s_server = nullptr;
        return (int)err;
    }

    httpd_register_uri_handler(s_server, &s_uri);
    ESP_LOGI(TAG, "HTTP command API on :80%s", API_URI);
    return 0;
}

void http_api_stop(void)
{
    if (!s_server) return;
    httpd_stop(s_server);
    s_server = nullptr;
    ESP_LOGI(TAG, "command API stopped");
}

#endif // CONFIG_TOUCHY_WIFI

