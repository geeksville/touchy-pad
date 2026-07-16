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
#include "tc_tag.h"
#include "touch.h"
#include "usb_hid.h"

#if CONFIG_TOUCHY_WIFI
#include "network.h"
#endif

#include "lvgl.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cinttypes>
#include <string>

static const char *TAG = TOUCHY_TAG("main");

// Fires on *every* failed heap allocation (malloc / calloc / realloc /
// heap_caps_* / C++ new — they all route through the heap component).
// Keep it allocation-free and re-entrancy-safe: only ESP_LOG + heap query
// helpers. The free/largest-block figures distinguish true exhaustion from
// fragmentation, and `caps` distinguishes internal RAM vs PSRAM. These
// ESP_LOGE lines ride the Stage 64.1 proto log tunnel back to the host.
static void heap_alloc_failed_cb(size_t size, uint32_t caps, const char *func)
{
    ESP_LOGE(TAG,
             "ALLOC FAILED: %u bytes, caps=0x%" PRIx32 " in %s "
             "(free=%u, largest=%u)",
             (unsigned)size, caps, func ? func : "?",
             (unsigned)heap_caps_get_free_size(caps),
             (unsigned)heap_caps_get_largest_free_block(caps));
}

extern "C" void app_main(void)
{
    // Stage 64.1: hook esp_log_set_vprintf() before any other ESP_LOG
    // call so the host can drain tunneled log records from its normal
    // event-poll loop. No-op when CONFIG_TOUCHY_LOG_OVER_PROTO=n.
    log_proto_start();

    // Log an ERROR on any failed heap allocation, device-wide. Registered
    // right after the log tunnel so even early-boot allocation failures are
    // captured and forwarded to the host.
    heap_caps_register_failed_alloc_callback(heap_alloc_failed_cb);

    // If the previous boot panicked, decode the saved core dump and log
    // its summary now — first thing, so the report lands in the first
    // few proto log records and survives the boot flood before the host
    // connects. Erases the image so it reports exactly once.
    coredump_report_check_and_log();

    ESP_LOGW(TAG, "touchy-pad v2 booting");

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

    // Mount the on-device filesystems (stage 51). host_api command handlers
    // expect both F: (littlefs) and R: (psram) to be ready, and the LVGL
    // R: driver must be registered before any screen mentioning an `R:`
    // image is loaded.
    fs_init();

    // Load persisted preferences (screen timeout, etc.) before any
    // subsystem that might query them.
    Prefs::instance().begin();

    // FIXME - in early boot for some reason we often crash if DEBUG logs are enabled.  leave the the default
    // level at at least INFO level until late boot
    touchy_LogPriority log_level = (touchy_LogPriority)Prefs::instance().min_log_level();
    log_proto_set_min_level(log_level >= touchy_LogPriority_INFO 
        ? log_level 
        : touchy_LogPriority_INFO);

    // Stage 82: optional early-boot delay. The transports (USB vendor link /
    // serial) are already up from host_api_start() above, so a host can
    // attach to the log tunnel during this window and catch the earliest
    // subsystem bring-up logs. 0 = disabled.
    if (uint32_t delay_s = Prefs::instance().boot_delay_s()) {
        ESP_LOGW(TAG, "boot-delay: sleeping %" PRIu32 "s for debug attach", delay_s);
        vTaskDelay(pdMS_TO_TICKS(delay_s * 1000));
    }

    // Initialise the host-uploaded screen registry (stage 15). Idempotent;
    // safe to call before LVGL is up because no LVGL APIs are touched yet.
    screens_init();

    // Spawn the macro replay task (stage 16). Safe to call before TinyUSB
    // is fully ready — the runner blocks on its queue until macros arrive.
    macros_init();

    esp_lcd_touch_handle_t tp = nullptr;
    static Display *s_display = nullptr;
#if CONFIG_TOUCHY_NO_DISPLAY
    // Stage 64.4: the display + touchscreen hardware is disabled at build
    // time. Skip board_init()/backlight/panel/touch entirely and stand up a
    // headless LVGL display so the rest of the app runs unchanged. One
    // warning here; no per-use logging anywhere else.
    ESP_LOGW(TAG, "Display hardware disabled due to build options.");
    s_display = new HeadlessDisplay();
    s_display->init();
    lv_display_t *disp = s_display->raw();
#else
    board_init();

    // Start the backlight auto-sleep timer with the persisted timeout.
    // A timeout of 0 disables auto-sleep.
    backlight_init(Prefs::instance().screen_timeout_ms());

    // Stage lb7: the board provides a Display subclass via display_create();
    // main owns it and drives its lifecycle. On failure fall back to a
    // headless display so the rest of the app still runs.
    s_display = display_create();
    if (!s_display->init()) {
        ESP_LOGE(TAG, "display init failed; running headless");
        delete s_display;
        s_display = new HeadlessDisplay();
        s_display->init();
    }
    lv_display_t *disp = s_display->raw();
    if (disp) {
        tp = touch_init(disp);
    }
#endif
    (void)disp;  // handle not needed past bring-up (LVGL tracks the default)

    // enable storing log records from this point forward (enabling before this can cause hangs if DEBUG logging is enabled
    // via CONFIG_LOG_DEFAULT_LEVEL_DEBUG=y
    log_proto_enable();

    // Display bring-up calls lv_init() which clears LVGL's FS driver
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

#if CONFIG_TOUCHY_WIFI
    // Stage lb8 — bring up WiFi + the HTTP(S) command API if the host has
    // programmed credentials into NetworkConfig. Connection is async
    // (event-driven), so this returns immediately and never blocks boot;
    // with no credentials it's a no-op (radio stays off).
    network_apply(Prefs::instance().network());
#endif

    ESP_LOGI(TAG, "Ready");

    // FIXME - in early boot for some reason we often crash if DEBUG logs are enabled.  leave the the default
    //level at at least INFO level until late boot
    log_proto_set_min_level(log_level);
    // Nothing else to do here — host_api dispatches screen loads driven
    // by the host CLI, Trackpad widgets inside loaded screens react to
    // LVGL touch events on the LVGL task, and TinyUSB runs in its own
    // task.
    vTaskDelete(NULL);
}
