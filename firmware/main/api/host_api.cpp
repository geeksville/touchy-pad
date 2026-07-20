// SPDX-License-Identifier: GPL-3.0-or-later
//
// Touchy-Pad host_api dispatcher — see host_api.h.

#include "host_api.h"
#include "tc_tag.h"

#include "backlight.h"
#include "display.h"
#include "fs.h"
#include "flash_fs.h"
#include "log_proto.h"
#include "protobuf.h"
#include "platform.h"
#include "prefs.h"
#include "screens.h"
#include "touchy.pb.h"
#include "usb_hid.h"
#include "widgets/widget_actions.h"
#include "widgets/widget_property.h"
#include "lvgl.h"
#include "pb_decode.h"
#include "pb_encode.h"

// Stage LB5 — per-transport link classes now live in their own files.
#include "host_api_link.h"
#include "serial_link.h"
#include "uart_link.h"
#include "vendor_link.h"

#include "version.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <atomic>
#include <string.h>
#include <string>

static const char *TAG = TOUCHY_TAG("host_api");

// Wire protocol version. Bump when the on-the-wire framing or any oneof
// numbering changes in a way that breaks existing host clients.
// v1: stage 13 baseline (SysVersionGet + file/screen ops).
// v2: stage 16 — Action repeated + ActionHost/ActionMacro variants,
//     LvEvent.host_code field added; events delivered via host polling
//     of EventConsumeCmd (no dedicated interrupt-IN mailbox endpoint).
#define TOUCHY_PROTOCOL_VERSION  touchy_SysBoardInfoResponse_ProtocolVersion_CURRENT

// HOST_API_RX_MAX / HOST_API_TX_MAX and the HostApiLink base live in
// host_api_link.h (shared with the per-transport link .cpp files).


// ---------------------------------------------------------------------------
// Stage 64.3 — self-synchronising frame.
//
// Every transport (USB vendor bulk, TCP simulator, serial port) carries
// frames shaped as:
//
//     MAGIC(2) | LEN(uint16 LE) | payload | CRC8(1)
//
// MAGIC = 0xA5 0x5A is a resync anchor; CRC8 (poly 0x07, init 0x00) over
// `LEN || payload` lets a reader reject corruption and re-acquire frame
// alignment. See docs/design.md stage 64.3 and proto/touchy.proto.
// ---------------------------------------------------------------------------

static const uint8_t FRAME_MAGIC0 = 0xA5;
static const uint8_t FRAME_MAGIC1 = 0x5A;

static uint8_t crc8_update(uint8_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}


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
// Transport links (Stage 64.3, generalised to an array in Stage LB5)
//
// A HostApiLink (host_api_link.h) abstracts the byte stream the dispatcher
// reads Commands from / writes Responses to. The concrete subclasses live
// in their own files: VendorLink (vendor_link.cpp, USB vendor bulk),
// SerialLink (serial_link.cpp, USB-CDC ACM) and UartLink (uart_link.cpp,
// hardware UART). Each is gated by its own CONFIG_TOUCHY_PROTO_OVER_* flag
// AND the backing hardware, so a board only builds the links it can use.
// host_api_start() registers every available link into s_links[] and spawns
// one dispatcher task per link.
//
// "Last used wins" (Stage LB5): responses are always written back on the
// link their Command arrived on, so request/response routes per-link for
// free. Events are drained by EventConsumeCmd polling, so they too flow to
// whichever link the host is currently polling. s_active_link records the
// most-recently-used link — the anchor for any future unsolicited push
// (e.g. a TCPLink event mailbox); under the one-active-client assumption it
// is otherwise advisory.
// ---------------------------------------------------------------------------

static std::atomic<HostApiLink *> s_active_link{nullptr};


// ---------------------------------------------------------------------------
// Framing helpers (blocking on the dispatcher task)
// ---------------------------------------------------------------------------

// Read exactly `want` bytes from `link`. Returns false if the link drops
// mid-frame; the caller then restarts magic-scanning when it reconnects.
static bool link_read_exact(HostApiLink *link, uint8_t *dst, size_t want)
{
    size_t got = 0;
    while (got < want) {
        size_t n = link->read_some(dst + got, want - got);
        if (n > 0) {
            got += n;
            continue;
        }
        if (!link->connected()) return false;
        link->wait_rx(1000);
    }
    return true;
}

