// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stage 20.1 / 24 — widget builders + dispatch.
//
// Each kind in `touchy.Widget` has a corresponding `build_*` function
// that constructs the LVGL object and wires up actions (when the widget
// has interactive event slots). The set of builders used to live inline
// in screens.cpp; it was split out during the Stage 24 refactor so the
// screens module can focus on registry / decode / dispatch concerns.

#include "widget_builders.h"

#include "fps_widget.h"
#include "force_render_widget.h"
#include "log_line.h"
#include "screen_layout.h"     // apply_layout / apply_rect / apply_grid_cell / widget_is_layout
#include "screens.h"           // screens_get_touch()
#include "trackpad_widget.h"
#include "widget_actions.h"
#include "widget_styles.h"

#include "esp_log.h"
#include "lvgl.h"

#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

static const char *TAG = "screens.builders";

namespace {

lv_obj_t *build_button(lv_obj_t *parent, const touchy_Widget &w)
{
    lv_obj_t *btn = lv_button_create(parent);
    if (w.kind.button.text[0] != '\0') {
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, w.kind.button.text);
        lv_obj_center(lbl);
    }
    widget_attach_actions(btn, w.id,
                          w.kind.button.on_click, w.kind.button.on_click_count,
                          LV_EVENT_CLICKED, widget_value_none);
    // Press / release edges — see widgets.proto for the contract. The
    // LVGL event code is forwarded verbatim in `LvEvent.code` so the
    // host can distinguish press (1) / release (8) / click (7). We
    // also bind on_release to LV_EVENT_PRESS_LOST so a cancelled press
    // (e.g. swiped off the button) still produces a matching release.
    widget_attach_actions(btn, w.id,
                          w.kind.button.on_press, w.kind.button.on_press_count,
                          LV_EVENT_PRESSED, widget_value_none);
    widget_attach_actions(btn, w.id,
                          w.kind.button.on_release, w.kind.button.on_release_count,
                          LV_EVENT_RELEASED, widget_value_none);
    widget_attach_actions(btn, w.id,
                          w.kind.button.on_release, w.kind.button.on_release_count,
                          LV_EVENT_PRESS_LOST, widget_value_none);
    return btn;
}

lv_obj_t *build_label(lv_obj_t *parent, const touchy_Widget &w)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, w.kind.label.text);
    // LVGL 9 base object default is opaque white background; make labels
    // transparent unless the caller explicitly sets a bg_color via Style.
    lv_obj_set_style_bg_opa(lbl, LV_OPA_TRANSP, 0);
    // Ensure text is fully opaque regardless of theme defaults.
    lv_obj_set_style_text_opa(lbl, LV_OPA_COVER, 0);
    // Text alignment within the label's bounding box.
    if (w.kind.label.text_align != touchy_TextAlign_TEXT_ALIGN_AUTO) {
        static const lv_text_align_t map[] = {
            LV_TEXT_ALIGN_AUTO,
            LV_TEXT_ALIGN_LEFT,
            LV_TEXT_ALIGN_CENTER,
            LV_TEXT_ALIGN_RIGHT,
        };
        int idx = (int)w.kind.label.text_align;
        if (idx >= 0 && idx < 4)
            lv_obj_set_style_text_align(lbl, map[idx], 0);
    }
    ESP_LOGI(TAG, "build_label id='%s' text='%.40s' font_size=%d text_align=%d",
             w.id, w.kind.label.text, (int)w.kind.label.font_size,
             (int)w.kind.label.text_align);
    // font_size is advisory: we only honour it if a matching Montserrat
    // build-in is compiled. Anything else falls back to theme default.
    return lbl;
}

lv_obj_t *build_slider(lv_obj_t *parent, const touchy_Widget &w)
{
    lv_obj_t *s = lv_slider_create(parent);
    int32_t mn = w.kind.slider.min;
    int32_t mx = w.kind.slider.max;
    if (mn == mx) { mn = 0; mx = 100; }
    lv_slider_set_range(s, mn, mx);
    lv_slider_set_value(s, w.kind.slider.value, LV_ANIM_OFF);
    widget_attach_actions(s, w.id,
                          w.kind.slider.on_change, w.kind.slider.on_change_count,
                          LV_EVENT_VALUE_CHANGED, widget_value_slider);
    return s;
}

