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

    // Stage 91 — swipe-gesture tuning defaults. Only used for the
    // *secondary* knobs; `swipe_initial_distance` has no default because
    // its presence is the master enable (see `_swipe_enabled`).
    static constexpr uint32_t DEFAULT_SWIPE_INITIAL_TIME = 300;  // ms
    static constexpr uint32_t DEFAULT_SWIPE_ANGLE        = 30;   // degrees

    // Stage 92 — zoom-gesture tuning default for the secondary time knob;
    // `zoom_initial_distance` has no default (its presence is the master
    // enable, see `_zoom_enabled`).
    static constexpr uint32_t DEFAULT_ZOOM_INITIAL_TIME = 300;  // ms

    // Stage 93 — twist-gesture tuning default for the secondary time knob;
    // `twist_initial_angle` has no default (its presence is the master
    // enable, see `_twist_enabled`).
    static constexpr uint32_t DEFAULT_TWIST_INITIAL_TIME = 300;  // ms

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

    // Stage 90 — Action lists for the trackpad gestures, borrowed from
    // the active screen's decoded proto (which outlives the widget — the
    // same lifetime argument as the per-widget ActionSlots). An empty
    // list (count == 0) means "do nothing" for that gesture; the host DSL
    // is responsible for populating sensible defaults. Click lists run on
    // the async macro runner; the high-frequency move / scroll lists run
    // synchronously inline with the live per-frame delta.
    const touchy_Action *_on_left_click    = nullptr;
    pb_size_t            _on_left_click_n   = 0;
    const touchy_Action *_on_middle_click  = nullptr;
    pb_size_t            _on_middle_click_n = 0;
    const touchy_Action *_on_right_click   = nullptr;
    pb_size_t            _on_right_click_n  = 0;
    const touchy_Action *_on_move          = nullptr;
    pb_size_t            _on_move_n         = 0;
    const touchy_Action *_on_scroll        = nullptr;
    pb_size_t            _on_scroll_n       = 0;

    // Stage 91 — single-finger swipe gesture engine. The whole feature is
    // gated on `_swipe_enabled` (set iff the proto carries
    // `swipe_initial_distance`); when false none of the state below is
    // touched and the per-frame check bails immediately.
    bool                _swipe_enabled              = false;
    uint32_t            _swipe_initial_distance     = 0;
    uint32_t            _swipe_initial_time         = DEFAULT_SWIPE_INITIAL_TIME;
    bool                _swipe_has_consecutive      = false;
    uint32_t            _swipe_consecutive_distance = 0;
    uint32_t            _swipe_consecutive_time     = DEFAULT_SWIPE_INITIAL_TIME;
    // tan(angle) as a fixed-point ratio (×256) so the per-frame cone test
    // stays integer-only: a vector is "within the X cone" when
    // |dy|*256 <= |dx|*_swipe_tan_q8 (and symmetrically for Y).
    uint32_t            _swipe_tan_q8               = 0;
    const touchy_Action *_on_left_swipe   = nullptr;
    pb_size_t            _on_left_swipe_n  = 0;
    const touchy_Action *_on_right_swipe  = nullptr;
    pb_size_t            _on_right_swipe_n = 0;
    const touchy_Action *_on_up_swipe     = nullptr;
    pb_size_t            _on_up_swipe_n    = 0;
    const touchy_Action *_on_down_swipe   = nullptr;
    pb_size_t            _on_down_swipe_n  = 0;

    // Per-touch swipe state (reset on all-fingers-up). The anchor is the
    // point/time the current swipe evaluation window started from; it
    // rolls forward on a too-slow drag and re-anchors after each
    // recognised (consecutive) swipe.
    int16_t  _swipe_anchor_x   = 0;
    int16_t  _swipe_anchor_y   = 0;
    uint32_t _swipe_anchor_ms  = 0;
    bool     _swipe_done       = false;  // single-shot guard (no consecutive)
    bool     _swipe_locked     = false;  // consecutive mode: axis/dir fixed
    bool     _swipe_locked_h   = false;  // locked axis: true = X (horizontal)
    int8_t   _swipe_locked_sign = 0;     // locked direction: +1 / -1

    // Run the swipe engine for the current single-finger sample at
    // widget-input coords (`x`, `y`) at time `now`. No-op unless
    // `_swipe_enabled`.
    void _swipe_process(int16_t x, int16_t y, uint32_t now);
    // Fire the directional swipe action list + log, given the travel
    // `(dx, dy)` since the anchor. `axis_h`/`sign` pick the direction.
    void _emit_swipe(bool axis_h, int8_t sign, int16_t dx, int16_t dy);

    // Stage 92 — two-finger zoom (pinch) gesture engine. Gated on
    // `_zoom_enabled` (set iff the proto carries `zoom_initial_distance`);
    // when false none of the state below is touched and the per-frame
    // check bails immediately. The measured quantity is the *span* (the
    // Euclidean distance between the two touch points); the axis the
    // fingers move along is irrelevant — only the change in span matters.
    bool                _zoom_enabled              = false;
    uint32_t            _zoom_initial_distance     = 0;
    uint32_t            _zoom_initial_time         = DEFAULT_ZOOM_INITIAL_TIME;
    bool                _zoom_has_consecutive      = false;
    uint32_t            _zoom_consecutive_distance = 0;
    uint32_t            _zoom_consecutive_time     = DEFAULT_ZOOM_INITIAL_TIME;
    const touchy_Action *_on_zoom_in   = nullptr;
    pb_size_t            _on_zoom_in_n  = 0;
    const touchy_Action *_on_zoom_out  = nullptr;
    pb_size_t            _on_zoom_out_n = 0;

    // Per-touch zoom state (reset on all-fingers-up). The anchor is the
    // span/time the current zoom evaluation window started from; it rolls
    // forward on a too-slow pinch and re-anchors after each recognised
    // (consecutive) zoom.
    float    _zoom_anchor_span = 0.0f;
    uint32_t _zoom_anchor_ms   = 0;
    bool     _zoom_done        = false;  // single-shot guard (no consecutive)
    bool     _zoom_locked      = false;  // consecutive mode: direction fixed
    int8_t   _zoom_dir_sign    = 0;      // locked direction: +1 in / -1 out

    // Run the zoom engine for the current two-finger `span` (px, the
    // distance between the two touch points) at time `now`. No-op unless
    // `_zoom_enabled`.
    void _zoom_process(float span, uint32_t now);
    // Fire the zoom action list + log given the signed span change
    // `delta` (Relative X; + = zoom in, - = zoom out).
    void _emit_zoom(int32_t delta);

    // Stage 93 — two-finger twist (rotate) gesture engine. Gated on
    // `_twist_enabled` (set iff the proto carries `twist_initial_angle`);
    // when false none of the state below is touched and the per-frame
    // check bails immediately. The measured quantity is the *angle* of the
    // line between the two touch points, treated as an undirected line
    // (mod 180°) so a touch-controller slot swap never injects a 180° jump.
    bool                _twist_enabled           = false;
    uint32_t            _twist_initial_angle     = 0;
    uint32_t            _twist_initial_time      = DEFAULT_TWIST_INITIAL_TIME;
    bool                _twist_has_consecutive   = false;
    uint32_t            _twist_consecutive_angle = 0;
    uint32_t            _twist_consecutive_time  = DEFAULT_TWIST_INITIAL_TIME;
    const touchy_Action *_on_cw_twist   = nullptr;
    pb_size_t            _on_cw_twist_n  = 0;
    const touchy_Action *_on_ccw_twist  = nullptr;
    pb_size_t            _on_ccw_twist_n = 0;

    // Per-touch twist state (reset on all-fingers-up). The anchor is the
    // angle/time the current twist evaluation window started from; it rolls
    // forward on a too-slow rotation and re-anchors after each recognised
    // (consecutive) twist.
    float    _twist_anchor_deg = 0.0f;
    uint32_t _twist_anchor_ms  = 0;
    bool     _twist_done       = false;  // single-shot guard (no consecutive)
    bool     _twist_locked     = false;  // consecutive mode: direction fixed
    int8_t   _twist_dir_sign   = 0;      // locked direction: +1 cw / -1 ccw

    // Run the twist engine for the current two-finger inter-touch `angle`
    // (degrees, the orientation of `p1 - p0`) at time `now`. No-op unless
    // `_twist_enabled`.
    void _twist_process(float angle, uint32_t now);
    // Fire the twist action list + log given the signed angle change
    // `delta` in degrees (Relative X; + = clockwise, - = counter-clockwise).
    void _emit_twist(int32_t delta);

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