// Scan the byte stream until the two-byte MAGIC is seen. Returns false if
// the link drops while scanning.
static bool link_sync_magic(HostApiLink *link)
{
    int state = 0;  // 0 = awaiting MAGIC0, 1 = awaiting MAGIC1
    uint8_t b;
    for (;;) {
        if (!link_read_exact(link, &b, 1)) return false;
        if (state == 0) {
            if (b == FRAME_MAGIC0) state = 1;
        } else {
            if (b == FRAME_MAGIC1) return true;
            // Allow a run of MAGIC0 bytes to keep us armed.
            state = (b == FRAME_MAGIC0) ? 1 : 0;
        }
    }
}

// Read one validated frame's payload into link->rx_buf. Resyncs past
// oversize-length and CRC errors by rescanning for the next MAGIC. The
// device reads from a reliable USB/UART FIFO so corruption is rare; the
// magic+CRC mainly let the *host* resync against a freshly-booted or
// noisy device. Returns false only if the link drops.
static bool read_frame(HostApiLink *link, size_t *out_len)
{
    for (;;) {
        if (!link_sync_magic(link)) return false;
        uint8_t lenb[2];
        if (!link_read_exact(link, lenb, 2)) return false;
        uint16_t len = (uint16_t)lenb[0] | ((uint16_t)lenb[1] << 8);
        if (len > HOST_API_RX_MAX) {
            ESP_LOGE(TAG, "%s: frame too large (%u bytes), resync",
                     link->name(), (unsigned)len);
            continue;
        }
        if (!link_read_exact(link, link->rx_buf, len)) return false;
        uint8_t crc;
        if (!link_read_exact(link, &crc, 1)) return false;
        uint8_t calc = crc8_update(0x00, lenb, 2);
        calc = crc8_update(calc, link->rx_buf, len);
        if (calc != crc) {
            ESP_LOGW(TAG, "%s: CRC mismatch (got 0x%02x want 0x%02x), resync",
                     link->name(), crc, calc);
            continue;
        }
        *out_len = len;
        return true;
    }
}

static bool write_frame(HostApiLink *link, const uint8_t *payload, size_t len)
{
    uint8_t hdr[4] = {
        FRAME_MAGIC0,
        FRAME_MAGIC1,
        (uint8_t)(len & 0xFF),
        (uint8_t)((len >> 8) & 0xFF),
    };
    uint8_t crc = crc8_update(0x00, hdr + 2, 2);
    crc = crc8_update(crc, payload, len);
    if (!link->write_all(hdr, sizeof(hdr))) return false;
    if (len && !link->write_all(payload, len)) return false;
    if (!link->write_all(&crc, 1)) return false;
    link->flush();
    return true;
}


// ---------------------------------------------------------------------------
// Command handlers
// ---------------------------------------------------------------------------

static void fill_board_info(touchy_Response *resp)
{
    resp->code = touchy_ResultCode_OK;
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

    // Stage 65 capability flags, sourced from the board's platform descriptor.
    const Platform *plat = platform_get();
    v->is_multitouch = plat->is_multitouch;
    v->has_usb       = plat->has_usb;

    // Stage 71: stable MAC-derived serial (matches USB iSerialNumber).
    strncpy(v->serial, platform_serial(), sizeof(v->serial) - 1);
    v->serial[sizeof(v->serial) - 1] = '\0';

    // Stage 81: runtime memory / storage headroom (snapshot at query time).
    v->free_heap_bytes  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    v->free_psram_bytes = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t fs_total = 0, fs_used = 0;
    if (FlashFs::instance().usage(&fs_total, &fs_used)) {
        v->fs_total_bytes = fs_total;
        v->fs_used_bytes  = fs_used;
    }

    // Stage 87: advisory hint — is the transient 'T:' drive flash-backed
    // (no-PSRAM board) rather than a PSRAM ramdisk? Host writers of
    // throwaway assets use this to throttle high-frequency refreshes.
    v->temp_is_flash = temp_is_flash();

    // Stage LB1: does the board have a touch panel at all? Defaults true
    // via the weak platform_is_touchable(); touch-less boards override it.
    v->is_touchable = platform_is_touchable();
}

