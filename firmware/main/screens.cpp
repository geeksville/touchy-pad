// SPDX-License-Identifier: Apache-2.0
//
// Touchy-Pad host-uploaded screen registry — see screens.h.
//
// Each .pb file under /from_host/screens/ is a serialised `touchy.Screen`
// (see proto/touchy.proto). On FileSave we cache the raw encoded bytes
// keyed by filename stem; on ScreenLoad we decode and walk the message,
// dispatching widgets to LVGL's C API. Decoded structures are large
// (~16 KB; nanopb generates fixed-size arrays sized for our worst case)
// so we never keep one resident longer than a single load call.

#include "screens.h"

#include "fs.h"
#include "touchy.pb.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "pb_decode.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <string>
#include <vector>

static const char *TAG = "screens";

// ---------------------------------------------------------------------------
// Cache: filename stem (e.g. "home") -> encoded touchy.Screen bytes
// ---------------------------------------------------------------------------

namespace {

std::map<std::string, std::vector<uint8_t>> &registry()
{
    // Heap-allocated through static-init so we don't pay the cost on boards
    // that never use the screen system.
    static auto *m = new std::map<std::string, std::vector<uint8_t>>();
    return *m;
}

bool ends_with(const char *s, const char *suffix)
{
    size_t ls = strlen(s);
    size_t lx = strlen(suffix);
    if (lx > ls) return false;
    return strcasecmp(s + ls - lx, suffix) == 0;
}

// Extract "home" from "screens/home.pb".
std::string stem_from_path(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *base  = slash ? slash + 1 : path;
    const char *dot   = strrchr(base, '.');
    return dot ? std::string(base, dot - base) : std::string(base);
}

// LVGL colour from packed 0x00RRGGBB.
lv_color_t color_from_u32(uint32_t rgb)
{
    return lv_color_make((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

// ---------------------------------------------------------------------------
// Event hookup
//
// `user_data` stored on each LVGL object is a heap-copied string of the form
// "<widget_id>|<event_name>" — both come from the host-side DSL. The host
// transmit path for events is wired in a later stage; for now we just log.
// ---------------------------------------------------------------------------

void widget_event_cb(lv_event_t *e)
{
    auto *ud = static_cast<const char *>(lv_event_get_user_data(e));
    if (!ud) return;
    ESP_LOGI(TAG, "widget event code=%d user_data=%s",
             (int)lv_event_get_code(e), ud);
}

void widget_delete_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    auto *ud = static_cast<char *>(lv_event_get_user_data(e));
    free(ud);
}

void attach_action(lv_obj_t *obj,
                   const char *widget_id,
                   const touchy_Action &act,
                   lv_event_code_t code)
{
    if (act.event[0] == '\0') return;
    // "<id>|<event>" — keeps both halves on a single allocation so we can
    // free it from a single LV_EVENT_DELETE callback.
    size_t need = strlen(widget_id) + 1 + strlen(act.event) + 1;
    auto *ud = static_cast<char *>(malloc(need));
    if (!ud) return;
    snprintf(ud, need, "%s|%s", widget_id, act.event);
    lv_obj_add_event_cb(obj, widget_event_cb, code, ud);
    lv_obj_add_event_cb(obj, widget_delete_cb, LV_EVENT_DELETE, ud);
}

// ---------------------------------------------------------------------------
// Per-widget builders
// ---------------------------------------------------------------------------

lv_obj_t *build_button(lv_obj_t *parent, const touchy_Widget &w)
{
    lv_obj_t *btn = lv_button_create(parent);
    if (w.kind.button.text[0] != '\0') {
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, w.kind.button.text);
        lv_obj_center(lbl);
    }
    if (w.kind.button.has_on_click) {
        attach_action(btn, w.id, w.kind.button.on_click, LV_EVENT_CLICKED);
    }
    return btn;
}

lv_obj_t *build_label(lv_obj_t *parent, const touchy_Widget &w)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, w.kind.label.text);
    // font_size is advisory: we only honour it if a matching Montserrat
    // build-in is compiled. Anything else falls back to theme default.
    if (w.kind.label.font_size > 0) {
        ESP_LOGD(TAG, "label %s requested font_size=%d (using theme default)",
                 w.id, (int)w.kind.label.font_size);
    }
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
    if (w.kind.slider.has_on_change) {
        attach_action(s, w.id, w.kind.slider.on_change, LV_EVENT_VALUE_CHANGED);
    }
    return s;
}

