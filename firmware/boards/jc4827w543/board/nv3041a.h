// SPDX-License-Identifier: Apache-2.0
//
// Minimal NV3041A 480x272 QSPI panel driver for ESP-IDF.
//
// Sends commands and pixel data using ESP32-S3 SPI master in 4-line (QSPI)
// mode. The chip uses the same framing as most "QSPI" TFTs in this product
// family:
//
//   <CS↓> <8-bit cmd> <24-bit addr> <data...> <CS↑>
//
// where:
//   cmd = 0x02 for register writes  (data: 1 byte parameter)
//   cmd = 0x32 for memory writes    (data: 16-bit RGB565 pixels)
//   addr = (0x00 << 16) | (chip_reg << 8) | 0x00
//
// Initialisation sequence is copied verbatim from
// https://github.com/moononournation/Arduino_GFX/blob/master/src/display/Arduino_NV3041A.cpp
// which is the authoritative reference shipped with the board.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    spi_host_device_t host;
    int               cs_gpio;
    int               sck_gpio;
    int               d0_gpio;
    int               d1_gpio;
    int               d2_gpio;
    int               d3_gpio;
    int               rst_gpio;        // -1 if tied to MCU reset
    int               pclk_hz;
} nv3041a_config_t;

typedef struct nv3041a_dev_t *nv3041a_handle_t;

esp_err_t nv3041a_create(const nv3041a_config_t *cfg, nv3041a_handle_t *out);

// Set the active pixel rectangle (inclusive coordinates) and prime the chip
// for an upcoming pixel write.
esp_err_t nv3041a_set_window(nv3041a_handle_t dev,
                             uint16_t x1, uint16_t y1,
                             uint16_t x2, uint16_t y2);

// Blast `count` RGB565 pixels (16 bit big-endian on the wire) into the
// previously selected window. Safe to call repeatedly for one set_window().
//
// NOTE: this call is asynchronous — it enqueues the transactions onto the SPI
// driver's internal queue via spi_device_queue_trans() and returns as soon as
// the work is enqueued. The caller MUST NOT modify, free, or reuse the
// `pixels` buffer until `done_cb` has been called or until
// nv3041a_wait_idle() has returned.
//
// If `done_cb` is non-NULL, it will be invoked **from the SPI ISR** (so it
// must be IRAM-safe, do minimal work, and return quickly) after the final
// byte of the pixel buffer has hit the wire. `done_user` is passed through.
// Pass NULL/NULL if you intend to synchronise via nv3041a_wait_idle().
typedef void (*nv3041a_done_cb_t)(void *user);

esp_err_t nv3041a_write_pixels(nv3041a_handle_t dev,
                               const uint16_t *pixels, size_t count,
                               nv3041a_done_cb_t done_cb, void *done_user);

// Block until every queued transaction has actually finished on the wire.
// Call this before reusing any buffer that was passed to nv3041a_write_pixels
// or nv3041a_set_window.
esp_err_t nv3041a_wait_idle(nv3041a_handle_t dev);

#ifdef __cplusplus
}
#endif
