// SPDX-License-Identifier: GPL-3.0-or-later
//
// Touchy-Pad host_api dispatcher — see host_api.h.

#include "host_api.h"

#include "backlight.h"
#include "display.h"
#include "fs.h"
#include "log_proto.h"
#include "protobuf.h"
#include "screens.h"
#include "touchy.pb.h"
#include "usb_hid.h"
#include "lvgl.h"
#include "pb_decode.h"
#include "pb_encode.h"

#include "version.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tusb.h"

#include <string.h>
#include <string>

static const char *TAG = "host_api";

// Wire protocol version. Bump when the on-the-wire framing or any oneof
// numbering changes in a way that breaks existing host clients.
// v1: stage 13 baseline (SysVersionGet + file/screen ops).
// v2: stage 16 — Action repeated + ActionHost/ActionMacro variants,
//     LvEvent.host_code field added; events delivered via host polling
//     of EventConsumeCmd (no dedicated interrupt-IN mailbox endpoint).
#define TOUCHY_PROTOCOL_VERSION  touchy_SysBoardInfoResponse_ProtocolVersion_CURRENT

// Largest serialised Command we accept. With streaming writes (Stage 51)
// every individual `FileWriteCmd` carries at most ~4 KiB of payload, so
// the on-wire frame cap shrinks from the old 32 KiB FileSave budget down
// to roughly one USB-HS bulk transfer's worth of data plus nanopb
// framing overhead.
#define HOST_API_RX_MAX  (5 * 1024)
#define HOST_API_TX_MAX  (4  * 1024)

static SemaphoreHandle_t s_rx_sem;            // signalled by tud_vendor_rx_cb
static TaskHandle_t      s_task;
static uint8_t           s_rx_buf[HOST_API_RX_MAX];
static uint8_t           s_tx_buf[HOST_API_TX_MAX];

// ---------------------------------------------------------------------------
// Stage 16 — event queue.
//
// Widgets with an `ActionHost` event_cb push fully-formed `touchy_LvEvent`
// values onto this queue; the host drains them by polling repeatedly with
// `EventConsumeCmd` on the vendor bulk endpoint pair. The ESP32-S3 USB-OTG
// controller only supports 5 concurrent IN endpoints (EP0 + CDC notif +
// CDC bulk-IN + HID + vendor bulk-IN), so there is no budget for a
// dedicated interrupt-IN "event ready" mailbox.
// ---------------------------------------------------------------------------

#define EVENT_QUEUE_DEPTH  16

static QueueHandle_t s_evt_queue;

// Path of the file currently being streamed via FileOpenWrite/Write/Close.
// We capture it on open so the close handler can pass the freshly-
// committed file to screens_register_from_file() without the host
// having to issue a separate registration command. Empty when no
// transaction is in flight. Only one transaction is permitted system-
// wide (see fs.cpp) so a single static string is sufficient.
static std::string s_active_write_path;

extern "C" void host_api_post_event(const struct _touchy_LvEvent *evt)
{
    if (!s_evt_queue || !evt) return;
    touchy_LvEvent copy = *evt;
    // Drop the oldest event when the queue is full so a busy widget can
    // never wedge the dispatcher.
    if (xQueueSend(s_evt_queue, &copy, 0) != pdTRUE) {
        touchy_LvEvent discard;
        xQueueReceive(s_evt_queue, &discard, 0);
        xQueueSend(s_evt_queue, &copy, 0);
        ESP_LOGW(TAG, "event queue overflow, oldest discarded");
    }
}

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