// Run one already-decoded Command and fill `resp`. Public seam (declared
// in host_api.h) shared by the byte-stream dispatcher task, the bare
// serialized entry point, and the JSON path in net/http_api.cpp.
void host_api_dispatch_message(const touchy_Command *cmd, touchy_Response *resp)
{
    // Default: every unimplemented command returns NOT_SUPPORTED. Stages
    // 14+ will fill these in.
    resp->code          = touchy_ResultCode_NOT_SUPPORTED;
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
            resp->code = touchy_ResultCode_INVALID_ARG;
            break;
        }
        // Try as a tree first; FlashFs::removeTree falls back to a
        // file unlink when the path doesn't refer to a directory.
        bool ok = fs->removeTree(rest);
        // Stage 85: drop only the registered screens at/under the
        // deleted path rather than wiping the whole registry. Deleting
        // an unrelated tree (e.g. the `R:host/icache/` image cache)
        // must not unregister the active screen, or its auto-reload
        // after a later stub write would fail with "not registered".
        screens_notify_path_deleted(path);
        resp->code = ok ? touchy_ResultCode_OK
                        : touchy_ResultCode_IO_ERROR;
        break;
    }

    case touchy_Command_file_open_write_tag: {
        const char *path = cmd->cmd.file_open_write.path;
        uint32_t handle = fs_open_write(path);
        if (handle == 0) {
            resp->code = touchy_ResultCode_IO_ERROR;
            s_active_write_path.clear();
        } else {
            resp->code          = touchy_ResultCode_OK;
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
        resp->code = ok ? touchy_ResultCode_OK
                        : touchy_ResultCode_IO_ERROR;
        break;
    }

    case touchy_Command_file_close_tag: {
        const auto &fc = cmd->cmd.file_close;
        // Stage 80: a flash commit renames the temp file over the
        // destination. If an animated GIF on the active screen is still
        // rendering that destination, its decoder holds the file open
        // and the rename fails with EBUSY. Release that handle first;
        // the screens_notify_file_changed() below re-applies the source.
        ESP_LOGD(TAG, "file_close: handle=%u commit=%d path='%s'",
                 (unsigned)fc.handle, (int)fc.commit,
                 s_active_write_path.c_str());
        int64_t t0 = esp_timer_get_time();
        if (fc.commit && !s_active_write_path.empty()) {
            screens_prepare_file_overwrite(s_active_write_path.c_str());
        }
        int64_t t_prep = esp_timer_get_time();
        bool ok = fs_close_write(fc.handle, fc.commit);
        int64_t t_close = esp_timer_get_time();
        ESP_LOGD(TAG, "file_close: fs_close_write -> %d (path='%s')",
                 (int)ok, s_active_write_path.c_str());
        if (ok && fc.commit && !s_active_write_path.empty()) {
            // Hand the freshly-committed file to the screen registry;
            // it's a no-op for anything outside `*:host/s/*.pb`.
            screens_register_from_file(s_active_write_path.c_str());
            int64_t t_reg = esp_timer_get_time();
            // Stage 55: poke the active screen so any widget that
            // references this path picks up the new bytes. Cheap when
            // nothing references it (a path-comparison walk over the
            // currently-displayed widget tree), expensive — one full
            // screen reload — when something does. The reload is
            // deferred onto the LVGL task (lv_async_call), so this
            // returns immediately and the host_api task stays free to
            // service event_consume polls (draining the log tunnel)
            // while the rebuild runs.
            screens_notify_file_changed(s_active_write_path.c_str());
            int64_t t_notify = esp_timer_get_time();
            ESP_LOGD(TAG, "file_close: notify scheduled for '%s'",
                     s_active_write_path.c_str());
            // Surface slow commits: anything near the host's 2 s RPC
            // timeout is a prime suspect for the intermittent failures.
            int64_t total_us = t_notify - t0;
            if (total_us >= 200000) {
                ESP_LOGW(TAG,
                         "file_close SLOW '%s': total=%lldms (prep=%lld close=%lld reg=%lld notify=%lld)",
                         s_active_write_path.c_str(), total_us / 1000,
                         (t_prep - t0) / 1000, (t_close - t_prep) / 1000,
                         (t_reg - t_close) / 1000, (t_notify - t_reg) / 1000);
            }
        }
        s_active_write_path.clear();
        resp->code = ok ? touchy_ResultCode_OK
                        : touchy_ResultCode_IO_ERROR;
        break;
    }

    case touchy_Command_event_consume_tag: {
        touchy_LvEvent evt;
        if (s_evt_queue && xQueueReceive(s_evt_queue, &evt, 0) == pdTRUE) {
            resp->code          = touchy_ResultCode_OK;
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
            resp->code          = touchy_ResultCode_OK;
            resp->which_payload = touchy_Response_log_record_tag;
            resp->payload.log_record = rec;
            break;
        }
        // Both queues empty — host will back off on this code.
        resp->code = touchy_ResultCode_NOT_FOUND;
        break;
    }

    case touchy_Command_screen_wake_tag:
        backlight_wake();
        resp->code = touchy_ResultCode_OK;
        break;

    case touchy_Command_set_preferences_tag:
        // Stage 82 — apply a partial preferences update. The device merges
        // only the present fields and fires each one's side effect
        // (backlight timeout, screen switch, log threshold). Returns
        // RESULT_NOT_FOUND if a requested current_screen can't be loaded.
        resp->code = Prefs::instance().apply_partial(cmd->cmd.set_preferences.prefs)
                         ? touchy_ResultCode_OK
                         : touchy_ResultCode_NOT_FOUND;
        break;

    case touchy_Command_get_preferences_tag: {
        // Stage LB4 — return the device's current, merged preferences.
        // file_version is device-owned and always set so the host can use
        // its presence as a validity canary.
        resp->which_payload = touchy_Response_preferences_read_tag;
        resp->payload.preferences_read.has_prefs = true;
        resp->payload.preferences_read.prefs = Prefs::instance().to_proto();
        resp->code = touchy_ResultCode_OK;
        break;
    }

    case touchy_Command_run_actions_tag: {
        // Stage 71 — run a host-supplied list of Actions as if a local
        // widget had fired them. `actions` is FT_POINTER (heap), so its
        // pointer/count live in the repeated-field pair.
        const auto &ra = cmd->cmd.run_actions;
        widget_run_actions(ra.actions, ra.actions_count);
        resp->code = touchy_ResultCode_OK;
        break;
    }

    case touchy_Command_set_property_tag: {
        // Stage lb12 — override a single LVGL property on a named widget.
        // Stored as a sticky session override and applied now if the widget
        // is on screen; INVALID_ARG only when the command is malformed
        // (no property name/id) — an absent widget is not an error.
        resp->code = widget_property_set(cmd->cmd.set_property)
                         ? touchy_ResultCode_OK
                         : touchy_ResultCode_INVALID_ARG;
        break;
    }
    case touchy_Command_sys_reboot_bootloader_tag:
        ESP_LOGW(TAG, "command tag %u not yet implemented",
                 (unsigned)cmd->which_cmd);
        break;

    default:
        ESP_LOGW(TAG, "unknown command tag %u", (unsigned)cmd->which_cmd);
        resp->code = touchy_ResultCode_INVALID_ARG;
        break;
    }
}

