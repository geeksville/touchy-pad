// SPDX-License-Identifier: Apache-2.0
//
// touchy-pad v2 entry point — ESP-IDF / FreeRTOS port.

#include "backlight.h"
#include "board.h"
#include "display.h"
#include "fs.h"
#include "macros.h"
#include "prefs.h"
#include "screens.h"
#include "touch.h"
#include "usb_hid.h"

#include "lvgl.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "touchy-pad v2 booting");

    // Bring USB up first so the host sees the HID device enumerate quickly,
    // right after the USB-Serial/JTAG peripheral is disabled at IDF startup.
    // If USB were initialised after display/touch (which can take ~1 s) the
    // host port may time out waiting for a re-connection.
    usb_hid_init();

    // Give enough time for user to open a debug serial port to our board
    //vTaskDelay(pdMS_TO_TICKS(5000));

    // Mount the on-device filesystem (stage 14). host_api command handlers
    // expect /littlefs/from_host to exist by the time they run.
    Fs::instance().begin();

    // Load persisted preferences (screen timeout, etc.) before any
    // subsystem that might query them.
    Prefs::instance().begin();

    // Initialise the host-uploaded screen registry (stage 15). Idempotent;
    // safe to call before LVGL is up because no LVGL APIs are touched yet.
    screens_init();

    // Spawn the macro replay task (stage 16). Safe to call before TinyUSB
    // is fully ready — the runner blocks on its queue until macros arrive.
    macros_init();

    board_init();

    // Start the backlight auto-sleep timer with the persisted timeout.
    // A timeout of 0 disables auto-sleep.
    backlight_init(Prefs::instance().screen_timeout_ms());

    lv_display_t *disp = display_init();
    esp_lcd_touch_handle_t tp = touch_init(disp);

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

    // Show something on the display from the very first frame: either
    // the first host-uploaded screen discovered during screens_init(),
    // or a built-in "No screens configured" fallback when the device
    // hasn't been provisioned yet. Hosts can override at any time via
    // ScreenLoadCmd.
    screens_load(nullptr);

    ESP_LOGI(TAG, "Ready");
    // Nothing else to do here — host_api dispatches screen loads driven
    // by the host CLI, Trackpad widgets inside loaded screens react to
    // LVGL touch events on the LVGL task, and TinyUSB runs in its own
    // task.
    vTaskDelete(NULL);
}
