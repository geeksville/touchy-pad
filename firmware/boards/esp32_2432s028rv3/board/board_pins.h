// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pin map for the ESP32-2432S028Rv3 ("Cheap Yellow Display", CYD2USB).
//   - MCU:     classic ESP32-D0WD-V3, 520 KB SRAM, no PSRAM, 4 MB flash
//   - Display: 2.8" 320x240 ST7789 (v3 revision) over SPI
//   - Touch:   XPT2046 resistive on a SEPARATE SPI bus
//   - Host:    CH340 UART bridge on UART0 (no native USB)
//
// Pin numbers are from the community-maturated CYD pinout
// (github.com/rzeldent/esp32-smartdisplay and the Cheap-Yellow-Display repo).
// A few values are board-revision-dependent; those are called out inline and
// kept here as named constants so they can be flipped in one place once
// validated on real hardware.

#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"

// ---------- ST7789 SPI display ----------
#define BOARD_LCD_H_RES                 320
#define BOARD_LCD_V_RES                 240

#define BOARD_LCD_SPI_HOST              SPI2_HOST       // HSPI on classic ESP32
#define BOARD_LCD_PIXEL_CLOCK_HZ        (40 * 1000 * 1000)

#define BOARD_LCD_GPIO_MOSI             GPIO_NUM_13
#define BOARD_LCD_GPIO_MISO             GPIO_NUM_12     // unused by the panel
#define BOARD_LCD_GPIO_SCK              GPIO_NUM_14
#define BOARD_LCD_GPIO_CS               GPIO_NUM_15
#define BOARD_LCD_GPIO_DC               GPIO_NUM_2
#define BOARD_LCD_GPIO_RST              GPIO_NUM_NC     // tied to module reset

// Backlight GPIO. The Rv3 board drives the LED via GPIO21 on most units, but
// some early/alternate revisions use GPIO27. Flip here if the screen stays
// dark. Active-high.
#define BOARD_LCD_GPIO_BACKLIGHT        GPIO_NUM_27

// ST7789 v3 colour quirks. The 2432S028 v3 panel wants BGR colour order and
// inverted pixels; older v2 boards want neither. Flip these two if reds/blues
// are swapped or the image is photo-negative.
#define BOARD_LCD_BGR_ORDER             1
#define BOARD_LCD_INVERT_COLOR          1

// Display orientation: landscape (320 wide). swap_xy + mirror_x gives the
// USB-port-on-the-right landscape used by the CYD reference projects.
#define BOARD_LCD_SWAP_XY               1
#define BOARD_LCD_MIRROR_X              1
#define BOARD_LCD_MIRROR_Y              0

// ---------- XPT2046 resistive touch (separate SPI bus) ----------
#define BOARD_TOUCH_SPI_HOST            SPI3_HOST       // VSPI on classic ESP32
#define BOARD_TOUCH_PIXEL_CLOCK_HZ      (2 * 1000 * 1000)

#define BOARD_TOUCH_GPIO_MOSI           GPIO_NUM_32
#define BOARD_TOUCH_GPIO_MISO           GPIO_NUM_39
#define BOARD_TOUCH_GPIO_SCK            GPIO_NUM_25
#define BOARD_TOUCH_GPIO_CS             GPIO_NUM_33
#define BOARD_TOUCH_GPIO_IRQ            GPIO_NUM_36

// XPT2046 is resistive: the raw ADC range maps to screen coordinates. These
// match esp_lcd_touch_xpt2046's default full-scale mapping; per-unit
// calibration can refine them later (open item in design.md Stage 65).

// ---------- RGB status LED (active-low) ----------
#define BOARD_LED_GPIO_R                GPIO_NUM_4
#define BOARD_LED_GPIO_G                GPIO_NUM_16
#define BOARD_LED_GPIO_B                GPIO_NUM_17
