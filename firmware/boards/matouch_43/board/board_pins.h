// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pin map and timing constants for the MakerFabs MaTouch 4.3"
// (ESP32-S3-WROOM-1-N16R8, 800x480 RGB-16-bit panel, GT911 touch).
//
// Source: ~/ESP32-S3-Parallel-TFT-with-Touch-4.3inch/examples/ESP-IDF/
//         matouch/main/boards/4-3-unknown-gt911/config.h  (V3.1 hardware)

#pragma once

#include "driver/gpio.h"
#include "hal/i2c_types.h"

// ---------- Display resolution ----------

#define BOARD_LCD_H_RES          800
#define BOARD_LCD_V_RES          480
#define BOARD_LCD_PIXEL_CLOCK_HZ (16 * 1000 * 1000)

// ---------- RGB panel sync / clock ----------

#define BOARD_LCD_PCLK   GPIO_NUM_42
#define BOARD_LCD_HSYNC  GPIO_NUM_39
#define BOARD_LCD_VSYNC  GPIO_NUM_41
#define BOARD_LCD_DE     GPIO_NUM_NC   // DE not connected on this board

// ---------- RGB565 data lines (D0=B0 .. D15=R4) ----------
//
// data_gpio_nums[i] = GPIO wired to LCD data bit i.
// Bit ordering: D[0..4]=B[0..4], D[5..10]=G[0..5], D[11..15]=R[0..4].

#define BOARD_LCD_D0   GPIO_NUM_8    // B0
#define BOARD_LCD_D1   GPIO_NUM_3    // B1
#define BOARD_LCD_D2   GPIO_NUM_46   // B2
#define BOARD_LCD_D3   GPIO_NUM_9    // B3
#define BOARD_LCD_D4   GPIO_NUM_1    // B4
#define BOARD_LCD_D5   GPIO_NUM_5    // G0
#define BOARD_LCD_D6   GPIO_NUM_6    // G1
#define BOARD_LCD_D7   GPIO_NUM_7    // G2
#define BOARD_LCD_D8   GPIO_NUM_15   // G3
#define BOARD_LCD_D9   GPIO_NUM_16   // G4
#define BOARD_LCD_D10  GPIO_NUM_4    // G5
#define BOARD_LCD_D11  GPIO_NUM_45   // R0
#define BOARD_LCD_D12  GPIO_NUM_48   // R1
#define BOARD_LCD_D13  GPIO_NUM_47   // R2
#define BOARD_LCD_D14  GPIO_NUM_21   // R3
#define BOARD_LCD_D15  GPIO_NUM_14   // R4

// ---------- RGB panel timing ----------

#define BOARD_LCD_HSYNC_BACK_PORCH   8
#define BOARD_LCD_HSYNC_FRONT_PORCH  8
#define BOARD_LCD_HSYNC_PULSE_WIDTH  4
#define BOARD_LCD_VSYNC_BACK_PORCH   8
#define BOARD_LCD_VSYNC_FRONT_PORCH  8
#define BOARD_LCD_VSYNC_PULSE_WIDTH  4
#define BOARD_LCD_PCLK_ACTIVE_NEG    0   // sample on rising edge

// ---------- Backlight ----------

#define BOARD_BACKLIGHT_GPIO  GPIO_NUM_44   // HIGH = on

// ---------- I2C bus (GT911 touch) ----------

#define BOARD_I2C_PORT    I2C_NUM_1
#define BOARD_I2C_SCL     GPIO_NUM_18
#define BOARD_I2C_SDA     GPIO_NUM_17
#define BOARD_I2C_CLK_HZ  400000

// ---------- GT911 touch ----------

#define BOARD_TOUCH_I2C_ADDR_PRIMARY  0x5D
#define BOARD_TOUCH_I2C_ADDR_BACKUP   0x14
#define BOARD_TOUCH_RST_GPIO          GPIO_NUM_38
#define BOARD_TOUCH_INT_GPIO          GPIO_NUM_NC
