// SPDX-License-Identifier: GPL-3.0-or-later

#include "display.h"
#include "board.h"
#include "board_pins.h"
#include "tc_tag.h"

#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = TOUCHY_TAG("display");

// ST7701S init command sequence (ported from SQUiXL squixl.h / CIC project).
// Format: [len, cmd, data...] where len=0xff is a delay and next byte is ms.
// Terminated by a zero byte.
static const uint8_t st7701s_init_commands[] = {
    1, 0x11,
    0xff, 120,
    6, 0xff, 0x77, 0x01, 0x00, 0x00, 0x10,
    3, 0xc0, 0x3b, 0x00,
    3, 0xc1, 0x0d, 0x02,
    3, 0xc2, 0x21, 0x08,
    2, 0xcd, 0x08,
    17, 0xb0, 0x00, 0x11, 0x18, 0x0e, 0x11, 0x06, 0x07, 0x08, 0x07, 0x22, 0x04, 0x12, 0x0f, 0xaa, 0x31, 0x18,
    17, 0xb1, 0x00, 0x11, 0x19, 0x0e, 0x12, 0x07, 0x08, 0x08, 0x08, 0x22, 0x04, 0x11, 0x11, 0xa9, 0x32, 0x18,
    6, 0xff, 0x77, 0x01, 0x00, 0x00, 0x11,
    2, 0xb0, 0x60,
    2, 0xb1, 0x30,
    2, 0xb2, 0x87,
    2, 0xb3, 0x80,
    2, 0xb5, 0x49,
    2, 0xb7, 0x85,
    2, 0xb8, 0x21,
    2, 0xc1, 0x78,
    2, 0xc2, 0x78,
    0xff, 20,
    4, 0xe0, 0x00, 0x1b, 0x02,
    12, 0xe1, 0x08, 0xa0, 0x00, 0x00, 0x07, 0xa0, 0x00, 0x00, 0x00, 0x44, 0x44,
    13, 0xe2, 0x11, 0x11, 0x44, 0x44, 0xed, 0xa0, 0x00, 0x00, 0xec, 0xa0, 0x00, 0x00,
    5, 0xe3, 0x00, 0x00, 0x11, 0x11,
    3, 0xe4, 0x44, 0x44,
    17, 0xe5, 0x0a, 0xe9, 0xd8, 0xa0, 0x0c, 0xeb, 0xd8, 0xa0, 0x0e, 0xed, 0xd8, 0xa0, 0x10, 0xef, 0xd8, 0xa0,
    5, 0xe6, 0x00, 0x00, 0x11, 0x11,
    3, 0xe7, 0x44, 0x44,
    17, 0xe8, 0x09, 0xe8, 0xd8, 0xa0, 0x0b, 0xea, 0xd8, 0xa0, 0x0d, 0xec, 0xd8, 0xa0, 0x0f, 0xee, 0xd8, 0xa0,
    8, 0xeb, 0x02, 0x00, 0xe4, 0xe4, 0x88, 0x00, 0x40,
    3, 0xec, 0x3c, 0x00,
    17, 0xed, 0xab, 0x89, 0x76, 0x54, 0x02, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x20, 0x45, 0x67, 0x98, 0xba,
    6, 0xff, 0x77, 0x01, 0x00, 0x00, 0x00,
    2, 0x36, 0x00,
    2, 0x3a, 0x66,
    1, 0x21,
    0xff, 120,
    0xff, 120,
    1, 0x29,
    0xff, 120,
    0  // terminator
};

// 9-bit SPI: D/C bit then 8 data bits, MSB first. Bit-banged over LCA9555.
static void bitbang_write_byte(Lca9555 &ioex, uint8_t byte, bool is_data)
{
    ioex.write(BOARD_IOEX_SPI_CLK,  false);
    ioex.write(BOARD_IOEX_SPI_MOSI, is_data);
    ioex.write(BOARD_IOEX_SPI_CLK,  true);

    for (int i = 7; i >= 0; i--) {
        ioex.write(BOARD_IOEX_SPI_CLK,  false);
        ioex.write(BOARD_IOEX_SPI_MOSI, (byte >> i) & 0x01);
        ioex.write(BOARD_IOEX_SPI_CLK,  true);
    }
}

static void send_st7701s_commands(Lca9555 &ioex, const uint8_t *data)
{
    ioex.write(BOARD_IOEX_SPI_CS,   true);
    ioex.write(BOARD_IOEX_SPI_CLK,  false);
    ioex.write(BOARD_IOEX_SPI_MOSI, false);

    while (*data) {
        uint8_t len = *data++;

        if (len == 0xff) {
            vTaskDelay(pdMS_TO_TICKS(*data++));
            continue;
        }

        uint8_t cmd = *data++;
        ioex.write(BOARD_IOEX_SPI_CS, false);
        bitbang_write_byte(ioex, cmd, false);  // command byte: DC=0
        for (uint8_t i = 0; i < len - 1; i++) {
            bitbang_write_byte(ioex, *data++, true);  // data bytes: DC=1
        }
        ioex.write(BOARD_IOEX_SPI_CS, true);
    }
}

static void init_st7701s(Lca9555 &ioex)
{
    ioex.write(BOARD_IOEX_LCD_RST, false);
    vTaskDelay(pdMS_TO_TICKS(10));
    ioex.write(BOARD_IOEX_LCD_RST, true);
    vTaskDelay(pdMS_TO_TICKS(120));
    send_st7701s_commands(ioex, st7701s_init_commands);
}

