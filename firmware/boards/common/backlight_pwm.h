// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared LEDC-PWM backlight driver (Stage 94).
//
// A single implementation of the board backlight primitive
// `backlight_set(uint8_t level)` (declared in main/board.h) for every board
// whose backlight is a PWM-capable GPIO. Modelled on the original SQUiXL
// LEDC code. Boards adopt it by listing `../../common/backlight_pwm.cpp` in
// their `board/CMakeLists.txt` (with `esp_driver_ledc` in REQUIRES) and
// defining at least `BOARD_BL_GPIO` in their private `board_pins.h`.
//
// Optional `board_pins.h` knobs (all defaulted):
//   BOARD_BL_GPIO          (required) LEDC output pin for the backlight.
//   BACKLIGHT_MIN_PWM      minimum-visible duty for a non-zero level (256).
//   BACKLIGHT_PWM_BITS     LEDC duty resolution in bits (12 → max 4095).
//   BACKLIGHT_PWM_FREQ     LEDC frequency in Hz (6000).
//   BACKLIGHT_PWM_INVERT   1 if the panel backlight is active-low (0).
//   BACKLIGHT_LEDC_TIMER   LEDC timer (LEDC_TIMER_0).
//   BACKLIGHT_LEDC_CHANNEL LEDC channel (LEDC_CHANNEL_0).
//   BACKLIGHT_LEDC_CLK     LEDC clock source (LEDC_AUTO_CLK).
//
// Boards whose backlight is behind an I2C IO-expander / companion MCU (and
// therefore can't PWM) do NOT use this file — they keep a board-local
// `backlight_set()` that quantises to on/off.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Configure the LEDC timer + channel for the board backlight, leaving it
// switched off (duty at the level-0 value). Call from board_init() before
// backlight_init(). The main backlight manager turns the light on at the
// remembered brightness via backlight_set().
void backlight_pwm_init(void);

#ifdef __cplusplus
}
#endif
