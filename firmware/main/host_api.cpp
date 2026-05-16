// SPDX-License-Identifier: Apache-2.0
//
// Touchy-Pad host_api dispatcher — see host_api.h.

#include "host_api.h"

#include "fs.h"
#include "screens.h"
#include "touchy.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"

#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tusb.h"

#include <string.h>

static const char *TAG = "host_api";

// Wire protocol version. Bump when the on-the-wire framing or any oneof
// numbering changes in a way that breaks existing host clients.
#define TOUCHY_PROTOCOL_VERSION  1

// Largest serialised Command we accept. Bounded by the nanopb option sizes
// in proto/touchy.options (ImageSaveCmd.data is the dominant contributor at
// 32 KB). Add a small overhead for tags/lengths.
#define HOST_API_RX_MAX  (32 * 1024 + 256)
#define HOST_API_TX_MAX  (4  * 1024)

static SemaphoreHandle_t s_rx_sem;            // signalled by tud_vendor_rx_cb
static TaskHandle_t      s_task;
static uint8_t           s_rx_buf[HOST_API_RX_MAX];
static uint8_t           s_tx_buf[HOST_API_TX_MAX];

// ---------------------------------------------------------------------------
// Vendor endpoint I/O helpers (blocking on the dispatcher task)
// ---------------------------------------------------------------------------

// Read exactly `want` bytes from the vendor OUT endpoint. Returns false if
// the host disconnects mid-frame; in that case the caller resyncs by
// dropping any partial frame and waiting for the next rx-cb wake-up.
static bool vendor_read_exact(uint8_t *dst, size_t want)
{
    size_t got = 0;
    while (got < want) {
        // tud_vendor_available() returns the count of bytes already in
        // TinyUSB's FIFO. tud_vendor_read() is non-blocking.
        uint32_t n = tud_vendor_read(dst + got, want - got);
        if (n > 0) {
            got += n;
            continue;
        }
        if (!tud_mounted()) return false;
        // Wait for the next chunk. The rx-cb gives the semaphore.
        xSemaphoreTake(s_rx_sem, pdMS_TO_TICKS(1000));
    }
    return true;
}

static bool vendor_write_frame(const uint8_t *payload, size_t len)
{
    // u32 LE length prefix; see docs/host-api.md.
    uint8_t hdr[4] = {
        (uint8_t)(len & 0xFF),
        (uint8_t)((len >> 8) & 0xFF),
        (uint8_t)((len >> 16) & 0xFF),
        (uint8_t)((len >> 24) & 0xFF),
    };

    // tud_vendor_write returns the number of bytes accepted into the FIFO;
    // it may be less than requested if the FIFO is full. Loop until done.
    auto push = [](const uint8_t *p, size_t n) -> bool {
        size_t sent = 0;
        while (sent < n) {
            uint32_t w = tud_vendor_write(p + sent, n - sent);
            if (w == 0) {
                if (!tud_mounted()) return false;
                // Encourage TinyUSB to drain the in-EP and yield briefly.
                tud_vendor_write_flush();
                vTaskDelay(1);
                continue;
            }
            sent += w;
        }
        return true;
    };
    if (!push(hdr, sizeof(hdr))) return false;
    if (len && !push(payload, len)) return false;
    tud_vendor_write_flush();
    return true;
}

// ---------------------------------------------------------------------------
// Command handlers
// ---------------------------------------------------------------------------

static void fill_sys_version(touchy_Response *resp)
{
    resp->code = touchy_ResultCode_RESULT_OK;
    resp->which_payload = touchy_Response_sys_version_tag;
    touchy_SysVersionResponse *v = &resp->payload.sys_version;
    v->protocol_version = TOUCHY_PROTOCOL_VERSION;

    const esp_app_desc_t *app = esp_app_get_description();
    // firmware_version is a numeric build id; we don't yet bake one into
    // CMake, so synthesise a small hash of the version string for now.
    uint32_t hash = 0;
    for (const char *p = app->version; *p; ++p) {
        hash = hash * 131u + (uint8_t)*p;
    }
    v->firmware_version = hash;
    strncpy(v->firmware_version_str, app->version,
            sizeof(v->firmware_version_str) - 1);
    v->firmware_version_str[sizeof(v->firmware_version_str) - 1] = '\0';
}

