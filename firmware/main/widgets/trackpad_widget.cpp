// SPDX-License-Identifier: GPL-3.0-or-later

#include "trackpad_widget.h"
#include "tc_tag.h"

#include "log_line.h"
#include "macros.h"
#include "widget_actions.h"

#include "esp_log.h"
#include "esp_timer.h"

#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <new>

static const char *TAG = TOUCHY_TAG("trackpad");

static uint32_t millis()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

// ---------------------------------------------------------------------------
// Ripple animation helpers
//
// A ripple is a single transient LVGL object: a coloured circle child of
// the trackpad container, spawned at the touch point, that grows from
// radius 0 to `cfg.max_radius` while fading `cfg.start_opa` → 0 over
// `cfg.duration_ms`. Implementation follows the lv_anim docs / the
// `lv_example_anim_2.c` pattern: one reused `lv_anim_t` template drives
// two parallel animations (size and opacity) on the same target. The
// opacity anim's `completed_cb` deletes the widget; widget deletion
// auto-cancels the still-running size anim, so no extra bookkeeping is
// required.
// ---------------------------------------------------------------------------
namespace {

// Padding (in px) added on every side when invalidating an object's
// pre-move footprint. Rounded-corner anti-aliasing writes a sub-pixel
// fringe slightly outside the integer bounding box; without this pad
// translating/resizing objects leaves faint AA artifacts behind.
constexpr int32_t AA_PAD = 4;

// Invalidate the object's current (pre-move) area on its parent,
// expanded by AA_PAD. Call this immediately before any
// `lv_obj_set_pos` / `lv_obj_set_size` so the old footprint (plus its
// AA fringe) gets repainted along with the new one.
void invalidate_old_footprint(lv_obj_t *obj)
{
    lv_obj_t *parent = lv_obj_get_parent(obj);
    if (!parent) return;
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    a.x1 -= AA_PAD;
    a.y1 -= AA_PAD;
    a.x2 += AA_PAD;
    a.y2 += AA_PAD;
    lv_obj_invalidate_area(parent, &a);
}

struct RippleCtx {
    int16_t cx;  // ripple center, widget-local px
    int16_t cy;
    // If non-null, points to the TrackpadWidget slot that tracks this
    // ripple. The opacity-completion callback nulls `*back_slot` (when
    // it still points at us) so the widget stops trying to follow /
    // recolor a ripple that's already gone. nullptr = untracked
    // (tap ripples).
    lv_obj_t **back_slot;
    // Remembers whether this ripple is rendered as a filled disc
    // (style: bg_color) or as a hollow ring (style: border_color), so
    // `_retint_ripple` knows which style property to overwrite when
    // the parent widget asks for an on-the-fly color change.
    bool has_border;
};

// Exec callback for the radius animation. `v` is the current radius;
// we resize the object to (2v × 2v) and reposition so the center stays
// pinned at the stored (cx, cy) regardless of size — and crucially
// reads `cx`/`cy` fresh each tick, so the parent widget can move the
// center while the animation is still running (drag-to-follow).
void ripple_size_cb(void *var, int32_t v)
{
    auto *o   = static_cast<lv_obj_t *>(var);
    auto *ctx = static_cast<RippleCtx *>(lv_obj_get_user_data(o));
    if (!ctx) return;
    invalidate_old_footprint(o);
    lv_obj_set_size(o, v * 2, v * 2);
    lv_obj_set_pos(o, ctx->cx - v, ctx->cy - v);
}

// Exec callback for the opacity animation. Uses selector 0 = main part /
// default state, the same selector our size/border/bg setup writes to.
void ripple_opa_cb(void *var, int32_t opa)
{
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(var),
                         static_cast<lv_opa_t>(opa), 0);
}

// `completed_cb` for the opacity animation. By the time this fires the
// ripple has fully faded; delete the LVGL object (which auto-cancels
// any sibling animations targeting it) and free the heap-allocated
// RippleCtx we stashed in user_data.
void ripple_opa_completed(lv_anim_t *a)
{
    auto *o   = static_cast<lv_obj_t *>(a->var);
    auto *ctx = static_cast<RippleCtx *>(lv_obj_get_user_data(o));
    if (ctx && ctx->back_slot && *ctx->back_slot == o) {
        // Detach from the widget's per-finger tracking slot so the
        // widget stops trying to follow / recolor a doomed ripple.
        *ctx->back_slot = nullptr;
    }
    lv_obj_set_user_data(o, nullptr);
    delete ctx;
    lv_obj_delete(o);
}

lv_anim_path_cb_t ripple_path_for(touchy_AnimPath p)
{
    switch (p) {
        case touchy_AnimPath_EASE_IN:     return lv_anim_path_ease_in;
        case touchy_AnimPath_EASE_OUT:    return lv_anim_path_ease_out;
        case touchy_AnimPath_EASE_IN_OUT: return lv_anim_path_ease_in_out;
        case touchy_AnimPath_OVERSHOOT:   return lv_anim_path_overshoot;
        case touchy_AnimPath_BOUNCE:      return lv_anim_path_bounce;
        case touchy_AnimPath_STEP:        return lv_anim_path_step;
        case touchy_AnimPath_LINEAR:
        default:                                    return lv_anim_path_linear;
    }
}

}  // namespace

