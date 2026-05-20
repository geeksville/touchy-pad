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

#include "default_screen_pb.h"
#include "fs.h"
#include "host_api.h"
#include "log_line.h"
#include "macros.h"
#include "protobuf.h"
#include "touchy.pb.h"
#include "trackpad_widget.h"
#include "widgets.pb.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "pb_decode.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <vector>

static const char *TAG = "screens";

// Touch controller handle, set once at boot by main.cpp via
// screens_set_touch(). Trackpad widgets need it to recover multi-finger
// snapshots that LVGL's single-point indev doesn't carry.
static esp_lcd_touch_handle_t s_touch_handle = nullptr;

extern "C" void screens_set_touch(esp_lcd_touch_handle_t handle)
{
    s_touch_handle = handle;
}

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
// Each widget event slot in the protobuf holds a `repeated Action` list
// (capped per touchy.options). When the trigger fires we walk the list:
//   * `ActionMacro` → enqueue on the macros runner task (HID replay).
//   * `ActionHost`  → build an `LvEvent` carrying the widget id, host_code,
//                     and the widget's current value packed little-endian
//                     into `extra`, then push it to the host_api queue.
//
// `user_data` on the LVGL object is a heap-copied ActionSlot struct that
// owns the action list pointer + widget id; freed on LV_EVENT_DELETE.
// ---------------------------------------------------------------------------

struct ActionSlot {
    char widget_id[32];
    const touchy_Action *actions;
    pb_size_t            actions_count;
    // Pointer-to-function so the same dispatch path handles every widget
    // kind; returns the bytes-written into `extra_out` (0 .. 32).
    size_t (*read_value)(lv_obj_t *obj, uint8_t *extra_out);
};

size_t read_value_none(lv_obj_t *, uint8_t *)        { return 0; }
size_t read_value_slider(lv_obj_t *obj, uint8_t *out)
{
    int32_t v = lv_slider_get_value(obj);
    memcpy(out, &v, sizeof(v));   // little-endian on xtensa / risc-v
    return sizeof(v);
}
size_t read_value_switch(lv_obj_t *obj, uint8_t *out)
{
    out[0] = lv_obj_has_state(obj, LV_STATE_CHECKED) ? 1 : 0;
    return 1;
}

void widget_event_cb(lv_event_t *e)
{
    auto *slot = static_cast<ActionSlot *>(lv_event_get_user_data(e));
    if (!slot || slot->actions_count == 0) return;
    lv_event_code_t code = lv_event_get_code(e);
    auto *obj = static_cast<lv_obj_t *>(lv_event_get_target(e));

    for (pb_size_t i = 0; i < slot->actions_count; i++) {
        const touchy_Action &act = slot->actions[i];
        switch (act.which_kind) {
        case touchy_Action_macro_tag:
            macros_run(&act.kind.macro);
            break;
        case touchy_Action_host_tag: {
            touchy_LvEvent evt = touchy_LvEvent_init_zero;
            evt.code = (uint32_t)code;
            snprintf(evt.user_data, sizeof(evt.user_data), "%s", slot->widget_id);
            evt.host_code = act.kind.host.code;
            evt.extra.size = (pb_size_t)slot->read_value(obj, evt.extra.bytes);
            host_api_post_event(&evt);
            break;
        }
        default:
            ESP_LOGD(TAG, "widget '%s' has unknown action kind %u",
                     slot->widget_id, (unsigned)act.which_kind);
            break;
        }
    }
}

void widget_delete_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    auto *slot = static_cast<ActionSlot *>(lv_event_get_user_data(e));
    delete slot;
}

