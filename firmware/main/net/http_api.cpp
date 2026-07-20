// SPDX-License-Identifier: GPL-3.0-or-later
//
// Touchy-Pad HTTP(S) command API (Stage lb8 + lb9 mTLS). See http_api.h.
//
// Plaintext HTTP (port 80) is the pre-provisioning mode. Once the host has
// pushed a mutual-TLS cert set (F:tls/server.crt, F:tls/server.key,
// F:tls/client_ca.crt) via `touchy pref provision-mtls`, the network
// subsystem starts us with mtls=true: HTTPS on port 443 where the server
// requires a client certificate signed by the provisioned CA (esp-tls sets
// MBEDTLS_SSL_VERIFY_REQUIRED whenever a CA cert is configured), so only
// provisioned clients can connect.

#include "sdkconfig.h"

#if CONFIG_TOUCHY_WIFI

#include "http_api.h"
#include "host_api.h"
#include "host_api_link.h"   // HOST_API_RX_MAX / HOST_API_TX_MAX
#include "json.h"            // Stage lb13 — JSON <-> Command/Response
#include "touchy.pb.h"
#include "fs.h"
#include "tc_tag.h"

#include "esp_http_server.h"
#include "esp_https_server.h"
#include "esp_log.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = TOUCHY_TAG("http_api");

// The one URI we serve.
static constexpr const char *API_URI = "/touchy/api/v1/command";
static constexpr const char *CT      = "application/protobuf";
static constexpr const char *CT_JSON = "application/json";

// Stage lb13 — does the request carry a JSON body? (Content-Type contains
// "json", so "application/json" and "application/json; charset=…" match.)
static bool request_is_json(httpd_req_t *req)
{
    char ct[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct)) != ESP_OK) {
        return false;
    }
    return strstr(ct, "json") != nullptr;
}

// mTLS certificate material (uploaded via FileWrite; see paths.py).
static constexpr const char *TLS_SERVER_CERT = "F:tls/server.crt";
static constexpr const char *TLS_SERVER_KEY  = "F:tls/server.key";
static constexpr const char *TLS_CLIENT_CA   = "F:tls/client_ca.crt";

static httpd_handle_t s_server   = nullptr;
static bool           s_is_https = false;

// The command handler puts a decoded touchy_Command + touchy_Response on
// its stack (and the JSON path adds cJSON churn), so bump the httpd worker
// task stack well above the 4 KB default to avoid an overflow crash.
static constexpr size_t HTTPD_STACK = 12 * 1024;

// Read a whole file (drive-prefixed path) into a freshly-malloc'd,
// NUL-terminated buffer. esp-tls detects PEM vs DER by checking the last
// byte is '\0' and expects the length to *include* that terminator, so we
// always append one. Returns nullptr on error; on success *len_out is the
// buffer length including the trailing NUL. Caller frees with free().
static uint8_t *read_pem(const char *path, size_t *len_out)
{
    std::string rest;
    Fs *fs = fs_resolve(path, &rest);
    if (!fs) return nullptr;
    size_t n = 0;
    uint8_t *raw = fs->readBinary(rest, &n);   // caller must delete[]
    if (!raw) return nullptr;
    uint8_t *buf = (uint8_t *)malloc(n + 1);
    if (buf) {
        memcpy(buf, raw, n);
        buf[n] = '\0';
        *len_out = n + 1;
    }
    delete[] raw;
    return buf;
}