TrackpadWidget::TrackpadWidget(esp_lcd_touch_handle_t touch, lv_obj_t *parent,
                               const touchy_Trackpad &cfg)
    : _touch(touch),
      _scroll_invert_y(cfg.scroll_invert_y),
      _scroll_invert_x(cfg.scroll_invert_x),
      _has_touch_ripple(cfg.has_touch_ripple),
      _has_tap_ripple(cfg.has_tap_ripple),
      _touch_ripple_cfg(cfg.touch_ripple),
      _tap_ripple_cfg(cfg.tap_ripple)
{
    // Resolve ripple colors: explicit proto value wins, otherwise keep
    // the built-in defaults from the header initialiser.
    if (cfg.has_left_touch_color)   _color_1 = cfg.left_touch_color;
    if (cfg.has_right_touch_color)  _color_2 = cfg.right_touch_color;
    if (cfg.has_middle_touch_color) _color_3 = cfg.middle_touch_color;

    // Stage 24.4: optional scrollbar overlay and configurable tap hold.
    if (cfg.has_scrollbar_color) {
        _has_scrollbar   = true;
        _scrollbar_color = cfg.scrollbar_color;
    }
    if (cfg.has_tap_max_ms) _tap_max_ms = cfg.tap_max_ms;

    // Stage 90 — borrow the gesture Action lists. These pointers index
    // into the active screen's decoded proto (FT_POINTER heap), which
    // outlives this widget; we never copy or free them.
    _on_left_click    = cfg.on_left_click;
    _on_left_click_n  = cfg.on_left_click_count;
    _on_middle_click  = cfg.on_middle_click;
    _on_middle_click_n = cfg.on_middle_click_count;
    _on_right_click   = cfg.on_right_click;
    _on_right_click_n = cfg.on_right_click_count;
    _on_move          = cfg.on_move;
    _on_move_n        = cfg.on_move_count;
    _on_scroll        = cfg.on_scroll;
    _on_scroll_n      = cfg.on_scroll_count;

    // Stage 91 — swipe engine. Master switch is the presence of
    // `swipe_initial_distance`; everything else has a sensible default.
    _swipe_enabled = cfg.has_swipe_initial_distance;
    if (_swipe_enabled) {
        _swipe_initial_distance = cfg.swipe_initial_distance;
        _swipe_initial_time =
            cfg.has_swipe_initial_time ? cfg.swipe_initial_time
                                       : DEFAULT_SWIPE_INITIAL_TIME;
        _swipe_has_consecutive = cfg.has_swipe_consecutive_distance;
        _swipe_consecutive_distance =
            cfg.has_swipe_consecutive_distance ? cfg.swipe_consecutive_distance
                                               : 0;
        _swipe_consecutive_time =
            cfg.has_swipe_consecutive_time ? cfg.swipe_consecutive_time
                                           : _swipe_initial_time;
        uint32_t angle =
            cfg.has_swipe_angle ? cfg.swipe_angle : DEFAULT_SWIPE_ANGLE;
        if (angle > 89) angle = 89;  // keep tan finite
        // One-time float trig at construction (not in the hot loop) →
        // fixed-point tangent ×256 for the per-frame integer cone test.
        _swipe_tan_q8 = (uint32_t)(tanf((float)angle * 3.14159265f / 180.0f)
                                   * 256.0f + 0.5f);
        _on_left_swipe    = cfg.on_left_swipe;
        _on_left_swipe_n  = cfg.on_left_swipe_count;
        _on_right_swipe   = cfg.on_right_swipe;
        _on_right_swipe_n = cfg.on_right_swipe_count;
        _on_up_swipe      = cfg.on_up_swipe;
        _on_up_swipe_n    = cfg.on_up_swipe_count;
        _on_down_swipe    = cfg.on_down_swipe;
        _on_down_swipe_n  = cfg.on_down_swipe_count;
    }

    // The widget *is* the LVGL container; caller sizes/styles it via the
    // host DSL's Rect / Style. The container is transparent so sibling
    // widgets drawn earlier in the same grid cell (background fill, hint
    // label) remain visible through it.
    _container = lv_obj_create(parent);
    lv_obj_set_style_bg_opa(_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_container, 0, 0);
    lv_obj_set_style_pad_all(_container, 0, 0);
    lv_obj_set_scrollbar_mode(_container, LV_SCROLLBAR_MODE_OFF);
    // Disable LVGL scroll detection on the trackpad container: even tiny finger
    // movements would otherwise trigger PRESS_LOST before we can capture a
    // multi-finger tap count.
    lv_obj_clear_flag(_container, LV_OBJ_FLAG_SCROLLABLE);

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
    // If `count` grew, re-anchor the centroid sums using ALL currently
    // reported pts at their CURRENT positions (not stale `_fingers[].start`
    // — those may be from a different slot ordering). This is the
    // slot-order-invariant baseline used by the tap-vs-drag check.
    uint8_t prev_max = _session_max_fingers;
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
        // Spawn the touch ripple, tracked in `_finger_ripples[i]` so
        // subsequent finger movements can drag its center along and
        // count-changes can recolor every still-running ripple in
        // unison (handled below). Suppressed while a scrollbar handle
        // is showing: during two-finger scroll the user only wants the
        // scrollbar visual, not finger ripples competing with it.
        if (_has_touch_ripple && !_scrollbar) {
            int16_t lx = static_cast<int16_t>(pts[i].x) -
                         static_cast<int16_t>(lv_obj_get_x(_container));
            int16_t ly = static_cast<int16_t>(pts[i].y) -
                         static_cast<int16_t>(lv_obj_get_y(_container));
            _spawn_ripple(lx, ly, _touch_ripple_cfg,
                          _color_for_count(count), &_finger_ripples[i],
                          /*is_touch=*/true);
        }
    }
    if (_session_max_fingers > prev_max) {
        int32_t sx = 0, sy = 0;
        for (int i = 0; i < count; i++) {
            sx += pts[i].x;
            sy += pts[i].y;
        }
        _centroid_start_sum_x = _centroid_last_sum_x = sx;
        _centroid_start_sum_y = _centroid_last_sum_y = sy;
        _centroid_n           = count;
    }

    // ── Stage 91: seed the swipe anchor on a fresh first-finger touch ─
    if (_swipe_enabled && _prev_count == 0 && count >= 1) {
        _swipe_anchor_x    = static_cast<int16_t>(pts[0].x);
        _swipe_anchor_y    = static_cast<int16_t>(pts[0].y);
        _swipe_anchor_ms   = now;
        _swipe_done        = false;
        _swipe_locked      = false;
        _swipe_locked_h    = false;
        _swipe_locked_sign = 0;
    }

    // ── Recolor all live touch ripples when the finger count changes ─
    // Without this, landing the 2nd finger would correctly spawn an
    // orange (right-click) ripple under itself but the 1st finger's
    // cyan (left-click) ripple — already mid-animation — would keep
    // its stale color, so the user sees a multi-coloured cluster
    // during 2/3-finger drags. Walk the tracked ripples and overwrite
    // every one to the current count's color.
    if (count > 0 && count != _prev_count) {
        const uint32_t now_color = _color_for_count(count);
        for (int i = 0; i < MAX_FINGERS; i++) {
            _retint_ripple(_finger_ripples[i], now_color);
        }
    }

    // ── Drag-to-follow: keep each ripple centered on its finger ──────
    // Update the ctx (read by the size anim's exec cb each tick) AND
    // reposition the object directly using its current size, so even
    // ripples whose size animation has already completed continue to
    // chase the finger during the fade-out tail.
    for (int i = 0; i < count; i++) {
        lv_obj_t *o = _finger_ripples[i];
        if (!o) continue;
        auto *ctx = static_cast<RippleCtx *>(lv_obj_get_user_data(o));
        if (!ctx) continue;
        ctx->cx = static_cast<int16_t>(pts[i].x) -
                  static_cast<int16_t>(lv_obj_get_x(_container));
        ctx->cy = static_cast<int16_t>(pts[i].y) -
                  static_cast<int16_t>(lv_obj_get_y(_container));
        int16_t r = static_cast<int16_t>(lv_obj_get_width(o) / 2);
        invalidate_old_footprint(o);
        lv_obj_set_pos(o, ctx->cx - r, ctx->cy - r);
    }

    // ── Single-finger drag (mouse move) ──────────────────────────────
    // Gated on `count == _session_max_fingers` so that during a
    // multi-finger session, once any finger lifts (count drops to 1
    // while max is 2/3), we do NOT enter this branch: the GT911
    // reuses slot 0 for the remaining physical finger and pts[0]
    // would now refer to a different finger than `_fingers[0]` was
    // tracking. Updating `_fingers[0].last_x/y` with the survivor's
    // coordinates contaminates the tap-vs-drag movement check at
    // all-fingers-lifted time, causing two-finger taps to fail to be
    // recognised as right-clicks (and just silently disappear).
    if (count == 1 && _fingers[0].active && _session_max_fingers == 1) {
        int16_t dx = static_cast<int16_t>(pts[0].x) - _fingers[0].last_x;
        int16_t dy = static_cast<int16_t>(pts[0].y) - _fingers[0].last_y;

        // Centroid (= single finger here) — refresh and re-check drag.
        _centroid_last_sum_x = pts[0].x;
        _centroid_last_sum_y = pts[0].y;
        int32_t centroid_move =
            std::abs(_centroid_last_sum_x - _centroid_start_sum_x) +
            std::abs(_centroid_last_sum_y - _centroid_start_sum_y);
        if (centroid_move > static_cast<int32_t>(TAP_MAX_MOVE) * _centroid_n) {
            _fingers[0].dragging = true;
        }

        if (_fingers[0].dragging && (dx != 0 || dy != 0)) {
            auto clamp = [](float v) -> int8_t {
                if (v > 127) return 127;
                if (v < -127) return -127;
                return static_cast<int8_t>(v);
            };
            int8_t mx = clamp(dx * MOVE_SCALE);
            int8_t my = clamp(dy * MOVE_SCALE);
            // Stage 90: run the on_move Action list inline with the live
            // delta as the ambient Move (mouse_move steps with unset
            // dx/dy pick it up). Empty list → no-op.
            MacroMoveCtx ctx{mx, my};
            widget_run_actions_inline(_on_move, _on_move_n, &ctx);
            log_line_post("drag %+d,%+d", mx, my);
        }

        // Stage 91 — swipe detection runs in parallel with the drag above
        // (additive; it does not suppress on_move). No-op unless enabled.
        _swipe_process(static_cast<int16_t>(pts[0].x),
                       static_cast<int16_t>(pts[0].y), now);

        _fingers[0].last_x = static_cast<int16_t>(pts[0].x);
        _fingers[0].last_y = static_cast<int16_t>(pts[0].y);
    }

    // ── Two-finger drag (scroll wheel) ───────────────────────────────
    // Lock to the dominant axis once movement crosses TAP_MAX_MOVE; that
    // also flags the fingers as `dragging`, suppressing the right-click
    // that a stationary 2-finger tap would otherwise produce on release.
    // Also gated on `count == _session_max_fingers`: if a third finger
    // briefly grazed the pad and lifted, we keep the session at max=3
    // and stop processing scroll until everyone lifts.
    if (count == 2 && _fingers[0].active && _fingers[1].active
            && _session_max_fingers == 2) {
        float dx0 = static_cast<float>(pts[0].x) - _fingers[0].last_x;
        float dy0 = static_cast<float>(pts[0].y) - _fingers[0].last_y;
        float dx1 = static_cast<float>(pts[1].x) - _fingers[1].last_x;
        float dy1 = static_cast<float>(pts[1].y) - _fingers[1].last_y;
        float dx = (dx0 + dx1) * 0.5f;
        float dy = (dy0 + dy1) * 0.5f;

        // Refresh the slot-order-invariant centroid sum for this frame
        // (only when at peak count — see comment on `_centroid_*`).
        _centroid_last_sum_x = static_cast<int32_t>(pts[0].x) + pts[1].x;
        _centroid_last_sum_y = static_cast<int32_t>(pts[0].y) + pts[1].y;
        // Threshold scales with `_centroid_n` because we are comparing
        // sums (not averages) of N fingers; per-finger budget stays at
        // TAP_MAX_MOVE.
        int32_t cdx = _centroid_last_sum_x - _centroid_start_sum_x;
        int32_t cdy = _centroid_last_sum_y - _centroid_start_sum_y;
        int32_t centroid_move = std::abs(cdx) + std::abs(cdy);
        if (centroid_move > static_cast<int32_t>(TAP_MAX_MOVE) * _centroid_n) {
            _fingers[0].dragging = true;
            _fingers[1].dragging = true;
            _scrolling = true;
        }

        if (_scrolling) {
            if (!_scroll_axis_locked) {
                // Use centroid travel for axis selection so a swapped
                // slot index can't flip the chosen axis.
                int adx = std::abs(static_cast<int>(cdx));
                int ady = std::abs(static_cast<int>(cdy));
                _scroll_axis_h      = adx > ady;
                _scroll_axis_locked = true;
                // First frame after axis lock-in: spawn the optional
                // scrollbar overlay. The bar is a small handle centered
                // on the two-finger centroid and perpendicular to the
                // scroll axis; it grows outward to fill the widget
                // along that perpendicular dimension.
                if (_has_scrollbar && !_scrollbar) {
                    int16_t cx = static_cast<int16_t>(
                        (static_cast<int>(pts[0].x) + static_cast<int>(pts[1].x)) / 2 -
                        lv_obj_get_x(_container));
                    int16_t cy = static_cast<int16_t>(
                        (static_cast<int>(pts[0].y) + static_cast<int>(pts[1].y)) / 2 -
                        lv_obj_get_y(_container));
                    _spawn_scrollbar(_scroll_axis_h, cx, cy);
                    // The scrollbar is now the primary visual; fade
                    // out any active finger ripples so they don't
                    // compete with the handle during the scroll.
                    for (int i = 0; i < MAX_FINGERS; i++) {
                        if (_finger_ripples[i]) {
                            _fade_out_ripple(_finger_ripples[i]);
                            _finger_ripples[i] = nullptr;
                        }
                    }
                }
            }
            if (_scroll_axis_h) {
                // HID AC Pan: positive = scroll right. Fingers moving
                // right should pan content right; invert flips that.
                float sign = _scroll_invert_x ? -1.0f : 1.0f;
                _scroll_accum_h += sign * dx * SCROLL_SCALE;
            } else {
                // HID Wheel: positive = scroll up. Fingers moving down
                // should scroll content down → negative wheel ticks.
                // Invert gives macOS "natural scrolling" (fingers-down = up).
                float sign = _scroll_invert_y ? 1.0f : -1.0f;
                _scroll_accum_v += sign * dy * SCROLL_SCALE;
            }
            auto emit = [](float &accum) -> int8_t {
                int v = static_cast<int>(accum);
                accum -= static_cast<float>(v);
                if (v >  127) v =  127;
                if (v < -127) v = -127;
                return static_cast<int8_t>(v);
            };
            int8_t v = emit(_scroll_accum_v);
            int8_t h = emit(_scroll_accum_h);
            if (v != 0 || h != 0) {
                // Stage 90: run the on_scroll Action list inline. A
                // scroll_move step maps dy→vertical wheel, dx→horizontal
                // pan, so the ambient ctx carries {dx=h, dy=v}. Empty
                // list → no-op.
                MacroMoveCtx ctx{h, v};
                widget_run_actions_inline(_on_scroll, _on_scroll_n, &ctx);
                log_line_post("scroll v=%+d h=%+d", v, h);
            }

            // Slide the scrollbar handle to follow the fingers as they
            // drag. The grow animation owns the handle's length; this
            // just updates its position via the centroid.
            if (_scrollbar) {
                int16_t cx = static_cast<int16_t>(
                    (static_cast<int>(pts[0].x) + static_cast<int>(pts[1].x)) / 2 -
                    lv_obj_get_x(_container));
                int16_t cy = static_cast<int16_t>(
                    (static_cast<int>(pts[0].y) + static_cast<int>(pts[1].y)) / 2 -
                    lv_obj_get_y(_container));
                _update_scrollbar(cx, cy);
            }
        }

        _fingers[0].last_x = static_cast<int16_t>(pts[0].x);
        _fingers[0].last_y = static_cast<int16_t>(pts[0].y);
        _fingers[1].last_x = static_cast<int16_t>(pts[1].x);
        _fingers[1].last_y = static_cast<int16_t>(pts[1].y);
    }

    // ── Fingers that lifted this frame → fade out their ripples ──────
    // Indices in [count, _prev_count) were touching last frame but are
    // gone this frame. Detach each ripple from its slot and start the
    // dismissal animation so the trailing "last frame following the
    // finger" sticks around just long enough to be seen.
    if (count < _prev_count) {
        for (int i = count; i < _prev_count && i < MAX_FINGERS; i++) {
            lv_obj_t *o = _finger_ripples[i];
            if (o) {
                _finger_ripples[i] = nullptr;
                _fade_out_ripple(o);
            }
        }
    }

    // ── All fingers lifted → check for tap ───────────────────────────
    if (_prev_count > 0 && count == 0) {
        // Slot-order-invariant tap check: compare centroid sum at last
        // peak-count frame to centroid sum at the moment peak count
        // was first established. The threshold scales with the number
        // of fingers contributing to the sums (per-finger budget =
        // TAP_MAX_MOVE).
        //
        // Hold-time still uses per-finger `start_ms` (slot identity
        // doesn't matter for time): if any finger was down too long
        // it isn't a tap.
        int32_t centroid_move =
            std::abs(_centroid_last_sum_x - _centroid_start_sum_x) +
            std::abs(_centroid_last_sum_y - _centroid_start_sum_y);
        bool is_tap = _centroid_n > 0 &&
            centroid_move <= static_cast<int32_t>(TAP_MAX_MOVE) * _centroid_n;
        if (is_tap) {
            for (int i = 0; i < _session_max_fingers && i < MAX_FINGERS; i++) {
                uint32_t held = now - _fingers[i].start_ms;
                if (held > _tap_max_ms) { is_tap = false; break; }
            }
        }
        if (is_tap) {
            // Spawn the tap-burst ripple at the centroid of the tapping
            // fingers' touch-down positions; color reflects the gesture
            // (1/2/3-finger \u2192 left/right/middle palette).
            if (_has_tap_ripple && _session_max_fingers > 0) {
                int32_t sx = 0, sy = 0;
                int n = _session_max_fingers;
                if (n > MAX_FINGERS) n = MAX_FINGERS;
                for (int i = 0; i < n; i++) {
                    sx += _fingers[i].start_x;
                    sy += _fingers[i].start_y;
                }
                int16_t lx = static_cast<int16_t>(sx / n) -
                             static_cast<int16_t>(lv_obj_get_x(_container));
                int16_t ly = static_cast<int16_t>(sy / n) -
                             static_cast<int16_t>(lv_obj_get_y(_container));
                _spawn_ripple(lx, ly, _tap_ripple_cfg,
                              _color_for_count(_session_max_fingers));
            }
            _clickButton(_session_max_fingers);
        }

        _session_max_fingers = 0;
        _scrolling           = false;
        _scroll_axis_locked  = false;
        _scroll_axis_h       = false;
        _scroll_accum_v      = 0.0f;
        _scroll_accum_h      = 0.0f;
        _centroid_n          = 0;
        _centroid_start_sum_x = _centroid_start_sum_y = 0;
        _centroid_last_sum_x  = _centroid_last_sum_y  = 0;
        // Stage 91 — reset the per-touch swipe window.
        _swipe_done        = false;
        _swipe_locked      = false;
        _swipe_locked_sign = 0;
        for (int i = 0; i < MAX_FINGERS; i++) _fingers[i] = FingerState{};
        // Take down the scrollbar (if it was up). Animation is a quick
        // fade-out; the bar deletes itself when the opacity anim ends.
        if (_scrollbar) _dismiss_scrollbar();
    }

    _prev_count = count;
}

