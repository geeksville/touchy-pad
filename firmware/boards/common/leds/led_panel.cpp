// SPDX-License-Identifier: GPL-3.0-or-later

#include "led_panel.h"
#include "tc_tag.h"

#include "esp_log.h"

static const char *TAG = TOUCHY_TAG("led_panel");

// ─────────────────────────── LEDPanel (tile) ───────────────────────────

LEDPanel::LEDPanel(int width, int height, int x_off, int y_off, int base,
                   const LedWiring &wiring)
    : _width(width), _height(height), _x_off(x_off), _y_off(y_off), _base(base),
      _rows_snaked(wiring.rows_snaked), _cols_snaked(wiring.cols_snaked),
      _row_major(wiring.row_major), _cols_flipped(wiring.cols_flipped),
      _rows_flipped(wiring.rows_flipped)
{
}

// ─────────────────────────── LEDChain (surface) ────────────────────────

LEDChain::LEDChain(int gpio, int total_w, int total_h, uint8_t brightness)
    : _gpio(gpio), _total_w(total_w), _total_h(total_h), _brightness(brightness)
{
}

LEDChain::~LEDChain()
{
    if (_strip) {
        led_strip_del(_strip);
        _strip = nullptr;
    }
    for (int i = 0; i < _panel_count; ++i) {
        delete _panels[i];
        _panels[i] = nullptr;
    }
}

bool LEDChain::add_tile(int width, int height, int x_off, int y_off,
                        const LedWiring &wiring)
{
    if (_panel_count >= LED_CHAIN_CAP) {
        ESP_LOGE(TAG, "chain full (%d panels); ignoring extra tile",
                 LED_CHAIN_CAP);
        return false;
    }
    _panels[_panel_count++] = new LEDPanel(width, height, x_off, y_off,
                                           _led_count, wiring);
    _led_count += width * height;  // running base for the next tile
    return true;
}

bool LEDChain::begin()
{
    if (_strip) {
        return true;  // already up
    }

    led_strip_config_t strip_cfg = {};
    strip_cfg.strip_gpio_num = _gpio;
    strip_cfg.max_leds       = (uint32_t)_led_count;
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

    ESP_LOGI(TAG, "Creating LED strip: gpio=%d %d LEDs (%d tiles → %dx%d), RMT+DMA",
             _gpio, _led_count, _panel_count, _total_w, _total_h);
    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device failed: %s",
                 esp_err_to_name(err));
        _strip = nullptr;
        return false;
    }

    ESP_LOGI(TAG, "LEDChain up: gpio=%d %dx%d (%d LEDs), RMT+DMA",
             _gpio, _total_w, _total_h, _led_count);
    return true;
}

void LEDChain::set_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if (!_strip || x < 0 || y < 0 || x >= _total_w || y >= _total_h) {
        return;
    }
    // Apply the global brightness scalar (integer 0..255 multiply).
    r = (uint8_t)((uint16_t)r * _brightness / 255);
    g = (uint8_t)((uint16_t)g * _brightness / 255);
    b = (uint8_t)((uint16_t)b * _brightness / 255);

    // Small linear scan (≤4 tiles). The tile ranges are disjoint, so the
    // first match owns the pixel; coordinates in a tiling gap (ragged
    // panels) fall through and are dropped.
    for (int i = 0; i < _panel_count; ++i) {
        if (_panels[i]->contains(x, y)) {
            led_strip_set_pixel(_strip, _panels[i]->global_index(x, y), r, g, b);
            return;
        }
    }
}

void LEDChain::flush()
{
    if (_strip) {
        led_strip_refresh(_strip);
    }
}

void LEDChain::clear()
{
    if (_strip) {
        led_strip_clear(_strip);
    }
}
