// SPDX-License-Identifier: GPL-3.0-or-later

#include "board.h"
#include "board_pins.h"
#include "platform.h"

#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

static ElecrowBoardConfig      s_config;
static i2c_master_bus_handle_t s_i2c_bus   = nullptr;
static i2c_master_dev_handle_t s_pca9557   = nullptr;
static uint8_t                 s_pca9557_output = 0xFF;
static i2c_master_dev_handle_t s_stc_dev   = nullptr;

// ---------- PCA9557 helpers ----------

static esp_err_t pca9557_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_pca9557, buf, 2, /*timeout_ms*/ 50);
}

// ---------- Variant detection ----------

static bool detect_advance(void)
{
    // The Advance board has an RTC (PCF8563) at 0x51 on GPIO15/16 (I2C_NUM_0).
    // The regular v2/v3 use those same GPIOs as RGB data lines, so the probe
    // silently fails there (no I2C pull-ups, no device to ACK).
    i2c_master_bus_config_t cfg = {};
    cfg.clk_source                   = I2C_CLK_SRC_DEFAULT;
    cfg.i2c_port                     = I2C_NUM_0;
    cfg.scl_io_num                   = BOARD_ADVANCE_PROBE_SCL;
    cfg.sda_io_num                   = BOARD_ADVANCE_PROBE_SDA;
    cfg.glitch_ignore_cnt            = 7;
    cfg.flags.enable_internal_pullup = true;

    i2c_master_bus_handle_t bus = nullptr;
    if (i2c_new_master_bus(&cfg, &bus) != ESP_OK) {
        return false;
    }
    bool found = (i2c_master_probe(bus, 0x51, /*timeout_ms*/ 50) == ESP_OK);
    i2c_del_master_bus(bus);
    return found;
}

// ---------- Per-variant config tables ----------

static void fill_regular(ElecrowBoardConfig &c)
{
    c.is_advance     = false;
    c.is_advance_1_2 = false;

    c.i2c_scl    = GPIO_NUM_20;
    c.i2c_sda    = GPIO_NUM_19;
    c.i2c_clk_hz = 100'000;
    c.i2c_port   = I2C_NUM_1;

    // INT is wired to GPIO38 on v3+, but PCA9557 IO0 also drives this line
    // HIGH as an output after init, preventing GT911's open-drain INT from
    // asserting. Use polling mode (NC) so reads happen every 5 ms regardless.
    c.touch_int_gpio = GPIO_NUM_NC;

    c.lcd_hsync = GPIO_NUM_39;
    c.lcd_vsync = GPIO_NUM_40;
    c.lcd_pclk  = GPIO_NUM_0;
    c.lcd_de    = GPIO_NUM_41;

    // RGB565 data_gpio_nums[i] = GPIO physically wired to LCD data pin i.
    c.lcd_data[0]  = GPIO_NUM_15;   // D0
    c.lcd_data[1]  = GPIO_NUM_7;    // D1
    c.lcd_data[2]  = GPIO_NUM_6;    // D2
    c.lcd_data[3]  = GPIO_NUM_5;    // D3
    c.lcd_data[4]  = GPIO_NUM_4;    // D4
    c.lcd_data[5]  = GPIO_NUM_9;    // D5
    c.lcd_data[6]  = GPIO_NUM_46;   // D6
    c.lcd_data[7]  = GPIO_NUM_3;    // D7
    c.lcd_data[8]  = GPIO_NUM_8;    // D8
    c.lcd_data[9]  = GPIO_NUM_16;   // D9
    c.lcd_data[10] = GPIO_NUM_1;    // D10
    c.lcd_data[11] = GPIO_NUM_14;   // D11
    c.lcd_data[12] = GPIO_NUM_21;   // D12
    c.lcd_data[13] = GPIO_NUM_47;   // D13
    c.lcd_data[14] = GPIO_NUM_48;   // D14
    c.lcd_data[15] = GPIO_NUM_45;   // D15

    c.hsync_back_porch  = 40;
    c.hsync_front_porch = 40;
    c.hsync_pulse_width = 48;
    c.vsync_back_porch  = 13;
    c.vsync_front_porch = 1;
    c.vsync_pulse_width = 31;
    c.pclk_active_neg   = 1;

    // Backlight on regular v3 is GPIO2 via LEDC, not PCA9557.
    // PCA9557 IO0/IO1 are used only for GT911 reset sequencing.
    c.pca9557_backlight_bit  = 0;     // not used for backlight
    c.pca9557_output_shadow  = 0xFD;  // IO0=HIGH latch after init
}

