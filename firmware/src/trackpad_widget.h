#pragma once

#include <Arduino.h>
#include <USB.h>
#include <USBHIDMouse.h>
#include <lvgl.h>
#include <esp_display_panel.hpp>

class TrackpadWidget {
public:
    // Tuning constants
    static constexpr float   MOVE_SCALE    = 1.5f;  // pixel-to-HID-unit multiplier
    static constexpr uint32_t TAP_MAX_MS   = 200;   // max duration for a tap
    static constexpr int16_t TAP_MAX_MOVE  = 12;    // max total travel (px) for a tap
    static constexpr uint8_t MAX_FINGERS   = 3;

    // touch: result of board->getTouch().  parent: lv_scr_act() or similar.
    TrackpadWidget(esp_panel::drivers::Touch *touch, lv_obj_t *parent);

    // Call once from setup() – starts USB HID
    void begin();

    // Call every loop() iteration
    void poll();

private:
    esp_panel::drivers::Touch *_touch;
    USBHIDMouse  _mouse;
    lv_obj_t    *_container;
    lv_obj_t    *_debug_label;

    struct FingerState {
        bool     active    = false;
        int16_t  start_x   = 0, start_y  = 0;
        int16_t  last_x    = 0, last_y   = 0;
        uint32_t start_ms  = 0;
        bool     dragging  = false;
    };
    FingerState _fingers[MAX_FINGERS];
    uint8_t     _prev_count         = 0;
    uint8_t     _session_max_fingers = 0;

    void _clickButton(int finger_count);
    void _setDebug(const char *fmt, ...);
};
