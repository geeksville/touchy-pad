// SPDX-License-Identifier: GPL-3.0-or-later

#include "board.h"
#include "board_pins.h"
#include "backlight_pwm.h"   // shared LEDC backlight driver (Stage 94)

#include "esp_log.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "platform.h"

static const char *TAG = "board";

static i2c_master_bus_handle_t s_i2c_bus = nullptr;
static Lca9555                 s_ioex;

Lca9555 *board_get_ioex(void) { return &s_ioex; }

i2c_master_bus_handle_t board_get_i2c_bus(void) { return s_i2c_bus; }

extern "C" void board_init(void)
{
    ESP_LOGI(TAG, "SQUiXL board init (SCL=%d SDA=%d)", BOARD_I2C_SCL, BOARD_I2C_SDA);

    // I2C bus shared by GT911 and LCA9555
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.clk_source                   = I2C_CLK_SRC_DEFAULT;
    bus_cfg.i2c_port                     = BOARD_I2C_PORT;
    bus_cfg.scl_io_num                   = BOARD_I2C_SCL;
    bus_cfg.sda_io_num                   = BOARD_I2C_SDA;
    bus_cfg.glitch_ignore_cnt            = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_i2c_bus));

    // LCA9555 IO expander — controls LCD reset, backlight enable, ST7701S SPI,
    // and GT911 touch reset.
    ESP_LOGI(TAG, "Initialising LCA9555 at 0x%02x", BOARD_LCA9555_ADDR);
    if (!s_ioex.init(s_i2c_bus, BOARD_LCA9555_ADDR)) {
        ESP_LOGE(TAG, "LCA9555 init failed — display will not work");
        return;
    }
    s_ioex.pinMode(BOARD_IOEX_BL_EN,    false, false);  // backlight off during init
    s_ioex.pinMode(BOARD_IOEX_LCD_RST,  false, true);   // LCD reset deasserted
    s_ioex.pinMode(BOARD_IOEX_SPI_MOSI, false, false);
    s_ioex.pinMode(BOARD_IOEX_SPI_CLK,  false, false);
    s_ioex.pinMode(BOARD_IOEX_SPI_CS,   false, true);   // CS idle high
    s_ioex.pinMode(BOARD_IOEX_TP_RST,   false, true);   // touch reset deasserted

    // Pulse GT911 reset so it starts from a known state.
    s_ioex.write(BOARD_IOEX_TP_RST, false);
    vTaskDelay(pdMS_TO_TICKS(20));
    s_ioex.write(BOARD_IOEX_TP_RST, true);
    vTaskDelay(pdMS_TO_TICKS(100));

    // Set up the LEDC PWM backlight (duty left off until display_init()
    // enables BL_EN power; switched on by backlight_init()).
    backlight_pwm_init();

    ESP_LOGI(TAG, "Board init done");
}

extern "C" const struct Platform *platform_get(void)
{
    static const struct Platform p = { .is_multitouch = true, .has_usb = true };
    return &p;
}