// ---------------------------------------------------------------------------
// Stage lb8 — bare (unframed) dispatch entry point.
//
// Transports that already delimit the payload themselves (the HTTP(S)
// network API — HTTP Content-Length frames each body) hand us a raw
// serialized Command and want a raw serialized Response back, without the
// MAGIC | LEN | CRC8 wrapper the byte-stream links use. This shares the
// exact same `dispatch()` core as host_api_task().
// ---------------------------------------------------------------------------

bool host_api_dispatch_serialized(const uint8_t *in, size_t in_len,
                                  uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (out_len) *out_len = 0;

    PbMessage<touchy_Command>  cmd(touchy_Command_fields);
    PbMessage<touchy_Response> resp(touchy_Response_fields);

    if (!cmd.decode(in, in_len)) {
        ESP_LOGE(TAG, "http: pb_decode failed");
        resp->code = touchy_ResultCode_INVALID_ARG;
    } else {
        host_api_dispatch_message(cmd.get(), resp.get());
    }

    std::size_t n = 0;
    if (!resp.encode(out, out_cap, &n)) {
        ESP_LOGE(TAG, "http: pb_encode failed");
        return false;
    }
    if (out_len) *out_len = n;
    return true;
}

// ---------------------------------------------------------------------------
// Dispatcher task
// ---------------------------------------------------------------------------

