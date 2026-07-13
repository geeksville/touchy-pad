// SPDX-License-Identifier: GPL-3.0-or-later

#include "board.h"
#include "board_pins.h"
#include "platform.h"
#include "backlight_pwm.h"
#include "tc_tag.h"

#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"

static const char *TAG = TOUCHY_TAG("board");

static i2c_master_bus_handle_t s_i2c_bus = nullptr;

i2c_master_bus_handle_t board_get_i2c_bus(void) { return s_i2c_bus; }

extern "C" void board_init(void)
{
    // Backlight boost power rail: always HIGH.
    gpio_set_direction(BOARD_BK_PWR_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_BK_PWR_GPIO, 1);

    // Hold ESP32-C6 companion in reset (WiFi not used; prevents SDIO lines floating).
    gpio_set_direction(BOARD_C6_RESET_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_C6_RESET_GPIO, 0);

    // I2C bus shared by GT911 touch (and any future peripherals).
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.clk_source                   = I2C_CLK_SRC_DEFAULT;
    bus_cfg.i2c_port                     = BOARD_TOUCH_I2C_PORT;
    bus_cfg.scl_io_num                   = BOARD_TOUCH_I2C_SCL;
    bus_cfg.sda_io_num                   = BOARD_TOUCH_I2C_SDA;
    bus_cfg.glitch_ignore_cnt            = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_i2c_bus));

    // Stage 94 — backlight PWM via the shared LEDC driver (11-bit, PLL clock;
    // left off, switched on by backlight_init() at the persisted brightness).
    backlight_pwm_init();

    ESP_LOGI(TAG, "Board init done (ESP32-P4, backlight ready)");
}

extern "C" const struct Platform *platform_get(void)
{
    static const struct Platform p = { .is_multitouch = true, .has_usb = true };
    return &p;
}