void attach_actions(lv_obj_t *obj,
                    const char *widget_id,
                    const touchy_Action *actions,
                    pb_size_t actions_count,
                    lv_event_code_t code,
                    size_t (*read_value)(lv_obj_t *, uint8_t *))
{
    if (actions_count == 0) return;
    auto *slot = new (std::nothrow) ActionSlot{};
    if (!slot) return;
    // Always NUL-terminate; widget_id is at most 31 chars + NUL by virtue
    // of the proto cap, but we copy via snprintf to keep gcc's
    // stringop-truncation checker quiet.
    snprintf(slot->widget_id, sizeof(slot->widget_id), "%s", widget_id);
    slot->actions       = actions;
    slot->actions_count = actions_count;
    slot->read_value    = read_value;
    lv_obj_add_event_cb(obj, widget_event_cb, code, slot);
    lv_obj_add_event_cb(obj, widget_delete_cb, LV_EVENT_DELETE, slot);
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
    attach_actions(btn, w.id,
                   w.kind.button.on_click, w.kind.button.on_click_count,
                   LV_EVENT_CLICKED, read_value_none);
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
    attach_actions(s, w.id,
                   w.kind.slider.on_change, w.kind.slider.on_change_count,
                   LV_EVENT_VALUE_CHANGED, read_value_slider);
    return s;
}