static void fill_board_info(touchy_Response *resp)
{
    resp->code = touchy_ResultCode_RESULT_OK;
    resp->which_payload = touchy_Response_sys_board_info_tag;
    touchy_SysBoardInfoResponse *v = &resp->payload.sys_board_info;
    v->protocol_version = TOUCHY_PROTOCOL_VERSION;

    v->firmware_version = FIRMWARE_BUILD_NUMBER;
    strncpy(v->firmware_version_str, FIRMWARE_VERSION_STR,
            sizeof(v->firmware_version_str) - 1);
    v->firmware_version_str[sizeof(v->firmware_version_str) - 1] = '\0';
    strncpy(v->board_name, TOUCHY_BOARD_NAME, sizeof(v->board_name) - 1);
    v->board_name[sizeof(v->board_name) - 1] = '\0';

    // Active display panel resolution. Read from the default LVGL
    // display so we automatically pick up whatever the board driver
    // registered (no per-board #ifdef here). Zero means no display is
    // currently registered — host adapters treat that as unknown.
    lv_display_t *disp = lv_display_get_default();
    v->display_width  = disp ? (uint32_t)lv_display_get_horizontal_resolution(disp) : 0;
    v->display_height = disp ? (uint32_t)lv_display_get_vertical_resolution(disp)   : 0;
}

