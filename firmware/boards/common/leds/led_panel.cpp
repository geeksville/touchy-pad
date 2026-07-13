// SPDX-License-Identifier: GPL-3.0-or-later

#include "led_panel.h"
#include "tc_tag.h"

#include "esp_log.h"

static const char *TAG = TOUCHY_TAG("led_panel");

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
    strip_cfg.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB;
    strip_cfg.flags.invert_out = false;

    // RMT backend with DMA: keeps the strict WS2812B timing rock-solid even
    // while Wi-Fi / other ISRs run, and frees the CPU during transmission.
    led_strip_rmt_config_t rmt_cfg = {};
    rmt_cfg.clk_src       = RMT_CLK_SRC_DEFAULT;
    rmt_cfg.resolution_hz = 10 * 1000 * 1000;  // 10 MHz → 0.1 µs tick
    rmt_cfg.mem_block_symbols = 64;
    rmt_cfg.flags.with_dma = true;

    ESP_LOGI(TAG, "Creating LED strip: gpio=%d %u LEDs, RMT+DMA",
             _gpio, (unsigned)led_count);
    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device failed: %s",
                 esp_err_to_name(err));
        _strip = nullptr;
        return false;
    }

    // led_strip_clear(_strip);
    ESP_LOGI(TAG, "LEDPanel up: gpio=%d %dx%d (%u LEDs), RMT+DMA",
             _gpio, _width, _height, (unsigned)led_count);
    return true;
}

int LEDPanel::serpentine_index(int x, int y) const
{
#ifdef LED_ROWS_SNAKED
    // Even rows run left→right; odd rows are reversed (see lightbar.md).
    const int col = (y & 1) ? (_width - 1 - x) : x;
#else
    const int col = x;
#endif
#define LED_COLS_SNAKED 1
#if LED_COLS_SNAKED
    // Even columns run top→bottom; odd columns are reversed.
    const int row = (col & 1) ? (_height - 1 - y) : y;
#else
    const int row = y;
#endif
#ifdef LED_ROW_MAJOR
    return row * _width + col;
#else
    return col * _height + row;
#endif
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
        static bool first = false;
        if (1 || !first) {
            ESP_LOGI(TAG, "First flush: showing staged pixels for the first time");
            first = true;
        }
        led_strip_refresh(_strip);
    }
}

void LEDPanel::clear()
{
    if (_strip) {
        static bool first = false;
        if (!first) {
            ESP_LOGI(TAG, "First clear: clearing staged pixels for the first time");
            first = true;
        }        
        led_strip_clear(_strip);
    }
}
