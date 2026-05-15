// SPDX-License-Identifier: Apache-2.0
//
// LVGL display bring-up for the JC4827W543 board:
//   1. Pull backlight GPIO high.
//   2. Initialise the NV3041A QSPI panel (see nv3041a.c).
//   3. Register a software LVGL display object that flushes via direct
//      spi_master calls (no esp_lcd panel-io – esp_lcd's SPI panel-io does
//      not natively speak the NV3041A QSPI framing).

#include "display.h"

#include "board.h"
#include "board_pins.h"
#include "nv3041a.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"

static const char *TAG = "display";

static nv3041a_handle_t s_panel = nullptr;

// SPI ISR callback: fires when the final byte of a flush has left the wire.
// Tells LVGL the buffer is free for reuse so it can start the next slice
// concurrently with whatever the LVGL task is doing. lv_display_flush_ready()
// only mutates a couple of flags on the display object — safe to call from an
// ISR (same pattern as esp_lcd_panel_io_spi's on_color_trans_done hook).
//
// Marked IRAM_ATTR so it remains callable while flash cache is disabled.
static IRAM_ATTR void flush_done_isr(void *user)
{
    auto *disp = static_cast<lv_display_t *>(user);
    lv_display_flush_ready(disp);
}

// LVGL flush callback: queue the SPI transactions and return immediately;
// flush_done_isr() will report completion from the ISR once the panel has
// actually consumed the buffer. This lets LVGL render the next slice into
// the other partial buffer while the SPI peripheral DMAs this one out.
// LVGL serialises flush_cb calls so we don't need our own mutex here.
//
// The NV3041A QSPI panel expects MSB-first RGB565 on the wire. LVGL v9
// renders RGB565 in native (little-endian) byte order, so we swap in place
// before handing the buffer to the SPI peripheral.
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area,
                          uint8_t *px_map)
{
    const uint16_t x1 = area->x1, y1 = area->y1;
    const uint16_t x2 = area->x2, y2 = area->y2;
    const size_t   n  = (size_t)(x2 - x1 + 1) * (y2 - y1 + 1);

    lv_draw_sw_rgb565_swap(px_map, n);

    nv3041a_set_window(s_panel, x1, y1, x2, y2);
    nv3041a_write_pixels(s_panel, reinterpret_cast<uint16_t *>(px_map), n,
                         flush_done_isr, disp);
}

extern "C" lv_display_t *display_init(void)
{
    // Backlight high.
    if (BOARD_LCD_GPIO_BACKLIGHT != GPIO_NUM_NC) {
        gpio_config_t bl = {
            .pin_bit_mask = 1ULL << BOARD_LCD_GPIO_BACKLIGHT,
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&bl);
        gpio_set_level(BOARD_LCD_GPIO_BACKLIGHT, 1);
    }

    ESP_LOGI(TAG, "Configuring NV3041A QSPI panel %dx%d @ %d MHz",
             BOARD_LCD_H_RES, BOARD_LCD_V_RES,
             BOARD_LCD_QSPI_CLK_HZ / 1000000);

    nv3041a_config_t cfg = {
        .host     = BOARD_LCD_QSPI_HOST,
        .cs_gpio  = BOARD_LCD_GPIO_CS,
        .sck_gpio = BOARD_LCD_GPIO_SCK,
        .d0_gpio  = BOARD_LCD_GPIO_D0,
        .d1_gpio  = BOARD_LCD_GPIO_D1,
        .d2_gpio  = BOARD_LCD_GPIO_D2,
        .d3_gpio  = BOARD_LCD_GPIO_D3,
        .rst_gpio = (BOARD_LCD_GPIO_RST == GPIO_NUM_NC) ? -1 : BOARD_LCD_GPIO_RST,
        .pclk_hz  = BOARD_LCD_QSPI_CLK_HZ,
    };
    ESP_ERROR_CHECK(nv3041a_create(&cfg, &s_panel));

    // ----- LVGL port task -----
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority   = 4;
    port_cfg.task_stack      = 8 * 1024;
    port_cfg.timer_period_ms = 5;
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    // ----- Software LVGL display (no esp_lcd) -----
    // Allocate two partial draw buffers in DMA-capable internal RAM so LVGL
    // can ping-pong while the SPI peripheral streams the previous slice.
    constexpr size_t LINES_PER_BUF = 40;
    const size_t buf_px    = BOARD_LCD_H_RES * LINES_PER_BUF;
    const size_t buf_bytes = buf_px * sizeof(uint16_t);  // RGB565
    auto *buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA);
    auto *buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA);
    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "Failed to allocate LVGL draw buffers");
        return nullptr;
    }

    lvgl_port_lock(0);
    lv_display_t *disp = lv_display_create(BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    if (!disp) {
        lvgl_port_unlock();
        ESP_LOGE(TAG, "lv_display_create failed");
        return nullptr;
    }
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, buf1, buf2, buf_bytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, lvgl_flush_cb);
    lvgl_port_unlock();

    return disp;
}
