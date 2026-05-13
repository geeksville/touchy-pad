// SPDX-License-Identifier: Apache-2.0
//
// Ported from v1/firmware/src/trackpad_widget.{h,cpp}. Same gesture rules,
// but rewritten against ESP-IDF: esp_lcd_touch handle for multi-finger data,
// esp_timer for millis(), TinyUSB HID for mouse output.

#pragma once

#include "lvgl.h"
#include "esp_lcd_touch.h"

class TrackpadWidget {
public:
    // Tuning constants (kept in sync with v1).
    static constexpr float    MOVE_SCALE    = 1.5f;
    static constexpr uint32_t TAP_MAX_MS    = 200;
    static constexpr int16_t  TAP_MAX_MOVE  = 12;
    static constexpr uint8_t  MAX_FINGERS   = 3;

    TrackpadWidget(esp_lcd_touch_handle_t touch, lv_obj_t *parent);

    // Call from main loop / dedicated task; reads touch + emits HID reports.
    void poll();

private:
    esp_lcd_touch_handle_t _touch;
    lv_obj_t *_container    = nullptr;
    lv_obj_t *_debug_label  = nullptr;

    struct FingerState {
        bool     active   = false;
        int16_t  start_x  = 0;
        int16_t  start_y  = 0;
        int16_t  last_x   = 0;
        int16_t  last_y   = 0;
        uint32_t start_ms = 0;
        bool     dragging = false;
    };
    FingerState _fingers[MAX_FINGERS]{};
    uint8_t     _prev_count          = 0;
    uint8_t     _session_max_fingers = 0;

    void _clickButton(int finger_count);
    void _setDebug(const char *fmt, ...);
};
