// SPDX-License-Identifier: GPL-3.0-or-later
//
// touchy-pad v2 entry point — ESP-IDF / FreeRTOS port.

#include "backlight.h"
#include "board.h"
#include "coredump_report.h"
#include "display.h"
#include "fs.h"
#include "host_api.h"
#include "log_proto.h"
#include "macros.h"
#include "prefs.h"
#include "screens.h"
#include "touch.h"
#include "usb_hid.h"

#include "lvgl.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string>

static const char *TAG = "main";

#if CONFIG_TOUCHY_NO_DISPLAY
// Stage 64.4: bring LVGL up against an off-screen framebuffer with a no-op
// flush callback. This keeps the entire screen / host_api stack running
// unchanged while the board's (possibly buggy) panel and touch drivers stay
// completely out of the boot path. Returns the headless LVGL display, or
// nullptr if the draw buffer could not be allocated.
static lv_display_t *display_init_headless(void)
{
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority   = 4;
    port_cfg.task_stack      = 8 * 1024;
    port_cfg.timer_period_ms = 5;
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    constexpr int    W             = CONFIG_TOUCHY_HEADLESS_HRES;
    constexpr int    H             = CONFIG_TOUCHY_HEADLESS_VRES;
    constexpr size_t LINES_PER_BUF = 40;
    const size_t     buf_bytes     = (size_t)W * LINES_PER_BUF * sizeof(uint16_t);

    auto *buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_DEFAULT);
    if (!buf) {
        ESP_LOGE(TAG, "headless: failed to allocate %u-byte draw buffer",
                 (unsigned)buf_bytes);
        return nullptr;
    }

    lvgl_port_lock(0);
    lv_display_t *disp = lv_display_create(W, H);
    if (disp) {
        lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
        lv_display_set_buffers(disp, buf, nullptr, buf_bytes,
                               LV_DISPLAY_RENDER_MODE_PARTIAL);
        // Discard every rendered slice; just acknowledge it immediately.
        lv_display_set_flush_cb(disp,
            [](lv_display_t *d, const lv_area_t *, uint8_t *) {
                lv_display_flush_ready(d);
            });
    } else {
        ESP_LOGE(TAG, "headless: lv_display_create failed");
    }
    lvgl_port_unlock();
    return disp;
}
#endif  // CONFIG_TOUCHY_NO_DISPLAY

extern "C" void app_main(void)
{
    // Stage 64.1: hook esp_log_set_vprintf() before any other ESP_LOG
    // call so the host can drain tunneled log records from its normal
    // event-poll loop. No-op when CONFIG_TOUCHY_LOG_OVER_PROTO=n.
    log_proto_start();

    // If the previous boot panicked, decode the saved core dump and log
    // its summary now — first thing, so the report lands in the first
    // few proto log records and survives the boot flood before the host
    // connects. Erases the image so it reports exactly once.
    coredump_report_check_and_log();

    ESP_LOGI(TAG, "touchy-pad v2 booting");

#if CONFIG_SOC_USB_OTG_SUPPORTED
    // Bring USB up first so the host sees the HID device enumerate quickly,
    // right after the USB-Serial/JTAG peripheral is disabled at IDF startup.
    // If USB were initialised after display/touch (which can take ~1 s) the
    // host port may time out waiting for a re-connection.
    usb_hid_init();
#endif

    // Start the host_api command/response dispatcher(s). Decoupled from
    // usb_hid_init() (Stage 65) so it also runs on no-USB chips, where it
    // brings up the serial/UART transport instead of the vendor bulk pair.
    // Must run after usb_hid_init() so the vendor link's TinyUSB stack is up.
    host_api_start();

    // Give enough time for user to open a debug serial port to our board
    //vTaskDelay(pdMS_TO_TICKS(5000));

    // Mount the on-device filesystems (stage 51). host_api command handlers
    // expect both F: (littlefs) and R: (psram) to be ready, and the LVGL
    // R: driver must be registered before any screen mentioning an `R:`
    // image is loaded.
    fs_init();

    // Load persisted preferences (screen timeout, etc.) before any
    // subsystem that might query them.
    Prefs::instance().begin();

    // Initialise the host-uploaded screen registry (stage 15). Idempotent;
    // safe to call before LVGL is up because no LVGL APIs are touched yet.
    screens_init();

    // Spawn the macro replay task (stage 16). Safe to call before TinyUSB
    // is fully ready — the runner blocks on its queue until macros arrive.
    macros_init();

#if CONFIG_TOUCHY_NO_DISPLAY
    // Stage 64.4: the display + touchscreen hardware is disabled at build
    // time. Skip board_init()/backlight/panel/touch entirely and stand up a
    // headless LVGL display so the rest of the app runs unchanged. One
    // warning here; no per-use logging anywhere else.
    ESP_LOGW(TAG, "Display hardware disabled due to build options.");
    lv_display_t *disp = display_init_headless();
    esp_lcd_touch_handle_t tp = nullptr;
#else
    board_init();

    // Start the backlight auto-sleep timer with the persisted timeout.
    // A timeout of 0 disables auto-sleep.
    backlight_init(Prefs::instance().screen_timeout_ms());

    lv_display_t *disp = display_init();
    esp_lcd_touch_handle_t tp = touch_init(disp);
#endif
    (void)disp;  // handle not needed past bring-up (LVGL tracks the default)

    // display_init() calls lv_init() which clears LVGL's FS driver
    // linked list. (Re)register our 'R:' driver now so host-uploaded
    // images in PSRAM are reachable via lv_image_set_src("R:...").
    fs_register_lvgl_drivers();

    // Register a lightweight LVGL indev callback so any touch event resets
    // the backlight sleep timer and turns the backlight back on if it was off.
    lv_indev_t *indev = touch_get_indev();
    if (indev) {
        lv_indev_add_event_cb(indev,
            [](lv_event_t *) { backlight_touch_activity(); },
            LV_EVENT_PRESSED, nullptr);
    }

    // Hand the touch controller to the screen subsystem so Trackpad
    // widgets inside host-uploaded screens can recover multi-finger
    // snapshots (LVGL's indev only carries a single point).
    screens_set_touch(tp);

    // Show something on the display from the very first frame. Preference
    // order:
    //   1. The screen the user was last viewing (saved by screens.cpp into
    //      `prefs/prefs.pb` after every successful screens_load). Stored
    //      as a full drive-prefixed path (e.g. `F:host/s/home.pb`).
    //   2. If that path isn't currently registered, screens_load() falls
    //      back to the first discovered host screen.
    //   3. If nothing has been provisioned, the built-in fallback compiled
    //      in from proto/default_screen.json.
    // Hosts can override at any time via ScreenLoadCmd.
    const std::string &last = Prefs::instance().current_screen();
    bool loaded = last.empty() ? screens_load(nullptr)
                               : screens_load(last.c_str());
    if (!loaded && !last.empty()) {
        // Saved screen path is no longer registered (e.g. it lived on R:
        // and didn't survive reboot, or a firmware upgrade changed the
        // layout version). Clear the stale pref and fall back to the
        // built-in default so the display isn't left blank.
        ESP_LOGW(TAG, "saved screen '%s' not found — resetting to default",
                 last.c_str());
        Prefs::instance().set_current_screen("");
        screens_load(nullptr);
    }

    ESP_LOGI(TAG, "Ready");
    // Nothing else to do here — host_api dispatches screen loads driven
    // by the host CLI, Trackpad widgets inside loaded screens react to
    // LVGL touch events on the LVGL task, and TinyUSB runs in its own
    // task.
    vTaskDelete(NULL);
}