// Stage lb7 — this board's Display subclass + factory.
namespace {
class BoardLCDDisplay : public Display {
protected:
    lv_display_t *hw_init() override;
};
}  // namespace

Display *display_create(void)
{
    return new BoardLCDDisplay();
}

lv_display_t *BoardLCDDisplay::hw_init(void)
{
    Lca9555 *ioex = board_get_ioex();

    ESP_LOGI(TAG, "Initialising ST7701S via LCA9555 bit-bang SPI");
    init_st7701s(*ioex);

    ESP_LOGI(TAG, "Configuring %dx%d RGB panel @ %d MHz",
             BOARD_LCD_H_RES, BOARD_LCD_V_RES,
             BOARD_LCD_PIXEL_CLOCK_HZ / 1000000);

    esp_lcd_rgb_panel_config_t panel_cfg = {};
    panel_cfg.clk_src            = LCD_CLK_SRC_DEFAULT;
    panel_cfg.data_width         = 16;
    panel_cfg.in_color_format    = LCD_COLOR_FMT_RGB565;
    panel_cfg.out_color_format   = LCD_COLOR_FMT_RGB565;
    panel_cfg.num_fbs            = 1;
    panel_cfg.bounce_buffer_size_px = BOARD_LCD_H_RES * 10;
    panel_cfg.dma_burst_size     = 64;
    panel_cfg.hsync_gpio_num     = BOARD_LCD_HSYNC;
    panel_cfg.vsync_gpio_num     = BOARD_LCD_VSYNC;
    panel_cfg.de_gpio_num        = BOARD_LCD_DE;
    panel_cfg.pclk_gpio_num      = BOARD_LCD_PCLK;
    panel_cfg.disp_gpio_num      = GPIO_NUM_NC;
    panel_cfg.data_gpio_nums[0]  = BOARD_LCD_D0;
    panel_cfg.data_gpio_nums[1]  = BOARD_LCD_D1;
    panel_cfg.data_gpio_nums[2]  = BOARD_LCD_D2;
    panel_cfg.data_gpio_nums[3]  = BOARD_LCD_D3;
    panel_cfg.data_gpio_nums[4]  = BOARD_LCD_D4;
    panel_cfg.data_gpio_nums[5]  = BOARD_LCD_D5;
    panel_cfg.data_gpio_nums[6]  = BOARD_LCD_D6;
    panel_cfg.data_gpio_nums[7]  = BOARD_LCD_D7;
    panel_cfg.data_gpio_nums[8]  = BOARD_LCD_D8;
    panel_cfg.data_gpio_nums[9]  = BOARD_LCD_D9;
    panel_cfg.data_gpio_nums[10] = BOARD_LCD_D10;
    panel_cfg.data_gpio_nums[11] = BOARD_LCD_D11;
    panel_cfg.data_gpio_nums[12] = BOARD_LCD_D12;
    panel_cfg.data_gpio_nums[13] = BOARD_LCD_D13;
    panel_cfg.data_gpio_nums[14] = BOARD_LCD_D14;
    panel_cfg.data_gpio_nums[15] = BOARD_LCD_D15;
    panel_cfg.timings.pclk_hz           = BOARD_LCD_PIXEL_CLOCK_HZ;
    panel_cfg.timings.h_res             = BOARD_LCD_H_RES;
    panel_cfg.timings.v_res             = BOARD_LCD_V_RES;
    panel_cfg.timings.hsync_pulse_width = BOARD_LCD_HSYNC_PULSE_WIDTH;
    panel_cfg.timings.hsync_back_porch  = BOARD_LCD_HSYNC_BACK_PORCH;
    panel_cfg.timings.hsync_front_porch = BOARD_LCD_HSYNC_FRONT_PORCH;
    panel_cfg.timings.vsync_pulse_width = BOARD_LCD_VSYNC_PULSE_WIDTH;
    panel_cfg.timings.vsync_back_porch  = BOARD_LCD_VSYNC_BACK_PORCH;
    panel_cfg.timings.vsync_front_porch = BOARD_LCD_VSYNC_FRONT_PORCH;
    panel_cfg.timings.flags.pclk_active_neg = 1;
    panel_cfg.flags.fb_in_psram = 1;

    esp_lcd_panel_handle_t panel = nullptr;
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority   = 4;
    port_cfg.task_stack      = 8 * 1024;
    port_cfg.timer_period_ms = 5;
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    lvgl_port_display_cfg_t disp_cfg = {};
    disp_cfg.panel_handle  = panel;
    disp_cfg.buffer_size   = BOARD_LCD_H_RES * BOARD_LCD_V_RES;
    disp_cfg.double_buffer = true;
    disp_cfg.hres          = BOARD_LCD_H_RES;
    disp_cfg.vres          = BOARD_LCD_V_RES;
    disp_cfg.monochrome    = false;
    disp_cfg.flags.buff_dma    = false;
    disp_cfg.flags.buff_spiram = true;

    lvgl_port_display_rgb_cfg_t rgb_cfg = {};
    rgb_cfg.flags.bb_mode       = true;
    rgb_cfg.flags.avoid_tearing = false;

    lv_disp_t *disp = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
    if (disp == nullptr) {
        ESP_LOGE(TAG, "lvgl_port_add_disp_rgb failed");
        return nullptr;
    }

    // Enable backlight power now that the panel is initialised. The LEDC duty
    // was already set by backlight_init() → backlight_set() between
    // board_init() and display_init().
    ioex->write(BOARD_IOEX_BL_EN, true);
    ESP_LOGI(TAG, "Display init done, backlight enabled");

    return disp;
}
