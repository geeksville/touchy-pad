// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pin map for the ESP32-2432S024 ("Cheap Yellow Display", 2.4" CYD2USB).
//   - MCU:     classic ESP32-D0WD-V3, 520 KB SRAM, no PSRAM, 4 MB flash
//   - Display: 2.4" 320x240 ILI9341 over SPI
//   - Touch:   XPT2046 resistive on a SEPARATE SPI bus
//   - Host:    CH340 UART bridge on UART0 (no native USB)
//
// This board is electrically the 2432S028Rv3's twin apart from the display
// CONTROLLER (ILI9341 here vs ST7789 there), so the pin map mirrors that
// board; only BOARD_LCD_CONTROLLER_* and the colour-quirk flags differ. The
// C++ bring-up is shared in boards/cyd_common/. Pin numbers are from the
// community CYD pinout (github.com/F1ATB/ESP32-2432S028-2432S024-2432S032).
// Revision-dependent values are named constants so they can be flipped in one
// place once validated on real hardware.

#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"

// ---------- ILI9341 SPI display ----------
#define BOARD_LCD_CONTROLLER_ILI9341    1       // selects the panel driver
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

// Backlight GPIO. Like the 2432S028, most units drive the LED via GPIO21 but
// some revisions use GPIO27. Flip here if the screen stays dark. Active-high.
#define BOARD_LCD_GPIO_BACKLIGHT        GPIO_NUM_27

// ILI9341 colour quirks. CYD ILI9341 panels are wired BGR; unlike the ST7789
// they are typically NOT colour-inverted (an ST7789-specific quirk). Flip
// these if reds/blues are swapped or the image looks like a photo-negative.
#define BOARD_LCD_BGR_ORDER             0
#define BOARD_LCD_INVERT_COLOR          0

// Display orientation: landscape (320 wide). swap_xy + mirror_x gives the
// USB-port-on-the-right landscape used by the CYD reference projects.
#define BOARD_LCD_SWAP_XY               0
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
