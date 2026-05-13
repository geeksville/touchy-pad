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

    board_init();
    lv_disp_t *disp = display_init();
    esp_lcd_touch_handle_t tp = touch_init(disp);

    // Build the LVGL UI under the port lock.
    TrackpadWidget *pad = nullptr;
    lvgl_port_lock(0);
    pad = new TrackpadWidget(tp, lv_scr_act());
    lvgl_port_unlock();

    usb_hid_init();

    ESP_LOGI(TAG, "Ready");
    while (true) {
        pad->poll();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
