// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stage 15 / 20.2 — LVGL style + transition application. See header for
// the rationale; this file used to live inline in screens.cpp.

#include "widget_styles.h"
#include "tc_tag.h"

#include "widgets.pb.h"

#include "esp_log.h"
#include "lvgl.h"

#include <new>
#include <vector>

static const char *TAG = TOUCHY_TAG("screens.styles");

namespace {

// Heap-allocated `lv_style_t` instances owned by a widget for its lifetime.
// LVGL keeps a pointer to each style added via `lv_obj_add_style` and reads
// from it on every redraw, so we cannot stack-allocate them inside the
// build loop. Freed via an LV_EVENT_DELETE callback below.
//
// `transitions` / `prop_arrays` are owned the same way: a
// `lv_style_transition_dsc_t` stored via `lv_style_set_transition` is
// referenced by pointer, and the descriptor itself holds a pointer to a
// 0-terminated `lv_style_prop_t[]`. Both must outlive every style they
// belong to.
struct WidgetStyles {
    std::vector<lv_style_t *> styles;
    std::vector<lv_style_transition_dsc_t *> transitions;
    std::vector<lv_style_prop_t *> prop_arrays;
};

void widget_styles_delete_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    auto *ws = static_cast<WidgetStyles *>(lv_event_get_user_data(e));
    if (!ws) return;
    for (lv_style_t *st : ws->styles) {
        lv_style_reset(st);
        delete st;
    }
    for (lv_style_transition_dsc_t *tr : ws->transitions) {
        delete tr;
    }
    for (lv_style_prop_t *props : ws->prop_arrays) {
        delete[] props;
    }
    delete ws;
}

}  // namespace (WidgetStyles internals)

// Translate a wire-stable `touchy_StyleProp` into the matching
// `lv_style_prop_t` value. Unknown values become `LV_STYLE_PROP_INV`
// so LVGL silently skips them. Exposed (non-anonymous) so
// `widget_animations.cpp` can reuse it.
lv_style_prop_t lv_prop_from_proto(touchy_StyleProp p)
{
    switch (p) {
        case touchy_StyleProp_BG_COLOR:          return LV_STYLE_BG_COLOR;
        case touchy_StyleProp_BG_OPA:            return LV_STYLE_BG_OPA;
        case touchy_StyleProp_RADIUS:            return LV_STYLE_RADIUS;
        case touchy_StyleProp_BORDER_WIDTH:      return LV_STYLE_BORDER_WIDTH;
        case touchy_StyleProp_BORDER_COLOR:      return LV_STYLE_BORDER_COLOR;
        case touchy_StyleProp_PAD_TOP:           return LV_STYLE_PAD_TOP;
        case touchy_StyleProp_PAD_BOTTOM:        return LV_STYLE_PAD_BOTTOM;
        case touchy_StyleProp_PAD_LEFT:          return LV_STYLE_PAD_LEFT;
        case touchy_StyleProp_PAD_RIGHT:         return LV_STYLE_PAD_RIGHT;
        case touchy_StyleProp_TEXT_COLOR:        return LV_STYLE_TEXT_COLOR;
        case touchy_StyleProp_IMAGE_RECOLOR:     return LV_STYLE_IMAGE_RECOLOR;
        case touchy_StyleProp_IMAGE_RECOLOR_OPA: return LV_STYLE_IMAGE_RECOLOR_OPA;
        case touchy_StyleProp_TRANSFORM_WIDTH:   return LV_STYLE_TRANSFORM_WIDTH;
        case touchy_StyleProp_TRANSFORM_HEIGHT:  return LV_STYLE_TRANSFORM_HEIGHT;
        // Stage 59 — geometry / opacity for declarative animations.
        case touchy_StyleProp_X:                 return LV_STYLE_X;
        case touchy_StyleProp_Y:                 return LV_STYLE_Y;
        case touchy_StyleProp_WIDTH:             return LV_STYLE_WIDTH;
        case touchy_StyleProp_HEIGHT:            return LV_STYLE_HEIGHT;
        case touchy_StyleProp_OPA:               return LV_STYLE_OPA;
        default:                                            return LV_STYLE_PROP_INV;
    }
}

// Translate a wire-stable `touchy_AnimPath` into the matching LVGL path
// callback. Unknown values fall back to linear. Exposed (non-anonymous)
// so `widget_animations.cpp` can reuse it.
lv_anim_path_cb_t lv_path_from_proto(touchy_AnimPath p)
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

