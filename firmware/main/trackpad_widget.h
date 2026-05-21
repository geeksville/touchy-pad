// SPDX-License-Identifier: Apache-2.0
//
// Stage 18 — multitouch trackpad as a Screen-instantiable widget.
//
// The widget owns a single LVGL container (returned by `obj()`); the
// gesture state machine reacts to LVGL PRESSED / PRESSING / RELEASED
// events on that container and emits USB HID mouse reports. Lifetime
// is tied to the LVGL object: a LV_EVENT_DELETE callback deletes the
// C++ instance when the parent screen is replaced.
//
// Status messages (recognised gestures) are pushed to the shared log
// sink via `log_line_post(...)`; place a `LogLine` widget on the same
// screen to surface them to the user.
//
// Tuning constants come from the v1 proof-of-concept; the gesture rules
// are intentionally unchanged (1/2/3-finger tap = L/R/M click,
// single-finger drag = mouse move). Multi-finger drag / scroll is still
// deferred to a later stage.

#pragma once

#include "lvgl.h"
#include "esp_lcd_touch.h"
#include "proto/widgets.pb.h"

class TrackpadWidget {
public:
    // Tuning constants (kept in sync with v1).
    static constexpr float    MOVE_SCALE    = 1.5f;
    // Pixels of two-finger drag per emitted wheel unit. ~10 px/notch gives
    // a comfortable scroll rate without overshooting on quick flicks.
    static constexpr float    SCROLL_SCALE  = 0.1f;
    static constexpr uint32_t TAP_MAX_MS    = 200;
    static constexpr int16_t  TAP_MAX_MOVE  = 12;
    static constexpr uint8_t  MAX_FINGERS   = 3;

    // Construct the widget as a child of `parent`. The caller is
    // expected to size/style it via the usual `apply_rect` /
    // `apply_style` pass after construction.
    //
    // `touch` may be nullptr: the widget still draws and reacts to LVGL
    // press events, but multi-finger taps degrade to single-finger taps
    // because the LVGL indev only carries one point.
    //
    // `cfg` carries every host-tunable knob from the `Trackpad` proto
    // message: scroll inversion flags, per-finger-count ripple colors,
    // and the optional touch / tap ripple animation descriptors. It is
    // copied by value at construction; the caller does not need to keep
    // it alive.
    TrackpadWidget(esp_lcd_touch_handle_t touch, lv_obj_t *parent,
                   const touchy_Trackpad &cfg);

    // The LVGL container — pass back to `apply_rect` / `apply_style`.
    lv_obj_t *obj() const { return _container; }

private:
    esp_lcd_touch_handle_t _touch;
    lv_obj_t *_container = nullptr;

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

    // Two-finger scroll state (reset on all-fingers-up).
    bool  _scrolling          = false;
    bool  _scroll_axis_locked = false;
    bool  _scroll_axis_h      = false;
    float _scroll_accum_v     = 0.0f;
    float _scroll_accum_h     = 0.0f;

    // Inversion flags set at construction from the proto Trackpad message.
    bool _scroll_invert_y = false;
    bool _scroll_invert_x = false;

    // Ripple animation configs (copied from the proto Trackpad message).
    // `_has_*` mirrors the proto `has_*_ripple` flag so we can fall back
    // to "disabled" cheaply in the hot finger-down loop.
    bool _has_touch_ripple = false;
    bool _has_tap_ripple   = false;
    touchy_RippleAnimation _touch_ripple_cfg{};
    touchy_RippleAnimation _tap_ripple_cfg{};

    // Resolved ripple colors (defaults applied for unset proto fields).
    // Indexed by finger count via `_color_for_count`.
    uint32_t _color_1 = 0x00BFFFu;  // deep sky blue
    uint32_t _color_2 = 0xFFA500u;  // orange
    uint32_t _color_3 = 0xFF44FFu;  // magenta

    // Spawn a ripple animation at widget-local (`cx`, `cy`) coordinates
    // using `cfg` and `color_rgb`. If `back_slot` is non-null, the
    // resulting LVGL object pointer is stored there and the ripple's
    // completion callback nulls the slot (so the widget can later
    // re-target the still-running ripple to a moving finger or a new
    // color). Pass `nullptr` for fire-and-forget ripples (taps).
    void _spawn_ripple(int16_t cx, int16_t cy,
                       const touchy_RippleAnimation &cfg,
                       uint32_t color_rgb,
                       lv_obj_t **back_slot = nullptr);

    // Re-color a single live ripple object in place. Picks bg_color vs
    // border_color based on the stored `has_border` flag in its ctx.
    static void _retint_ripple(lv_obj_t *o, uint32_t color_rgb);

    // Returns `_color_1` / `_color_2` / `_color_3` for `n` = 1 / 2 / 3+.
    uint32_t _color_for_count(uint8_t n) const;

    // One live touch-ripple per finger slot. Tap ripples are not tracked
    // (fire-and-forget). A slot holds the LVGL object pointer while the
    // ripple animates; the opacity-completion callback nulls it again.
    lv_obj_t *_finger_ripples[MAX_FINGERS] = {};

    // LVGL event entry points. `_process()` is the shared state machine,
    // dispatched from PRESSED / PRESSING / RELEASED. `_deleteCb` frees
    // `this` on LV_EVENT_DELETE.
    static void _eventCb(lv_event_t *e);
    static void _deleteCb(lv_event_t *e);
    void _process();

    void _clickButton(int finger_count);
};
