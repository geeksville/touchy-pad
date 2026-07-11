// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pin map + panel geometry for the Adafruit ESP32-S3 Feather LED-matrix
// board (Stage LB2). Display-less / touch-less: a single 8x32 WS2812B
// matrix driven from one GPIO, no LCD and no touch controller.

#pragma once

#include "driver/gpio.h"

// ----- WS2812B LED matrix -----
// One 8-row × 32-column panel (256 LEDs) on the data GPIO below. The LVGL
// display is presented as 32 wide × 8 tall (see display.cpp); the panel's
// serpentine wiring is handled by LEDPanel.
#define BOARD_LED_PANEL_GPIO   GPIO_NUM_4
#define BOARD_LED_PANEL_W      32
#define BOARD_LED_PANEL_H      8
