// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stage LB1 — no touch controller on the JC-ESP32P4-M3 LED-matrix board.
// main.cpp still calls touch_init()/touch_get_indev() unconditionally on
// non-headless boards, so provide harmless no-ops: no indev is registered
// and no touch events are ever produced.

#include "touch.h"

#include "esp_log.h"

static const char *TAG = TOUCHY_TAG("touch");

lv_indev_t *touch_get_indev(void) { return nullptr; }

esp_lcd_touch_handle_t touch_init(lv_display_t *disp)
{
    (void)disp;
    ESP_LOGI(TAG, "no touch panel on this board");
    return nullptr;
}