namespace {

// Allocate (heap-owned) a `lv_style_transition_dsc_t` + a 0-terminated
// prop array describing `t`, stash both in `ws`, and return the
// descriptor pointer (suitable for passing to `lv_style_set_transition`).
// Returns nullptr on allocation failure or empty prop list.
lv_style_transition_dsc_t *build_lv_transition(const touchy_Transition &t,
                                               WidgetStyles *ws)
{
    if (t.props_count == 0 || t.props == nullptr || ws == nullptr) return nullptr;
    // +1 for the trailing LV_STYLE_PROP_INV terminator LVGL scans for.
    auto *props = new (std::nothrow) lv_style_prop_t[t.props_count + 1];
    if (!props) return nullptr;
    pb_size_t n = 0;
    for (pb_size_t i = 0; i < t.props_count; ++i) {
        lv_style_prop_t lp = lv_prop_from_proto(t.props[i]);
        if (lp != LV_STYLE_PROP_INV) props[n++] = lp;
    }
    props[n] = LV_STYLE_PROP_INV;   // 0-terminator
    if (n == 0) {
        delete[] props;
        return nullptr;
    }
    auto *tr = new (std::nothrow) lv_style_transition_dsc_t;
    if (!tr) {
        delete[] props;
        return nullptr;
    }
    lv_style_transition_dsc_init(tr, props, lv_path_from_proto(t.path),
                                 t.duration_ms, t.delay_ms, nullptr);
    ws->prop_arrays.push_back(props);
    ws->transitions.push_back(tr);
    return tr;
}

// Map a requested pixel font size to the nearest Montserrat font that is
// compiled into this firmware build. Returns nullptr for sizes with no
// matching font (the caller then leaves the theme default in place).
// Add new sizes here as they are enabled in sdkconfig.defaults.
const lv_font_t *font_for_size(int32_t size)
{
    switch (size) {
#if LV_FONT_MONTSERRAT_8
    case 8:  return &lv_font_montserrat_8;
#endif
#if LV_FONT_MONTSERRAT_10
    case 10: return &lv_font_montserrat_10;
#endif
#if LV_FONT_MONTSERRAT_12
    case 12: return &lv_font_montserrat_12;
#endif
#if LV_FONT_MONTSERRAT_14
    case 14: return &lv_font_montserrat_14;
#endif
#if LV_FONT_MONTSERRAT_16
    case 16: return &lv_font_montserrat_16;
#endif
#if LV_FONT_MONTSERRAT_18
    case 18: return &lv_font_montserrat_18;
#endif
#if LV_FONT_MONTSERRAT_20
    case 20: return &lv_font_montserrat_20;
#endif
#if LV_FONT_MONTSERRAT_22
    case 22: return &lv_font_montserrat_22;
#endif
#if LV_FONT_MONTSERRAT_24
    case 24: return &lv_font_montserrat_24;
#endif
#if LV_FONT_MONTSERRAT_26
    case 26: return &lv_font_montserrat_26;
#endif
#if LV_FONT_MONTSERRAT_28
    case 28: return &lv_font_montserrat_28;
#endif
#if LV_FONT_MONTSERRAT_30
    case 30: return &lv_font_montserrat_30;
#endif
#if LV_FONT_MONTSERRAT_32
    case 32: return &lv_font_montserrat_32;
#endif
#if LV_FONT_MONTSERRAT_34
    case 34: return &lv_font_montserrat_34;
#endif
#if LV_FONT_MONTSERRAT_36
    case 36: return &lv_font_montserrat_36;
#endif
#if LV_FONT_MONTSERRAT_38
    case 38: return &lv_font_montserrat_38;
#endif
#if LV_FONT_MONTSERRAT_40
    case 40: return &lv_font_montserrat_40;
#endif
#if LV_FONT_MONTSERRAT_42
    case 42: return &lv_font_montserrat_42;
#endif
#if LV_FONT_MONTSERRAT_44
    case 44: return &lv_font_montserrat_44;
#endif
#if LV_FONT_MONTSERRAT_46
    case 46: return &lv_font_montserrat_46;
#endif
#if LV_FONT_MONTSERRAT_48
    case 48: return &lv_font_montserrat_48;
#endif
    default: return nullptr;
    }
}

// Build one `lv_style_t` from a `touchy_Style` message. Each populated
// scalar contributes one `lv_style_set_<prop>` call; fields whose
// `has_<field>` is false inherit the theme. Returns a heap-allocated
// style — caller takes ownership and is responsible for `lv_style_reset`
// + `delete` (handled by `widget_styles_delete_cb`).
lv_style_t *build_lv_style(const touchy_Style &s, WidgetStyles *ws)
{
    auto *st = new (std::nothrow) lv_style_t;
    if (!st) return nullptr;
    lv_style_init(st);
    if (s.has_bg_color) {
        lv_style_set_bg_color(st, color_from_u32(s.bg_color));
        lv_style_set_bg_opa(st, LV_OPA_COVER);
    }
    if (s.has_radius)     lv_style_set_radius(st, s.radius);
    if (s.has_border_w)   lv_style_set_border_width(st, s.border_w);
    if (s.has_pad)        lv_style_set_pad_all(st, s.pad);
    if (s.has_text_color) lv_style_set_text_color(st, color_from_u32(s.text_color));
    if (s.has_recolor)    lv_style_set_image_recolor(st, color_from_u32(s.recolor));
    if (s.has_recolor_opa)
        lv_style_set_image_recolor_opa(st, (lv_opa_t)(s.recolor_opa & 0xFF));
    if (s.has_transform_width) lv_style_set_transform_width(st, s.transform_width);
    if (s.has_border_opa)
        lv_style_set_border_opa(st, (lv_opa_t)(s.border_opa & 0xFF));
    if (s.has_border_color) lv_style_set_border_color(st, color_from_u32(s.border_color));
    if (s.has_shadow_width)    lv_style_set_shadow_width(st, s.shadow_width);
    if (s.has_shadow_color)    lv_style_set_shadow_color(st, color_from_u32(s.shadow_color));
    if (s.has_shadow_offset_y) lv_style_set_shadow_offset_y(st, s.shadow_offset_y);
    if (s.has_outline_opa)
        lv_style_set_outline_opa(st, (lv_opa_t)(s.outline_opa & 0xFF));
    if (s.has_outline_color) lv_style_set_outline_color(st, color_from_u32(s.outline_color));
    if (s.has_outline_width) lv_style_set_outline_width(st, s.outline_width);
    if (s.has_transform_scale_x) lv_style_set_transform_scale_x(st, s.transform_scale_x);
    if (s.has_transform_scale_y) lv_style_set_transform_scale_y(st, s.transform_scale_y);
    if (s.has_font_size) {
        // Font size is a style property in LVGL (lv_style_set_text_font).
        // Map the requested pixel size to the nearest Montserrat font
        // compiled into the firmware; sizes without a matching font fall
        // back to the theme default (no set_text_font call).
        if (const lv_font_t *font = font_for_size(s.font_size))
            lv_style_set_text_font(st, font);
    }
    if (s.has_transition) {
        lv_style_transition_dsc_t *tr = build_lv_transition(s.transition, ws);
        if (tr) lv_style_set_transition(st, tr);
    }
    return st;
}

}  // namespace