lv_obj_t *build_switch(lv_obj_t *parent, const touchy_Widget &w)
{
    lv_obj_t *sw = lv_switch_create(parent);
    if (w.kind.toggle.on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    widget_attach_actions(sw, w.id,
                          w.kind.toggle.on_change, w.kind.toggle.on_change_count,
                          LV_EVENT_VALUE_CHANGED, widget_value_switch);
    return sw;
}

lv_obj_t *build_checkbox(lv_obj_t *parent, const touchy_Widget &w)
{
    lv_obj_t *cb = lv_checkbox_create(parent);
    if (w.kind.checkbox.text[0] != '\0') {
        lv_checkbox_set_text(cb, w.kind.checkbox.text);
    }
    if (w.kind.checkbox.checked) lv_obj_add_state(cb, LV_STATE_CHECKED);
    widget_attach_actions(cb, w.id,
                          w.kind.checkbox.on_change, w.kind.checkbox.on_change_count,
                          LV_EVENT_VALUE_CHANGED, widget_value_switch);
    return cb;
}

// Apply the contents of a `touchy.Image` (asset path + optional scale
// and rotation) to an lv_image. lv_image_set_src() with an `F:`
// filename triggers a string-clone internally, so passing a stack
// buffer is safe here.
void apply_image_attrs(lv_obj_t *img, const touchy_Image &im)
{
    if (im.asset[0] != '\0') {
        std::string lv_path = std::string("F:/from_host/") + im.asset;
        lv_image_set_src(img, lv_path.c_str());
    }
    if (im.has_scale) lv_image_set_scale(img, (uint16_t)im.scale);
    if (im.has_rotation) lv_image_set_rotation(img, im.rotation);
}

lv_obj_t *build_image(lv_obj_t *parent, const touchy_Widget &w)
{
    lv_obj_t *img = lv_image_create(parent);
    // The host-uploaded files live under /littlefs/from_host/; LVGL's
    // POSIX FS bridge (CONFIG_LV_USE_FS_POSIX) exposes /littlefs as the
    // "F:" drive, so the wire-format asset path is rebased here. The
    // LVGL native `.bin` decoder is always built in; BMP/PNG/JPG
    // require their respective `LV_USE_*` flag in sdkconfig.
    ESP_LOGI(TAG, "build_image id='%s' src='%s'", w.id, w.kind.image.asset);
    apply_image_attrs(img, w.kind.image);
    return img;
}

// Image-button architecture: an `lv_button` (clickable, full state
// machine, theme-aware bg/border) wraps a non-clickable `lv_image`
// child that just renders the bitmap. This is LVGL's canonical pattern
// for "image button with state feedback" and avoids the pitfalls of
// the alternatives:
//
//   * `lv_imagebutton` is a 9-patch widget. With only `src_mid` it
//     tiles to fill the widget when grid/flex stretches it; with only
//     `src_left` its `refr_image()` short-circuits so press-state
//     redraws never fire.
//   * A bare `lv_image` made clickable enters LV_STATE_PRESSED, but its
//     own draw path (`lv_image_event` → `draw_image`) and the base
//     obj's bg/border draw don't compose cleanly with style states,
//     transitions, or transform_width — visually nothing changes on
//     press even though events fire.
//
// The wrapping button receives all per-state styles from the protobuf
// (so apply_styles attaches them to the button); the inner image just
// renders. Press/release src-swap is attached to the button and forwards
// to the child image.

// User-data for the press/release src-swap handler. `img_child` is the
// inner lv_image owned by the button. Each state owns a heap-allocated
// LVGL path string (e.g. "F:/from_host/foo.bin") plus optional scale /
// rotation overrides. Strings and the struct itself are freed in
// image_button_state_delete_cb when the button is destroyed.
struct ImageButtonSrc {
    char *path;          // strdup'd; nullptr if state isn't configured
    bool has_scale;
    uint16_t scale;
    bool has_rotation;
    int32_t rotation;
};
struct ImageButtonState {
    lv_obj_t *img_child;
    ImageButtonSrc released;
    ImageButtonSrc pressed;
};

void image_button_state_delete_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    auto *st = static_cast<ImageButtonState *>(lv_event_get_user_data(e));
    if (!st) return;
    free(st->released.path);
    free(st->pressed.path);
    delete st;
}

