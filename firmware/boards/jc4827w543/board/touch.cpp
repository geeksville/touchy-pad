// SPDX-License-Identifier: Apache-2.0

#include "touch.h"

#include "board.h"
#include "board_pins.h"

#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lvgl_port.h"

static const char *TAG = "touch";

extern "C" esp_lcd_touch_handle_t touch_init(lv_display_t *disp)
{
    ESP_LOGI(TAG, "Initialising GT911 (addr 0x%02x, RST=%d INT=%d)",
             BOARD_TOUCH_I2C_ADDR,
             (int)BOARD_TOUCH_RST_GPIO, (int)BOARD_TOUCH_INT_GPIO);

    esp_lcd_panel_io_handle_t tp_io = nullptr;
    esp_lcd_panel_io_i2c_config_t io_cfg = {};
    io_cfg.dev_addr             = BOARD_TOUCH_I2C_ADDR;
    io_cfg.scl_speed_hz         = 400000;
    io_cfg.control_phase_bytes  = 1;
    io_cfg.dc_bit_offset        = 0;
    io_cfg.lcd_cmd_bits         = 16;
    io_cfg.lcd_param_bits       = 0;
    io_cfg.flags.disable_control_phase = 1;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(board_get_i2c_bus(), &io_cfg, &tp_io));

    esp_lcd_touch_config_t tp_cfg = {};
    tp_cfg.x_max        = BOARD_LCD_H_RES;
    tp_cfg.y_max        = BOARD_LCD_V_RES;
    tp_cfg.rst_gpio_num = BOARD_TOUCH_RST_GPIO;
    tp_cfg.int_gpio_num = BOARD_TOUCH_INT_GPIO;
    tp_cfg.levels.reset     = 0;
    tp_cfg.levels.interrupt = 0;
    tp_cfg.flags.swap_xy    = 0;
    tp_cfg.flags.mirror_x   = 0;
    tp_cfg.flags.mirror_y   = 0;

    esp_lcd_touch_handle_t tp = nullptr;
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &tp));

    lvgl_port_touch_cfg_t lv_cfg = {};
    lv_cfg.disp   = disp;
    lv_cfg.handle = tp;
    if (lvgl_port_add_touch(&lv_cfg) == nullptr) {
        ESP_LOGW(TAG, "lvgl_port_add_touch failed");
    }
    return tp;
}