static void fill_advance(ElecrowBoardConfig &c)
{
    c.is_advance     = true;
    c.is_advance_1_2 = false;  // updated in board_init() after probing 0x30

    c.i2c_scl    = GPIO_NUM_16;
    c.i2c_sda    = GPIO_NUM_15;
    c.i2c_clk_hz = 400'000;
    c.i2c_port   = I2C_NUM_0;

    c.touch_int_gpio = GPIO_NUM_NC;

    c.lcd_hsync = GPIO_NUM_40;
    c.lcd_vsync = GPIO_NUM_41;
    c.lcd_pclk  = GPIO_NUM_39;
    c.lcd_de    = GPIO_NUM_42;

    // RGB565 data_gpio_nums[i] = GPIO physically wired to LCD data pin i.
    c.lcd_data[0]  = GPIO_NUM_21;   // D0
    c.lcd_data[1]  = GPIO_NUM_47;   // D1
    c.lcd_data[2]  = GPIO_NUM_48;   // D2
    c.lcd_data[3]  = GPIO_NUM_45;   // D3
    c.lcd_data[4]  = GPIO_NUM_38;   // D4
    c.lcd_data[5]  = GPIO_NUM_9;    // D5
    c.lcd_data[6]  = GPIO_NUM_10;   // D6
    c.lcd_data[7]  = GPIO_NUM_11;   // D7
    c.lcd_data[8]  = GPIO_NUM_12;   // D8
    c.lcd_data[9]  = GPIO_NUM_13;   // D9
    c.lcd_data[10] = GPIO_NUM_14;   // D10
    c.lcd_data[11] = GPIO_NUM_7;    // D11
    c.lcd_data[12] = GPIO_NUM_17;   // D12
    c.lcd_data[13] = GPIO_NUM_18;   // D13
    c.lcd_data[14] = GPIO_NUM_3;    // D14
    c.lcd_data[15] = GPIO_NUM_46;   // D15

    c.hsync_back_porch  = 8;
    c.hsync_front_porch = 8;
    c.hsync_pulse_width = 4;
    c.vsync_back_porch  = 8;
    c.vsync_front_porch = 8;
    c.vsync_pulse_width = 4;
    c.pclk_active_neg   = 1;

    // Backlight: IO1 is left HIGH after the advance PCA9557 init sequence.
    c.pca9557_backlight_bit  = 0x02;
    c.pca9557_output_shadow  = 0x06;
}

// ---------- PCA9557 init sequences ----------
//
// Ported from cic project (platform_espidf.cc / initPca9557()).
// PCA9557 config register: 0 = output, 1 = input.

static void pca9557_init_regular(void)
{
    // IO1 = output (RST); everything else = input.
    // 0xFD = 11111101b → bit1 = 0 = output.
    ESP_ERROR_CHECK(pca9557_write_reg(BOARD_PCA9557_REG_CONFIG, 0xFD));
    vTaskDelay(pdMS_TO_TICKS(50));

    // IO0=LOW, IO1=LOW → GT911 RST asserted (IO1 driven, IO0 latch set).
    ESP_ERROR_CHECK(pca9557_write_reg(BOARD_PCA9557_REG_OUTPUT, 0xFC));
    vTaskDelay(pdMS_TO_TICKS(20));

    // Pre-load IO0 latch HIGH while keeping IO1 (RST) LOW.
    ESP_ERROR_CHECK(pca9557_write_reg(BOARD_PCA9557_REG_OUTPUT, 0xFD));
    vTaskDelay(pdMS_TO_TICKS(100));

    // IO1 → input (GT911 INT); IO0,2-7 → output.
    // IO0 immediately drives HIGH from its pre-loaded latch = backlight on.
    // 0x02 = 00000010b → bit1 = 1 = input.
    ESP_ERROR_CHECK(pca9557_write_reg(BOARD_PCA9557_REG_CONFIG, 0x02));
}