static void dispatch(const touchy_Command *cmd, touchy_Response *resp)
{
    // Default: every unimplemented command returns NOT_SUPPORTED. Stages
    // 14+ will fill these in.
    resp->code          = touchy_ResultCode_RESULT_NOT_SUPPORTED;
    resp->which_payload = 0;

    switch (cmd->which_cmd) {
    case touchy_Command_sys_board_info_get_tag:
        fill_board_info(resp);
        break;

    case touchy_Command_file_delete_tag: {
        // FileDelete subsumes the old FileReset/FileSave wipe: passing
        // "F:host" deletes the whole host directory; passing a single
        // file path deletes just that file. Drive-letter prefix is
        // mandatory.
        const char  *path = cmd->cmd.file_delete.path;
        std::string  rest;
        Fs *fs = fs_resolve(path, &rest);
        if (!fs) {
            resp->code = touchy_ResultCode_RESULT_INVALID_ARG;
            break;
        }
        // Try as a tree first; FlashFs::removeTree falls back to a
        // file unlink when the path doesn't refer to a directory.
        bool ok = fs->removeTree(rest);
        screens_clear();   // any cached screen objects may now be stale
        resp->code = ok ? touchy_ResultCode_RESULT_OK
                        : touchy_ResultCode_RESULT_IO_ERROR;
        break;
    }

    case touchy_Command_file_open_write_tag: {
        const char *path = cmd->cmd.file_open_write.path;
        uint32_t handle = fs_open_write(path);
        if (handle == 0) {
            resp->code = touchy_ResultCode_RESULT_IO_ERROR;
            s_active_write_path.clear();
        } else {
            resp->code          = touchy_ResultCode_RESULT_OK;
            resp->which_payload = touchy_Response_file_open_write_tag;
            resp->payload.file_open_write.handle = handle;
            s_active_write_path = path ? path : "";
        }
        break;
    }

    case touchy_Command_file_write_tag: {
        const auto &fw = cmd->cmd.file_write;
        const uint8_t *bytes = fw.data ? fw.data->bytes : nullptr;
        size_t         nbytes = fw.data ? fw.data->size  : 0;
        bool ok = fs_append_write(fw.handle, bytes, nbytes);
        resp->code = ok ? touchy_ResultCode_RESULT_OK
                        : touchy_ResultCode_RESULT_IO_ERROR;
        break;
    }

    case touchy_Command_file_close_tag: {
        const auto &fc = cmd->cmd.file_close;
        bool ok = fs_close_write(fc.handle, fc.commit);
        if (ok && fc.commit && !s_active_write_path.empty()) {
            // Hand the freshly-committed file to the screen registry;
            // it's a no-op for anything outside `*:host/screens/*.pb`.
            screens_register_from_file(s_active_write_path.c_str());
            // Stage 55: poke the active screen so any widget that
            // references this path picks up the new bytes. Cheap when
            // nothing references it (a path-comparison walk over the
            // currently-displayed widget tree), expensive — one full
            // screen reload — when something does.
            screens_notify_file_changed(s_active_write_path.c_str());
        }
        s_active_write_path.clear();
        resp->code = ok ? touchy_ResultCode_RESULT_OK
                        : touchy_ResultCode_RESULT_IO_ERROR;
        break;
    }

    case touchy_Command_screen_load_tag:
        resp->code = screens_load(cmd->cmd.screen_load.path)
                         ? touchy_ResultCode_RESULT_OK
                         : touchy_ResultCode_RESULT_NOT_FOUND;
        break;

    case touchy_Command_event_consume_tag: {
        touchy_LvEvent evt;
        if (s_evt_queue && xQueueReceive(s_evt_queue, &evt, 0) == pdTRUE) {
            resp->code          = touchy_ResultCode_RESULT_OK;
            resp->which_payload = touchy_Response_event_consume_tag;
            resp->payload.event_consume.has_event = true;
            resp->payload.event_consume.event     = evt;
            break;
        }
        // Stage 64.1: the host's existing event-poll drains tunneled
        // log records too. Events win on ties so a busy UI never
        // starves log delivery, but a quiet UI lets logs flow out
        // promptly.
        touchy_LogRecord rec;
        if (log_proto_pop(&rec)) {
            resp->code          = touchy_ResultCode_RESULT_OK;
            resp->which_payload = touchy_Response_log_record_tag;
            resp->payload.log_record = rec;
            break;
        }
        // Both queues empty — host will back off on this code.
        resp->code = touchy_ResultCode_RESULT_NOT_FOUND;
        break;
    }

    case touchy_Command_screen_wake_tag:
        backlight_wake();
        resp->code = touchy_ResultCode_RESULT_OK;
        break;

    case touchy_Command_screen_sleep_timeout_tag:
        backlight_set_timeout(cmd->cmd.screen_sleep_timeout.timeout_ms);
        resp->code = touchy_ResultCode_RESULT_OK;
        break;

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

        // Stage 51: PbMessage owns any heap-allocated FT_POINTER fields
        // (currently just `FileWriteCmd.data`) and calls pb_release() in
        // its destructor at the bottom of the loop. The structs
        // themselves remain on the dispatcher task's stack — they're
        // small now (~few hundred bytes) since the file payload no
        // longer lives inline.
        PbMessage<touchy_Command>  cmd(touchy_Command_fields);
        PbMessage<touchy_Response> resp(touchy_Response_fields);

        if (!cmd.decode(s_rx_buf, payload_len)) {
            ESP_LOGE(TAG, "pb_decode failed");
            resp->code = touchy_ResultCode_RESULT_INVALID_ARG;
            std::size_t n = 0;
            if (resp.encode(s_tx_buf, sizeof(s_tx_buf), &n)) {
                vendor_write_frame(s_tx_buf, n);
            }
            continue;
        }

        // Dispatch.
        dispatch(cmd.get(), resp.get());

        // Encode + send.
        std::size_t n = 0;
        if (!resp.encode(s_tx_buf, sizeof(s_tx_buf), &n)) {
            ESP_LOGE(TAG, "pb_encode failed");
            continue;
        }
        if (!vendor_write_frame(s_tx_buf, n)) {
            ESP_LOGW(TAG, "vendor_write_frame failed (host gone?)");
        }
    }
}

extern "C" void host_api_start(void)
{
    if (s_task) return;
    s_rx_sem = xSemaphoreCreateBinary();
    s_evt_queue = xQueueCreate(EVENT_QUEUE_DEPTH, sizeof(touchy_LvEvent));
    // 8 KB stack: pb_decode of an ImageSaveCmd uses static buffers (the
    // generated struct is heap/.bss), so this is mostly for nanopb's small
    // call stack. Pin to APP CPU to keep TinyUSB on the PRO CPU.
    xTaskCreatePinnedToCore(host_api_task, "host_api", 10 * 1024,
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
