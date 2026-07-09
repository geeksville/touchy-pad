// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stage LB1 — abstract "panel" surface for the lightbar.
//
// A Panel is a rectangular grid of RGB pixels that something can draw into
// and then present. It is the hardware-agnostic seam between the LVGL
// display driver (which produces a framebuffer) and a concrete output
// device (an addressable LED matrix, and eventually tiled/parallel groups
// of them). LB1 has exactly one concrete subclass — LEDPanel — driving a
// single WS2812B matrix, but the base class is kept minimal so future
// stages can tile several panels into one LVGL display, or map several
// LVGL displays onto several panels, without touching the display driver.

#pragma once

#include <cstdint>

class Panel {
public:
    virtual ~Panel() = default;

    // Logical pixel dimensions of this panel. Origin (0,0) is the top-left;
    // x grows right, y grows down. Subclasses translate this logical layout
    // to whatever physical ordering the hardware uses.
    virtual int width() const = 0;
    virtual int height() const = 0;

    // Stage a single pixel. Coordinates outside [0,width) × [0,height) are
    // ignored. Colour components are 8-bit (0..255). Nothing is shown until
    // flush() is called.
    virtual void set_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) = 0;

    // Present all staged pixels to the hardware.
    virtual void flush() = 0;

    // Convenience: stage every pixel to black. Does not present; call
    // flush() afterwards. The default walks the grid via set_pixel();
    // subclasses may override with a faster path.
    virtual void clear()
    {
        for (int y = 0; y < height(); ++y) {
            for (int x = 0; x < width(); ++x) {
                set_pixel(x, y, 0, 0, 0);
            }
        }
    }
};
