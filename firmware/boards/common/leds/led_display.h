// SPDX-License-Identifier: GPL-3.0-or-later
//
// Internal seam between the shared LED-matrix display driver
// (led_display.cpp) and a board's backlight_set() (board.cpp).
//
// The LEDPanel is owned by led_display.cpp; brightness changes flow
// through here so the board layer need not know about LVGL or the panel.
// Boards using the WS2812B LED-matrix display driver
// (firmware/boards/common/leds/led_display.cpp) include this header from their
// board.cpp to forward backlight/brightness calls.

#pragma once

#include <cstdint>

// Set the LED matrix brightness as a percentage (0 = off … 100 = max).
// Safe to call before display_init() (the level is remembered and applied
// when the panel comes up). Triggers a repaint so the change is visible
// immediately when the display is already running.
void led_display_set_brightness(uint8_t level_0_100);

// Stage lb10 — proto-free description of one tiled panel chain, read from
// the persisted BoardConfig. Implemented in prefs.cpp (which owns the
// protobuf) so the board-compiled led_display.cpp stays free of
// proto/nanopb includes.
//
// A chain is one data GPIO driving up to LED_CHAIN_MAX_PANELS daisy-chained
// LED matrices, tiled into one logical surface. Per-panel wiring flags map
// each matrix's logical (x, y) to its physical LED order (see
// led_panel.{h,cpp}); their defaults reproduce the pre-lb10 frozen wiring
// (only column-snaking on), and `cols_snaked` here already resolves the
// proto's UNSET-means-true presence.
static constexpr int LED_CHAIN_MAX_PANELS = 4;  // matches nanopb max_count

struct LedPanelDesc {
    int  width;
    int  height;
    bool rows_snaked;
    bool cols_snaked;
    bool row_major;
    bool cols_flipped;
    bool rows_flipped;
};

struct LedChainDesc {
    int          gpio;
    bool         tile_by_row;   // false ⇒ tile horizontally
    int          panel_count;   // 1..LED_CHAIN_MAX_PANELS
    LedPanelDesc panels[LED_CHAIN_MAX_PANELS];
};

// Fills *out and returns true when exactly one LED panel chain is
// configured; returns false when the device has no board_config (fresh /
// unconfigured) so the display driver can come up headless.
bool led_chain_config(LedChainDesc *out);