void TrackpadWidget::_clickButton(int finger_count)
{
    // Stage 90: map finger count → gesture Action list and run it on the
    // async macro runner (clicks are low-frequency, unlike move/scroll).
    // 1 finger = left, 2 = right, 3+ = middle. Empty list → no-op.
    const touchy_Action *actions;
    pb_size_t            count;
    const char         *name;
    switch (finger_count) {
        case 1:  actions = _on_left_click;   count = _on_left_click_n;
                 name = "LEFT click";   break;
        case 2:  actions = _on_right_click;  count = _on_right_click_n;
                 name = "RIGHT click";  break;
        default: actions = _on_middle_click; count = _on_middle_click_n;
                 name = "MIDDLE click"; break;
    }
    widget_run_actions(actions, count);
    log_line_post("%s (%d finger%s)", name, finger_count,
                  finger_count > 1 ? "s" : "");
    ESP_LOGI(TAG, "%s", name);
}

void TrackpadWidget::_emit_swipe(bool axis_h, int8_t sign, int16_t dx, int16_t dy)
{
    const touchy_Action *actions = nullptr;
    pb_size_t            n        = 0;
    const char         *name     = "?";
    if (axis_h) {
        if (sign > 0) { actions = _on_right_swipe; n = _on_right_swipe_n; name = "right"; }
        else          { actions = _on_left_swipe;  n = _on_left_swipe_n;  name = "left";  }
    } else {
        if (sign > 0) { actions = _on_down_swipe;  n = _on_down_swipe_n;  name = "down";  }
        else          { actions = _on_up_swipe;    n = _on_up_swipe_n;    name = "up";    }
    }
    // Report the relative swipe travel as the ambient Move delta, run
    // inline (same path as on_move). Empty list → no-op, but we still log.
    int8_t cx = dx >  127 ? 127 : dx < -127 ? -127 : (int8_t)dx;
    int8_t cy = dy >  127 ? 127 : dy < -127 ? -127 : (int8_t)dy;
    MacroMoveCtx ctx{cx, cy};
    widget_run_actions_inline(actions, n, &ctx);
    log_line_post("swipe %s %+d,%+d", name, dx, dy);
    ESP_LOGI(TAG, "swipe %s dx=%d dy=%d", name, (int)dx, (int)dy);
}

