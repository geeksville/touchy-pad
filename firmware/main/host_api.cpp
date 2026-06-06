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
#include "lvgl.h"
#include "pb_decode.h"
#include "pb_encode.h"

#include "version.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#if CONFIG_SOC_USB_OTG_SUPPORTED
#include "tinyusb.h"
#include "tusb.h"
#endif
#if CONFIG_TOUCHY_PROTO_OVER_SERIAL && (!CONFIG_SOC_USB_OTG_SUPPORTED || !CONFIG_TINYUSB_CDC_COUNT)
#include "driver/uart.h"
#endif

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

// Largest serialised Command we accept. With streaming writes (Stage 51)
// every individual `FileWriteCmd` carries at most ~4 KiB of payload, so
// the on-wire frame cap shrinks from the old 32 KiB FileSave budget down
// to roughly one USB-HS bulk transfer's worth of data plus nanopb
// framing overhead.
#define HOST_API_RX_MAX  (5 * 1024)
#define HOST_API_TX_MAX  (4  * 1024)

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
// Transport links (Stage 64.3)
//
// A HostApiLink abstracts the byte stream the dispatcher reads commands
// from / writes responses to. VendorLink rides the TinyUSB vendor bulk
// pair (native-USB chips only); SerialLink rides a USB-CDC ACM port; and
// UartLink rides a hardware UART on chips with no native USB at all. The
// serial links are compiled in only when CONFIG_TOUCHY_PROTO_OVER_SERIAL is
// set. Each link owns its own rx/tx scratch buffers and wake semaphore so
// the dispatcher tasks never alias state.
// ---------------------------------------------------------------------------

struct HostApiLink {
    SemaphoreHandle_t rx_sem = nullptr;
    uint8_t rx_buf[HOST_API_RX_MAX];
    uint8_t tx_buf[HOST_API_TX_MAX];

    virtual ~HostApiLink() {}
    virtual const char *name() const = 0;
    virtual bool connected() = 0;
    // Non-blocking: copy up to `max` available bytes into `dst`, return count.
    virtual size_t read_some(uint8_t *dst, size_t max) = 0;
    // Blocking write of exactly `n` bytes; false if the link drops.
    virtual bool write_all(const uint8_t *p, size_t n) = 0;
    virtual void flush() {}

    void wait_rx(uint32_t ms) {
        if (rx_sem) xSemaphoreTake(rx_sem, pdMS_TO_TICKS(ms));
        else        vTaskDelay(pdMS_TO_TICKS(ms));
    }
};

#if CONFIG_SOC_USB_OTG_SUPPORTED
struct VendorLink : HostApiLink {
    const char *name() const override { return "vendor"; }
    bool connected() override { return tud_mounted(); }
    size_t read_some(uint8_t *dst, size_t max) override {
        return tud_vendor_read(dst, max);
    }
    bool write_all(const uint8_t *p, size_t n) override {
        size_t sent = 0;
        while (sent < n) {
            uint32_t w = tud_vendor_write(p + sent, n - sent);
            if (w == 0) {
                if (!tud_mounted()) return false;
                tud_vendor_write_flush();
                vTaskDelay(1);
                continue;
            }
            sent += w;
        }
        return true;
    }
    void flush() override { tud_vendor_write_flush(); }
};

static VendorLink s_vendor_link;
#endif  // CONFIG_SOC_USB_OTG_SUPPORTED

#if CONFIG_TOUCHY_PROTO_OVER_SERIAL && CONFIG_SOC_USB_OTG_SUPPORTED && CONFIG_TINYUSB_CDC_COUNT
struct SerialLink : HostApiLink {
    const char *name() const override { return "serial"; }
    // tud_cdc_connected() tracks DTR; pyserial asserts it on open.
    bool connected() override { return tud_cdc_connected(); }
    size_t read_some(uint8_t *dst, size_t max) override {
        if (!tud_cdc_available()) return 0;
        return tud_cdc_read(dst, max);
    }
    bool write_all(const uint8_t *p, size_t n) override {
        size_t sent = 0;
        while (sent < n) {
            uint32_t w = tud_cdc_write(p + sent, n - sent);
            if (w == 0) {
                if (!tud_cdc_connected()) return false;
                tud_cdc_write_flush();
                vTaskDelay(1);
                continue;
            }
            sent += w;
        }
        return true;
    }
    void flush() override { tud_cdc_write_flush(); }
};

static SerialLink s_serial_link;
#endif  // CONFIG_TOUCHY_PROTO_OVER_SERIAL && CONFIG_SOC_USB_OTG_SUPPORTED && CONFIG_TINYUSB_CDC_COUNT