void apply_styles(lv_obj_t *obj, const touchy_Widget &w)
{
    ESP_LOGD(TAG, "apply_styles id='%s' count=%u", w.id, (unsigned)w.styles_count);
    if (w.styles_count == 0 || w.styles == nullptr) return;
    auto *ws = new (std::nothrow) WidgetStyles{};
    if (!ws) return;
    for (pb_size_t i = 0; i < w.styles_count; ++i) {
        const touchy_Style &s = w.styles[i];
        lv_style_t *st = build_lv_style(s, ws);
        if (!st) continue;
        ws->styles.push_back(st);
        ESP_LOGD(TAG, "  [%u] for_state=0x%08lx bg=0x%06lx text=0x%06lx "
                      "radius=%ld border=%ld pad=%ld recolor=0x%06lx opa=%lu "
                      "tw=%ld transition=%d",
                 (unsigned)i, (unsigned long)s.for_state,
                 (unsigned long)s.bg_color, (unsigned long)s.text_color,
                 (long)s.radius, (long)s.border_w, (long)s.pad,
                 (unsigned long)s.recolor, (unsigned long)s.recolor_opa,
                 (long)s.transform_width, (int)s.has_transition);
        lv_obj_add_style(obj, st, (lv_style_selector_t)s.for_state);
    }
    if (ws->styles.empty()) {
        delete ws;
        return;
    }
    lv_obj_add_event_cb(obj, widget_styles_delete_cb, LV_EVENT_DELETE, ws);
}
