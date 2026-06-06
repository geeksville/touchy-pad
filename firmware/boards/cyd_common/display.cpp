// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared LVGL display bring-up for the "Cheap Yellow Display" (CYD2USB) family
// of classic-ESP32 boards.
//
// CYD panels are standard SPI controllers (ST7789 on the 2.8" 2432S028Rv3,
// ILI9341 on the 2.4" 2432S024), driven over the stock esp_lcd SPI panel-io
// and handed to esp_lvgl_port. The controller is selected per-board in
// board_pins.h via BOARD_LCD_CONTROLLER_*; everything else (pins, clock,
// orientation, colour quirks) is identical plumbing. There is no PSRAM on
// these modules, so the LVGL draw buffers live in internal DMA RAM and are
// deliberately small (a few dozen lines, ping-ponged).

#include "display.h"
#include "tc_tag.h"

#include "board_pins.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

// Select the panel-controller constructor. ST7789 ships inside the core
// esp_lcd component; ILI9341 comes from the espressif/esp_lcd_ili9341 managed
// component (declared in the board's idf_component.yml).
#if defined(BOARD_LCD_CONTROLLER_ILI9341)
#  include "esp_lcd_ili9341.h"
#  define BOARD_LCD_NEW_PANEL esp_lcd_new_panel_ili9341
#  define BOARD_LCD_CONTROLLER_NAME "ILI9341"
#else  // default: ST7789
#  define BOARD_LCD_NEW_PANEL esp_lcd_new_panel_st7789
#  define BOARD_LCD_CONTROLLER_NAME "ST7789"
#endif

static const char *TAG = TOUCHY_TAG("display");

extern "C" lv_display_t *display_init(void)
{
    // ----- Backlight GPIO high -----
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

    // ----- SPI bus for the panel -----
    ESP_LOGI(TAG, "Configuring %s SPI panel %dx%d @ %d MHz",
             BOARD_LCD_CONTROLLER_NAME,
             BOARD_LCD_H_RES, BOARD_LCD_V_RES,
             BOARD_LCD_PIXEL_CLOCK_HZ / 1000000);

    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num     = BOARD_LCD_GPIO_MOSI;
    bus_cfg.miso_io_num     = BOARD_LCD_GPIO_MISO;
    bus_cfg.sclk_io_num     = BOARD_LCD_GPIO_SCK;
    bus_cfg.quadwp_io_num   = -1;
    bus_cfg.quadhd_io_num   = -1;
    // Worst-case single transfer is a full-width strip of RGB565 pixels.
    bus_cfg.max_transfer_sz = BOARD_LCD_H_RES * 80 * sizeof(uint16_t);
    ESP_ERROR_CHECK(spi_bus_initialize(BOARD_LCD_SPI_HOST, &bus_cfg,
                                       SPI_DMA_CH_AUTO));

    // ----- Panel IO (command/data over SPI) -----
    esp_lcd_panel_io_handle_t io_handle = nullptr;
    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num       = BOARD_LCD_GPIO_CS;
    io_cfg.dc_gpio_num       = BOARD_LCD_GPIO_DC;
    io_cfg.spi_mode          = 0;
    io_cfg.pclk_hz           = BOARD_LCD_PIXEL_CLOCK_HZ;
    io_cfg.trans_queue_depth = 10;
    io_cfg.lcd_cmd_bits      = 8;
    io_cfg.lcd_param_bits    = 8;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)BOARD_LCD_SPI_HOST, &io_cfg, &io_handle));

    // ----- Panel (ST7789 or ILI9341, selected above) -----
    esp_lcd_panel_handle_t panel = nullptr;
    esp_lcd_panel_dev_config_t panel_cfg = {};
    // GPIO_NUM_NC (-1) tells esp_lcd "no dedicated reset line" — the panel on
    // this module shares the system reset.
    panel_cfg.reset_gpio_num = BOARD_LCD_GPIO_RST;
#if BOARD_LCD_BGR_ORDER
    panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
#else
    panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
#endif
    panel_cfg.bits_per_pixel = 16;
    ESP_ERROR_CHECK(BOARD_LCD_NEW_PANEL(io_handle, &panel_cfg, &panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, BOARD_LCD_INVERT_COLOR));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    // ----- LVGL port task + display -----
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority   = 4;
    port_cfg.task_stack      = 8 * 1024;
    port_cfg.timer_period_ms = 5;
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    lvgl_port_display_cfg_t disp_cfg = {};
    disp_cfg.io_handle     = io_handle;
    disp_cfg.panel_handle  = panel;
    // 40 lines per buffer, double-buffered, in internal DMA RAM (no PSRAM).
    disp_cfg.buffer_size   = BOARD_LCD_H_RES * 40;
    disp_cfg.double_buffer = true;
    disp_cfg.hres          = BOARD_LCD_H_RES;
    disp_cfg.vres          = BOARD_LCD_V_RES;
    disp_cfg.monochrome    = false;
    disp_cfg.color_format  = LV_COLOR_FORMAT_RGB565;
    disp_cfg.flags.buff_dma    = true;
    // These controllers expect big-endian RGB565 on the wire; LVGL renders
    // little-endian.
    disp_cfg.flags.swap_bytes  = true;

    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (!disp) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return nullptr;
    }

    // Apply orientation AFTER lvgl_port_add_disp: the port internally calls
    // esp_lcd_panel_swap_xy / mirror to match disp_cfg.rotation (zero →
    // identity), which would overwrite anything set beforehand.
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, BOARD_LCD_SWAP_XY));
    ESP_ERROR_CHECK(
        esp_lcd_panel_mirror(panel, BOARD_LCD_MIRROR_X, BOARD_LCD_MIRROR_Y));

    return disp;
}