static esp_err_t command_post_handler(httpd_req_t *req)
{
    size_t total = req->content_len;
    if (total == 0 || total > HOST_API_RX_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad command length");
        return ESP_FAIL;
    }

    const bool is_json = request_is_json(req);

    // Request body buffer (kept off the small httpd task stack).
    uint8_t *in = (uint8_t *)malloc(total);
    if (!in) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    size_t off = 0;
    while (off < total) {
        int r = httpd_req_recv(req, (char *)in + off, total - off);
        if (r <= 0) {
            free(in);
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "recv timeout");
            }
            return ESP_FAIL;
        }
        off += (size_t)r;
    }

    // Stage lb13 — JSON path: parse → dispatch → render, all on the shared
    // command core (host_api_dispatch_message). The response mirrors the
    // request encoding.
    if (is_json) {
        touchy_Command cmd;
        const char *err = nullptr;
        if (!json_to_command((const char *)in, total, &cmd, &err)) {
            free(in);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                err ? err : "bad JSON command");
            return ESP_FAIL;
        }
        free(in);

        touchy_Response resp = touchy_Response_init_zero;
        host_api_dispatch_message(&cmd, &resp);

        char *js = response_to_json(&resp);
        if (!js) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
            return ESP_FAIL;
        }
        httpd_resp_set_type(req, CT_JSON);
        esp_err_t send = httpd_resp_send(req, js, HTTPD_RESP_USE_STRLEN);
        free(js);
        return send;
    }

    // Binary protobuf path (default / application/protobuf).
    uint8_t *out = (uint8_t *)malloc(HOST_API_TX_MAX);
    if (!out) {
        free(in);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
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

static int start_https_mtls(void)
{
    // Load the three PEMs. esp_https_server copies them into its own
    // context during startup, so we can free ours right after start.
    size_t cert_len = 0, key_len = 0, ca_len = 0;
    uint8_t *cert = read_pem(TLS_SERVER_CERT, &cert_len);
    uint8_t *key  = read_pem(TLS_SERVER_KEY,  &key_len);
    uint8_t *ca   = read_pem(TLS_CLIENT_CA,   &ca_len);
    if (!cert || !key || !ca) {
        ESP_LOGE(TAG, "mTLS requested but a cert file is missing/unreadable");
        free(cert);
        free(key);
        free(ca);
        return -1;
    }

    httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
    conf.transport_mode = HTTPD_SSL_TRANSPORT_SECURE;
    conf.port_secure    = 443;
    conf.httpd.stack_size = HTTPD_STACK;
    conf.servercert     = cert;
    conf.servercert_len = cert_len;
    conf.prvtkey_pem    = key;
    conf.prvtkey_len    = key_len;
    // Setting cacert_pem makes esp-tls require + verify a client cert
    // signed by this CA — i.e. mutual TLS.
    conf.cacert_pem     = ca;
    conf.cacert_len     = ca_len;

    esp_err_t err = httpd_ssl_start(&s_server, &conf);

    free(cert);
    free(key);
    free(ca);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ssl_start failed: %s", esp_err_to_name(err));
        s_server = nullptr;
        return (int)err;
    }
    s_is_https = true;
    httpd_register_uri_handler(s_server, &s_uri);
    ESP_LOGI(TAG, "HTTPS command API (mTLS) on :443%s", API_URI);
    return 0;
}

static int start_http_plain(void)
{
    httpd_config_t conf = HTTPD_DEFAULT_CONFIG();
    conf.server_port      = 80;
    conf.lru_purge_enable = true;
    conf.stack_size       = HTTPD_STACK;

    esp_err_t err = httpd_start(&s_server, &conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        s_server = nullptr;
        return (int)err;
    }
    s_is_https = false;
    httpd_register_uri_handler(s_server, &s_uri);
    ESP_LOGI(TAG, "HTTP command API on :80%s", API_URI);
    return 0;
}

int http_api_start(bool mtls)
{
    if (s_server) {
        ESP_LOGW(TAG, "already running; stopping first");
        http_api_stop();
    }
    return mtls ? start_https_mtls() : start_http_plain();
}

void http_api_stop(void)
{
    if (!s_server) return;
    if (s_is_https) {
        httpd_ssl_stop(s_server);
    } else {
        httpd_stop(s_server);
    }
    s_server = nullptr;
    ESP_LOGI(TAG, "command API stopped");
}

#endif // CONFIG_TOUCHY_WIFI