#if CONFIG_TOUCHY_PROTO_OVER_SERIAL && (!CONFIG_SOC_USB_OTG_SUPPORTED || !CONFIG_TINYUSB_CDC_COUNT)
// Hardware-UART transport for chips with no native USB (Stage 65). The
// CH340 bridge on boards like the esp32_2432s028rv3 is wired to UART0, so we
// reuse the console UART at the standard protocol baud. The IDF console must
// be routed off UART0 (CONFIG_ESP_CONSOLE_UART_NONE) on such boards so it
// doesn't interleave bytes with the protocol frames.
#ifndef HOST_API_UART_NUM
#define HOST_API_UART_NUM UART_NUM_0
#endif
#ifndef HOST_API_UART_BAUD
#define HOST_API_UART_BAUD 460800
#endif

struct UartLink : HostApiLink {
    QueueHandle_t evt_queue = nullptr;

    const char *name() const override { return "uart"; }
    // A UART has no link-up notion; the host is "connected" whenever bytes
    // can flow. Report true so the dispatcher always services frames.
    bool connected() override { return true; }
    size_t read_some(uint8_t *dst, size_t max) override {
        int n = uart_read_bytes(HOST_API_UART_NUM, dst, max, 0);
        return n > 0 ? (size_t)n : 0;
    }
    bool write_all(const uint8_t *p, size_t n) override {
        int w = uart_write_bytes(HOST_API_UART_NUM, (const char *)p, n);
        return w == (int)n;
    }
    void flush() override {
        uart_wait_tx_done(HOST_API_UART_NUM, pdMS_TO_TICKS(100));
    }
};

static UartLink s_uart_link;

// Pumps UART rx events into the link's wake semaphore so the dispatcher
// task blocks (rather than polling) while waiting for the next frame.
static void uart_rx_pump_task(void *arg)
{
    UartLink *link = (UartLink *)arg;
    uart_event_t evt;
    for (;;) {
        if (xQueueReceive(link->evt_queue, &evt, portMAX_DELAY)) {
            if (evt.type == UART_DATA && link->rx_sem) {
                xSemaphoreGive(link->rx_sem);
            }
        }
    }
}

// Install the UART driver + rx pump. Safe to call once from host_api_start().
static void uart_link_init(UartLink *link)
{
    uart_config_t cfg = {};
    cfg.baud_rate = HOST_API_UART_BAUD;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    ESP_ERROR_CHECK(uart_driver_install(HOST_API_UART_NUM, HOST_API_RX_MAX * 2,
                                        HOST_API_TX_MAX * 2, 16,
                                        &link->evt_queue, 0));
    ESP_ERROR_CHECK(uart_param_config(HOST_API_UART_NUM, &cfg));
    // Keep the default console pins for UART0; explicit boards may override.
    xTaskCreatePinnedToCore(uart_rx_pump_task, "uart_rx", 2048, link,
                            tskIDLE_PRIORITY + 2, nullptr, tskNO_AFFINITY);
}
#endif  // CONFIG_TOUCHY_PROTO_OVER_SERIAL && (!CONFIG_SOC_USB_OTG_SUPPORTED || !CONFIG_TINYUSB_CDC_COUNT)

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
        // Stage 80: a flash commit renames the temp file over the
        // destination. If an animated GIF on the active screen is still
        // rendering that destination, its decoder holds the file open
        // and the rename fails with EBUSY. Release that handle first;
        // the screens_notify_file_changed() below re-applies the source.
        ESP_LOGD(TAG, "file_close: handle=%u commit=%d path='%s'",
                 (unsigned)fc.handle, (int)fc.commit,
                 s_active_write_path.c_str());
        if (fc.commit && !s_active_write_path.empty()) {
            screens_prepare_file_overwrite(s_active_write_path.c_str());
        }
        bool ok = fs_close_write(fc.handle, fc.commit);
        ESP_LOGD(TAG, "file_close: fs_close_write -> %d (path='%s')",
                 (int)ok, s_active_write_path.c_str());
        if (ok && fc.commit && !s_active_write_path.empty()) {
            // Hand the freshly-committed file to the screen registry;
            // it's a no-op for anything outside `*:host/s/*.pb`.
            screens_register_from_file(s_active_write_path.c_str());
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
            ESP_LOGD(TAG, "file_close: notify scheduled for '%s'",
                     s_active_write_path.c_str());
        }
        s_active_write_path.clear();
        resp->code = ok ? touchy_ResultCode_RESULT_OK
                        : touchy_ResultCode_RESULT_IO_ERROR;
        break;
    }

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

    case touchy_Command_set_preferences_tag:
        // Stage 82 — apply a partial preferences update. The device merges
        // only the present fields and fires each one's side effect
        // (backlight timeout, screen switch, log threshold). Returns
        // RESULT_NOT_FOUND if a requested current_screen can't be loaded.
        resp->code = Prefs::instance().apply_partial(cmd->cmd.set_preferences.prefs)
                         ? touchy_ResultCode_RESULT_OK
                         : touchy_ResultCode_RESULT_NOT_FOUND;
        break;

    case touchy_Command_run_actions_tag: {
        // Stage 71 — run a host-supplied list of Actions as if a local
        // widget had fired them. `actions` is FT_POINTER (heap), so its
        // pointer/count live in the repeated-field pair.
        const auto &ra = cmd->cmd.run_actions;
        widget_run_actions(ra.actions, ra.actions_count);
        resp->code = touchy_ResultCode_RESULT_OK;
        break;
    }

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