void TrackpadWidget::_swipe_process(int16_t x, int16_t y, uint32_t now)
{
    if (!_swipe_enabled || _swipe_done) return;

    int32_t dx = x - _swipe_anchor_x;
    int32_t dy = y - _swipe_anchor_y;
    uint32_t t = now - _swipe_anchor_ms;

    if (!_swipe_locked) {
        // ── Initial recognition ──────────────────────────────────────
        int32_t dist2 = dx * dx + dy * dy;
        int32_t need  = (int32_t)_swipe_initial_distance;
        if (dist2 < need * need) return;  // distance threshold not met yet

        if (t > _swipe_initial_time) {
            // Distance reached but too slow → roll the window forward so
            // a later quick flick can still register (a slow drag never
            // latches a swipe).
            _swipe_anchor_x  = x;
            _swipe_anchor_y  = y;
            _swipe_anchor_ms = now;
            return;
        }

        // ── Angle / dominant-axis test (integer, fixed-point tan ×256) ─
        int32_t adx = dx < 0 ? -dx : dx;
        int32_t ady = dy < 0 ? -dy : dy;
        bool axis_h = adx >= ady;
        // Within the dominant axis's cone? off-axis*256 <= on-axis*tan.
        bool in_cone = axis_h
            ? (ady * 256 <= adx * (int32_t)_swipe_tan_q8)
            : (adx * 256 <= ady * (int32_t)_swipe_tan_q8);
        if (!in_cone) return;  // too diagonal — not a swipe

        int8_t sign = axis_h ? (dx > 0 ? 1 : -1) : (dy > 0 ? 1 : -1);
        _emit_swipe(axis_h, sign, (int16_t)dx, (int16_t)dy);

        if (_swipe_has_consecutive) {
            // Re-anchor at the recognition point and lock the direction;
            // subsequent events use the consecutive thresholds.
            _swipe_locked      = true;
            _swipe_locked_h    = axis_h;
            _swipe_locked_sign = sign;
            _swipe_anchor_x    = x;
            _swipe_anchor_y    = y;
            _swipe_anchor_ms   = now;
        } else {
            _swipe_done = true;  // single event per touch
        }
        return;
    }

    // ── Consecutive mode: only the locked axis matters ───────────────
    int32_t along = _swipe_locked_h ? dx : dy;
    along *= _swipe_locked_sign;  // travel in the locked direction (signed)
    int32_t need = (int32_t)_swipe_consecutive_distance;
    if (along < need) {
        // Not far enough yet. If the consecutive window expired without
        // clearing the distance, roll the anchor forward in time so the
        // user can resume (keeps a paused-then-resumed flick alive).
        if (t > _swipe_consecutive_time) {
            _swipe_anchor_x  = x;
            _swipe_anchor_y  = y;
            _swipe_anchor_ms = now;
        }
        return;
    }
    if (t > _swipe_consecutive_time) {
        // Cleared the distance but too slowly → re-anchor, no event.
        _swipe_anchor_x  = x;
        _swipe_anchor_y  = y;
        _swipe_anchor_ms = now;
        return;
    }
    _emit_swipe(_swipe_locked_h, _swipe_locked_sign, (int16_t)dx, (int16_t)dy);
    _swipe_anchor_x  = x;
    _swipe_anchor_y  = y;
    _swipe_anchor_ms = now;
}

