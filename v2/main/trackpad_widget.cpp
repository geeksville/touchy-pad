// SPDX-License-Identifier: Apache-2.0

#include "trackpad_widget.h"

#include "usb_hid.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char *TAG = "trackpad";

static uint32_t millis()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

TrackpadWidget::TrackpadWidget(esp_lcd_touch_handle_t touch, lv_obj_t *parent)
    : _touch(touch)
{
    lv_coord_t screen_w = lv_obj_get_width(parent);
    lv_coord_t screen_h = lv_obj_get_height(parent);
    const lv_coord_t debug_h = 30;

    // ── Debug label strip across the top ──────────────────────────────
    _debug_label = lv_label_create(parent);
    lv_obj_set_size(_debug_label, screen_w, debug_h);
    lv_obj_set_pos(_debug_label, 0, 0);
    lv_obj_set_style_bg_color(_debug_label, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(_debug_label, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(_debug_label, lv_color_hex(0x00FF88), 0);
    lv_obj_set_style_text_font(_debug_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_pad_left(_debug_label, 8, 0);
    lv_obj_set_style_pad_top(_debug_label, 6, 0);
    lv_label_set_text(_debug_label, "Touchpad ready");

    // ── Touchpad area filling the rest ────────────────────────────────
    _container = lv_obj_create(parent);
    lv_obj_set_size(_container, screen_w, screen_h - debug_h);
    lv_obj_set_pos(_container, 0, debug_h);
    lv_obj_set_style_bg_color(_container, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_border_width(_container, 0, 0);
    lv_obj_set_style_pad_all(_container, 0, 0);
    lv_obj_set_scrollbar_mode(_container, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *hint = lv_label_create(_container);
    lv_label_set_text(hint, "Touch here");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x334466), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_30, 0);
    lv_obj_center(hint);
}

void TrackpadWidget::poll()
{
    // Pull a fresh multi-point sample from the GT911.
    esp_lcd_touch_read_data(_touch);

    uint16_t xs[MAX_FINGERS] = {0};
    uint16_t ys[MAX_FINGERS] = {0};
    uint16_t strengths[MAX_FINGERS] = {0};
    uint8_t  count = 0;
    bool pressed = esp_lcd_touch_get_coordinates(_touch, xs, ys, strengths,
                                                  &count, MAX_FINGERS);
    if (!pressed) count = 0;
    if (count > MAX_FINGERS) count = MAX_FINGERS;

    uint32_t now = millis();

    // ── New fingers touching down ────────────────────────────────────
    int new_start = (_prev_count < MAX_FINGERS) ? _prev_count : MAX_FINGERS;
    for (int i = new_start; i < count; i++) {
        _fingers[i].active   = true;
        _fingers[i].start_x  = _fingers[i].last_x = static_cast<int16_t>(xs[i]);
        _fingers[i].start_y  = _fingers[i].last_y = static_cast<int16_t>(ys[i]);
        _fingers[i].start_ms = now;
        _fingers[i].dragging = false;
        if (static_cast<uint8_t>(i + 1) > _session_max_fingers) {
            _session_max_fingers = static_cast<uint8_t>(i + 1);
        }
    }

    // ── Single-finger drag (mouse move) ──────────────────────────────
    if (count == 1 && _fingers[0].active) {
        int16_t dx = static_cast<int16_t>(xs[0]) - _fingers[0].last_x;
        int16_t dy = static_cast<int16_t>(ys[0]) - _fingers[0].last_y;

        int16_t total_move = std::abs(static_cast<int>(xs[0]) - _fingers[0].start_x) +
                             std::abs(static_cast<int>(ys[0]) - _fingers[0].start_y);
        if (total_move > TAP_MAX_MOVE) _fingers[0].dragging = true;

        if (_fingers[0].dragging && (dx != 0 || dy != 0)) {
            auto clamp = [](float v) -> int8_t {
                if (v > 127) return 127;
                if (v < -127) return -127;
                return static_cast<int8_t>(v);
            };
            int8_t mx = clamp(dx * MOVE_SCALE);
            int8_t my = clamp(dy * MOVE_SCALE);
            usb_hid_mouse_move(mx, my);
            _setDebug("drag %+d,%+d", mx, my);
        }

        _fingers[0].last_x = static_cast<int16_t>(xs[0]);
        _fingers[0].last_y = static_cast<int16_t>(ys[0]);
    }

    // ── All fingers lifted → check for tap ───────────────────────────
    if (_prev_count > 0 && count == 0) {
        bool is_tap = true;
        for (int i = 0; i < _session_max_fingers && i < MAX_FINGERS; i++) {
            int16_t moved = std::abs(_fingers[i].last_x - _fingers[i].start_x) +
                            std::abs(_fingers[i].last_y - _fingers[i].start_y);
            uint32_t held = now - _fingers[i].start_ms;
            if (moved > TAP_MAX_MOVE || held > TAP_MAX_MS) {
                is_tap = false;
                break;
            }
        }
        if (is_tap) _clickButton(_session_max_fingers);

        _session_max_fingers = 0;
        for (int i = 0; i < MAX_FINGERS; i++) _fingers[i] = FingerState{};
    }

    _prev_count = count;
}

void TrackpadWidget::_clickButton(int finger_count)
{
    uint8_t btn;
    const char *name;
    switch (finger_count) {
        case 1:  btn = HID_MOUSE_BTN_LEFT;   name = "LEFT click";   break;
        case 2:  btn = HID_MOUSE_BTN_RIGHT;  name = "RIGHT click";  break;
        default: btn = HID_MOUSE_BTN_MIDDLE; name = "MIDDLE click"; break;
    }
    usb_hid_mouse_click(btn);
    _setDebug("%s (%d finger%s)", name, finger_count, finger_count > 1 ? "s" : "");
}

void TrackpadWidget::_setDebug(const char *fmt, ...)
{
    char buf[80];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    ESP_LOGI(TAG, "%s", buf);

    if (lvgl_port_lock(50)) {
        lv_label_set_text(_debug_label, buf);
        lvgl_port_unlock();
    }
}
