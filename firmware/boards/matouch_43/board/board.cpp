// SPDX-License-Identifier: GPL-3.0-or-later

#include "board.h"
#include "board_pins.h"
#include "platform.h"
#include "tc_tag.h"

#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"

static const char *TAG = TOUCHY_TAG("board");

static i2c_master_bus_handle_t s_i2c_bus = nullptr;

i2c_master_bus_handle_t board_get_i2c_bus(void) { return s_i2c_bus; }

extern "C" void board_backlight_set(bool on)
{
    gpio_set_level(BOARD_BACKLIGHT_GPIO, on ? 1 : 0);
}

extern "C" void board_init(void)
{
    // Backlight: simple GPIO, HIGH = on.
    gpio_config_t bl_cfg = {};
    bl_cfg.pin_bit_mask = 1ULL << BOARD_BACKLIGHT_GPIO;
    bl_cfg.mode         = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&bl_cfg));
    board_backlight_set(true);
    ESP_LOGI(TAG, "Backlight on (GPIO%d)", (int)BOARD_BACKLIGHT_GPIO);

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
