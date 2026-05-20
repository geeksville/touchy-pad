// SPDX-License-Identifier: Apache-2.0

#include "trackpad_widget.h"

#include "log_line.h"
#include "usb_hid.h"

#include "esp_log.h"
#include "esp_timer.h"

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
    // The widget *is* the LVGL container; caller sizes/styles it via the
    // host DSL's Rect / Style. Defaults are kept lean (no padding,
    // dark background) so the trackpad surface is visually unambiguous.
    _container = lv_obj_create(parent);
    lv_obj_set_style_bg_color(_container, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_border_width(_container, 0, 0);
    lv_obj_set_style_pad_all(_container, 0, 0);
    lv_obj_set_scrollbar_mode(_container, LV_SCROLLBAR_MODE_OFF);
    // Disable LVGL scroll detection on the trackpad container: even tiny finger
    // movements would otherwise trigger PRESS_LOST before we can capture a
    // multi-finger tap count.
    lv_obj_clear_flag(_container, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hint = lv_label_create(_container);
    lv_label_set_text(hint, "Touch here");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x334466), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_30, 0);
    lv_obj_center(hint);

    // We hook into the input events LVGL already generates for this object
    // instead of running a busy poll loop. LVGL's indev driver (installed
    // by lvgl_port_add_touch) reads the GT911 every refresh tick and
    // dispatches PRESSED / PRESSING / RELEASED to the object under the
    // finger. From inside those callbacks we re-query the GT911 via
    // esp_lcd_touch_read_data + esp_lcd_touch_get_data to recover the
    // *multi-finger* snapshot (LVGL itself only tracks one point). The
    // explicit read_data() call ensures the freshest hardware state even
    // when several GT911 interrupts coalesce into a single LVGL task
    // wakeup. All event callbacks run on the LVGL task with the port lock
    // already held, so log_line_post() can update sibling LogLine widgets
    // without extra locking.
    lv_obj_add_event_cb(_container, _eventCb,  LV_EVENT_PRESSED,    this);
    lv_obj_add_event_cb(_container, _eventCb,  LV_EVENT_PRESSING,   this);
    lv_obj_add_event_cb(_container, _eventCb,  LV_EVENT_RELEASED,   this);
    // PRESS_LOST fires when LVGL decides to redirect a press (e.g. to a
    // scroll container parent). Treat it like RELEASED so a multi-finger
    // tap is still counted even if the container's press is stolen.
    lv_obj_add_event_cb(_container, _eventCb,  LV_EVENT_PRESS_LOST, this);
    lv_obj_add_event_cb(_container, _deleteCb, LV_EVENT_DELETE,     this);
}

void TrackpadWidget::_eventCb(lv_event_t *e)
{
    static_cast<TrackpadWidget *>(lv_event_get_user_data(e))->_process();
}

void TrackpadWidget::_deleteCb(lv_event_t *e)
{
    delete static_cast<TrackpadWidget *>(lv_event_get_user_data(e));
}

void TrackpadWidget::_process()
{
    // Always pull a fresh hardware snapshot so we don't miss intermediate
    // multitouch states that the LVGL indev may have coalesced.  The LVGL
    // port's lvgl_port_touchpad_read() was called just before this callback
    // fired, but multiple GT911 interrupts (finger-1-down, finger-2-down,
    // lift) can coalesce into a single LVGL task wakeup, leaving the cached
    // data stale.  The extra I2C round-trip is cheap relative to the gesture
    // latency budget.
    if (_touch) esp_lcd_touch_read_data(_touch);

    esp_lcd_touch_point_data_t pts[MAX_FINGERS] = {};
    uint8_t count = 0;
    bool pressed = _touch
        && (esp_lcd_touch_get_data(_touch, pts, &count, MAX_FINGERS) == ESP_OK)
        && count > 0;
    if (!pressed) count = 0;
    if (count > MAX_FINGERS) count = MAX_FINGERS;

    uint32_t now = millis();

    // ── New fingers touching down ────────────────────────────────────
    int new_start = (_prev_count < MAX_FINGERS) ? _prev_count : MAX_FINGERS;
    for (int i = new_start; i < count; i++) {
        _fingers[i].active   = true;
        _fingers[i].start_x  = _fingers[i].last_x = static_cast<int16_t>(pts[i].x);
        _fingers[i].start_y  = _fingers[i].last_y = static_cast<int16_t>(pts[i].y);
        _fingers[i].start_ms = now;
        _fingers[i].dragging = false;
        if (static_cast<uint8_t>(i + 1) > _session_max_fingers) {
            _session_max_fingers = static_cast<uint8_t>(i + 1);
        }
    }

    // ── Single-finger drag (mouse move) ──────────────────────────────
    if (count == 1 && _fingers[0].active) {
        int16_t dx = static_cast<int16_t>(pts[0].x) - _fingers[0].last_x;
        int16_t dy = static_cast<int16_t>(pts[0].y) - _fingers[0].last_y;

        int16_t total_move = std::abs(static_cast<int>(pts[0].x) - _fingers[0].start_x) +
                             std::abs(static_cast<int>(pts[0].y) - _fingers[0].start_y);
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
            log_line_post("drag %+d,%+d", mx, my);
        }

        _fingers[0].last_x = static_cast<int16_t>(pts[0].x);
        _fingers[0].last_y = static_cast<int16_t>(pts[0].y);
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
    log_line_post("%s (%d finger%s)", name, finger_count,
                  finger_count > 1 ? "s" : "");
    ESP_LOGI(TAG, "%s", name);
}