lv_obj_t *build_switch(lv_obj_t *parent, const touchy_Widget &w)
{
    lv_obj_t *sw = lv_switch_create(parent);
    if (w.kind.toggle.on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    if (w.kind.toggle.has_on_change) {
        attach_action(sw, w.id, w.kind.toggle.on_change, LV_EVENT_VALUE_CHANGED);
    }
    return sw;
}

lv_obj_t *build_image(lv_obj_t *parent, const touchy_Widget &w)
{
    lv_obj_t *img = lv_image_create(parent);
    if (w.kind.image.asset[0] != '\0') {
        // The host-uploaded files live under /from_host/; LVGL's FS bridge
        // (registered separately for image assets) exposes them under the
        // "F:" drive letter. Image decoder availability depends on
        // sdkconfig CONFIG_LV_USE_PNG / CONFIG_LV_USE_BMP / etc.
        std::string lv_path = std::string("F:") + w.kind.image.asset;
        lv_image_set_src(img, lv_path.c_str());
    }
    return img;
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

// ---------------------------------------------------------------------------
// Style / placement application
// ---------------------------------------------------------------------------

void apply_style(lv_obj_t *obj, const touchy_Widget &w)
{
    if (!w.has_style) return;
    const auto &s = w.style;
    if (s.bg_color != 0) {
        lv_obj_set_style_bg_color(obj, color_from_u32(s.bg_color), 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    }
    if (s.radius > 0)     lv_obj_set_style_radius(obj, s.radius, 0);
    if (s.border_w > 0)   lv_obj_set_style_border_width(obj, s.border_w, 0);
    if (s.pad > 0)        lv_obj_set_style_pad_all(obj, s.pad, 0);
    if (s.text_color != 0)
        lv_obj_set_style_text_color(obj, color_from_u32(s.text_color), 0);
}

void apply_rect(lv_obj_t *obj, const touchy_Widget &w, bool absolute_layout)
{
    if (!w.has_rect) return;
    const auto &r = w.rect;
    if (absolute_layout) {
        lv_obj_set_pos(obj, r.x, r.y);
    }
    int32_t w_ = r.w > 0 ? r.w : LV_SIZE_CONTENT;
    int32_t h_ = r.h > 0 ? r.h : LV_SIZE_CONTENT;
    lv_obj_set_size(obj, w_, h_);
}

void apply_layout(lv_obj_t *scr, const touchy_Layout &layout)
{
    switch (layout.kind) {
    case touchy_Layout_Kind_ROW:
        lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_ROW);
        if (layout.gap > 0) lv_obj_set_style_pad_column(scr, layout.gap, 0);
        break;
    case touchy_Layout_Kind_COL:
        lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
        if (layout.gap > 0) lv_obj_set_style_pad_row(scr, layout.gap, 0);
        break;
    case touchy_Layout_Kind_GRID: {
        // Even-cols grid: every column gets the same fr unit, every row sizes
        // to content. We pre-size the descriptor for up to 16 columns; that's
        // plenty for any sensible touch UI.
        static int32_t col_dsc[17];
        static int32_t row_dsc[2] = { LV_GRID_CONTENT, LV_GRID_TEMPLATE_LAST };
        int cols = layout.cols > 0 ? layout.cols : 1;
        if (cols > 16) cols = 16;
        for (int i = 0; i < cols; i++) col_dsc[i] = LV_GRID_FR(1);
        col_dsc[cols] = LV_GRID_TEMPLATE_LAST;
        lv_obj_set_grid_dsc_array(scr, col_dsc, row_dsc);
        lv_obj_set_layout(scr, LV_LAYOUT_GRID);
        if (layout.gap > 0) {
            lv_obj_set_style_pad_column(scr, layout.gap, 0);
            lv_obj_set_style_pad_row(scr, layout.gap, 0);
        }
        break;
    }
    case touchy_Layout_Kind_ABSOLUTE:
    default:
        // No layout manager — widgets place themselves via lv_obj_set_pos.
        break;
    }
}

// ---------------------------------------------------------------------------
// Decoder helpers
// ---------------------------------------------------------------------------

bool decode_screen(const std::vector<uint8_t> &bytes, touchy_Screen *out)
{
    *out = touchy_Screen_init_default;
    pb_istream_t s = pb_istream_from_buffer(bytes.data(), bytes.size());
    if (!pb_decode(&s, touchy_Screen_fields, out)) {
        ESP_LOGE(TAG, "pb_decode failed: %s", PB_GET_ERROR(&s));
        return false;
    }
    return true;
}

// Heap-allocate the Screen struct so we don't blow the LVGL task's stack.
struct ScreenHolder {
    touchy_Screen *msg;
    ScreenHolder()  { msg = new (std::nothrow) touchy_Screen(); }
    ~ScreenHolder() { delete msg; }
};

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void screens_init(void)
{
    static bool inited = false;
    if (inited) return;
    inited = true;
    ESP_LOGI(TAG, "screens registry initialised");
}

bool screens_register_from_file(const char *path)
{
    if (!path || !*path) return false;

    // Only "screens/*.pb" files are layout descriptors; everything else
    // (images, fonts, ...) is on disk for LVGL's loaders to pick up via
    // "F:" paths and needs no per-file registration step.
    if (!ends_with(path, ".pb")) {
        ESP_LOGD(TAG, "ignoring non-screen upload: %s", path);
        return true;
    }
    if (strncmp(path, "screens/", 8) != 0) {
        ESP_LOGD(TAG, "ignoring .pb outside screens/: %s", path);
        return true;
    }

    size_t len = 0;
    // Files arrive via host_api under /littlefs/from_host/<path>; the path
    // we're handed is FS-virtual (no "from_host/" prefix), so prepend it
    // when reading back through Fs.
    std::string fs_path = std::string("from_host/") + path;
    uint8_t *raw = Fs::instance().readBinary(fs_path, &len);
    if (!raw) {
        ESP_LOGE(TAG, "register_from_file: cannot read %s", fs_path.c_str());
        return false;
    }

    std::string stem = stem_from_path(path);
    registry()[stem].assign(raw, raw + len);
    delete[] raw;

    ESP_LOGI(TAG, "registered screen '%s' (%u bytes)",
             stem.c_str(), (unsigned)len);
    return true;
}

bool screens_load(const char *name)
{
    if (!name || !*name) return false;

    auto it = registry().find(name);
    if (it == registry().end()) {
        ESP_LOGE(TAG, "screen '%s' not registered", name);
        return false;
    }

    ScreenHolder holder;
    if (!holder.msg) {
        ESP_LOGE(TAG, "out of memory decoding screen '%s'", name);
        return false;
    }
    if (!decode_screen(it->second, holder.msg)) {
        return false;
    }
    const touchy_Screen &S = *holder.msg;

    lvgl_port_lock(0);

    lv_obj_t *scr = lv_obj_create(NULL);
    if (!scr) {
        lvgl_port_unlock();
        ESP_LOGE(TAG, "lv_obj_create(NULL) returned NULL");
        return false;
    }

    if (S.has_layout) apply_layout(scr, S.layout);
    const bool absolute_layout =
        !S.has_layout || S.layout.kind == touchy_Layout_Kind_ABSOLUTE;

    for (pb_size_t i = 0; i < S.widgets_count; i++) {
        const touchy_Widget &w = S.widgets[i];
        lv_obj_t *obj = nullptr;
        switch (w.which_kind) {
        case touchy_Widget_button_tag: obj = build_button(scr, w); break;
        case touchy_Widget_label_tag:  obj = build_label(scr, w);  break;
        case touchy_Widget_slider_tag: obj = build_slider(scr, w); break;
        case touchy_Widget_toggle_tag: obj = build_switch(scr, w); break;
        case touchy_Widget_image_tag:  obj = build_image(scr, w);  break;
        case touchy_Widget_arc_tag:    obj = build_arc(scr, w);    break;
        case touchy_Widget_spacer_tag: obj = build_spacer(scr, w); break;
        default:
            ESP_LOGW(TAG, "widget %s has unknown kind %d, skipping",
                     w.id, (int)w.which_kind);
            continue;
        }
        if (!obj) continue;
        apply_style(obj, w);
        apply_rect(obj, w, absolute_layout);
    }

    lv_screen_load(scr);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "loaded screen '%s' (%u widgets)",
             name, (unsigned)S.widgets_count);
    return true;
}

void screens_clear(void)
{
    registry().clear();
    ESP_LOGI(TAG, "screen registry cleared");
}
