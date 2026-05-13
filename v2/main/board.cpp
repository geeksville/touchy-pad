// SPDX-License-Identifier: Apache-2.0

#include "board.h"

#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

static i2c_master_bus_handle_t s_i2c_bus     = nullptr;
static i2c_master_dev_handle_t s_ch422_wrset = nullptr;
static i2c_master_dev_handle_t s_ch422_wrio  = nullptr;

// Shadow of the IO0..IO7 output register. CH422G has no read-modify-write so we
// must track it ourselves. After power-on it is 0xFF (all high).
static uint8_t s_wr_io_shadow = 0xFF;

i2c_master_bus_handle_t board_get_i2c_bus(void)
{
    return s_i2c_bus;
}

// Tiny inline CH422G driver. Each "register" is a separate I2C device address
// and accepts exactly one data byte.
static esp_err_t ch422_attach(uint8_t addr, i2c_master_dev_handle_t *out)
{
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address  = addr;
    cfg.scl_speed_hz    = BOARD_I2C_CLK_HZ;
    return i2c_master_bus_add_device(s_i2c_bus, &cfg, out);
}

static esp_err_t ch422_write(i2c_master_dev_handle_t dev, uint8_t byte)
{
    return i2c_master_transmit(dev, &byte, 1, /*timeout_ms*/ 50);
}

static void ch422_set_pin(uint8_t pin, bool level)
{
    if (level) {
        s_wr_io_shadow |= (uint8_t)(1u << pin);
    } else {
        s_wr_io_shadow &= (uint8_t)~(1u << pin);
    }
    ESP_ERROR_CHECK(ch422_write(s_ch422_wrio, s_wr_io_shadow));
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

    ESP_LOGI(TAG, "Initialising CH422G IO expander (WR_SET=0x%02x WR_IO=0x%02x)",
             BOARD_CH422G_I2C_ADDR_WR_SET, BOARD_CH422G_I2C_ADDR_WR_IO);
    ESP_ERROR_CHECK(ch422_attach(BOARD_CH422G_I2C_ADDR_WR_SET, &s_ch422_wrset));
    ESP_ERROR_CHECK(ch422_attach(BOARD_CH422G_I2C_ADDR_WR_IO,  &s_ch422_wrio));

    // WR_SET: bit0 IO_OE = 1 → enable IO0..IO7 as outputs (push-pull, no sleep).
    ESP_ERROR_CHECK(ch422_write(s_ch422_wrset, 0x01));

    // Sync the shadow to the chip.
    ESP_ERROR_CHECK(ch422_write(s_ch422_wrio, s_wr_io_shadow));

    // Pulse LCD reset (low → high).
    ch422_set_pin(BOARD_CH422G_IO_LCD_RST, false);
    vTaskDelay(pdMS_TO_TICKS(10));
    ch422_set_pin(BOARD_CH422G_IO_LCD_RST, true);
    vTaskDelay(pdMS_TO_TICKS(100));

    // Pulse touch reset, drive INT low while doing so so GT911 picks default addr 0x5D.
    gpio_set_direction(BOARD_TOUCH_INT_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_TOUCH_INT_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    ch422_set_pin(BOARD_CH422G_IO_TP_RST, false);
    vTaskDelay(pdMS_TO_TICKS(100));
    ch422_set_pin(BOARD_CH422G_IO_TP_RST, true);
    vTaskDelay(pdMS_TO_TICKS(200));
    // Release INT to input so the touch driver can use it.
    gpio_reset_pin(BOARD_TOUCH_INT_GPIO);

    // Turn the backlight on.
    ESP_LOGI(TAG, "Backlight on (CH422G IO%d)", BOARD_CH422G_IO_BACKLIGHT);
    ch422_set_pin(BOARD_CH422G_IO_BACKLIGHT, true);
}