static void pca9557_init_advance(void)
{
    // IO1 = output; everything else = input.
    ESP_ERROR_CHECK(pca9557_write_reg(BOARD_PCA9557_REG_CONFIG, 0xFD));
    vTaskDelay(pdMS_TO_TICKS(50));

    // IO1=HIGH (backlight), IO2=LOW (GT911 RST asserted via latch).
    // 0x02 = 00000010b → bit1=1, bit2=0.
    ESP_ERROR_CHECK(pca9557_write_reg(BOARD_PCA9557_REG_OUTPUT, 0x02));
    vTaskDelay(pdMS_TO_TICKS(20));

    // Release RST; keep backlight on.
    // 0x06 = 00000110b → bit1=1 (IO1 HIGH), bit2=1 (IO2 HIGH).
    ESP_ERROR_CHECK(pca9557_write_reg(BOARD_PCA9557_REG_OUTPUT, 0x06));
}

// ---------- Public API ----------

const ElecrowBoardConfig *board_get_config(void) { return &s_config; }

i2c_master_bus_handle_t board_get_i2c_bus(void) { return s_i2c_bus; }

// Stage 94 backlight entry point. The regular v3 variant has a real LEDC PWM
// pin so it honours the 0-100 brightness level; the "advance" variants drive
// the backlight through an MCU/IO-expander that can only do on/off, so any
// non-zero level is quantised to fully on.
extern "C" void backlight_set(uint8_t level)
{
    if (level > 100) level = 100;
    if (!s_config.is_advance) {
        // Regular v3: backlight on GPIO2 via LEDC (output_invert=1: duty 0=bright,
        // 255=off). Scale the 0-100 level across the 8-bit duty range.
        uint32_t duty = (level == 0) ? 255 : (255 - (255u * level) / 100);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        return;
    }
    bool on = level > 0;
    if (s_config.is_advance_1_2) {
        // Advance v1.2: STC8H1K28. 0x10 = on/max; off = 0x10 then 0x05.
        if (s_stc_dev == nullptr) return;
        uint8_t max_cmd = 0x10;
        i2c_master_transmit(s_stc_dev, &max_cmd, 1, 50);
        if (!on) {
            uint8_t off_cmd = 0x05;
            i2c_master_transmit(s_stc_dev, &off_cmd, 1, 50);
        }
        return;
    }
    // Advance v1.0: backlight is PCA9557 IO1.
    if (s_pca9557 == nullptr) return;
    if (on) {
        s_pca9557_output |= s_config.pca9557_backlight_bit;
    } else {
        s_pca9557_output &= (uint8_t)(~s_config.pca9557_backlight_bit);
    }
    pca9557_write_reg(BOARD_PCA9557_REG_OUTPUT, s_pca9557_output);
}

