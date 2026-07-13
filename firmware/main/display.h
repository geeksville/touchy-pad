// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lvgl.h"

// Stage lb7 — the board→main display seam is a small C++ class hierarchy
// instead of the old free `lv_display_t *display_init(void)`.
//
//   Display            — abstract base. init() runs hw_init() + post_init();
//                        raw() returns the created lv_display_t*.
//   LEDMatrixDisplay   — WS2812B LED-matrix boards (boards/common/leds).
//   <board>Display     — each LCD board's own Display subclass, defined
//                        locally in that board's display.cpp.
//   HeadlessDisplay    — off-screen LVGL display with a no-op flush, used
//                        when there is no panel (or CONFIG_TOUCHY_NO_DISPLAY).
//
// Each board component provides exactly one strong `display_create()` that
// returns its concrete subclass; `main.cpp` owns the instance.

class Display {
public:
    virtual ~Display() = default;

    // Bring the display up: hw_init() then, on success, post_init().
    // Returns false if the hardware display could not be created.
    bool init();

    // The LVGL display handle created by hw_init(), or nullptr if init()
    // has not run / failed.
    lv_display_t *raw() const { return m_disp; }

protected:
    // Board/panel-specific bring-up. Must create and return an
    // lv_display_t* (stored into m_disp by init()), or nullptr on failure.
    virtual lv_display_t *hw_init() = 0;

    // Post-bring-up tweaks common to all displays. The base sets a dim
    // blue background so a blank screen reads as "on"; override to change.
    virtual void post_init();

    lv_display_t *m_disp = nullptr;
};

// Off-screen fallback display (no panel / CONFIG_TOUCHY_NO_DISPLAY). Its
// flush callback discards every frame. Declared here so main.cpp can
// construct it directly when a board's display_create()/init() yields none.
class HeadlessDisplay : public Display {
protected:
    lv_display_t *hw_init() override;
};

// Board factory — exactly one strong definition per board component.
// Returns a newly-allocated Display for the selected board (caller owns it).
Display *display_create(void);
