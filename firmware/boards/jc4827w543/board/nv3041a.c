// SPDX-License-Identifier: Apache-2.0
//
// NV3041A 480x272 QSPI driver — see nv3041a.h for protocol notes.

#include "nv3041a.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "nv3041a";

struct nv3041a_dev_t {
    spi_device_handle_t spi;
};

// NV3041A commands (subset; full set in the Arduino_GFX header).
#define NV3041A_SLPOUT  0x11
#define NV3041A_DISPON  0x29
#define NV3041A_CASET   0x2A
#define NV3041A_RASET   0x2B
#define NV3041A_RAMWR   0x2C

// QSPI prefix bytes.
#define NV3041A_PREFIX_WR_REG  0x02   // register write  (data on single line)
#define NV3041A_PREFIX_WR_MEM  0x32   // memory write    (data on quad lines)

// Register pointer used in the address phase of a 0x32 (quad) memory-write
// transaction. NOTE: this is *not* the MIPI RAMWR (0x2C). The panel expects a
// separate RAMWR register write first (prefix 0x02, reg 0x2C) to switch into
// memory-write mode, after which the pixel stream must address register 0x3C.
// Getting this wrong makes only the very first flush succeed (because the
// post-init write pointer is still at (0,0)); every subsequent chunk silently
// drops on the floor. See Arduino_GFX/Arduino_ESP32QSPI.cpp for reference.
#define NV3041A_QSPI_MEM_REG   0x3C

// ---- One-byte register write -----------------------------------------------
static esp_err_t nv_write_reg(nv3041a_handle_t dev, uint8_t reg, uint8_t val)
{
    spi_transaction_ext_t t = {};
    t.base.flags    = SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR |
                      SPI_TRANS_USE_TXDATA;
    t.command_bits  = 8;
    t.address_bits  = 24;
    t.dummy_bits    = 0;
    t.base.cmd      = NV3041A_PREFIX_WR_REG;
    t.base.addr     = ((uint32_t)reg) << 8;
    t.base.length   = 8;        // 8 data bits
    t.base.tx_data[0] = val;
    return spi_device_transmit(dev->spi, (spi_transaction_t *)&t);
}

// ---- Command with no parameters -------------------------------------------
static esp_err_t nv_send_cmd(nv3041a_handle_t dev, uint8_t reg)
{
    spi_transaction_ext_t t = {};
    t.base.flags    = SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR;
    t.command_bits  = 8;
    t.address_bits  = 24;
    t.dummy_bits    = 0;
    t.base.cmd      = NV3041A_PREFIX_WR_REG;
    t.base.addr     = ((uint32_t)reg) << 8;
    t.base.length   = 0;
    return spi_device_transmit(dev->spi, (spi_transaction_t *)&t);
}

// ---- Memory write (pixel stream on 4 data lines) ---------------------------
static esp_err_t nv_write_pixels_raw(nv3041a_handle_t dev,
                                     const uint16_t *pixels, size_t count)
{
    if (count == 0) return ESP_OK;

    // 1. Switch the panel into memory-write mode (single-line RAMWR, no data).
    esp_err_t err = nv_send_cmd(dev, NV3041A_RAMWR);
    if (err != ESP_OK) return err;

    // 2. Stream pixels in chunks. The ESP32-S3 SPI master rejects QIO
    //    transactions larger than ~32 KB at a time (returns ESP_ERR_INVALID_ARG)
    //    so we mirror Arduino_GFX's ESP32QSPI driver and split big buffers
    //    into chunks. First chunk carries the prefix 0x32 + addr 0x3C; later
    //    chunks continue with SPI_TRANS_VARIABLE_DUMMY so the cmd/addr phases
    //    are skipped and the data simply concatenates.
    //
    //    Every chunk except the final one sets SPI_TRANS_CS_KEEP_ACTIVE so
    //    hardware CS stays asserted across the whole burst (otherwise the
    //    SPI peripheral would pulse CS between transactions and the panel
    //    would terminate the memory write after the first chunk). This
    //    flag requires the bus to be acquired — nv3041a_create() did that
    //    once at startup and never releases it.
    const size_t CHUNK_PIXELS = 8192;   // 16 KB, well below any limit
    bool first = true;
    size_t off = 0;
    while (off < count) {
        const size_t this_count =
            (count - off > CHUNK_PIXELS) ? CHUNK_PIXELS : (count - off);
        const bool   last       = (off + this_count == count);

        spi_transaction_ext_t t = {};
        t.command_bits  = 8;
        t.address_bits  = 24;
        t.dummy_bits    = 0;

        uint32_t flags = SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR |
                         SPI_TRANS_MODE_QIO;
        if (!last) flags |= SPI_TRANS_CS_KEEP_ACTIVE;

        if (first) {
            t.base.flags = flags;
            t.base.cmd   = NV3041A_PREFIX_WR_MEM;
            t.base.addr  = ((uint32_t)NV3041A_QSPI_MEM_REG) << 8;
            first = false;
        } else {
            // Continuation: no cmd, no addr, no dummy; just keep clocking data.
            t.base.flags   = flags | SPI_TRANS_VARIABLE_DUMMY;
            t.command_bits = 0;
            t.address_bits = 0;
        }
        t.base.length    = this_count * 16;
        t.base.tx_buffer = pixels + off;

        err = spi_device_transmit(dev->spi, (spi_transaction_t *)&t);
        if (err != ESP_OK) return err;
        off += this_count;
    }
    return ESP_OK;
}

