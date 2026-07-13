// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pin map for the Adafruit ESP32-S3 Feather LED-matrix board (Stage LB2).
// Display-less / touch-less: a WS2812B matrix, no LCD and no touch
// controller. Stage lb6 moved the LED panel geometry (data GPIO, width,
// height) out of this file and into the persisted BoardConfig preference
// (see `touchy pref from-template`), so no BOARD_LED_PANEL_* macros here.

#pragma once

#include "driver/gpio.h"
