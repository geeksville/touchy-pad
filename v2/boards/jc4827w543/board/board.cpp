// SPDX-License-Identifier: Apache-2.0
//
// Board bring-up for the JC4827W543. This board has no IO-expander, so the
// only thing we set up here is the shared I2C bus that the GT911 touch
// controller hangs off of. The display backlight is driven from a real GPIO
// and is configured in display.cpp.

#include "board.h"           // public API (main/board.h)
#include "board_pins.h"      // private pin map

#include "esp_log.h"
#include "driver/i2c_master.h"

static const char *TAG = "board";

static i2c_master_bus_handle_t s_i2c_bus = nullptr;

i2c_master_bus_handle_t board_get_i2c_bus(void)
{
    return s_i2c_bus;
}

extern "C" void board_init(void)
{
    ESP_LOGI(TAG, "Initialising shared I2C bus (SCL=%d SDA=%d)",
             BOARD_I2C_SCL, BOARD_I2C_SDA);

    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.clk_source                   = I2C_CLK_SRC_DEFAULT;
    bus_cfg.i2c_port                     = BOARD_I2C_PORT;
    bus_cfg.scl_io_num                   = BOARD_I2C_SCL;
    bus_cfg.sda_io_num                   = BOARD_I2C_SDA;
    bus_cfg.glitch_ignore_cnt            = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_i2c_bus));
}
