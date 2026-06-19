// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pin map for the JC4827W543R "4.3-inch ESP32-S3 board".
//   - MCU:     ESP32-S3 with 8 MB OPI PSRAM, 4 MB QSPI flash
//   - Display: 480x272 NV3041A IPS via 4-bit QSPI
//   - Touch:   XPT2046 resistive single-touch over SPI
//
// Display pins match the JC4827W543 family. Touch pins are from the
// manufacturer's JC4827W543 Arduino LVGL widgets example for the R variant.

#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"

// ---------- NV3041A QSPI display ----------
#define BOARD_LCD_H_RES                 480
#define BOARD_LCD_V_RES                 272

// Native NV3041A 4-bit QSPI bus runs at up to ~32 MHz. according to random reb searches
// But my read of the datasheet says it is supposed to go up to 80Mhz!  So I'm tried that but it had 
// numerous bit errors.  50Mhz was better but still some errors!  40MHz was better still but not perfect.
// I guess the ardu folks were right! 32MHz really is the max speed for this device.
// - @geeksville
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

// ---------- XPT2046 resistive touch (separate SPI bus) ----------
#define BOARD_TOUCH_SPI_HOST            SPI3_HOST
#define BOARD_TOUCH_PIXEL_CLOCK_HZ      (2 * 1000 * 1000)

#define BOARD_TOUCH_GPIO_MOSI           GPIO_NUM_11
#define BOARD_TOUCH_GPIO_MISO           GPIO_NUM_13
#define BOARD_TOUCH_GPIO_SCK            GPIO_NUM_12
#define BOARD_TOUCH_GPIO_CS             GPIO_NUM_38
#define BOARD_TOUCH_GPIO_IRQ            GPIO_NUM_3
