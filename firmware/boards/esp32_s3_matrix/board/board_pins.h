// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pin map for the Waveshare ESP32-S3-Matrix LED-matrix board (Stage matrix1).
// Display-less / touch-less: an 8×8 WS2812B matrix on GPIO 14, no LCD and
// no touch controller. Stage lb6 moved the LED panel geometry (data GPIO,
// width, height) out of this file and into the persisted BoardConfig
// preference (see `touchy pref from-template`), so no BOARD_LED_PANEL_*
// macros here.

#pragma once

#include "driver/gpio.h"

// Data line for the onboard 8×8 WS2812B matrix. The actual strip is created
// at runtime from the BoardConfig preference; this constant is informational.
#define BOARD_LED_GPIO GPIO_NUM_14
