// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stage LB1 — LEDPanel: a Panel backed by a WS2812B addressable LED matrix.
//
// One LEDPanel instance owns one led_strip handle bound to a single GPIO
// and driven by the RMT peripheral with DMA (espressif/led_strip). The
// matrix is wired in the usual "zigzag / serpentine" order (see
// docs/hardware/lightbar/lightbar.md): row 0 runs left→right, row 1
// right→left, and so on. LEDPanel hides that ordering behind the logical
// (x, y) coordinate space of the Panel base class.
//
// Board-independent: shared by every LED-matrix board (jc_esp32p4_m3,
// esp32-s3-devkitc-1, …). It lives under firmware/boards/common/leds/
// by the selected board component, which owns the espressif/led_strip
// dependency.

#pragma once

#include "panel.h"

#include "led_strip.h"

class LEDPanel : public Panel {
public:
    // Construct a panel of `width` × `height` LEDs driven from `gpio`.
    // `brightness` (0..255) scales every colour component at flush time so
    // the board backlight/brightness preference can dim the whole matrix
    // without the caller re-sending pixels. The led_strip device is created
    // lazily in begin() (call it once after construction) so a failed
    // hardware bring-up doesn't throw from a constructor.
    LEDPanel(int gpio, int width, int height, uint8_t brightness = 255);
    ~LEDPanel() override;

    // Create the underlying RMT+DMA led_strip device. Returns true on
    // success; logs and returns false otherwise. Safe to call once.
    bool begin();

    int width() const override { return _width; }
    int height() const override { return _height; }

    void set_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) override;
    void flush() override;
    void clear() override;

    // Set the global brightness scalar (0 = off, 255 = full). Takes effect
    // on the next flush(); does not itself present.
    void set_brightness(uint8_t brightness) { _brightness = brightness; }

    // Map a logical (x, y) to the physical LED index along the data chain,
    // honouring the serpentine wiring. Exposed for testing / reuse.
    int serpentine_index(int x, int y) const;

private:
    int              _gpio;
    int              _width;
    int              _height;
    uint8_t          _brightness;
    led_strip_handle_t _strip = nullptr;
};
