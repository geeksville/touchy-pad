// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared XPT2046 resistive-touch bring-up for the "Cheap Yellow Display"
// (CYD2USB) family of classic-ESP32 boards.
//
// The XPT2046 hangs off its own dedicated SPI bus (separate from the display
// bus), so we initialise that bus here, attach the touch panel-io, create the
// esp_lcd_touch handle, and register it with esp_lvgl_port. Being resistive it
// reports a single contact point — TOUCH_MAX_POINTS still bounds the buffer
// the trackpad widget reads, but only one slot is ever populated.
//
// Every CYD variant wires the XPT2046 identically; the only per-board
// differences (resolution, orientation flags) come from board_pins.h.

#include "touch.h"
#include "tc_tag.h"

#include "board_pins.h"

#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_xpt2046.h"
#include "esp_lvgl_port.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = TOUCHY_TAG("touch");

static lv_indev_t            *s_indev = nullptr;
static esp_lcd_touch_handle_t s_tp    = nullptr;
static esp_lcd_panel_io_handle_t s_tp_io = nullptr;

// XPT2046 command bytes (PD0=1, matching the driver's non-interrupt mode).
enum {
    XPT_Z1   = 0xB1,
    XPT_Z2   = 0xC1,
    XPT_X    = 0xD1,
    XPT_Y    = 0x91,
    // Auxiliary single-ended channels with internal VREF enabled. These read a
    // stable, non-zero value whenever the chip is powered and MISO is wired —
    // independent of touch — so they double as a "is MISO connected?" probe.
    XPT_TEMP = 0x87,   // internal temperature diode (~0x600 at room temp)
    XPT_VBAT = 0xA7,   // battery monitor input
    XPT_AUX  = 0xE7,   // auxiliary ADC input
};

// Read one 12-bit XPT2046 channel directly over SPI.
static uint16_t xpt_read_raw(uint8_t reg)
{
    uint8_t buf[2] = {0, 0};
    if (esp_lcd_panel_io_rx_param(s_tp_io, reg, buf, 2) != ESP_OK) {
        return 0xFFFF;
    }
    return (((buf[0] << 8) | buf[1]) >> 3) & 0x0FFF;   // 12-bit
}

// One-shot MISO sanity check: read the internal aux channels. If these come
// back all-zero (or all-0xFFF) the MISO line is not delivering data.
static void xpt_miso_probe(void)
{
    uint16_t temp = xpt_read_raw(XPT_TEMP);
    uint16_t vbat = xpt_read_raw(XPT_VBAT);
    uint16_t aux  = xpt_read_raw(XPT_AUX);
    bool alive = (temp != 0 && temp != 0x0FFF) ||
                 (vbat != 0 && vbat != 0x0FFF) ||
                 (aux  != 0 && aux  != 0x0FFF);
    ESP_LOGI(TAG, "MISO probe: TEMP0=%u VBAT=%u AUX=%u  → %s",
             temp, vbat, aux,
             alive ? "MISO ALIVE (chip responding)"
                   : "MISO DEAD (all 0x000/0xFFF — check wiring/CS/clock)");
}

// Debug task: polls the PENIRQ GPIO and calls read_data directly so we can
// see raw z/x/y values independent of the LVGL event-mode indev. Runs at
// 20 Hz — fast enough to catch a normal touch press.
static void touch_debug_task(void *)
{
    int      prev_level = 1;    // assume pen-up at start
    uint32_t polls      = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));
        int level = gpio_get_level((gpio_num_t)BOARD_TOUCH_GPIO_IRQ);

        if (level != prev_level) {
            ESP_LOGI(TAG, "PENIRQ GPIO%d → %s",
                     (int)BOARD_TOUCH_GPIO_IRQ,
                     level == 0 ? "LOW (pen-down)" : "HIGH (pen-up)");
            prev_level = level;
        }

        // When pin is LOW, read the raw XPT2046 channels directly so we can see
        // the actual pressure (z) and position the chip reports — bypassing the
        // driver's z-threshold and 50..4045 validity gate entirely.
        if (level == 0 && s_tp_io) {
            uint16_t z1 = xpt_read_raw(XPT_Z1);
            uint16_t z2 = xpt_read_raw(XPT_Z2);
            int      z  = (int)z1 + (4096 - (int)z2);     // same formula as driver
            uint16_t xr = xpt_read_raw(XPT_X);
            (void)xpt_read_raw(XPT_X);                    // discard first (driver does)
            uint16_t x  = xpt_read_raw(XPT_X);
            uint16_t y  = xpt_read_raw(XPT_Y);
            ESP_LOGI(TAG, "  raw: z1=%u z2=%u z=%d (thresh=%d)  x=%u y=%u  (xfirst=%u)",
                     z1, z2, z, CONFIG_XPT2046_Z_THRESHOLD, x, y, xr);
        }

        // Keep-alive every 5 s so we know the task is running, and re-probe the
        // MISO line so we can tell wiring faults from a simply-untouched panel.
        if (++polls % 100 == 0) {
            ESP_LOGI(TAG, "touch monitor alive (PENIRQ=%d)", level);
            xpt_miso_probe();
        }
    }
}