// Apply the chosen state's src + scale + rotation to the inner image.
// When the state has no scale/rotation override, fall back to the
// released-state value so the press transition only changes what was
// actually configured (and src always swaps).
void image_button_apply(lv_obj_t *img,
                        const ImageButtonSrc &state,
                        const ImageButtonSrc &fallback)
{
    if (state.path) lv_image_set_src(img, state.path);
    if (state.has_scale)         lv_image_set_scale(img, state.scale);
    else if (fallback.has_scale) lv_image_set_scale(img, fallback.scale);
    if (state.has_rotation)         lv_image_set_rotation(img, state.rotation);
    else if (fallback.has_rotation) lv_image_set_rotation(img, fallback.rotation);
}

void image_button_press_release_cb(lv_event_t *e)
{
    auto *st = static_cast<ImageButtonState *>(lv_event_get_user_data(e));
    if (!st || !st->pressed.path || !st->img_child) return;
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        image_button_apply(st->img_child, st->pressed, st->released);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        image_button_apply(st->img_child, st->released, st->released);
    }
}

lv_obj_t *build_image_button(lv_obj_t *parent, const touchy_Widget &w)
{
    // Outer clickable button — receives styles and emits click events.
    lv_obj_t *btn = lv_button_create(parent);
    // Inner image — non-clickable so events bubble up to the button.
    lv_obj_t *img = lv_image_create(btn);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(img);

    const touchy_ImageButton &ib = w.kind.image_button;
    const touchy_Image &released = ib.released;
    if (released.asset[0] == '\0') return btn;

    auto *st = new (std::nothrow) ImageButtonState{};
    if (!st) return btn;
    st->img_child = img;

    std::string released_path = std::string("F:/from_host/") + released.asset;
    st->released = {strdup(released_path.c_str()),
                    released.has_scale,    (uint16_t)released.scale,
                    released.has_rotation, released.rotation};
    if (ib.has_pressed && ib.pressed.asset[0] != '\0') {
        std::string pressed_path = std::string("F:/from_host/") + ib.pressed.asset;
        st->pressed = {strdup(pressed_path.c_str()),
                       ib.pressed.has_scale,    (uint16_t)ib.pressed.scale,
                       ib.pressed.has_rotation, ib.pressed.rotation};
    }
    ESP_LOGI(TAG, "build_image_button id='%s' released='%s' has_pressed=%d",
             w.id, released_path.c_str(), (int)ib.has_pressed);

    // Apply released-state attrs immediately.
    image_button_apply(img, st->released, st->released);

    if (st->pressed.path) {
        // Listen on the button (which owns the press state machine);
        // the handler updates the inner image's src/scale/rotation.
        lv_obj_add_event_cb(btn, image_button_press_release_cb,
                            LV_EVENT_PRESSED, st);
        lv_obj_add_event_cb(btn, image_button_press_release_cb,
                            LV_EVENT_RELEASED, st);
        lv_obj_add_event_cb(btn, image_button_press_release_cb,
                            LV_EVENT_PRESS_LOST, st);
    }
    // Free `st` (and its strings) when the button is deleted; the
    // child image is destroyed automatically by the parent.
    lv_obj_add_event_cb(btn, image_button_state_delete_cb,
                        LV_EVENT_DELETE, st);
    widget_attach_actions(btn, w.id,
                          ib.on_click, ib.on_click_count,
                          LV_EVENT_CLICKED, widget_value_none);
    // See `build_button` for the press/release edge contract.
    widget_attach_actions(btn, w.id,
                          ib.on_press, ib.on_press_count,
                          LV_EVENT_PRESSED, widget_value_none);
    widget_attach_actions(btn, w.id,
                          ib.on_release, ib.on_release_count,
                          LV_EVENT_RELEASED, widget_value_none);
    widget_attach_actions(btn, w.id,
                          ib.on_release, ib.on_release_count,
                          LV_EVENT_PRESS_LOST, widget_value_none);
    return btn;
}

lv_obj_t *build_arc(lv_obj_t *parent, const touchy_Widget &w)
{
    lv_obj_t *a = lv_arc_create(parent);
    int32_t mn = w.kind.arc.min;
    int32_t mx = w.kind.arc.max;
    if (mn == mx) { mn = 0; mx = 100; }
    lv_arc_set_range(a, mn, mx);
    lv_arc_set_value(a, w.kind.arc.value);
    return a;
}

lv_obj_t *build_spacer(lv_obj_t *parent, const touchy_Widget &)
{
    lv_obj_t *o = lv_obj_create(parent);
    // Transparent, borderless padding object.
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    return o;
}

