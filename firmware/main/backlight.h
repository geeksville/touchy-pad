// SPDX-License-Identifier: GPL-3.0-or-later
//
// Touchy-Pad backlight control + auto-sleep (stage 19).
//
// Provides a board-agnostic API for turning the display backlight on/off
// and scheduling an auto-sleep timeout. The actual GPIO/I2C toggle is
// delegated to `backlight_set(uint8_t level)` (declared in `board.h`), which
// each board implements in its own `board.cpp` (or, for PWM-capable boards,
// the shared boards/common/backlight_pwm.cpp).
//
// Call order:
//   1. board_init()  — initialises the backlight hardware (GPIO/expander).
//   2. backlight_init(timeout_ms)  — sets up the FreeRTOS esp_timer.
//   3. On every touch: backlight_touch_activity()
//   4. From host commands: backlight_wake() / backlight_set_timeout()

#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// Initialise the backlight subsystem and arm the auto-sleep timer.
// `timeout_ms == 0` means "never sleep". Must be called after board_init().
void backlight_init(uint32_t timeout_ms);

// Force the backlight on and restart the auto-sleep countdown.
// Safe to call from any task (not ISR).
void backlight_wake(void);

// Notify of user touch activity — identical to backlight_wake().
// Intended as the LVGL indev event callback target so the call sites are
// self-documenting.
void backlight_touch_activity(void);

// Change the auto-sleep timeout. `ms == 0` disables auto-sleep and turns
// the backlight on. Persists to Prefs; call site is the ScreenSleepTimeout
// host command handler.
void backlight_set_timeout(uint32_t ms);

// Stage 94 — set the remembered display brightness (0 = off … 100 = max).
// Applied immediately if the display is awake (otherwise on the next wake),
// and persisted to Prefs so it survives a reboot. The auto-sleep on/off
// transitions restore this remembered level on wake. Call site is the
// backlight_level field of a SetPreferencesCmd.
void backlight_set_level(uint8_t level);

#ifdef __cplusplus
}
#endif
