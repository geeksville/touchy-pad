// SPDX-License-Identifier: GPL-3.0-or-later
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
    // Default tap-vs-drag hold threshold (ms). Overridable per-instance
    // via `Trackpad.tap_max_ms`.
    static constexpr uint32_t DEFAULT_TAP_MAX_MS = 200;
    static constexpr int16_t  TAP_MAX_MOVE  = 12;
    static constexpr uint8_t  MAX_FINGERS   = 3;
    // Scrollbar grow animation duration (ms). Hardcoded; the host only
    // toggles the bar on/off via `Trackpad.scrollbar_color`.
    static constexpr uint32_t SCROLLBAR_GROW_MS = 200;
    static constexpr uint32_t SCROLLBAR_FADE_MS = 200;
    // Visual thickness of the scrollbar (px), perpendicular to scroll axis.
    static constexpr int16_t  SCROLLBAR_THICK = 4;
    // Initial visible length of the scrollbar handle (px) before it
    // starts growing outward from the centroid of the two fingers.
    // Small enough to read as "a handle the fingers are holding", big
    // enough to actually see at touch-down.
    static constexpr int16_t  SCROLLBAR_INIT_LEN = 24;

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

    // Slot-order-invariant centroid tracking. The GT911 does not
    // guarantee that `pts[i]` keeps a stable identity across frames —
    // notably, when a 2nd finger lands the controller may sort the
    // two reported points by position rather than landing order, so
    // `pts[0]` can flip between the older and newer finger. Per-slot
    // tap-vs-drag detection using `_fingers[i].start_x/y` then sees
    // huge spurious deltas (≈ inter-finger distance) and triggers
    // drag/scroll on a stationary 2-finger tap.
    //
    // We additionally track the SUM of all current pts (a centroid up
    // to a constant divisor of `_centroid_n`) re-anchored each time
    // the peak finger count grows. Centroid is invariant to any
    // reordering of `pts[]`, so a stationary multi-finger tap shows a
    // small `|last − start|` regardless of slot reshuffling.
    int32_t _centroid_start_sum_x = 0;
    int32_t _centroid_start_sum_y = 0;
    int32_t _centroid_last_sum_x  = 0;
    int32_t _centroid_last_sum_y  = 0;
    uint8_t _centroid_n           = 0;

    // Two-finger scroll state (reset on all-fingers-up).
    bool  _scrolling          = false;
    bool  _scroll_axis_locked = false;
    bool  _scroll_axis_h      = false;
    float _scroll_accum_v     = 0.0f;
    float _scroll_accum_h     = 0.0f;

    // Inversion flags set at construction from the proto Trackpad message.
    bool _scroll_invert_y = false;
    bool _scroll_invert_x = false;

    // Per-instance tap-vs-drag hold threshold in ms, sourced from
    // `Trackpad.tap_max_ms` (or `DEFAULT_TAP_MAX_MS` if unset).
    uint32_t _tap_max_ms = DEFAULT_TAP_MAX_MS;

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
    //
    // When `is_touch` is true the ripple uses Stage-24.4 semantics:
    // it starts at `max_radius` and shrinks to a small resting radius
    // while staying fully visible — the parent widget is responsible
    // for calling `_fade_out_ripple()` on finger release. When false
    // (tap ripples), the legacy grow + fade-out + auto-delete behavior
    // is used.
    void _spawn_ripple(int16_t cx, int16_t cy,
                       const touchy_RippleAnimation &cfg,
                       uint32_t color_rgb,
                       lv_obj_t **back_slot = nullptr,
                       bool is_touch = false);

    // Start the dismissal animation for a touch ripple that has been
    // detached from its finger slot. Fades opacity to zero over a
    // short window and deletes the object on completion. No-op for
    // null pointers.
    void _fade_out_ripple(lv_obj_t *o);

    // Re-color a single live ripple object in place. Picks bg_color vs
    // border_color based on the stored `has_border` flag in its ctx.
    static void _retint_ripple(lv_obj_t *o, uint32_t color_rgb);

    // Returns `_color_1` / `_color_2` / `_color_3` for `n` = 1 / 2 / 3+.
    uint32_t _color_for_count(uint8_t n) const;

    // One live touch-ripple per finger slot. Tap ripples are not tracked
    // (fire-and-forget). A slot holds the LVGL object pointer while the
    // ripple animates; the opacity-completion callback nulls it again.
    lv_obj_t *_finger_ripples[MAX_FINGERS] = {};

    // Scroll-progress bar overlay. Spawned on `_spawn_scrollbar` when a
    // two-finger scroll starts and removed on lift via
    // `_dismiss_scrollbar`. Only present when `_has_scrollbar` is true.
    bool      _has_scrollbar    = false;
    uint32_t  _scrollbar_color  = 0u;
    lv_obj_t *_scrollbar        = nullptr;
    // Spawn the scrollbar handle at the centroid of the two fingers
    // (`anchor_x`,`anchor_y` are container-local coords). The handle
    // is perpendicular to the scroll axis (horizontal bar for vertical
    // scrolling and vice-versa), starts as a small box at the centroid
    // and grows outward in both directions to fill the widget along
    // that perpendicular axis, clamping at the edges.
    void _spawn_scrollbar(bool horizontal, int16_t anchor_x, int16_t anchor_y);
    // Update the scrollbar's anchor as the fingers move. Re-positions
    // the handle so that:
    //   * along the grow axis it stays centered on the anchor until it
    //     bumps a container edge, then clamps;
    //   * along the cross (finger-travel) axis it tracks the anchor so
    //     the handle visually follows the fingers, clamped to the
    //     container.
    // No-op if no scrollbar is currently spawned.
    void _update_scrollbar(int16_t anchor_x, int16_t anchor_y);
    void _dismiss_scrollbar();

    // LVGL event entry points. `_process()` is the shared state machine,
    // dispatched from PRESSED / PRESSING / RELEASED. `_deleteCb` frees
    // `this` on LV_EVENT_DELETE.
    static void _eventCb(lv_event_t *e);
    static void _deleteCb(lv_event_t *e);
    void _process();

    void _clickButton(int finger_count);
};
