// SPDX-License-Identifier: Apache-2.0

#include "display.h"

#include "board.h"
#include "board_pins.h"

#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lvgl_port.h"

static const char *TAG = "display";

extern "C" lv_display_t *display_init(void)
{
    ESP_LOGI(TAG, "Configuring %dx%d RGB panel @ %d MHz",
             BOARD_LCD_H_RES, BOARD_LCD_V_RES,
             BOARD_LCD_PIXEL_CLOCK_HZ / 1000000);

    esp_lcd_rgb_panel_config_t panel_cfg = {};
    panel_cfg.clk_src       = LCD_CLK_SRC_DEFAULT;
    panel_cfg.data_width    = 16;
    panel_cfg.in_color_format  = LCD_COLOR_FMT_RGB565;
    panel_cfg.out_color_format = LCD_COLOR_FMT_RGB565;
    panel_cfg.num_fbs       = 2;            // double buffer in PSRAM
    panel_cfg.bounce_buffer_size_px = BOARD_LCD_H_RES * 10;
    panel_cfg.dma_burst_size = 64;
    panel_cfg.hsync_gpio_num = BOARD_LCD_GPIO_HSYNC;
    panel_cfg.vsync_gpio_num = BOARD_LCD_GPIO_VSYNC;
    panel_cfg.de_gpio_num    = BOARD_LCD_GPIO_DE;
    panel_cfg.pclk_gpio_num  = BOARD_LCD_GPIO_PCLK;
    panel_cfg.disp_gpio_num  = BOARD_LCD_GPIO_DISP;
    panel_cfg.data_gpio_nums[0]  = BOARD_LCD_GPIO_DATA0;
    panel_cfg.data_gpio_nums[1]  = BOARD_LCD_GPIO_DATA1;
    panel_cfg.data_gpio_nums[2]  = BOARD_LCD_GPIO_DATA2;
    panel_cfg.data_gpio_nums[3]  = BOARD_LCD_GPIO_DATA3;
    panel_cfg.data_gpio_nums[4]  = BOARD_LCD_GPIO_DATA4;
    panel_cfg.data_gpio_nums[5]  = BOARD_LCD_GPIO_DATA5;
    panel_cfg.data_gpio_nums[6]  = BOARD_LCD_GPIO_DATA6;
    panel_cfg.data_gpio_nums[7]  = BOARD_LCD_GPIO_DATA7;
    panel_cfg.data_gpio_nums[8]  = BOARD_LCD_GPIO_DATA8;
    panel_cfg.data_gpio_nums[9]  = BOARD_LCD_GPIO_DATA9;
    panel_cfg.data_gpio_nums[10] = BOARD_LCD_GPIO_DATA10;
    panel_cfg.data_gpio_nums[11] = BOARD_LCD_GPIO_DATA11;
    panel_cfg.data_gpio_nums[12] = BOARD_LCD_GPIO_DATA12;
    panel_cfg.data_gpio_nums[13] = BOARD_LCD_GPIO_DATA13;
    panel_cfg.data_gpio_nums[14] = BOARD_LCD_GPIO_DATA14;
    panel_cfg.data_gpio_nums[15] = BOARD_LCD_GPIO_DATA15;
    panel_cfg.timings.pclk_hz           = BOARD_LCD_PIXEL_CLOCK_HZ;
    panel_cfg.timings.h_res             = BOARD_LCD_H_RES;
    panel_cfg.timings.v_res             = BOARD_LCD_V_RES;
    panel_cfg.timings.hsync_pulse_width = BOARD_LCD_HSYNC_PULSE_WIDTH;
    panel_cfg.timings.hsync_back_porch  = BOARD_LCD_HSYNC_BACK_PORCH;
    panel_cfg.timings.hsync_front_porch = BOARD_LCD_HSYNC_FRONT_PORCH;
    panel_cfg.timings.vsync_pulse_width = BOARD_LCD_VSYNC_PULSE_WIDTH;
    panel_cfg.timings.vsync_back_porch  = BOARD_LCD_VSYNC_BACK_PORCH;
    panel_cfg.timings.vsync_front_porch = BOARD_LCD_VSYNC_FRONT_PORCH;
    panel_cfg.timings.flags.pclk_active_neg = BOARD_LCD_PCLK_ACTIVE_NEG;
    panel_cfg.flags.fb_in_psram = 1;

    esp_lcd_panel_handle_t panel = nullptr;
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));

    // ----- Start LVGL port task -----
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 4;
    port_cfg.task_stack    = 8 * 1024;
    port_cfg.timer_period_ms = 5;
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    // ----- Register panel with LVGL -----
    lvgl_port_display_cfg_t disp_cfg = {};
    disp_cfg.panel_handle    = panel;
    disp_cfg.buffer_size     = BOARD_LCD_H_RES * BOARD_LCD_V_RES;
    disp_cfg.double_buffer   = true;
    disp_cfg.hres            = BOARD_LCD_H_RES;
    disp_cfg.vres            = BOARD_LCD_V_RES;
    disp_cfg.monochrome      = false;
    disp_cfg.color_format    = LV_COLOR_FORMAT_RGB565;
    disp_cfg.flags.buff_dma  = false;
    disp_cfg.flags.buff_spiram = true;

    lvgl_port_display_rgb_cfg_t rgb_cfg = {};
    rgb_cfg.flags.bb_mode     = true;   // bounce buffer mode
    rgb_cfg.flags.avoid_tearing = true;

    lv_display_t *disp = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
    if (disp == nullptr) {
        ESP_LOGE(TAG, "lvgl_port_add_disp_rgb failed");
    }
    return disp;
}