static void dispatch(const touchy_Command *cmd, touchy_Response *resp)
{
    // Default: every unimplemented command returns NOT_SUPPORTED. Stages
    // 14+ will fill these in.
    resp->code          = touchy_ResultCode_RESULT_NOT_SUPPORTED;
    resp->which_payload = 0;

    switch (cmd->which_cmd) {
    case touchy_Command_sys_version_get_tag:
        fill_sys_version(resp);
        break;

    case touchy_Command_file_reset_tag:
        // Wipe everything the host previously uploaded. Device-local prefs
        // under /littlefs/prefs are intentionally preserved.
        if (Fs::instance().removeTree("from_host")) {
            screens_clear();
            resp->code = touchy_ResultCode_RESULT_OK;
        } else {
            resp->code = touchy_ResultCode_RESULT_IO_ERROR;
        }
        break;

    case touchy_Command_file_save_tag: {
        const auto &fs_cmd = cmd->cmd.file_save;
        // Host paths land under /littlefs/from_host/<path> so they can't
        // collide with on-device preferences or other future namespaces.
        std::string path = std::string("from_host/") + fs_cmd.path;
        if (Fs::instance().writeFile(path,
                                     fs_cmd.data.bytes,
                                     fs_cmd.data.size)) {
            // Let the screen registry pick up "screens/*.pb" uploads. The
            // registry reads back through Fs, so the post-write hook is
            // the right place rather than passing bytes around.
            screens_register_from_file(fs_cmd.path);
            resp->code = touchy_ResultCode_RESULT_OK;
        } else {
            resp->code = touchy_ResultCode_RESULT_IO_ERROR;
        }
        break;
    }

    case touchy_Command_screen_load_tag:
        resp->code = screens_load(cmd->cmd.screen_load.name)
                         ? touchy_ResultCode_RESULT_OK
                         : touchy_ResultCode_RESULT_NOT_FOUND;
        break;

    case touchy_Command_screen_wake_tag:
    case touchy_Command_screen_sleep_timeout_tag:
    case touchy_Command_event_consume_tag:
    case touchy_Command_sys_reboot_bootloader_tag:
        ESP_LOGW(TAG, "command tag %u not yet implemented",
                 (unsigned)cmd->which_cmd);
        break;

    default:
        ESP_LOGW(TAG, "unknown command tag %u", (unsigned)cmd->which_cmd);
        resp->code = touchy_ResultCode_RESULT_INVALID_ARG;
        break;
    }
}

// ---------------------------------------------------------------------------
// Dispatcher task
// ---------------------------------------------------------------------------

static void host_api_task(void *)
{
    ESP_LOGI(TAG, "host_api dispatcher started");

    for (;;) {
        // Wait until we're enumerated AND there are bytes to read. The
        // rx-cb fires on every chunk; we wake up and try to assemble one
        // frame at a time.
        if (!tud_mounted()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Read the 4-byte length prefix.
        uint8_t hdr[4];
        if (!vendor_read_exact(hdr, sizeof(hdr))) continue;
        uint32_t payload_len = (uint32_t)hdr[0]
                             | ((uint32_t)hdr[1] << 8)
                             | ((uint32_t)hdr[2] << 16)
                             | ((uint32_t)hdr[3] << 24);
        if (payload_len > sizeof(s_rx_buf)) {
            ESP_LOGE(TAG, "frame too large (%u bytes), dropping", (unsigned)payload_len);
            // Drain payload to resync. Can't selectively skip from
            // tud_vendor_read so just read into the buffer in chunks and
            // toss them.
            size_t left = payload_len;
            while (left) {
                size_t chunk = left > sizeof(s_rx_buf) ? sizeof(s_rx_buf) : left;
                if (!vendor_read_exact(s_rx_buf, chunk)) break;
                left -= chunk;
            }
            continue;
        }
        if (!vendor_read_exact(s_rx_buf, payload_len)) continue;

        // Decode. `touchy_Command` embeds a 32 KB FileSaveCmd byte buffer
        // inline, so we keep it in .bss instead of on the dispatcher task's
        // 8 KB stack to avoid blowing the stack on every file upload. Same
        // for the response struct (which embeds SysVersionResponse strings
        // — small today, but cheap to keep static for symmetry).
        static touchy_Command cmd;
        static touchy_Response resp;
        cmd  = touchy_Command_init_zero;
        resp = touchy_Response_init_zero;

        pb_istream_t in = pb_istream_from_buffer(s_rx_buf, payload_len);
        if (!pb_decode(&in, touchy_Command_fields, &cmd)) {
            ESP_LOGE(TAG, "pb_decode failed: %s", PB_GET_ERROR(&in));
            resp.code = touchy_ResultCode_RESULT_INVALID_ARG;
            pb_ostream_t out = pb_ostream_from_buffer(s_tx_buf, sizeof(s_tx_buf));
            if (pb_encode(&out, touchy_Response_fields, &resp)) {
                vendor_write_frame(s_tx_buf, out.bytes_written);
            }
            continue;
        }

        // Dispatch.
        dispatch(&cmd, &resp);

        // Encode + send.
        pb_ostream_t out = pb_ostream_from_buffer(s_tx_buf, sizeof(s_tx_buf));
        if (!pb_encode(&out, touchy_Response_fields, &resp)) {
            ESP_LOGE(TAG, "pb_encode failed: %s", PB_GET_ERROR(&out));
            continue;
        }
        if (!vendor_write_frame(s_tx_buf, out.bytes_written)) {
            ESP_LOGW(TAG, "vendor_write_frame failed (host gone?)");
        }
    }
}

extern "C" void host_api_start(void)
{
    if (s_task) return;
    s_rx_sem = xSemaphoreCreateBinary();
    // 8 KB stack: pb_decode of an ImageSaveCmd uses static buffers (the
    // generated struct is heap/.bss), so this is mostly for nanopb's small
    // call stack. Pin to APP CPU to keep TinyUSB on the PRO CPU.
    xTaskCreatePinnedToCore(host_api_task, "host_api", 8 * 1024,
                            nullptr, tskIDLE_PRIORITY + 4, &s_task, 1);
}

extern "C" void host_api_on_rx(void)
{
    if (s_rx_sem) {
        BaseType_t hpw = pdFALSE;
        xSemaphoreGiveFromISR(s_rx_sem, &hpw);
        portYIELD_FROM_ISR(hpw);
    }
}
