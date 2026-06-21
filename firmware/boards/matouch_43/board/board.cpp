// SPDX-License-Identifier: GPL-3.0-or-later

#include "board.h"
#include "board_pins.h"
#include "platform.h"
#include "tc_tag.h"
#include "backlight_pwm.h"   // shared LEDC backlight driver (Stage 94)

#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"

static const char *TAG = TOUCHY_TAG("board");

static i2c_master_bus_handle_t s_i2c_bus = nullptr;

i2c_master_bus_handle_t board_get_i2c_bus(void) { return s_i2c_bus; }

extern "C" void board_init(void)
{
    // Stage 94 — backlight on a LEDC PWM channel (left off; backlight_init()
    // turns it on at the persisted brightness).
    backlight_pwm_init();
    ESP_LOGI(TAG, "Backlight PWM ready (GPIO%d)", (int)BOARD_BACKLIGHT_GPIO);

    // I2C bus for GT911 touch controller.
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.clk_source                   = I2C_CLK_SRC_DEFAULT;
    bus_cfg.i2c_port                     = BOARD_I2C_PORT;
    bus_cfg.scl_io_num                   = BOARD_I2C_SCL;
    bus_cfg.sda_io_num                   = BOARD_I2C_SDA;
    bus_cfg.glitch_ignore_cnt            = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_i2c_bus));
    ESP_LOGI(TAG, "I2C bus ready (SCL=%d SDA=%d)", (int)BOARD_I2C_SCL, (int)BOARD_I2C_SDA);
}

extern "C" const struct Platform *platform_get(void)
{
    // GPIO19/20 are not used by display or touch — native USB-OTG is available.
    static const struct Platform p = { .is_multitouch = true, .has_usb = true };
    return &p;
}
