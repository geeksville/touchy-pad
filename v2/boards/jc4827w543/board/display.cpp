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

// LVGL flush callback: ship one rectangle to the panel and tell LVGL we're
// done. LVGL serialises calls so we don't need our own mutex here.
static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
                          lv_color_t *color_p)
{
    const uint16_t x1 = area->x1, y1 = area->y1;
    const uint16_t x2 = area->x2, y2 = area->y2;
    const size_t   n  = (size_t)(x2 - x1 + 1) * (y2 - y1 + 1);

    nv3041a_set_window(s_panel, x1, y1, x2, y2);
    nv3041a_write_pixels(s_panel, (uint16_t *)color_p, n);
    lv_disp_flush_ready(drv);
}

extern "C" lv_disp_t *display_init(void)
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
    // Allocate a partial draw buffer in internal RAM (~10 lines = ~10 KB).
    // Two buffers so LVGL can ping-pong.
    constexpr size_t LINES_PER_BUF = 40;
    const size_t buf_px = BOARD_LCD_H_RES * LINES_PER_BUF;
    auto *buf1 = (lv_color_t *)heap_caps_malloc(buf_px * sizeof(lv_color_t),
                                                MALLOC_CAP_DMA);
    auto *buf2 = (lv_color_t *)heap_caps_malloc(buf_px * sizeof(lv_color_t),
                                                MALLOC_CAP_DMA);
    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "Failed to allocate LVGL draw buffers");
        return nullptr;
    }

    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, buf_px);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res  = BOARD_LCD_H_RES;
    disp_drv.ver_res  = BOARD_LCD_V_RES;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &draw_buf;

    lvgl_port_lock(0);
    lv_disp_t *disp = lv_disp_drv_register(&disp_drv);
    lvgl_port_unlock();
    if (!disp) {
        ESP_LOGE(TAG, "lv_disp_drv_register failed");
    }
    return disp;
}
