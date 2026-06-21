// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pin map for the Elecrow CrowPanel Advanced 7" ESP32-P4.
// 1024×600 MIPI-DSI display (EK79007), GT911 capacitive touch.
// No runtime variant detection — single hardware revision.

#pragma once

#include "driver/gpio.h"
#include "hal/i2c_types.h"

// ----- Display resolution -----
#define BOARD_LCD_H_RES  1024
#define BOARD_LCD_V_RES   600

// ----- EK79007 MIPI-DSI timing (1024×600 @ 60 Hz) -----
#define BOARD_DSI_HSYNC_PW   10
#define BOARD_DSI_HSYNC_BP  160
#define BOARD_DSI_HSYNC_FP  160
#define BOARD_DSI_VSYNC_PW    1
#define BOARD_DSI_VSYNC_BP   23
#define BOARD_DSI_VSYNC_FP   12

// ----- GT911 capacitive touch -----
#define BOARD_TOUCH_I2C_SCL          GPIO_NUM_46
#define BOARD_TOUCH_I2C_SDA          GPIO_NUM_45
#define BOARD_TOUCH_I2C_PORT         I2C_NUM_0
#define BOARD_TOUCH_I2C_CLK_HZ       400000
#define BOARD_TOUCH_I2C_ADDR_PRIMARY 0x5D
#define BOARD_TOUCH_I2C_ADDR_BACKUP  0x14

// ----- Backlight -----
// GPIO31: PWM (30 kHz, 11-bit LEDC); GPIO29: power enable (always HIGH).
#define BOARD_BK_PWM_GPIO   GPIO_NUM_31
#define BOARD_BK_PWM_FREQ   30000
#define BOARD_BK_PWR_GPIO   GPIO_NUM_29

// Stage 94 — shared LEDC-PWM backlight driver (boards/common/backlight_pwm).
// The P4 needs the PLL-divided LEDC clock and uses 11-bit duty.
#define BOARD_BL_GPIO          BOARD_BK_PWM_GPIO
#define BACKLIGHT_PWM_BITS     11
#define BACKLIGHT_PWM_FREQ     BOARD_BK_PWM_FREQ
#define BACKLIGHT_MIN_PWM      8
#define BACKLIGHT_LEDC_CLK     LEDC_USE_PLL_DIV_CLK

// ----- ESP32-C6 WiFi companion -----
// GPIO32 is the C6 reset line. Drive LOW to hold it in reset (WiFi unused).
#define BOARD_C6_RESET_GPIO  GPIO_NUM_32