// process_coordinates: only fires when get_xy() returns true (touch detected
// AND z ≥ threshold). Shows raw coords before driver swap/mirror.
static void touch_log_coords(esp_lcd_touch_handle_t /*tp*/,
                              uint16_t *x, uint16_t *y, uint16_t *strength,
                              uint8_t *point_num, uint8_t /*max*/)
{
    if (*point_num > 0) {
        ESP_LOGI(TAG, "raw touch (process_coordinates): count=%u  x=%-4u y=%-4u  strength=%u",
                 *point_num, x[0], y[0], strength ? strength[0] : 0u);
    }
}

lv_indev_t *touch_get_indev(void) { return s_indev; }

extern "C" esp_lcd_touch_handle_t touch_init(lv_display_t *disp)
{
    ESP_LOGI(TAG, "Initialising XPT2046 (CS=%d IRQ=%d)",
             (int)BOARD_TOUCH_GPIO_CS, (int)BOARD_TOUCH_GPIO_IRQ);

    // ----- Dedicated SPI bus for the touch controller -----
    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num   = BOARD_TOUCH_GPIO_MOSI;
    bus_cfg.miso_io_num   = BOARD_TOUCH_GPIO_MISO;
    bus_cfg.sclk_io_num   = BOARD_TOUCH_GPIO_SCK;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    ESP_ERROR_CHECK(spi_bus_initialize(BOARD_TOUCH_SPI_HOST, &bus_cfg,
                                       SPI_DMA_CH_AUTO));

    // ----- Touch panel IO -----
    esp_lcd_panel_io_handle_t tp_io = nullptr;
    // ESP_LCD_TOUCH_IO_SPI_XPT2046_CONFIG doesn't initialise cs_ena_pretrans /
    // cs_ena_posttrans (they didn't exist in older IDF versions). Suppress the
    // -Wmissing-field-initializers diagnostic for this one macro expansion only;
    // the zero value is correct for both fields.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
    esp_lcd_panel_io_spi_config_t io_cfg = ESP_LCD_TOUCH_IO_SPI_XPT2046_CONFIG(
        BOARD_TOUCH_GPIO_CS);
#pragma GCC diagnostic pop
    io_cfg.pclk_hz = BOARD_TOUCH_PIXEL_CLOCK_HZ;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)BOARD_TOUCH_SPI_HOST, &io_cfg, &tp_io));
    s_tp_io = tp_io;   // give the debug task direct register access

    // ----- esp_lcd_touch handle -----
    esp_lcd_touch_config_t tp_cfg = {};
    tp_cfg.x_max        = BOARD_LCD_H_RES;
    tp_cfg.y_max        = BOARD_LCD_V_RES;
    tp_cfg.rst_gpio_num = GPIO_NUM_NC;
    tp_cfg.int_gpio_num = BOARD_TOUCH_GPIO_IRQ;
    // The XPT2046 raw axes are rotated/mirrored relative to the (already
    // rotated) display framebuffer; match the display orientation so taps land
    // under the finger. Tune alongside the BOARD_LCD_* orientation flags.
    tp_cfg.flags.swap_xy  = BOARD_LCD_SWAP_XY;
    tp_cfg.flags.mirror_x = BOARD_LCD_MIRROR_X;
    tp_cfg.flags.mirror_y = BOARD_LCD_MIRROR_Y;
    // Debug callbacks: touch_irq_cb counts IRQ edges (ISR-safe); touch_log_coords
    // logs raw coordinates from task context so we can see whether events arrive
    // and what values the ADC produces.
    tp_cfg.process_coordinates = touch_log_coords;
    // NOTE: do NOT set tp_cfg.interrupt_callback — that slot is owned by the
    // XPT2046 driver itself to signal pen-down to esp_lcd_touch_read_data().
    // Overriding it prevents read_data from ever doing an SPI read.

    esp_lcd_touch_handle_t tp = nullptr;
    ESP_ERROR_CHECK(esp_lcd_touch_new_spi_xpt2046(tp_io, &tp_cfg, &tp));

    // Log the PENIRQ pin level at rest — should be HIGH (active-low).
    // Stuck-LOW here means the panel is permanently asserting touch-down
    // (wiring issue or GPIO conflict).
    int irq_level = gpio_get_level((gpio_num_t)BOARD_TOUCH_GPIO_IRQ);
    ESP_LOGI(TAG, "XPT2046 ready — PENIRQ GPIO%d level at rest: %s (%d)",
             (int)BOARD_TOUCH_GPIO_IRQ,
             irq_level == 0 ? "LOW (pen-down / possible short!)" : "HIGH (idle, ok)",
             irq_level);

    s_tp = tp;   // give the debug task access to the handle

    // Immediately probe the MISO line via the aux channels (touch-independent).
    xpt_miso_probe();

    lvgl_port_touch_cfg_t lv_cfg = {};
    lv_cfg.disp   = disp;
    lv_cfg.handle = tp;
    s_indev = lvgl_port_add_touch(&lv_cfg);
    if (s_indev == nullptr) {
        ESP_LOGW(TAG, "lvgl_port_add_touch failed");
    }

    // Spawn the low-priority debug task that polls PENIRQ + calls read_data
    // directly, so we can see raw SPI data independently of the LVGL indev.
    xTaskCreate(touch_debug_task, "touch_dbg", 4096, nullptr, 1, nullptr);
    ESP_LOGI(TAG, "touch debug task started");

    return tp;
}
