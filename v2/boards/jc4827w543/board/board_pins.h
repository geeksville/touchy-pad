// SPDX-License-Identifier: Apache-2.0
//
// Pin map for the JC4827W543 "4.3-inch ESP32-S3 board".
//   - MCU:     ESP32-S3 with 8 MB OPI PSRAM, 4 MB QSPI flash
//   - Display: 480x272 NV3041A IPS via 4-bit QSPI
//   - Touch:   GT911 capacitive multi-touch over I2C
//
// Pin numbers below are taken from the manufacturer's reference Arduino
// project (docs/temp/JC4827W543_4.3inch_ESP32S3_board/Examples/Demo_Arduino).

#pragma once

#include "driver/gpio.h"

// ---------- NV3041A QSPI display ----------
#define BOARD_LCD_H_RES                 480
#define BOARD_LCD_V_RES                 272

// Native NV3041A 4-bit QSPI bus runs at up to ~32 MHz.
#define BOARD_LCD_QSPI_HOST             SPI2_HOST
#define BOARD_LCD_QSPI_CLK_HZ           (32 * 1000 * 1000)

#define BOARD_LCD_GPIO_CS               GPIO_NUM_45
#define BOARD_LCD_GPIO_SCK              GPIO_NUM_47
#define BOARD_LCD_GPIO_D0               GPIO_NUM_21
#define BOARD_LCD_GPIO_D1               GPIO_NUM_48
#define BOARD_LCD_GPIO_D2               GPIO_NUM_40
#define BOARD_LCD_GPIO_D3               GPIO_NUM_39
#define BOARD_LCD_GPIO_RST              GPIO_NUM_NC      // tied to system reset
#define BOARD_LCD_GPIO_BACKLIGHT        GPIO_NUM_1

// ---------- Shared I2C bus (touch only on this board) ----------
#define BOARD_I2C_PORT                  I2C_NUM_0
#define BOARD_I2C_SCL                   GPIO_NUM_4
#define BOARD_I2C_SDA                   GPIO_NUM_8
#define BOARD_I2C_CLK_HZ                (400 * 1000)

// ---------- GT911 touch ----------
#define BOARD_TOUCH_I2C_ADDR            0x5D
#define BOARD_TOUCH_INT_GPIO            GPIO_NUM_3
#define BOARD_TOUCH_RST_GPIO            GPIO_NUM_38