// ---- Full initialisation sequence ------------------------------------------
// Each row is (register, value). Lifted from Arduino_GFX Arduino_NV3041A.cpp.
static const uint8_t nv3041a_init_table[][2] = {
    {0xff, 0xa5},
    {0x36, 0xc0},  // MADCTL: MX | MY | RGB (rotation 0)
    {0x3a, 0x01},  // COLMOD: 01=RGB565, 00=RGB666
    {0x41, 0x03},  // 01=8bit, 03=16bit interface
    {0x44, 0x15}, {0x45, 0x15},                   // VBP/VFP
    {0x7d, 0x03}, {0xc1, 0xbb}, {0xc2, 0x05},
    {0xc3, 0x10}, {0xc6, 0x3e}, {0xc7, 0x25},
    {0xc8, 0x11}, {0x7a, 0x5f}, {0x6f, 0x44},
    {0x78, 0x70}, {0xc9, 0x00}, {0x67, 0x21},
    // gate
    {0x51, 0x0a}, {0x52, 0x76}, {0x53, 0x0a}, {0x54, 0x76},
    // source
    {0x46, 0x0a}, {0x47, 0x2a}, {0x48, 0x0a}, {0x49, 0x1a},
    {0x56, 0x43}, {0x57, 0x42}, {0x58, 0x3c}, {0x59, 0x64},
    {0x5a, 0x41}, {0x5b, 0x3c}, {0x5c, 0x02}, {0x5d, 0x3c},
    {0x5e, 0x1f}, {0x60, 0x80}, {0x61, 0x3f}, {0x62, 0x21},
    {0x63, 0x07}, {0x64, 0xe0}, {0x65, 0x02},
    {0xca, 0x20}, {0xcb, 0x52}, {0xcc, 0x10}, {0xcD, 0x42},
    {0xD0, 0x20}, {0xD1, 0x52}, {0xD2, 0x10}, {0xD3, 0x42},
    {0xD4, 0x0a}, {0xD5, 0x32},
    // gamma
    {0x80, 0x00}, {0xA0, 0x00},
    {0x81, 0x07}, {0xA1, 0x06},
    {0x82, 0x02}, {0xA2, 0x01},
    {0x86, 0x11}, {0xA6, 0x10},
    {0x87, 0x27}, {0xA7, 0x27},
    {0x83, 0x37}, {0xA3, 0x37},
    {0x84, 0x35}, {0xA4, 0x35},
    {0x85, 0x3f}, {0xA5, 0x3f},
    {0x88, 0x0b}, {0xA8, 0x0b},
    {0x89, 0x14}, {0xA9, 0x14},
    {0x8a, 0x1a}, {0xAa, 0x1a},
    {0x8b, 0x0a}, {0xAb, 0x0a},
    {0x8c, 0x14}, {0xAc, 0x08},
    {0x8d, 0x17}, {0xAd, 0x07},
    {0x8e, 0x16}, {0xAe, 0x06},
    {0x8f, 0x1B}, {0xAf, 0x07},
    {0x90, 0x04}, {0xB0, 0x04},
    {0x91, 0x0A}, {0xB1, 0x0A},
    {0x92, 0x16}, {0xB2, 0x15},
    {0xff, 0x00},
};

