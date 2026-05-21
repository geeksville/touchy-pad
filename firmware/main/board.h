// SPDX-License-Identifier: GPL-3.0-or-later
//
// Common board API consumed by main.cpp. Implementations live per-board
// under boards/<board>/board.cpp. See README.md for the multi-board layout.

#pragma once

#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

// Brings up board-specific peripherals: shared I2C bus (if any), IO
// expander, panel/touch resets, backlight. Must be called *before*
// display_init() / touch_init().
void board_init(void);

// Returns the shared I2C bus handle used by the touch controller (and
// possibly other peripherals like an IO expander). May return NULL on
// boards that do not expose I2C to the touch controller.
i2c_master_bus_handle_t board_get_i2c_bus(void);

// Set the display backlight on (true) or off (false). The backlight GPIO /
// IO-expander pin must already be configured by board_init(). Safe to call
// from any task.
void board_backlight_set(bool on);

#ifdef __cplusplus
}
#endif
