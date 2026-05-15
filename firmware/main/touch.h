// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include "lvgl.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

// Number of finger slots we report.
#define TOUCH_MAX_POINTS 5

// Initialises the GT911 touch controller on the shared I2C bus and registers
// it with the LVGL port (so on-screen widgets get tap events). Must be called
// after display_init().
//
// Returns the underlying esp_lcd_touch handle so the trackpad widget can read
// raw multi-point data (LVGL itself only forwards a single point).
esp_lcd_touch_handle_t touch_init(lv_disp_t *disp);

#ifdef __cplusplus
}
#endif