static void host_api_task(void *arg)
{
    HostApiLink *link = static_cast<HostApiLink *>(arg);
    ESP_LOGI(TAG, "host_api dispatcher started (%s)", link->name());

    bool was_connected = false;
    for (;;) {
        // Wait until the link is up before trying to read a frame.
        bool now_connected = link->connected();
        if (!now_connected) {
            if (was_connected) {
                // Host just disconnected — abort any in-progress write so the
                // next session starts clean.
                fs_abort_open_transaction();
            }
            was_connected = false;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        was_connected = true;

        // Read one validated, self-synchronising frame into link->rx_buf.
        size_t payload_len = 0;
        if (!read_frame(link, &payload_len)) {
            // read_frame returns false only when the link drops mid-frame.
            fs_abort_open_transaction();
            continue;
        }

        // Stage 51: PbMessage owns any heap-allocated FT_POINTER fields
        // (currently just `FileWriteCmd.data`) and calls pb_release() in
        // its destructor at the bottom of the loop. The structs
        // themselves remain on the dispatcher task's stack — they're
        // small now (~few hundred bytes) since the file payload no
        // longer lives inline.
        PbMessage<touchy_Command>  cmd(touchy_Command_fields);
        PbMessage<touchy_Response> resp(touchy_Response_fields);

        if (!cmd.decode(link->rx_buf, payload_len)) {
            ESP_LOGE(TAG, "pb_decode failed");
            resp->code = touchy_ResultCode_INVALID_ARG;
            std::size_t n = 0;
            if (resp.encode(link->tx_buf, HOST_API_TX_MAX, &n)) {
                write_frame(link, link->tx_buf, n);
            }
            continue;
        }

        // Dispatch.
        host_api_dispatch_message(cmd.get(), resp.get());

        // Stage LB5 — this link just carried a Command, so it is now the
        // active client. Records the anchor for any future unsolicited push
        // (see s_active_link); responses/events already route per-link.
        s_active_link.store(link, std::memory_order_relaxed);

        // Encode + send.
        std::size_t n = 0;
        if (!resp.encode(link->tx_buf, HOST_API_TX_MAX, &n)) {
            ESP_LOGE(TAG, "pb_encode failed");
            continue;
        }
        if (!write_frame(link, link->tx_buf, n)) {
            ESP_LOGW(TAG, "%s: write_frame failed (host gone?)", link->name());
        }
    }
}

// ---------------------------------------------------------------------------
// Link registry + startup (Stage LB5)
// ---------------------------------------------------------------------------

// Every transport link the build enabled *and* the board has hardware for.
// Capacity 3 = vendor + cdcacm + uart (the current maximum).
static HostApiLink *s_links[3];
static size_t       s_link_count = 0;

// Create the link's wake semaphore, run its one-time driver bring-up, and
// spawn its dispatcher task. No-op on nullptr / registry overflow.
static void register_link(HostApiLink *link)
{
    if (!link || s_link_count >= (sizeof(s_links) / sizeof(s_links[0]))) return;
    link->rx_sem = xSemaphoreCreateBinary();
    if (!link->init()) {
        ESP_LOGE(TAG, "link '%s' init failed — skipping", link->name());
        return;
    }
    s_links[s_link_count++] = link;
    // 10 KB stack: pb_decode uses static buffers (the generated struct is
    // heap/.bss), so this is mostly for nanopb's small call stack. Pin to
    // APP CPU to keep TinyUSB on the PRO CPU.
    xTaskCreatePinnedToCore(host_api_task, link->name(), 10 * 1024, link,
                            tskIDLE_PRIORITY + 4, nullptr, 1);
}

extern "C" void host_api_start(void)
{
    static bool started = false;
    if (started) return;
    started = true;

    s_evt_queue = xQueueCreate(EVENT_QUEUE_DEPTH, sizeof(touchy_LvEvent));

#if CONFIG_TOUCHY_PROTO_OVER_VENDORUSB && CONFIG_SOC_USB_OTG_SUPPORTED
    register_link(vendor_link_instance());
#endif
#if CONFIG_TOUCHY_PROTO_OVER_CDCACM && CONFIG_SOC_USB_OTG_SUPPORTED && CONFIG_TINYUSB_CDC_COUNT
    register_link(serial_link_instance());
#endif
#if CONFIG_TOUCHY_PROTO_OVER_UART && CONFIG_TOUCHY_HAS_PROTO_UART
    register_link(uart_link_instance());
#endif

    if (s_link_count == 0) {
        ESP_LOGW(TAG, "no host-API transport links enabled for this board");
    }
}

#if CONFIG_TOUCHY_PROTO_OVER_VENDORUSB && CONFIG_SOC_USB_OTG_SUPPORTED
extern "C" void host_api_on_rx(void)
{
    vendor_link_instance()->wake_from_isr();
}
#else
extern "C" void host_api_on_rx(void) {}
#endif

#if CONFIG_TOUCHY_PROTO_OVER_CDCACM && CONFIG_SOC_USB_OTG_SUPPORTED && CONFIG_TINYUSB_CDC_COUNT
extern "C" void host_api_on_cdc_rx(void)
{
    serial_link_instance()->wake_from_isr();
}
#else
extern "C" void host_api_on_cdc_rx(void) {}
#endif