esp_err_t nv3041a_create(const nv3041a_config_t *cfg, nv3041a_handle_t *out)
{
    ESP_RETURN_ON_FALSE(cfg && out, ESP_ERR_INVALID_ARG, TAG, "null arg");

    // Optional hardware reset.
    if (cfg->rst_gpio >= 0) {
        gpio_set_direction(cfg->rst_gpio, GPIO_MODE_OUTPUT);
        gpio_set_level(cfg->rst_gpio, 1);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(cfg->rst_gpio, 0);
        vTaskDelay(pdMS_TO_TICKS(120));
        gpio_set_level(cfg->rst_gpio, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    // SPI bus: 4 data lines, no MISO needed.
    // NOTE: data4..7 must be explicitly disabled, otherwise they default to
    // 0 (designated-initializer behaviour) and the SPI driver tries to claim
    // GPIO 0 three times — see the boot-time
    //   "GPIO 0 is conflict with others and be overwritten"
    // warnings. Leaving them at 0 also rewires GPIO 0 (the BOOT strap) into
    // the SPI mux which is bad.
    spi_bus_config_t bus_cfg = {
        .sclk_io_num     = cfg->sck_gpio,
        .data0_io_num    = cfg->d0_gpio,
        .data1_io_num    = cfg->d1_gpio,
        .data2_io_num    = cfg->d2_gpio,
        .data3_io_num    = cfg->d3_gpio,
        .data4_io_num    = -1,
        .data5_io_num    = -1,
        .data6_io_num    = -1,
        .data7_io_num    = -1,
        .max_transfer_sz = 8192 * 2 + 16,        // matches CHUNK_PIXELS below
        .flags           = SPICOMMON_BUSFLAG_QUAD | SPICOMMON_BUSFLAG_MASTER,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(cfg->host, &bus_cfg, SPI_DMA_CH_AUTO),
                        TAG, "spi_bus_initialize");

    spi_device_interface_config_t dev_cfg = {
        .mode           = 0,
        .clock_speed_hz = cfg->pclk_hz,
        // Hardware CS is fine here: the pixel-write path uses
        // SPI_TRANS_CS_KEEP_ACTIVE on every chunk except the last, so the
        // peripheral keeps CS asserted for the entire memory-write burst.
        .spics_io_num   = cfg->cs_gpio,
        .flags          = SPI_DEVICE_HALFDUPLEX,
        .queue_size     = 8,
    };
    nv3041a_handle_t dev = (nv3041a_handle_t)calloc(1, sizeof(*dev));
    ESP_RETURN_ON_FALSE(dev, ESP_ERR_NO_MEM, TAG, "no mem");
    ESP_RETURN_ON_ERROR(spi_bus_add_device(cfg->host, &dev_cfg, &dev->spi),
                        TAG, "spi_bus_add_device");

    // Acquire the bus for the lifetime of this device. Required by
    // SPI_TRANS_CS_KEEP_ACTIVE, and also avoids contention with any other
    // devices on this SPI host between consecutive RAMWR + pixel chunk
    // transactions.
    spi_device_acquire_bus(dev->spi, portMAX_DELAY);

    ESP_LOGI(TAG, "Sending init sequence (%d ops)", (int)(sizeof(nv3041a_init_table) / 2));
    for (size_t i = 0; i < sizeof(nv3041a_init_table) / 2; i++) {
        ESP_RETURN_ON_ERROR(nv_write_reg(dev, nv3041a_init_table[i][0],
                                              nv3041a_init_table[i][1]),
                            TAG, "init reg 0x%02x", nv3041a_init_table[i][0]);
    }

    // Sleep out + display on.
    ESP_RETURN_ON_ERROR(nv_send_cmd(dev, NV3041A_SLPOUT), TAG, "slpout");
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_RETURN_ON_ERROR(nv_send_cmd(dev, NV3041A_DISPON), TAG, "dispon");
    vTaskDelay(pdMS_TO_TICKS(50));

    *out = dev;
    return ESP_OK;
}

// Send one register write whose payload is up to 4 bytes, using the SPI
// peripheral's internal TX FIFO (SPI_TRANS_USE_TXDATA). This avoids relying
// on caller-supplied buffers being DMA-capable — task stacks on this board
// can land in PSRAM, which is *not* DMA-capable, and silently breaks small
// transfers like CASET/RASET. (That's exactly the bug we saw: flush #1
// works because the panel's post-init write pointer is still at (0,0) so
// CASET/RASET are no-ops; every later flush silently dropped its window
// update and the pixel data went into the void.)
static esp_err_t nv_write_reg_bytes_inline(nv3041a_handle_t dev, uint8_t reg,
                                            const uint8_t *data, size_t len)
{
    if (len > 4) return ESP_ERR_INVALID_ARG;
    spi_transaction_ext_t t = {};
    t.base.flags    = SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR |
                      SPI_TRANS_USE_TXDATA;
    t.command_bits  = 8;
    t.address_bits  = 24;
    t.dummy_bits    = 0;
    t.base.cmd      = NV3041A_PREFIX_WR_REG;
    t.base.addr     = ((uint32_t)reg) << 8;
    t.base.length   = len * 8;
    for (size_t i = 0; i < len; i++) t.base.tx_data[i] = data[i];
    return spi_device_transmit(dev->spi, (spi_transaction_t *)&t);
}

esp_err_t nv3041a_set_window(nv3041a_handle_t dev,
                             uint16_t x1, uint16_t y1,
                             uint16_t x2, uint16_t y2)
{
    uint8_t coords[4];

    coords[0] = x1 >> 8;  coords[1] = x1 & 0xff;
    coords[2] = x2 >> 8;  coords[3] = x2 & 0xff;
    esp_err_t err = nv_write_reg_bytes_inline(dev, NV3041A_CASET, coords, 4);
    if (err != ESP_OK) return err;

    coords[0] = y1 >> 8;  coords[1] = y1 & 0xff;
    coords[2] = y2 >> 8;  coords[3] = y2 & 0xff;
    return nv_write_reg_bytes_inline(dev, NV3041A_RASET, coords, 4);
}

esp_err_t nv3041a_write_pixels(nv3041a_handle_t dev,
                               const uint16_t *pixels, size_t count)
{
    return nv_write_pixels_raw(dev, pixels, count);
}