extern "C" void board_init(void)
{
    ESP_LOGI(TAG, "Detecting Elecrow S3 7\" variant...");
    if (detect_advance()) {
        ESP_LOGI(TAG, "Variant: Advance (RTC at 0x51 on GPIO15/16)");
        fill_advance(s_config);
    } else {
        ESP_LOGI(TAG, "Variant: Regular (v2/v3)");
        fill_regular(s_config);
    }

    ESP_LOGI(TAG, "Initialising I2C bus (SCL=%d SDA=%d @ %lu Hz)",
             (int)s_config.i2c_scl, (int)s_config.i2c_sda,
             (unsigned long)s_config.i2c_clk_hz);

    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.clk_source                   = I2C_CLK_SRC_DEFAULT;
    bus_cfg.i2c_port                     = s_config.i2c_port;
    bus_cfg.scl_io_num                   = s_config.i2c_scl;
    bus_cfg.sda_io_num                   = s_config.i2c_sda;
    bus_cfg.glitch_ignore_cnt            = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_i2c_bus));

    if (s_config.is_advance &&
        i2c_master_probe(s_i2c_bus, BOARD_STC8H1K28_ADDR, /*timeout_ms*/ 50) == ESP_OK) {
        // Advance v1.2: STC8H1K28 I2C backlight controller.
        s_config.is_advance_1_2 = true;
        i2c_device_config_t stc_cfg = {};
        stc_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        stc_cfg.device_address  = BOARD_STC8H1K28_ADDR;
        stc_cfg.scl_speed_hz    = s_config.i2c_clk_hz;
        ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus, &stc_cfg, &s_stc_dev));
        uint8_t on_cmd = 0x10;
        esp_err_t stc_err = i2c_master_transmit(s_stc_dev, &on_cmd, 1, 200);
        if (stc_err == ESP_OK) {
            ESP_LOGI(TAG, "Board init done; advance v1.2 backlight on (STC8H1K28)");
        } else {
            ESP_LOGW(TAG, "STC8H1K28 found but backlight cmd failed: %s", esp_err_to_name(stc_err));
        }
    } else if (i2c_master_probe(s_i2c_bus, BOARD_PCA9557_ADDR, /*timeout_ms*/ 50) == ESP_OK) {
        i2c_device_config_t pca_cfg = {};
        pca_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        pca_cfg.device_address  = BOARD_PCA9557_ADDR;
        pca_cfg.scl_speed_hz    = s_config.i2c_clk_hz;
        ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus, &pca_cfg, &s_pca9557));

        if (s_config.is_advance) {
            pca9557_init_advance();
            s_pca9557_output = s_config.pca9557_output_shadow;
            ESP_LOGI(TAG, "Board init done; advance v1.0 backlight on (PCA9557 IO1)");
        } else {
            pca9557_init_regular();
            s_pca9557_output = s_config.pca9557_output_shadow;
        }
    } else {
        ESP_LOGW(TAG, "Neither STC8H1K28 (0x%02x) nor PCA9557 (0x%02x) found — backlight and GT911 reset skipped",
                 BOARD_STC8H1K28_ADDR, BOARD_PCA9557_ADDR);
    }

    if (!s_config.is_advance) {
        // Regular v3: backlight on GPIO2 via LEDC (PCA9557 not used for backlight).
        ledc_timer_config_t ledc_timer = {};
        ledc_timer.duty_resolution = LEDC_TIMER_8_BIT;
        ledc_timer.freq_hz         = 200;
        ledc_timer.speed_mode      = LEDC_LOW_SPEED_MODE;
        ledc_timer.timer_num       = LEDC_TIMER_0;
        ledc_timer.clk_cfg         = LEDC_AUTO_CLK;
        ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

        ledc_channel_config_t ledc_ch = {};
        ledc_ch.channel             = LEDC_CHANNEL_0;
        ledc_ch.duty                = 0;
        ledc_ch.gpio_num            = GPIO_NUM_2;
        ledc_ch.speed_mode          = LEDC_LOW_SPEED_MODE;
        ledc_ch.timer_sel           = LEDC_TIMER_0;
        ledc_ch.flags.output_invert = 1;
        ESP_ERROR_CHECK(ledc_channel_config(&ledc_ch));

        // Leave it off; backlight_init() switches it on at the persisted
        // brightness (the inverted LEDC idles bright at duty 0).
        backlight_set(0);
        ESP_LOGI(TAG, "Board init done; regular backlight ready (GPIO2 LEDC)");
    }
}

extern "C" const struct Platform *platform_get(void)
{
    // GPIO19/20 are shared with I2C (PCA9557/GT911), so native USB is not usable.
    static const struct Platform p = { .is_multitouch = true, .has_usb = false };
    return &p;
}