lv_obj_t *build_trackpad(lv_obj_t *parent, const touchy_Widget &w)
{
    // Lifetime: the heap-allocated TrackpadWidget deletes itself on its
    // container's LV_EVENT_DELETE (registered in the constructor), so we
    // can fire-and-forget here.
    const touchy_Trackpad &tp_pb = w.kind.trackpad;
    auto *tp = new (std::nothrow) TrackpadWidget(
        screens_get_touch(), parent, tp_pb);
    return tp ? tp->obj() : nullptr;
}

lv_obj_t *build_log(lv_obj_t *parent, const touchy_Widget &)
{
    auto *lw = new (std::nothrow) LogLine(parent);
    return lw ? lw->obj() : nullptr;
}

lv_obj_t *build_fps(lv_obj_t *parent, const touchy_Widget &)
{
    auto *fw = new (std::nothrow) FpsWidget(parent);
    return fw ? fw->obj() : nullptr;
}

lv_obj_t *build_force_render(lv_obj_t *parent, const touchy_Widget &)
{
    auto *frw = new (std::nothrow) ForceRenderWidget(parent);
    return frw ? frw->obj() : nullptr;
}

// Nested layout-widget. Creates a bare `lv_obj` container, configures
// its layout manager from the widget's own `LayoutAbsolute/Flex/Grid`
// kind, then recursively builds its `Layout.children` into it. Styles
// and placement of the container itself are applied by our caller
// (the same path every other widget kind takes).
lv_obj_t *build_layout(lv_obj_t *parent, const touchy_Widget &w)
{
    lv_obj_t *cont = lv_obj_create(parent);
    if (!cont) return nullptr;
    // LVGL 9 base obj defaults: opaque white bg + border. Keep
    // layout containers visually transparent so authoring code only
    // pays for what it asks for via Style.
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 0, 0);
    apply_layout(cont, w);
    widget_build_children(cont, w);
    return cont;
}

}  // namespace

void widget_build_children(lv_obj_t *parent, const touchy_Widget &container)
{
    const touchy_Layout *L = nullptr;
    switch (container.which_kind) {
    case touchy_Widget_layout_flex_tag:
        L = &container.kind.layout_flex.layout; break;
    case touchy_Widget_layout_grid_tag:
        L = &container.kind.layout_grid.layout; break;
    case touchy_Widget_layout_absolute_tag:
        L = &container.kind.layout_absolute.layout; break;
    default:
        return;
    }
    const bool grid_layout = container.which_kind == touchy_Widget_layout_grid_tag;
    const bool absolute_layout = container.which_kind == touchy_Widget_layout_absolute_tag;
    for (pb_size_t i = 0; i < L->children_count; i++) {
        const touchy_Widget &w = L->children[i];
        lv_obj_t *obj = widget_build(parent, w);
        if (!obj) continue;
        apply_styles(obj, w);
        if (grid_layout) {
            apply_grid_cell(obj, w);
        } else {
            apply_rect(obj, w, absolute_layout);
        }
        if (w.centered) lv_obj_center(obj);
    }
}

lv_obj_t *widget_build(lv_obj_t *parent, const touchy_Widget &w)
{
    switch (w.which_kind) {
    case touchy_Widget_button_tag:       return build_button(parent, w);
    case touchy_Widget_label_tag:        return build_label(parent, w);
    case touchy_Widget_slider_tag:       return build_slider(parent, w);
    case touchy_Widget_toggle_tag:       return build_switch(parent, w);
    case touchy_Widget_image_tag:        return build_image(parent, w);
    case touchy_Widget_image_button_tag: return build_image_button(parent, w);
    case touchy_Widget_arc_tag:          return build_arc(parent, w);
    case touchy_Widget_spacer_tag:       return build_spacer(parent, w);
    case touchy_Widget_checkbox_tag:     return build_checkbox(parent, w);
    case touchy_Widget_trackpad_tag:     return build_trackpad(parent, w);
    case touchy_Widget_log_tag:          return build_log(parent, w);
    case touchy_Widget_fps_tag:          return build_fps(parent, w);
    case touchy_Widget_force_render_tag: return build_force_render(parent, w);
    case touchy_Widget_layout_absolute_tag:
    case touchy_Widget_layout_flex_tag:
    case touchy_Widget_layout_grid_tag:
        return build_layout(parent, w);
    default:
        ESP_LOGW(TAG, "widget %s has unknown kind %d, skipping",
                 w.id, (int)w.which_kind);
        return nullptr;
    }
}
