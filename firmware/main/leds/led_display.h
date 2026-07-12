// SPDX-License-Identifier: GPL-3.0-or-later
//
// Internal seam between the shared LED-matrix display driver
// (led_display.cpp) and a board's backlight_set() (board.cpp).
//
// The LEDPanel is owned by led_display.cpp; brightness changes flow
// through here so the board layer need not know about LVGL or the panel.
// Boards using the WS2812B LED-matrix display driver
// (firmware/main/leds/led_display.cpp) include this header from their
// board.cpp to forward backlight/brightness calls.

#pragma once

#include <cstdint>

// Set the LED matrix brightness as a percentage (0 = off … 100 = max).
// Safe to call before display_init() (the level is remembered and applied
// when the panel comes up). Triggers a repaint so the change is visible
// immediately when the display is already running.
void led_display_set_brightness(uint8_t level_0_100);