static void host_api_task(void *arg)
{
    HostApiLink *link = static_cast<HostApiLink *>(arg);
    ESP_LOGI(TAG, "host_api dispatcher started (%s)", link->name());

    for (;;) {
        // Wait until the link is up before trying to read a frame.
        if (!link->connected()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Read one validated, self-synchronising frame into link->rx_buf.
        size_t payload_len = 0;
        if (!read_frame(link, &payload_len)) continue;

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
            resp->code = touchy_ResultCode_RESULT_INVALID_ARG;
            std::size_t n = 0;
            if (resp.encode(link->tx_buf, HOST_API_TX_MAX, &n)) {
                write_frame(link, link->tx_buf, n);
            }
            continue;
        }

        // Dispatch.
        dispatch(cmd.get(), resp.get());

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

extern "C" void host_api_start(void)
{
    static bool started = false;
    if (started) return;
    started = true;

    s_evt_queue = xQueueCreate(EVENT_QUEUE_DEPTH, sizeof(touchy_LvEvent));

#if CONFIG_SOC_USB_OTG_SUPPORTED
    // Vendor (USB bulk) dispatcher — present on native-USB chips.
    s_vendor_link.rx_sem = xSemaphoreCreateBinary();
    // 10 KB stack: pb_decode of an ImageSaveCmd uses static buffers (the
    // generated struct is heap/.bss), so this is mostly for nanopb's small
    // call stack. Pin to APP CPU to keep TinyUSB on the PRO CPU.
    xTaskCreatePinnedToCore(host_api_task, "host_api", 10 * 1024,
                            &s_vendor_link, tskIDLE_PRIORITY + 4, nullptr, 1);
#endif

#if CONFIG_TOUCHY_PROTO_OVER_SERIAL && CONFIG_SOC_USB_OTG_SUPPORTED && CONFIG_TINYUSB_CDC_COUNT
    // Serial (USB-CDC ACM) dispatcher — same protocol, second link.
    s_serial_link.rx_sem = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(host_api_task, "host_api_ser", 10 * 1024,
                            &s_serial_link, tskIDLE_PRIORITY + 4, nullptr, 1);
#endif

#if CONFIG_TOUCHY_PROTO_OVER_SERIAL && (!CONFIG_SOC_USB_OTG_SUPPORTED || !CONFIG_TINYUSB_CDC_COUNT)
    // Hardware-UART dispatcher — for no-USB chips and S3 boards where USB pins
    // are unavailable (e.g. shared with I2C, so CDC cannot be used).
    s_uart_link.rx_sem = xSemaphoreCreateBinary();
    uart_link_init(&s_uart_link);
    xTaskCreatePinnedToCore(host_api_task, "host_api_uart", 10 * 1024,
                            &s_uart_link, tskIDLE_PRIORITY + 4, nullptr, 1);
#endif
}

#if CONFIG_SOC_USB_OTG_SUPPORTED
extern "C" void host_api_on_rx(void)
{
    if (s_vendor_link.rx_sem) {
        BaseType_t hpw = pdFALSE;
        xSemaphoreGiveFromISR(s_vendor_link.rx_sem, &hpw);
        portYIELD_FROM_ISR(hpw);
    }
}
#endif

#if CONFIG_TOUCHY_PROTO_OVER_SERIAL && CONFIG_SOC_USB_OTG_SUPPORTED && CONFIG_TINYUSB_CDC_COUNT
extern "C" void host_api_on_cdc_rx(void)
{
    if (s_serial_link.rx_sem) {
        BaseType_t hpw = pdFALSE;
        xSemaphoreGiveFromISR(s_serial_link.rx_sem, &hpw);
        portYIELD_FROM_ISR(hpw);
    }
}
#endif