lv_obj_t *build_switch(lv_obj_t *parent, const touchy_Widget &w)
{
    lv_obj_t *sw = lv_switch_create(parent);
    if (w.kind.toggle.on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    attach_actions(sw, w.id,
                   w.kind.toggle.on_change, w.kind.toggle.on_change_count,
                   LV_EVENT_VALUE_CHANGED, read_value_switch);
    return sw;
}

lv_obj_t *build_checkbox(lv_obj_t *parent, const touchy_Widget &w)
{
    lv_obj_t *cb = lv_checkbox_create(parent);
    if (w.kind.checkbox.text[0] != '\0') {
        lv_checkbox_set_text(cb, w.kind.checkbox.text);
    }
    if (w.kind.checkbox.checked) lv_obj_add_state(cb, LV_STATE_CHECKED);
    attach_actions(cb, w.id,
                   w.kind.checkbox.on_change, w.kind.checkbox.on_change_count,
                   LV_EVENT_VALUE_CHANGED, read_value_switch);
    return cb;
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

lv_obj_t *build_trackpad(lv_obj_t *parent, const touchy_Widget &)
{
    // Lifetime: the heap-allocated TrackpadWidget deletes itself on its
    // container's LV_EVENT_DELETE (registered in the constructor), so we
    // can fire-and-forget here.
    auto *tp = new (std::nothrow) TrackpadWidget(s_touch_handle, parent);
    return tp ? tp->obj() : nullptr;
}

lv_obj_t *build_log(lv_obj_t *parent, const touchy_Widget &)
{
    auto *lw = new (std::nothrow) LogLine(parent);
    return lw ? lw->obj() : nullptr;
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

// Place a widget inside a GRID-layout parent. The grid manager owns size
// and position, so we only forward the cell spec from the protobuf and ask
// LVGL to stretch the widget across its assigned track(s).
//
// Proto3 zero-default handling: in a proto3 binary the wire encoding for a
// scalar field whose value equals its default is omitted entirely. That
// means col=0, row=0, col_span=0, row_span=0 are all indistinguishable from
// "field not present". We therefore treat each value as its proto-comment
// specifies: col/row default to track 0 (top-left), col_span/row_span
// default to 1 (single track). Negative values are also clamped.
void apply_grid_cell(lv_obj_t *obj, const touchy_Widget &w)
{
    int32_t col = 0, row = 0, col_span = 1, row_span = 1;
    if (w.has_cell) {
        col      = w.cell.col      > 0 ? w.cell.col      : 0;
        row      = w.cell.row      > 0 ? w.cell.row      : 0;
        col_span = w.cell.col_span > 0 ? w.cell.col_span : 1;
        row_span = w.cell.row_span > 0 ? w.cell.row_span : 1;
    }
    lv_obj_set_grid_cell(obj,
                         LV_GRID_ALIGN_STRETCH, col, col_span,
                         LV_GRID_ALIGN_STRETCH, row, row_span);
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
        // Track templates. `cols` columns split the parent into equal
        // fractional units; `rows` does the same vertically when > 0,
        // otherwise we use a single content-sized row so the original
        // (pre-stage-18) GRID behaviour is preserved.
        //
        // Proto3 zero-default: `cols=0` is treated as "use 1 column".
        // `rows=0` deliberately means "content-sized single row" per the
        // proto comment; use rows ≥ 1 to get FR-sized rows.
        //
        // The descriptor arrays must outlive the call to
        // lv_obj_set_grid_dsc_array — LVGL only stores the pointer.
        // Since we have one active screen at a time we keep them as
        // static buffers; the next screen_load just rewrites them.
        static int32_t col_dsc[17];
        static int32_t row_dsc[17];
        static int32_t row_dsc_content[2] = { LV_GRID_CONTENT,
                                              LV_GRID_TEMPLATE_LAST };
        int cols = layout.cols > 0 ? layout.cols : 1;
        if (cols > 16) cols = 16;
        for (int i = 0; i < cols; i++) col_dsc[i] = LV_GRID_FR(1);
        col_dsc[cols] = LV_GRID_TEMPLATE_LAST;

        int32_t *row_ptr;
        int rows = layout.rows;
        if (rows > 0) {
            if (rows > 16) rows = 16;
            for (int i = 0; i < rows; i++) row_dsc[i] = LV_GRID_FR(1);
            row_dsc[rows] = LV_GRID_TEMPLATE_LAST;
            row_ptr = row_dsc;
        } else {
            row_ptr = row_dsc_content;
        }
        lv_obj_set_grid_dsc_array(scr, col_dsc, row_ptr);
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

// Heap-allocate the Screen wrapper so we don't blow the LVGL task's stack;
// also because we hand ownership across the lock boundary.
using ScreenMsg = PbMessage<touchy_Screen>;

std::unique_ptr<ScreenMsg> decode_screen(const std::vector<uint8_t> &bytes)
{
    auto msg = std::unique_ptr<ScreenMsg>(new (std::nothrow)
                                          ScreenMsg(touchy_Screen_fields));
    if (!msg) return nullptr;
    if (!msg->decode(bytes.data(), bytes.size())) {
        ESP_LOGE(TAG, "pb_decode failed");
        return nullptr;
    }
    return msg;
}

// Ownership of the currently-displayed decoded Screen. ActionSlots inside
// `widget_event_cb` hold pointers into this struct, so we keep it alive
// until the next ScreenLoad replaces it. The PbMessage destructor walks
// the message via pb_release(), freeing every heap-allocated widget /
// action / step array along the way.
std::unique_ptr<ScreenMsg> g_active_screen;

// Name of the registered screen the firmware autoloads on boot and when
// host code calls screens_load(NULL). Set by screens_init() to the first
// .pb file it discovers under /from_host/screens/, and updated by
// screens_register_from_file() when that's still the first arrival.
std::string g_default_screen_name;

// Render a freshly-decoded screen. Takes ownership of `holder` on
// success (moves it into `g_active_screen`); on failure the holder is
// destroyed by the caller's unique_ptr going out of scope.
bool load_decoded(std::unique_ptr<ScreenMsg> holder, const char *log_name)
{
    if (!holder) return false;
    const touchy_Screen &S = **holder;

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
    const bool grid_layout =
        S.has_layout && S.layout.kind == touchy_Layout_Kind_GRID;

    for (pb_size_t i = 0; i < S.widgets_count; i++) {
        const touchy_Widget &w = S.widgets[i];
        lv_obj_t *obj = nullptr;
        switch (w.which_kind) {
        case touchy_Widget_button_tag:   obj = build_button(scr, w);   break;
        case touchy_Widget_label_tag:    obj = build_label(scr, w);    break;
        case touchy_Widget_slider_tag:   obj = build_slider(scr, w);   break;
        case touchy_Widget_toggle_tag:   obj = build_switch(scr, w);   break;
        case touchy_Widget_image_tag:    obj = build_image(scr, w);    break;
        case touchy_Widget_arc_tag:      obj = build_arc(scr, w);      break;
        case touchy_Widget_spacer_tag:   obj = build_spacer(scr, w);   break;
        case touchy_Widget_checkbox_tag: obj = build_checkbox(scr, w); break;
        case touchy_Widget_trackpad_tag: obj = build_trackpad(scr, w); break;
        case touchy_Widget_log_tag:      obj = build_log(scr, w);      break;
        default:
            ESP_LOGW(TAG, "widget %s has unknown kind %d, skipping",
                     w.id, (int)w.which_kind);
            continue;
        }
        if (!obj) continue;
        apply_style(obj, w);
        if (grid_layout) {
            // Grid manager owns size/position; we only place the cell.
            apply_grid_cell(obj, w);
        } else {
            apply_rect(obj, w, absolute_layout);
        }
    }

    lv_screen_load(scr);
    // Replace the previously-active decoded Screen *after* loading the
    // new LVGL screen, so its widgets' delete-callbacks (which still
    // dereference the old action arrays) fire before the old struct is
    // freed by the unique_ptr reset.
    g_active_screen = std::move(holder);
    pb_size_t n_widgets = S.widgets_count;
    lvgl_port_unlock();

    ESP_LOGI(TAG, "loaded screen '%s' (%u widgets)",
             log_name, (unsigned)n_widgets);
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void screens_init(void)
{
    static bool inited = false;
    if (inited) return;
    inited = true;

    // Auto-discovery: scan /from_host/screens/ for any .pb files the host
    // has previously uploaded and register them. The first one we find
    // becomes the boot default (screens_load(NULL) target). Order is
    // whatever the filesystem returns from list() — for LittleFS this is
    // the on-disk order, so it's stable across reboots but not
    // alphabetical.
    Fs::instance().list("from_host/screens",
        [](const std::string &name, bool is_dir) {
            if (is_dir) return true;
            if (!ends_with(name.c_str(), ".pb")) return true;
            std::string virt = std::string("screens/") + name;
            screens_register_from_file(virt.c_str());
            return true;
        });

    if (g_default_screen_name.empty()) {
        ESP_LOGI(TAG, "screens registry initialised (no host screens; "
                      "built-in fallback will be used)");
    } else {
        ESP_LOGI(TAG, "screens registry initialised (default='%s')",
                 g_default_screen_name.c_str());
    }
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

    // First screen to land becomes the boot default. Subsequent uploads
    // don't reshuffle the default (host can always pick by name).
    if (g_default_screen_name.empty()) {
        g_default_screen_name = stem;
    }

    ESP_LOGI(TAG, "registered screen '%s' (%u bytes)",
             stem.c_str(), (unsigned)len);
    return true;
}

bool screens_load(const char *name)
{
    // NULL or empty name means "show the default screen". The default is
    // the first registered screen, or — if nothing is registered — a
    // built-in fallback compiled in from proto/default_screen.json.
    if (!name || !*name) {
        if (!g_default_screen_name.empty()) {
            return screens_load(g_default_screen_name.c_str());
        }
        ESP_LOGI(TAG, "loading built-in default screen");
        std::vector<uint8_t> bytes(
            default_screen_pb_data,
            default_screen_pb_data + default_screen_pb_len);
        auto holder = decode_screen(bytes);
        if (!holder) {
            ESP_LOGE(TAG, "failed to decode built-in default screen");
            return false;
        }
        return load_decoded(std::move(holder), "<built-in>");
    }

    auto it = registry().find(name);
    if (it == registry().end()) {
        ESP_LOGE(TAG, "screen '%s' not registered", name);
        return false;
    }

    auto holder = decode_screen(it->second);
    if (!holder) {
        ESP_LOGE(TAG, "out of memory decoding screen '%s'", name);
        return false;
    }
    return load_decoded(std::move(holder), name);
}

void screens_clear(void)
{
    registry().clear();
    g_default_screen_name.clear();
    ESP_LOGI(TAG, "screen registry cleared");
}