uint32_t TrackpadWidget::_color_for_count(uint8_t n) const
{
    if (n <= 1) return _color_1;
    if (n == 2) return _color_2;
    return _color_3;
}

void TrackpadWidget::_spawn_ripple(int16_t cx, int16_t cy,
                                   const touchy_RippleAnimation &cfg,
                                   uint32_t color_rgb,
                                   lv_obj_t **back_slot,
                                   bool is_touch)
{
    if (!_container) return;

    // Pull values with proto-default fallbacks.
    const uint32_t start_opa   = cfg.has_start_opa    ? cfg.start_opa    : 200u;
    const uint32_t max_radius  = cfg.has_max_radius   ? cfg.max_radius   : 40u;
    const uint32_t duration_ms = cfg.has_duration_ms  ? cfg.duration_ms  : 350u;
    const uint32_t border_w    = cfg.has_border_width ? cfg.border_width : 0u;
    if (max_radius == 0 || duration_ms == 0) return;

    // Stage 24.4: touch ripples shrink down to a small resting radius
    // (instead of growing 0 → max_radius and then fading out). The
    // resting radius is ~25 % of `max_radius`, floored at 6 px so the
    // dot stays visible even with tiny configured radii.
    const int32_t rest_radius = is_touch
        ? std::max<int32_t>(6, static_cast<int32_t>(max_radius) / 4)
        : 0;

    auto *ctx = new (std::nothrow) RippleCtx{cx, cy, back_slot, border_w > 0};
    if (!ctx) return;
    lv_obj_t *o = lv_obj_create(_container);
    if (!o) { delete ctx; return; }
    if (back_slot) *back_slot = o;

    // Decorations off so we don't get themed default borders/shadows.
    lv_obj_remove_style_all(o);
    lv_obj_set_user_data(o, ctx);
    // Touch ripples start visible at `max_radius`; tap ripples start at 0
    // and grow as before.
    const int32_t initial_r = is_touch ? static_cast<int32_t>(max_radius) : 0;
    lv_obj_set_size(o, initial_r * 2, initial_r * 2);
    lv_obj_set_pos(o, cx - initial_r, cy - initial_r);
    lv_obj_add_flag(o, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(o, static_cast<lv_obj_flag_t>(
        LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    const lv_color_t col = lv_color_make(static_cast<uint8_t>(color_rgb >> 16),
                                         static_cast<uint8_t>(color_rgb >> 8),
                                         static_cast<uint8_t>(color_rgb));
    // Always set radius huge so the rect renders as a circle regardless
    // of its current animated width/height.
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_opa(o, static_cast<lv_opa_t>(start_opa), 0);
    if (border_w > 0) {
        // Hollow ring: transparent fill, coloured border.
        lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(o, static_cast<lv_coord_t>(border_w), 0);
        lv_obj_set_style_border_color(o, col, 0);
        lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
    } else {
        // Filled disc.
        lv_obj_set_style_bg_color(o, col, 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(o, 0, 0);
    }

    lv_anim_path_cb_t path = ripple_path_for(cfg.path);

    // Single reusable template, copied by lv_anim_start each time.
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_duration(&a, duration_ms);
    lv_anim_set_path_cb(&a, path);

    // Size animation. Touch ripples: max_radius → rest_radius (shrink).
    // Tap ripples: 0 → max_radius (legacy grow).
    lv_anim_set_exec_cb(&a, ripple_size_cb);
    if (is_touch) {
        lv_anim_set_values(&a, static_cast<int32_t>(max_radius), rest_radius);
    } else {
        lv_anim_set_values(&a, 0, static_cast<int32_t>(max_radius));
    }
    lv_anim_start(&a);

    if (!is_touch) {
        // Tap ripple: legacy behavior — opacity fades to 0 over the
        // same duration and the completion callback deletes the object.
        lv_anim_set_exec_cb(&a, ripple_opa_cb);
        lv_anim_set_values(&a, static_cast<int32_t>(start_opa), 0);
        lv_anim_set_completed_cb(&a, ripple_opa_completed);
        lv_anim_start(&a);
    }
    // Touch ripple: no opacity animation here — it stays at start_opa,
    // following the finger via ctx->cx/cy updates from `_process()`.
    // `_fade_out_ripple()` will be called on finger lift to dismiss it.
}

void TrackpadWidget::_fade_out_ripple(lv_obj_t *o)
{
    if (!o) return;
    auto *ctx = static_cast<RippleCtx *>(lv_obj_get_user_data(o));
    // Clear the back_slot pointer so the completion callback doesn't try
    // to null a slot the parent widget has already detached from.
    if (ctx) ctx->back_slot = nullptr;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_duration(&a, SCROLLBAR_FADE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, ripple_opa_cb);
    const lv_opa_t cur_opa = lv_obj_get_style_opa(o, LV_PART_MAIN);
    lv_anim_set_values(&a, cur_opa, 0);
    lv_anim_set_completed_cb(&a, ripple_opa_completed);
    lv_anim_start(&a);
}

// ---------------------------------------------------------------------------
// Scrollbar overlay (Stage 24.4)
// ---------------------------------------------------------------------------
namespace {

// Per-bar context: where the user's fingers currently are
// (container-local; updated each frame the fingers move), the scroll
// axis (horizontal=true → horizontal scroll → vertical handle), and
// the container's full extent. Lives in the bar's user_data and is
// deleted by the opacity completion callback.
struct ScrollbarCtx {
    bool     horizontal;       // scroll axis is horizontal
    int16_t  anchor_x;
    int16_t  anchor_y;
    int16_t  cw;
    int16_t  ch;
    int16_t  max_len;          // 75 % of cw or ch
    int16_t  cur_len;          // current animated length (px)
};

// Re-apply size + position from the current ctx state. Called both by
// the grow animation (when `cur_len` changes) and by
// `_update_scrollbar` (when the anchor changes).
void scrollbar_reposition(lv_obj_t *bar)
{
    auto *ctx = static_cast<ScrollbarCtx *>(lv_obj_get_user_data(bar));
    if (!ctx) return;
    const int32_t len = ctx->cur_len;

    // Mark the bar's current (pre-move) area dirty on the parent so
    // the background + any ripples underneath get repainted there.
    // See `invalidate_old_footprint` for why this is needed.
    invalidate_old_footprint(bar);

    if (ctx->horizontal) {
        // Horizontal scroll → vertical handle. Length grows along Y;
        // X follows the fingers, clamped so the bar stays on-screen.
        int32_t y = ctx->anchor_y - len / 2;
        if (y < 0) y = 0;
        if (y + len > ctx->ch) y = ctx->ch - len;
        int32_t x = ctx->anchor_x - TrackpadWidget::SCROLLBAR_THICK / 2;
        if (x < 0) x = 0;
        if (x + TrackpadWidget::SCROLLBAR_THICK > ctx->cw)
            x = ctx->cw - TrackpadWidget::SCROLLBAR_THICK;
        lv_obj_set_size(bar, TrackpadWidget::SCROLLBAR_THICK,
                        static_cast<lv_coord_t>(len));
        lv_obj_set_pos(bar, static_cast<lv_coord_t>(x),
                       static_cast<lv_coord_t>(y));
    } else {
        // Vertical scroll → horizontal handle. Length grows along X;
        // Y follows the fingers, clamped.
        int32_t x = ctx->anchor_x - len / 2;
        if (x < 0) x = 0;
        if (x + len > ctx->cw) x = ctx->cw - len;
        int32_t y = ctx->anchor_y - TrackpadWidget::SCROLLBAR_THICK / 2;
        if (y < 0) y = 0;
        if (y + TrackpadWidget::SCROLLBAR_THICK > ctx->ch)
            y = ctx->ch - TrackpadWidget::SCROLLBAR_THICK;
        lv_obj_set_size(bar, static_cast<lv_coord_t>(len),
                        TrackpadWidget::SCROLLBAR_THICK);
        lv_obj_set_pos(bar, static_cast<lv_coord_t>(x),
                       static_cast<lv_coord_t>(y));
    }
}

// Single exec callback driven by a 0..1000 progress animation. Stores
// the interpolated length in the ctx then re-applies the layout.
void scrollbar_grow_cb(void *var, int32_t progress)
{
    auto *bar = static_cast<lv_obj_t *>(var);
    auto *ctx = static_cast<ScrollbarCtx *>(lv_obj_get_user_data(bar));
    if (!ctx) return;

    const int32_t init = TrackpadWidget::SCROLLBAR_INIT_LEN;
    int32_t len = init + ((ctx->max_len - init) * progress) / 1000;
    if (len < init) len = init;
    if (len > ctx->max_len) len = ctx->max_len;
    ctx->cur_len = static_cast<int16_t>(len);
    scrollbar_reposition(bar);
}

// Opacity animation exec callback for the scrollbar fade-out.
void scrollbar_opa_cb(void *var, int32_t opa)
{
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(var),
                         static_cast<lv_opa_t>(opa), 0);
}

// Completion callback for the fade-out: delete the ctx then the object.
void scrollbar_fade_done(lv_anim_t *a)
{
    auto *bar = static_cast<lv_obj_t *>(a->var);
    delete static_cast<ScrollbarCtx *>(lv_obj_get_user_data(bar));
    lv_obj_set_user_data(bar, nullptr);
    lv_obj_delete(bar);
}

}  // namespace

void TrackpadWidget::_spawn_scrollbar(bool horizontal,
                                      int16_t anchor_x, int16_t anchor_y)
{
    if (!_container || _scrollbar) return;
    lv_obj_t *bar = lv_obj_create(_container);
    if (!bar) return;
    _scrollbar = bar;

    lv_obj_remove_style_all(bar);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(bar, static_cast<lv_obj_flag_t>(
        LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    const lv_color_t col = lv_color_make(
        static_cast<uint8_t>(_scrollbar_color >> 16),
        static_cast<uint8_t>(_scrollbar_color >> 8),
        static_cast<uint8_t>(_scrollbar_color));
    lv_obj_set_style_bg_color(bar, col, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, SCROLLBAR_THICK / 2, 0);
    lv_obj_set_style_opa(bar, LV_OPA_COVER, 0);

    const lv_coord_t cw = lv_obj_get_width(_container);
    const lv_coord_t ch = lv_obj_get_height(_container);

    auto *ctx = new (std::nothrow) ScrollbarCtx{
        horizontal,
        anchor_x,
        anchor_y,
        static_cast<int16_t>(cw),
        static_cast<int16_t>(ch),
        // Max length is 75 % of the container along the grow axis.
        static_cast<int16_t>((horizontal ? ch : cw) * 3 / 4),
        // Initial visible length; the grow anim will ramp this up.
        SCROLLBAR_INIT_LEN,
    };
    if (!ctx) return;
    lv_obj_set_user_data(bar, ctx);

    // Initial visible state: a small box centered on the anchor.
    scrollbar_reposition(bar);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, bar);
    lv_anim_set_duration(&a, SCROLLBAR_GROW_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, scrollbar_grow_cb);
    lv_anim_set_values(&a, 0, 1000);
    lv_anim_start(&a);
}

void TrackpadWidget::_update_scrollbar(int16_t anchor_x, int16_t anchor_y)
{
    if (!_scrollbar) return;
    auto *ctx = static_cast<ScrollbarCtx *>(lv_obj_get_user_data(_scrollbar));
    if (!ctx) return;
    ctx->anchor_x = anchor_x;
    ctx->anchor_y = anchor_y;
    scrollbar_reposition(_scrollbar);
}

void TrackpadWidget::_dismiss_scrollbar()
{
    lv_obj_t *bar = _scrollbar;
    if (!bar) return;
    _scrollbar = nullptr;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, bar);
    lv_anim_set_duration(&a, SCROLLBAR_FADE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&a, scrollbar_opa_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, 0);
    lv_anim_set_completed_cb(&a, scrollbar_fade_done);
    lv_anim_start(&a);
}

void TrackpadWidget::_retint_ripple(lv_obj_t *o, uint32_t color_rgb)
{
    if (!o) return;
    auto *ctx = static_cast<RippleCtx *>(lv_obj_get_user_data(o));
    if (!ctx) return;
    const lv_color_t col = lv_color_make(static_cast<uint8_t>(color_rgb >> 16),
                                         static_cast<uint8_t>(color_rgb >> 8),
                                         static_cast<uint8_t>(color_rgb));
    if (ctx->has_border) {
        lv_obj_set_style_border_color(o, col, 0);
    } else {
        lv_obj_set_style_bg_color(o, col, 0);
    }
}
