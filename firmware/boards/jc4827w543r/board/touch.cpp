// SPDX-License-Identifier: GPL-3.0-or-later

#include "touch.h"
#include "tc_tag.h"

#include "board_pins.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_xpt2046.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

static const char *TAG = TOUCHY_TAG("touch");

static lv_indev_t *s_indev = nullptr;

lv_indev_t *touch_get_indev(void) { return s_indev; }

extern "C" esp_lcd_touch_handle_t touch_init(lv_display_t *disp)
{
    ESP_LOGI(TAG, "Initialising XPT2046 (CS=%d IRQ=%d)",
             (int)BOARD_TOUCH_GPIO_CS, (int)BOARD_TOUCH_GPIO_IRQ);

    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num   = BOARD_TOUCH_GPIO_MOSI;
    bus_cfg.miso_io_num   = BOARD_TOUCH_GPIO_MISO;
    bus_cfg.sclk_io_num   = BOARD_TOUCH_GPIO_SCK;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    ESP_ERROR_CHECK(spi_bus_initialize(BOARD_TOUCH_SPI_HOST, &bus_cfg,
                                       SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t tp_io = nullptr;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
    esp_lcd_panel_io_spi_config_t io_cfg = ESP_LCD_TOUCH_IO_SPI_XPT2046_CONFIG(
        BOARD_TOUCH_GPIO_CS);
#pragma GCC diagnostic pop
    io_cfg.pclk_hz = BOARD_TOUCH_PIXEL_CLOCK_HZ;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)BOARD_TOUCH_SPI_HOST, &io_cfg, &tp_io));

    esp_lcd_touch_config_t tp_cfg = {};
    tp_cfg.x_max        = BOARD_LCD_H_RES;
    tp_cfg.y_max        = BOARD_LCD_V_RES;
    tp_cfg.rst_gpio_num = GPIO_NUM_NC;
    tp_cfg.int_gpio_num = BOARD_TOUCH_GPIO_IRQ;
    tp_cfg.flags.swap_xy  = 0;
    tp_cfg.flags.mirror_x = 0;
    tp_cfg.flags.mirror_y = 0;

    esp_lcd_touch_handle_t tp = nullptr;
    ESP_ERROR_CHECK(esp_lcd_touch_new_spi_xpt2046(tp_io, &tp_cfg, &tp));

    lvgl_port_touch_cfg_t lv_cfg = {};
    lv_cfg.disp   = disp;
    lv_cfg.handle = tp;
    s_indev = lvgl_port_add_touch(&lv_cfg);
    if (s_indev == nullptr) {
        ESP_LOGW(TAG, "lvgl_port_add_touch failed");
    }
    return tp;
}
