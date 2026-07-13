// SPDX-License-Identifier: GPL-3.0-or-later
//
// MIPI-DSI display init for Elecrow CrowPanel Advanced 7" ESP32-P4.
// Panel IC: EK79007 at 1024×600, RGB565, 2-lane DSI @ 900 Mbps.
// Ported from ~/cic/src/platforms/espidf/display_espidf_dsi.cc.

#include "display.h"
#include "board_pins.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_ek79007.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

static const char *TAG = "display";

static esp_lcd_panel_handle_t   s_lcd   = nullptr;
static lv_display_t            *s_disp  = nullptr;
static void                    *s_fb[2] = {nullptr, nullptr};

// Called from the DPI vsync ISR — signals LVGL that the previous flush landed.
static bool IRAM_ATTR on_vsync(esp_lcd_panel_handle_t panel,
                               esp_lcd_dpi_panel_event_data_t *event_data,
                               void *user_ctx)
{
    lv_display_t *disp = static_cast<lv_display_t *>(user_ctx);
    lv_display_flush_ready(disp);
    return false;
}

// LVGL flush callback: hand the completed draw buffer to the DPI driver.
// lv_display_flush_ready() is called from the vsync ISR (on_vsync), not here,
// so LVGL won't start rendering into the next buffer until the hardware has
// actually finished displaying this one.
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_draw_bitmap(s_lcd, 0, 0, BOARD_LCD_H_RES, BOARD_LCD_V_RES, px_map);
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
    ESP_LOGI(TAG, "Initialising MIPI-DSI display (EK79007) %dx%d",
             BOARD_LCD_H_RES, BOARD_LCD_V_RES);

    // The MIPI-DSI PHY PLL requires LDO channel 3 at 2.5V.
    // Without this the PLL never locks and esp_lcd_new_dsi_bus() hangs.
    esp_ldo_channel_handle_t ldo_chan = nullptr;
    esp_ldo_channel_config_t ldo_cfg = {};
    ldo_cfg.chan_id    = 3;
    ldo_cfg.voltage_mv = 2500;
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &ldo_chan));

    esp_lcd_dsi_bus_handle_t dsi_bus = nullptr;
    esp_lcd_dsi_bus_config_t dsi_bus_cfg = {};
    dsi_bus_cfg.bus_id            = 0;
    dsi_bus_cfg.num_data_lanes    = 2;
    dsi_bus_cfg.phy_clk_src       = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
    dsi_bus_cfg.lane_bit_rate_mbps = 900;
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&dsi_bus_cfg, &dsi_bus));

    esp_lcd_panel_io_handle_t dbi_io = nullptr;
    esp_lcd_dbi_io_config_t dbi_cfg = {};
    dbi_cfg.virtual_channel  = 0;
    dbi_cfg.lcd_cmd_bits     = 8;
    dbi_cfg.lcd_param_bits   = 8;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &dbi_io));

    esp_lcd_dpi_panel_config_t dpi_cfg = {};
    dpi_cfg.virtual_channel         = 0;
    dpi_cfg.dpi_clk_src             = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    dpi_cfg.dpi_clock_freq_mhz      = 52;
    dpi_cfg.in_color_format         = LCD_COLOR_FMT_RGB565;
    dpi_cfg.out_color_format        = LCD_COLOR_FMT_RGB565;
    dpi_cfg.num_fbs                 = 2;
    dpi_cfg.video_timing.h_size             = BOARD_LCD_H_RES;
    dpi_cfg.video_timing.v_size             = BOARD_LCD_V_RES;
    dpi_cfg.video_timing.hsync_pulse_width  = BOARD_DSI_HSYNC_PW;
    dpi_cfg.video_timing.hsync_back_porch   = BOARD_DSI_HSYNC_BP;
    dpi_cfg.video_timing.hsync_front_porch  = BOARD_DSI_HSYNC_FP;
    dpi_cfg.video_timing.vsync_pulse_width  = BOARD_DSI_VSYNC_PW;
    dpi_cfg.video_timing.vsync_back_porch   = BOARD_DSI_VSYNC_BP;
    dpi_cfg.video_timing.vsync_front_porch  = BOARD_DSI_VSYNC_FP;

    ek79007_vendor_config_t vendor_cfg = {};
    vendor_cfg.mipi_config.dsi_bus    = dsi_bus;
    vendor_cfg.mipi_config.dpi_config = &dpi_cfg;
    vendor_cfg.mipi_config.lane_num   = 2;

    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num  = GPIO_NUM_NC;
    panel_cfg.rgb_ele_order   = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_cfg.bits_per_pixel  = 16;
    panel_cfg.vendor_config   = &vendor_cfg;

    ESP_ERROR_CHECK(esp_lcd_new_panel_ek79007(dbi_io, &panel_cfg, &s_lcd));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_lcd));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_lcd));

    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(s_lcd, 2, &s_fb[0], &s_fb[1]));

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority   = 4;
    port_cfg.task_stack      = 8 * 1024;
    port_cfg.timer_period_ms = 5;
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    s_disp = lv_display_create(BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_set_buffers(s_disp,
                           s_fb[0], s_fb[1],
                           BOARD_LCD_H_RES * BOARD_LCD_V_RES * sizeof(uint16_t),
                           LV_DISPLAY_RENDER_MODE_DIRECT);

    // flush_ready is called from the vsync ISR — no need to call it in flush_cb.
    esp_lcd_dpi_panel_event_callbacks_t cbs = {};
    cbs.on_color_trans_done = on_vsync;
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_register_event_callbacks(s_lcd, &cbs, s_disp));

    ESP_LOGI(TAG, "MIPI-DSI display initialised at %dx%d", BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    return s_disp;
}
