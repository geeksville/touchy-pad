// SPDX-License-Identifier: GPL-3.0-or-later

#include "touch.h"

#include "board.h"
#include "board_pins.h"
#include "tc_tag.h"

#include "esp_log.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lvgl_port.h"

static const char *TAG = TOUCHY_TAG("touch");

static lv_indev_t            *s_indev = nullptr;
static esp_lcd_touch_handle_t s_tp    = nullptr;

lv_indev_t *touch_get_indev(void) { return s_indev; }

static uint8_t probe_gt911_addr(i2c_master_bus_handle_t bus)
{
    if (i2c_master_probe(bus, BOARD_TOUCH_I2C_ADDR_PRIMARY, 50) == ESP_OK) {
        return BOARD_TOUCH_I2C_ADDR_PRIMARY;
    }
    if (i2c_master_probe(bus, BOARD_TOUCH_I2C_ADDR_BACKUP, 50) == ESP_OK) {
        return BOARD_TOUCH_I2C_ADDR_BACKUP;
    }
    return 0;
}

static esp_lcd_touch_handle_t create_gt911(void)
{
    uint8_t addr = probe_gt911_addr(board_get_i2c_bus());
    if (addr == 0) {
        ESP_LOGE(TAG, "GT911 not found at 0x%02x or 0x%02x",
                 BOARD_TOUCH_I2C_ADDR_PRIMARY, BOARD_TOUCH_I2C_ADDR_BACKUP);
        return nullptr;
    }
    ESP_LOGI(TAG, "GT911 found at 0x%02x", addr);

    esp_lcd_panel_io_handle_t tp_io = nullptr;
    esp_lcd_panel_io_i2c_config_t io_cfg = {};
    io_cfg.dev_addr             = addr;
    io_cfg.scl_speed_hz         = BOARD_I2C_CLK_HZ;
    io_cfg.control_phase_bytes  = 1;
    io_cfg.dc_bit_offset        = 0;
    io_cfg.lcd_cmd_bits         = 16;
    io_cfg.lcd_param_bits       = 0;
    io_cfg.flags.disable_control_phase = 1;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(board_get_i2c_bus(), &io_cfg, &tp_io));

    esp_lcd_touch_config_t tp_cfg = {};
    tp_cfg.x_max        = BOARD_LCD_H_RES;
    tp_cfg.y_max        = BOARD_LCD_V_RES;
    tp_cfg.rst_gpio_num = BOARD_TOUCH_RST_GPIO;  // GT911 driver handles reset
    tp_cfg.int_gpio_num = BOARD_TOUCH_INT_GPIO;
    tp_cfg.levels.reset     = 0;
    tp_cfg.levels.interrupt = 0;
    tp_cfg.flags.swap_xy    = 0;
    tp_cfg.flags.mirror_x   = 0;
    tp_cfg.flags.mirror_y   = 0;

    esp_lcd_touch_handle_t tp = nullptr;
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &tp));
    return tp;
}

extern "C" esp_lcd_touch_handle_t touch_init(lv_display_t *disp)
{
    if (s_tp == nullptr) {
        s_tp = create_gt911();
    }
    if (s_tp == nullptr) {
        return nullptr;
    }

    lvgl_port_touch_cfg_t lv_cfg = {};
    lv_cfg.disp   = disp;
    lv_cfg.handle = s_tp;
    s_indev = lvgl_port_add_touch(&lv_cfg);
    if (s_indev == nullptr) {
        ESP_LOGW(TAG, "lvgl_port_add_touch failed");
    }
    return s_tp;
}
