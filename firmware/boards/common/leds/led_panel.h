// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stage LB1 / lb10 — WS2812B addressable LED-matrix chain support.
//
// A physical build is a daisy-chain of up to 4 small LED matrices on one
// data GPIO, driven by the RMT peripheral with DMA (espressif/led_strip).
// The whole chain is one led_strip whose indices run 0..(ΣW·H − 1): the
// data line fills matrix 0 completely, then matrix 1, and so on.
//
//   * LEDChain — the LVGL-facing Panel. Owns the single led_strip, tiles
//     the matrices into one logical surface, and maps a surface (x, y) to
//     the right matrix + physical LED index.
//   * LEDPanel — one matrix tile. Pure coordinate math (no hardware): it
//     knows its size, its offset in the surface, its base index in the
//     strip, and its per-matrix serpentine/flip wiring. serpentine_index()
//     is exposed for testing.
//
// Board-independent: shared by every LED-matrix board (jc_esp32p4_m3,
// esp32-s3-devkitc-1, …). It lives under firmware/boards/common/leds/ and
// is compiled into the selected board component, which owns the
// espressif/led_strip dependency. Panel geometry + wiring arrive at runtime
// from the persisted BoardConfig (see led_display.h `LedChainDesc`).

#pragma once

#include "panel.h"

#include "led_strip.h"

#include <cstdint>

// Per-matrix physical-wiring flags (from the Panel proto). Defaults match
// the pre-lb10 frozen behaviour: only column-snaking on.
struct LedWiring {
    bool rows_snaked  = false;  // odd rows run right→left
    bool cols_snaked  = true;   // odd cols run bottom→top
    bool row_major    = false;  // index = row*width+col (else col-major)
    bool cols_flipped = false;  // mirror X across the whole matrix
    bool rows_flipped = false;  // mirror Y across the whole matrix
};

// One LED matrix tile within a daisy-chained strip. Coordinate math only —
// it owns no hardware. Maps a surface (global) coordinate that this tile
// covers to a physical LED index in the shared strip.
class LEDPanel {
public:
    LEDPanel(int width, int height, int x_off, int y_off, int base,
             const LedWiring &wiring);

    int width() const { return _width; }
    int height() const { return _height; }

    // True when the surface coordinate (gx, gy) falls inside this tile.
    bool contains(int gx, int gy) const
    {
        return gx >= _x_off && gy >= _y_off &&
               gx < _x_off + _width && gy < _y_off + _height;
    }

    // Physical LED index for a surface coordinate this tile contains.
    // Caller must ensure contains(gx, gy).
    int global_index(int gx, int gy) const
    {
        return _base + serpentine_index(gx - _x_off, gy - _y_off);
    }

    // Map a LOCAL (x, y) within this tile to its physical LED offset within
    // the tile's own segment (0 .. width*height − 1). Defined inline so the
    // compiler can hoist the (per-tile-constant) flag checks up into
    // set_pixel/global_index rather than paying a call per pixel. Exposed
    // for testing.
    inline int serpentine_index(int x, int y) const
    {
        // 1. Whole-matrix mirror (cheap, before any snake math).
        if (_cols_flipped) x = _width - 1 - x;
        if (_rows_flipped) y = _height - 1 - y;

        // 2. Row snaking: odd rows run right→left.
        const int col = (_rows_snaked && (y & 1)) ? (_width - 1 - x) : x;

        // 3. Column snaking: odd columns run bottom→top.
        const int row = (_cols_snaked && (col & 1)) ? (_height - 1 - y) : y;

        // 4. Major order.
        return _row_major ? (row * _width + col) : (col * _height + row);
    }

private:
    const int  _width;
    const int  _height;
    const int  _x_off;
    const int  _y_off;
    const int  _base;
    const bool _rows_snaked;
    const bool _cols_snaked;
    const bool _row_major;
    const bool _cols_flipped;
    const bool _rows_flipped;
};

// The full daisy-chained LED surface: one led_strip on one GPIO, tiled from
// up to LED_CHAIN_CAP LEDPanel matrices. This is the LVGL-facing Panel.
class LEDChain : public Panel {
public:
    static constexpr int LED_CHAIN_CAP = 4;

    // `brightness` (0..255) scales every colour component at set_pixel time
    // so the backlight/brightness preference can dim the whole surface
    // without the caller re-sending pixels. The led_strip device is created
    // lazily in begin() (call once after all tiles are added).
    LEDChain(int gpio, int total_w, int total_h, uint8_t brightness = 255);
    ~LEDChain() override;

    // Append a tile in chain order. Its base strip index is the running sum
    // of earlier tiles' LED counts. Returns false when the chain is full.
    bool add_tile(int width, int height, int x_off, int y_off,
                  const LedWiring &wiring);

    // Create the underlying RMT+DMA led_strip device sized to the total LED
    // count of all added tiles. Returns true on success; logs + returns
    // false otherwise. Safe to call once.
    bool begin();

    int width() const override { return _total_w; }
    int height() const override { return _total_h; }

    void set_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) override;
    void flush() override;
    void clear() override;

    // Set the global brightness scalar (0 = off, 255 = full). Takes effect
    // on the next staged pixel; does not itself present.
    void set_brightness(uint8_t brightness) { _brightness = brightness; }

private:
    const int          _gpio;
    const int          _total_w;
    const int          _total_h;
    uint8_t            _brightness;
    int                _led_count   = 0;
    int                _panel_count = 0;
    LEDPanel          *_panels[LED_CHAIN_CAP] = {};
    led_strip_handle_t _strip = nullptr;
};
