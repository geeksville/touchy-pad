// SPDX-License-Identifier: Apache-2.0
//
// touchy-pad v2 entry point — ESP-IDF / FreeRTOS port.

#include "board.h"
#include "display.h"
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

    // Give enough time for user to open a debug serial port to our board
    vTaskDelay(pdMS_TO_TICKS(5000));

    board_init();
    lv_disp_t *disp = display_init();
    esp_lcd_touch_handle_t tp = touch_init(disp);

    // Build the LVGL UI under the port lock.
    TrackpadWidget *pad = nullptr;
    lvgl_port_lock(0);
    pad = new TrackpadWidget(tp, lv_scr_act());
    lvgl_port_unlock();

    ESP_LOGI(TAG, "Ready");
    while (true) {
        pad->poll();
        vTaskDelay(pdMS_TO_TICKS(10)); // FIXME, remove this poll and try to not poll the touchpad at all, instead use some sort of interrupt
    }
}
