// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pin map for the Guition JC-ESP32P4-M3 LED-matrix board (Stage LB1).
// Display-less / touch-less: a WS2812B matrix, no LCD and no touch
// controller. Stage lb6 moved the LED panel geometry (data GPIO, width,
// height) out of this file and into the persisted BoardConfig preference
// (see `touchy pref from-template`), so no BOARD_LED_PANEL_* macros here.

#pragma once

#include "driver/gpio.h"

// ----- ESP32-C6 WiFi/BT coprocessor (ESP32-C6-MINI, "ESP32 Hosted") -----
// The module pairs a P4 (main) with a C6 (WiFi/BT) over SDIO. WiFi/BT are
// unused in LB1, so board_init() holds the C6 in reset (drive its reset
// line LOW) to keep the SDIO lines from floating.
#define BOARD_C6_RESET_GPIO    GPIO_NUM_54
// SDIO bus to the C6 (unused in LB1; recorded for future connectivity work).
#define BOARD_C6_SDIO_CMD_GPIO GPIO_NUM_19
#define BOARD_C6_SDIO_CLK_GPIO GPIO_NUM_18
#define BOARD_C6_SDIO_D0_GPIO  GPIO_NUM_14
#define BOARD_C6_SDIO_D1_GPIO  GPIO_NUM_15
#define BOARD_C6_SDIO_D2_GPIO  GPIO_NUM_16
#define BOARD_C6_SDIO_D3_GPIO  GPIO_NUM_17

// ----- Ethernet (IP101 PHY over RMII) -----
// Unused in LB1; recorded for future networking work.
#define BOARD_ETH_MDC_GPIO     GPIO_NUM_31
#define BOARD_ETH_MDIO_GPIO    GPIO_NUM_52
#define BOARD_ETH_POWER_GPIO   GPIO_NUM_51
#define BOARD_ETH_CLK_GPIO     GPIO_NUM_50

