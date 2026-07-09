// SPDX-License-Identifier: GPL-3.0-or-later

#include "led_panel.h"

#include "esp_log.h"

static const char *TAG = "led_panel";

LEDPanel::LEDPanel(int gpio, int width, int height, uint8_t brightness)
    : _gpio(gpio), _width(width), _height(height), _brightness(brightness)
{
}

LEDPanel::~LEDPanel()
{
    if (_strip) {
        led_strip_del(_strip);
        _strip = nullptr;
    }
}

bool LEDPanel::begin()
{
    if (_strip) {
        return true;  // already up
    }

    const uint32_t led_count = (uint32_t)_width * (uint32_t)_height;

    led_strip_config_t strip_cfg = {};
    strip_cfg.strip_gpio_num = _gpio;
    strip_cfg.max_leds       = led_count;
    strip_cfg.led_model      = LED_MODEL_WS2812;
    strip_cfg.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    strip_cfg.flags.invert_out = false;

    // RMT backend with DMA: keeps the strict WS2812B timing rock-solid even
    // while Wi-Fi / other ISRs run, and frees the CPU during transmission.
    led_strip_rmt_config_t rmt_cfg = {};
    rmt_cfg.clk_src       = RMT_CLK_SRC_DEFAULT;
    rmt_cfg.resolution_hz = 10 * 1000 * 1000;  // 10 MHz → 0.1 µs tick
    rmt_cfg.mem_block_symbols = 64;
    rmt_cfg.flags.with_dma = true;

    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device failed: %s",
                 esp_err_to_name(err));
        _strip = nullptr;
        return false;
    }

    led_strip_clear(_strip);
    ESP_LOGI(TAG, "LEDPanel up: gpio=%d %dx%d (%u LEDs), RMT+DMA",
             _gpio, _width, _height, (unsigned)led_count);
    return true;
}

int LEDPanel::serpentine_index(int x, int y) const
{
    // Even rows run left→right; odd rows are reversed (see lightbar.md).
    const int col = (y & 1) ? (_width - 1 - x) : x;
    return y * _width + col;
}

void LEDPanel::set_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if (!_strip || x < 0 || y < 0 || x >= _width || y >= _height) {
        return;
    }
    // Apply the global brightness scalar (integer 0..255 multiply).
    r = (uint8_t)((uint16_t)r * _brightness / 255);
    g = (uint8_t)((uint16_t)g * _brightness / 255);
    b = (uint8_t)((uint16_t)b * _brightness / 255);
    led_strip_set_pixel(_strip, serpentine_index(x, y), r, g, b);
}

void LEDPanel::flush()
{
    if (_strip) {
        led_strip_refresh(_strip);
    }
}

void LEDPanel::clear()
{
    if (_strip) {
        led_strip_clear(_strip);
    }
}
