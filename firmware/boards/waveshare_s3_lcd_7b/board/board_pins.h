// SPDX-License-Identifier: Apache-2.0
//
// Pin map and panel timing for the Waveshare ESP32-S3-Touch-LCD-7B.
//
// Pin numbers below were taken from the upstream board file shipped with
// `esp-arduino-libs/ESP32_Display_Panel` (BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_7.h)
// and from the Waveshare wiki. All values refer to ESP32-S3 GPIO indices,
// except the three pins marked CH422G_IO_* which sit on the on-board CH422G
// I2C IO-expander.
//
// This header is private to the board component – main/ never sees it.

#pragma once

#include "driver/gpio.h"

// ---------- Panel timing ----------
#define BOARD_LCD_H_RES                 800
#define BOARD_LCD_V_RES                 480
#define BOARD_LCD_PIXEL_CLOCK_HZ        (16 * 1000 * 1000)

#define BOARD_LCD_HSYNC_PULSE_WIDTH     4
#define BOARD_LCD_HSYNC_BACK_PORCH      8
#define BOARD_LCD_HSYNC_FRONT_PORCH     8
#define BOARD_LCD_VSYNC_PULSE_WIDTH     4
#define BOARD_LCD_VSYNC_BACK_PORCH      8
#define BOARD_LCD_VSYNC_FRONT_PORCH     8
#define BOARD_LCD_PCLK_ACTIVE_NEG       1

// ---------- RGB LCD GPIOs ----------
#define BOARD_LCD_GPIO_HSYNC            GPIO_NUM_46
#define BOARD_LCD_GPIO_VSYNC            GPIO_NUM_3
#define BOARD_LCD_GPIO_DE               GPIO_NUM_5
#define BOARD_LCD_GPIO_PCLK             GPIO_NUM_7
#define BOARD_LCD_GPIO_DISP             GPIO_NUM_NC

// 16-bit RGB565 data lines (LSB → MSB)
#define BOARD_LCD_GPIO_DATA0            GPIO_NUM_14   // B0
#define BOARD_LCD_GPIO_DATA1            GPIO_NUM_38   // B1
#define BOARD_LCD_GPIO_DATA2            GPIO_NUM_18   // B2
#define BOARD_LCD_GPIO_DATA3            GPIO_NUM_17   // B3
#define BOARD_LCD_GPIO_DATA4            GPIO_NUM_10   // B4
#define BOARD_LCD_GPIO_DATA5            GPIO_NUM_39   // G0
#define BOARD_LCD_GPIO_DATA6            GPIO_NUM_0    // G1
#define BOARD_LCD_GPIO_DATA7            GPIO_NUM_45   // G2
#define BOARD_LCD_GPIO_DATA8            GPIO_NUM_48   // G3
#define BOARD_LCD_GPIO_DATA9            GPIO_NUM_47   // G4
#define BOARD_LCD_GPIO_DATA10           GPIO_NUM_21   // G5
#define BOARD_LCD_GPIO_DATA11           GPIO_NUM_1    // R0
#define BOARD_LCD_GPIO_DATA12           GPIO_NUM_2    // R1
#define BOARD_LCD_GPIO_DATA13           GPIO_NUM_42   // R2
#define BOARD_LCD_GPIO_DATA14           GPIO_NUM_41   // R3
#define BOARD_LCD_GPIO_DATA15           GPIO_NUM_40   // R4

// ---------- Shared I2C bus (touch + io expander) ----------
#define BOARD_I2C_PORT                  I2C_NUM_0
#define BOARD_I2C_SCL                   GPIO_NUM_9
#define BOARD_I2C_SDA                   GPIO_NUM_8
#define BOARD_I2C_CLK_HZ                (400 * 1000)

// ---------- GT911 touch ----------
#define BOARD_TOUCH_I2C_ADDR            0x5D            // GT911 default
#define BOARD_TOUCH_INT_GPIO            GPIO_NUM_4
// RST is on the CH422G, not a direct GPIO – see below.
#define BOARD_TOUCH_RST_GPIO            GPIO_NUM_NC

// ---------- CH422G IO expander ----------
#define BOARD_CH422G_I2C_ADDR_WR_SET    (0x48 >> 1)     // 0x24 – system reg
#define BOARD_CH422G_I2C_ADDR_WR_OC     (0x46 >> 1)     // 0x23 – open-drain outputs
#define BOARD_CH422G_I2C_ADDR_WR_IO     (0x70 >> 1)     // 0x38 – IO0..IO7 outputs
#define BOARD_CH422G_I2C_ADDR_RD_IO     (0x4D >> 1)     // 0x26 – IO0..IO7 inputs
#define BOARD_CH422G_IO_TP_RST          1
#define BOARD_CH422G_IO_BACKLIGHT       2
#define BOARD_CH422G_IO_LCD_RST         3
