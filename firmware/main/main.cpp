// SPDX-License-Identifier: Apache-2.0
//
// touchy-pad v2 entry point — ESP-IDF / FreeRTOS port.

#include "board.h"
#include "display.h"
#include "fs.h"
#include "screens.h"
#include "touch.h"
#include "usb_hid.h"
#include "trackpad_widget.h"

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

    // Mount the on-device filesystem (stage 14). host_api command handlers
    // expect /littlefs/from_host to exist by the time they run.
    Fs::instance().begin();

    // Give enough time for user to open a debug serial port to our board
    vTaskDelay(pdMS_TO_TICKS(5000));

    board_init();
    lv_display_t *disp = display_init();
    esp_lcd_touch_handle_t tp = touch_init(disp);

    // Bring up the XML loader now that LVGL is fully alive. host_api
    // FileSave handlers call screens_register_from_file() — keep this
    // before the dispatcher task gets a chance to run any host commands.
    screens_init();

    // Build the LVGL UI under the port lock. The widget hooks itself into
    // LVGL's input events; no further driving needed from this task.
    lvgl_port_lock(0);
    new TrackpadWidget(tp, lv_screen_active());
    lvgl_port_unlock();

    ESP_LOGI(TAG, "Ready");
    // Nothing else to do here — TrackpadWidget reacts to LVGL touch events
    // on the LVGL task, and TinyUSB runs in its own task.
    vTaskDelete(NULL);
}
