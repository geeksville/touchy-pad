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
#include "fs/fs.h"
#include "image_mmap.h"
#include "log_line.h"
#include "protobuf.h"
#include "screen_layout.h"     // apply_layout / apply_rect / apply_grid_cell / widget_is_layout
#include "screens.h"           // screens_get_touch()
#include "trackpad_widget.h"
#include "widget_actions.h"
#include "widget_animations.h"
#include "widget_styles.h"

#include "esp_log.h"
#include "lvgl.h"
// Not exposed via lvgl.h umbrella; reach into the LVGL tree to grab
// `lv_image_cache_drop()` so we can evict stale decoder entries when a
// path-backed image is overwritten in place (Stage 60).
#include "src/misc/cache/instance/lv_image_cache.h"

#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <memory>
#include <new>
#include <string>
#include <unordered_set>
#include <vector>

static const char *TAG = "screens.builders";

namespace {

// ---------------------------------------------------------------------------
// Stage 54 — WidgetRef indirection state.
// ---------------------------------------------------------------------------

using WidgetMsg = PbMessage<touchy_Widget>;

// Stage 57 — one entry per resolved `widget_ref`. For the *outermost*
// ref in a chain (the one parented directly into an LVGL container)
// we additionally remember the LVGL parent + produced root and the
// originating outer widget, so `widget_refs_change` can rebuild the
// subtree in place. For deeper refs inside a chain the LVGL fields
// stay null but `holder` / `path` still keep the decoded payload
// alive and let `widget_refs_active_path` enumerate every referenced
// file.
struct ActiveRef {
    std::string                id;       // outer Widget.id, "" if unset
    std::string                path;     // current path (mutates on rebind)
    lv_obj_t                  *parent = nullptr;
    lv_obj_t                  *root   = nullptr;
    const touchy_Widget       *outer  = nullptr;  // outer widget proto (placement)
    // Stage 57 — context flags used when rebinding the subtree in place:
    bool                       layer_root      = false;  // expansion at layer level
    bool                       grid_cell       = false;  // parent layout is grid
    bool                       absolute_layout = false;  // parent layout is absolute
    std::unique_ptr<WidgetMsg> holder;
};

// Decoded refs picked up by the *current* build pass; committed into
// `g_active_refs` once the new screen has fully replaced the old.
std::vector<ActiveRef> g_pending_refs;
std::vector<ActiveRef> g_active_refs;

// Paths currently being expanded — guards against ref cycles within a
// single resolve_widget_ref() chain.
std::unordered_set<std::string> g_ref_expansion;

// Resolve `w` to the widget that should actually be built: returns &w
// for non-ref widgets, or a pointer into a freshly-decoded WidgetMsg
// parked in `g_pending_refs` for refs. Returns nullptr on error
// (empty path, read failure, decode failure, or cycle detected) —
// callers should skip the widget in that case.
const touchy_Widget *resolve_widget_ref(const touchy_Widget &w)
{
    if (w.which_kind != touchy_Widget_widget_ref_tag) return &w;

    const char *path = w.kind.widget_ref.path;
    if (!path || path[0] == '\0') {
        ESP_LOGW(TAG, "widget_ref with empty path — skipping");
        return nullptr;
    }
    std::string key(path);
    if (g_ref_expansion.count(key)) {
        ESP_LOGE(TAG, "widget_ref cycle detected at '%s' — skipping", path);
        return nullptr;
    }

    Fs *fs = nullptr;
    std::string rest;
    fs = fs_resolve(key, &rest);
    if (!fs) {
        ESP_LOGE(TAG, "widget_ref: cannot resolve drive on '%s'", path);
        return nullptr;
    }
    size_t len = 0;
    uint8_t *raw = fs->readBinary(rest, &len);
    if (!raw) {
        ESP_LOGE(TAG, "widget_ref: cannot read '%s'", path);
        return nullptr;
    }

    auto holder = std::unique_ptr<WidgetMsg>(
        new (std::nothrow) WidgetMsg(touchy_Widget_fields));
    if (!holder) {
        delete[] raw;
        ESP_LOGE(TAG, "widget_ref: OOM decoding '%s'", path);
        return nullptr;
    }
    bool ok = holder->decode(raw, len);
    delete[] raw;
    if (!ok) {
        ESP_LOGE(TAG, "widget_ref: pb_decode failed for '%s'", path);
        return nullptr;
    }

    // Stage 56: the wire-format version lives on the root `Widget`
    // of each file. Reject (and delete) mismatched files so the host
    // re-uploads a fresh copy on its next pass.
    if ((**holder).version != touchy_Widget_Version_CURRENT) {
        ESP_LOGW(TAG, "widget '%s' has wrong version (%d) — deleting",
                 path, (int)(**holder).version);
        fs->remove(rest);
        return nullptr;
    }

    // Recurse to support a ref pointing at another ref (rare but
    // legal); guard via the expansion stack.
    g_ref_expansion.insert(key);
    const touchy_Widget *resolved = resolve_widget_ref(**holder);
    g_ref_expansion.erase(key);
    if (!resolved) return nullptr;

    // We must keep `holder` alive even if `resolved` points into a
    // *deeper* ref's holder (its own entry was already pushed by the
    // recursive call). Always push so the chain of refs lives as long
    // as the resulting LVGL subtree. LVGL fields (parent/root/outer)
    // stay null here; `widget_build_children` patches them on the
    // outermost ref after the build returns.
    ActiveRef entry;
    entry.id     = std::string(w.id);
    entry.path   = key;
    entry.holder = std::move(holder);
    g_pending_refs.push_back(std::move(entry));
    return resolved;
}

}  // namespace (anonymous, continued below)

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
//
// Post-Stage 51 the wire `asset` is a full drive-prefixed path like
// `F:host/images/foo.bin` or `R:host/images/foo.bin`. LVGL's path
// parser expects a slash directly after the drive colon, so we splice
// one in before handing the string off.
std::string to_lvgl_path(const char *wire)
{
    if (!wire || !wire[0]) return {};
    if (wire[0] && wire[1] == ':') {
        // "F:host/foo" -> "F:/host/foo". Existing `F:/host/foo` (with
        // slash already in place) becomes `F://host/foo`, which the
        // POSIX driver collapses to `/littlefs//host/foo` — still a
        // valid path on every libc we target.
        return std::string(wire, 2) + "/" + (wire + 2);
    }
    return std::string(wire);
}

// Apply the bytes at `wire_path` as `img`'s source. If `*dsc_inout` is
// non-null on entry, that dsc was the previously-applied alias into
// FS storage and is freed after LVGL has been pointed somewhere else
// (the lv_image keeps an internal pointer to its current src). The
// updated dsc pointer (or nullptr for the file-read fallback) is
// written back to `*dsc_inout`.
//
// Used by both build-time `apply_image_attrs()` (called with
// `*dsc_inout == nullptr`) and the runtime image-binding registry
// (called with the previous dsc so it can hand back a fresh one when
// a file is overwritten in RamFs).
void apply_image_src_from_path(lv_obj_t *img,
                               const char *wire_path,
                               lv_image_dsc_t **dsc_inout)
{
    lv_image_dsc_t *old_dsc = *dsc_inout;
    lv_image_dsc_t *new_dsc = nullptr;
    if (wire_path && wire_path[0]) {
        // Stage 52 fast path: if the asset lives on a mmappable FS
        // (R:) and its on-disk pixel format matches the display
        // native, alias the bytes directly via an lv_image_dsc_t —
        // no decode/copy.
        auto *dsc = new (std::nothrow) lv_image_dsc_t{};
        const char *why = nullptr;
        if (dsc && try_mmap_image(wire_path, dsc, &why)) {
            lv_image_set_src(img, dsc);
            new_dsc = dsc;
        } else {
            if (dsc) delete dsc;
            if (why && *why) {
                ESP_LOGW(TAG, "image %s: mmap declined (%s); using file read",
                         wire_path, why);
            }
            // File-read fallback: drop any LVGL cache entry for this
            // path so a subsequent set_src re-reads the on-disk bytes
            // rather than serving the previous (now stale) decode.
            std::string lv_path = to_lvgl_path(wire_path);
            lv_image_cache_drop(lv_path.c_str());
            lv_image_set_src(img, lv_path.c_str());
        }
    }
    // Safe to free the old dsc now that LVGL is no longer reading it.
    delete old_dsc;
    *dsc_inout = new_dsc;
    // Force a redraw — for the mmap path LVGL may not invalidate
    // automatically when set_src happens within the same draw cycle.
    lv_obj_invalidate(img);
}

// ---------------------------------------------------------------------------
// Stage 60 — image binding registry.
//
// Records every plain `Image` and every `ImageButton` slot (released /
// pressed) on the currently-built widget tree so that when a file is
// overwritten via FileOpenWrite/Write/Close we can re-apply just the
// affected image source(s) in-place — without rebuilding the screen
// (which would destroy the widget currently receiving a touch and
// suppress its RELEASE / PRESS_LOST event).
//
// Each binding owns the lifetime of its associated dsc (for the mmap
// fast path) and removes itself on the anchor widget's LV_EVENT_DELETE.
// Plain images use the lv_image itself as anchor; ImageButton slots
// use the outer lv_button (so a single delete cb tears down both the
// state and any slot bindings).
// ---------------------------------------------------------------------------

struct ImageButtonState;  // fwd, defined below

struct PlainImageBinding {
    lv_obj_t       *img_obj;        // the lv_image
    lv_obj_t       *anchor;         // same as img_obj
    lv_image_dsc_t *dsc;            // current mmap dsc, or nullptr (file-read)
    std::string     path;           // wire path being tracked
};

struct ButtonSlotBinding {
    ImageButtonState *state;        // owning ImageButton state struct
    lv_obj_t         *anchor;       // outer lv_button (lifetime anchor)
    bool              is_pressed_slot;  // true → tracks `state->pressed`
    std::string       path;
};

std::vector<PlainImageBinding> g_plain_bindings;
std::vector<ButtonSlotBinding> g_button_bindings;

void plain_image_binding_delete_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    auto *anchor = static_cast<lv_obj_t *>(lv_event_get_target(e));
    for (auto it = g_plain_bindings.begin(); it != g_plain_bindings.end();) {
        if (it->anchor == anchor) {
            delete it->dsc;
            it = g_plain_bindings.erase(it);
        } else {
            ++it;
        }
    }
}

void button_bindings_remove(lv_obj_t *anchor)
{
    g_button_bindings.erase(
        std::remove_if(g_button_bindings.begin(), g_button_bindings.end(),
                       [&](const ButtonSlotBinding &b) {
                           return b.anchor == anchor;
                       }),
        g_button_bindings.end());
}

void apply_image_attrs(lv_obj_t *img, const touchy_Image &im)
{
    lv_image_dsc_t *dsc = nullptr;
    apply_image_src_from_path(img, im.path, &dsc);
    if (im.has_scale)    lv_image_set_scale(img, (uint16_t)im.scale);
    if (im.has_rotation) lv_image_set_rotation(img, im.rotation);
    // Register this image for in-place updates if it has a real path.
    if (im.path[0] != '\0') {
        PlainImageBinding b;
        b.img_obj = img;
        b.anchor  = img;
        b.dsc     = dsc;
        b.path    = im.path;
        g_plain_bindings.push_back(std::move(b));
        lv_obj_add_event_cb(img, plain_image_binding_delete_cb,
                            LV_EVENT_DELETE, nullptr);
    } else {
        // No path → nothing to track; orphaned dsc (none in this case)
        // is already cleaned up by apply_image_src_from_path.
        (void)dsc;
    }
}

lv_obj_t *build_image(lv_obj_t *parent, const touchy_Widget &w)
{
    lv_obj_t *img = lv_image_create(parent);
    // The wire `asset` is already drive-prefixed (e.g.
    // `F:host/images/avatar.bin` for persistent flash, or
    // `R:host/images/avatar.bin` for the transient RAM filesystem).
    // LVGL's POSIX FS bridge maps `F:` onto `/littlefs`, and our
    // custom LVGL driver (registered in fs.cpp) serves `R:` reads
    // from the in-memory RamFs. The LVGL native `.bin` decoder is
    // always built in; BMP/PNG/JPG require their respective
    // `LV_USE_*` flag in sdkconfig.
    ESP_LOGI(TAG, "build_image id='%s' src='%s'", w.id, w.kind.image.path);
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
// inner lv_image owned by the button. Each state owns either a
// heap-allocated LVGL path string (`path`, used by the standard file
// decode path) or — if the Stage 52 mmap fast path succeeded — a
// heap-allocated `lv_image_dsc_t` that aliases the image bytes in
// RamFs (`dsc`). Exactly one of the two pointers is non-null when the
// state is configured. Both are freed in image_button_state_delete_cb.
struct ImageButtonSrc {
    char            *path;        // strdup'd LVGL path; nullptr if state isn't configured or mmap'd
    lv_image_dsc_t  *dsc;         // heap; nullptr unless mmap fast path took over
    char            *wire_path;   // strdup'd original wire path (e.g. "R:host/images/foo.bin");
                                  // nullptr if state isn't configured. Used by the Stage 60
                                  // image-binding registry to re-mmap after a file overwrite.
    bool      has_scale;
    uint16_t  scale;
    bool      has_rotation;
    int32_t   rotation;
};
struct ImageButtonState {
    lv_obj_t       *img_child;
    ImageButtonSrc  released;
    ImageButtonSrc  pressed;
    // Pointer to whichever slot is currently displayed on `img_child`.
    // Starts as &released; flipped by image_button_press_release_cb as
    // the LVGL press state machine reports edges. Stage 60 uses this
    // to decide whether a slot change from the file-watcher should
    // also update `img_child` immediately (only when the changed slot
    // matches the displayed one).
    ImageButtonSrc *current_slot;
};

void image_button_state_delete_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    auto *st = static_cast<ImageButtonState *>(lv_event_get_user_data(e));
    if (!st) return;
    // Stage 60: drop any registry entries anchored on this button
    // before we free the state struct that those entries point at.
    auto *anchor = static_cast<lv_obj_t *>(lv_event_get_target(e));
    button_bindings_remove(anchor);
    free(st->released.path);
    free(st->pressed.path);
    free(st->released.wire_path);
    free(st->pressed.wire_path);
    delete st->released.dsc;
    delete st->pressed.dsc;
    delete st;
}

// Populate `dst` from a `touchy.Image` field. Mirrors the choice
// `apply_image_attrs` makes for plain images: try the Stage 52 mmap
// fast path first, otherwise fall back to a strdup'd LVGL path.
void image_button_src_init(ImageButtonSrc &dst, const touchy_Image &im)
{
    // Free any state from a previous occupancy of this slot (used by
    // Stage 60's in-place re-mmap on file overwrite).
    free(dst.path);
    free(dst.wire_path);
    delete dst.dsc;
    dst = {};
    if (im.path[0] == '\0') return;
    dst.has_scale    = im.has_scale;
    dst.scale        = (uint16_t)im.scale;
    dst.has_rotation = im.has_rotation;
    dst.rotation     = im.rotation;
    dst.wire_path    = strdup(im.path);

    auto *dsc = new (std::nothrow) lv_image_dsc_t{};
    const char *why = nullptr;
    if (dsc && try_mmap_image(im.path, dsc, &why)) {
        dst.dsc = dsc;
        return;
    }
    delete dsc;
    if (why && *why) {
        ESP_LOGW(TAG, "image_button %s: mmap declined (%s); using file read",
                 im.path, why);
    }
    std::string lv_path = to_lvgl_path(im.path);
    dst.path = strdup(lv_path.c_str());
}

// Re-populate `dst` after the on-disk bytes at `dst.wire_path` were
// overwritten. Keeps wire_path/scale/rotation intact and just swaps
// the underlying dsc/path. Returns true if anything was rebuilt.
bool image_button_src_reload(ImageButtonSrc &dst)
{
    if (!dst.wire_path) return false;
    // Preserve attrs across the rebuild.
    char *saved_wire   = dst.wire_path;
    bool  has_scale    = dst.has_scale;
    uint16_t scale     = dst.scale;
    bool  has_rotation = dst.has_rotation;
    int32_t rotation   = dst.rotation;

    free(dst.path);
    delete dst.dsc;
    dst.path = nullptr;
    dst.dsc  = nullptr;

    auto *dsc = new (std::nothrow) lv_image_dsc_t{};
    const char *why = nullptr;
    if (dsc && try_mmap_image(saved_wire, dsc, &why)) {
        dst.dsc = dsc;
    } else {
        delete dsc;
        std::string lv_path = to_lvgl_path(saved_wire);
        // Drop LVGL's decode cache so the next set_src re-reads bytes.
        lv_image_cache_drop(lv_path.c_str());
        dst.path = strdup(lv_path.c_str());
    }
    dst.has_scale    = has_scale;
    dst.scale        = scale;
    dst.has_rotation = has_rotation;
    dst.rotation     = rotation;
    return true;
}

// True iff this state has any image-source override (mmap or path).
bool image_button_src_configured(const ImageButtonSrc &s)
{
    return s.path != nullptr || s.dsc != nullptr;
}

// Apply the chosen state's src + scale + rotation to the inner image.
// When the state has no scale/rotation override, fall back to the
// released-state value so the press transition only changes what was
// actually configured (and src always swaps).
void image_button_apply(lv_obj_t *img,
                        const ImageButtonSrc &state,
                        const ImageButtonSrc &fallback)
{
    if (state.dsc)       lv_image_set_src(img, state.dsc);
    else if (state.path) lv_image_set_src(img, state.path);
    if (state.has_scale)         lv_image_set_scale(img, state.scale);
    else if (fallback.has_scale) lv_image_set_scale(img, fallback.scale);
    if (state.has_rotation)         lv_image_set_rotation(img, state.rotation);
    else if (fallback.has_rotation) lv_image_set_rotation(img, fallback.rotation);
    lv_obj_invalidate(img);
}

void image_button_press_release_cb(lv_event_t *e)
{
    auto *st = static_cast<ImageButtonState *>(lv_event_get_user_data(e));
    if (!st || !st->img_child) return;
    // When the `pressed` slot has no image override the visible
    // image stays on the released bytes for the whole press, so we
    // must NOT move `current_slot` — keeping it at &released lets the
    // Stage 60 registry push fresh bytes through to img_child if the
    // host overwrites the released slot mid-press (the StreamDeck
    // "flip on press" probe relies on this).
    if (!image_button_src_configured(st->pressed)) return;

    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        image_button_apply(st->img_child, st->pressed, st->released);
        st->current_slot = &st->pressed;
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        image_button_apply(st->img_child, st->released, st->released);
        st->current_slot = &st->released;
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
    if (released.path[0] == '\0') return btn;

    auto *st = new (std::nothrow) ImageButtonState{};
    if (!st) return btn;
    st->img_child    = img;
    st->current_slot = &st->released;

    image_button_src_init(st->released, released);
    if (ib.has_pressed && ib.pressed.path[0] != '\0') {
        image_button_src_init(st->pressed, ib.pressed);
    }
    ESP_LOGI(TAG, "build_image_button id='%s' released='%s' has_pressed=%d",
             w.id, released.path, (int)ib.has_pressed);

    // Apply released-state attrs immediately.
    image_button_apply(img, st->released, st->released);

    // Always wire press/release tracking so current_slot stays accurate
    // even when only the released slot is configured — the Stage 60
    // file-watcher consults current_slot to decide whether the swap is
    // visible. (When `pressed` is not configured the cb just updates
    // current_slot without touching img_child.)
    lv_obj_add_event_cb(btn, image_button_press_release_cb,
                        LV_EVENT_PRESSED, st);
    lv_obj_add_event_cb(btn, image_button_press_release_cb,
                        LV_EVENT_RELEASED, st);
    lv_obj_add_event_cb(btn, image_button_press_release_cb,
                        LV_EVENT_PRESS_LOST, st);

    // Stage 60 — register each configured slot so file overwrites can
    // update it in place without rebuilding the screen.
    auto register_slot = [&](ImageButtonSrc *slot, bool is_pressed) {
        if (!slot->wire_path) return;
        ButtonSlotBinding b;
        b.state            = st;
        b.anchor           = btn;
        b.is_pressed_slot  = is_pressed;
        b.path             = slot->wire_path;
        g_button_bindings.push_back(std::move(b));
    };
    register_slot(&st->released, /*is_pressed=*/false);
    register_slot(&st->pressed,  /*is_pressed=*/true);

    // Free `st` (and its strings) when the button is deleted; the
    // child image is destroyed automatically by the parent. This cb
    // also removes any registry entries anchored on `btn`.
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
    // Strip all theme-provided styles so the spacer is fully invisible by
    // default. User-applied styles (bg_color, etc.) are added afterwards
    // via apply_styles and will be the sole styling, rather than fighting
    // the local bg_opa / border_width we used to set here (local styles
    // have higher LVGL priority than lv_obj_add_style, which caused
    // bg_opa=TRANSP to win over a user-supplied bg_color style).
    lv_obj_remove_style_all(o);
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

// ---------------------------------------------------------------------------
// Stage 60 — public image-binding registry entry point.
// ---------------------------------------------------------------------------

bool widget_image_registry_notify(const char *wire_path)
{
    if (!wire_path || !*wire_path) return false;

    bool any = false;

    // Plain Image widgets: re-apply the source in place. The dsc slot
    // is owned by the binding; apply_image_src_from_path swaps it for
    // a fresh one (or transitions to a file-read fallback) and frees
    // the stale buffer.
    for (auto &b : g_plain_bindings) {
        if (b.path != wire_path) continue;
        ESP_LOGI(TAG, "image_registry: reloading plain image '%s'", wire_path);
        apply_image_src_from_path(b.img_obj, wire_path, &b.dsc);
        any = true;
    }

    // ImageButton slots: re-mmap the slot's dsc / re-strdup its path,
    // then push the bytes to the inner lv_image only if that slot is
    // currently displayed (so a finger-down user doesn't see the
    // pressed art flip back to released mid-touch, and vice versa).
    for (auto &b : g_button_bindings) {
        if (b.path != wire_path) continue;
        ImageButtonSrc &slot = b.is_pressed_slot ? b.state->pressed
                                                 : b.state->released;
        ESP_LOGI(TAG,
                 "image_registry: reloading image_button %s slot for '%s'%s",
                 b.is_pressed_slot ? "pressed" : "released",
                 wire_path,
                 b.state->current_slot == &slot ? " (visible)" : " (hidden)");
        image_button_src_reload(slot);
        if (b.state->current_slot == &slot && b.state->img_child) {
            image_button_apply(b.state->img_child, slot, b.state->released);
        }
        any = true;
    }
    return any;
}

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
        const bool is_ref =
            L->children[i].which_kind == touchy_Widget_widget_ref_tag;
        const size_t pre = g_pending_refs.size();
        const touchy_Widget *w = resolve_widget_ref(L->children[i]);
        if (!w) continue;
        lv_obj_t *obj = widget_build(parent, *w);
        if (!obj) continue;
        apply_styles(obj, *w);
        apply_animations(obj, *w);
        // Stage 57 — placement (grid cell / absolute rect) belongs to
        // the *outer* widget_ref node, since the referenced .pb file
        // typically has no opinion about where on the enclosing layout
        // it lives. For plain (non-ref) children, outer == resolved.
        const touchy_Widget &placement_src = is_ref ? L->children[i] : *w;
        if (grid_layout) {
            apply_grid_cell(obj, placement_src);
        } else {
            apply_rect(obj, placement_src, absolute_layout);
        }
        if (w->centered) lv_obj_center(obj);
        // Stage 57 — patch the outermost ref entry so it can be
        // rebuilt in place by `widget_refs_change`. The recursive
        // resolve pushes deeper entries first, so `back()` is the
        // outermost. We also re-stamp the outer widget id (overriding
        // anything the deeper resolution may have stashed) and the
        // outer widget proto pointer used for placement.
        if (is_ref && g_pending_refs.size() > pre) {
            ActiveRef &back = g_pending_refs.back();
            back.parent          = parent;
            back.root            = obj;
            back.outer           = &L->children[i];
            back.layer_root      = false;
            back.grid_cell       = grid_layout;
            back.absolute_layout = absolute_layout;
            back.id              = std::string(L->children[i].id);
            back.path            = std::string(L->children[i].kind.widget_ref.path);
        }
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

void widget_build_layer(lv_obj_t *parent, const touchy_Widget &root_in)
{
    const bool is_ref = root_in.which_kind == touchy_Widget_widget_ref_tag;
    const size_t pre  = g_pending_refs.size();
    const touchy_Widget *root_p = resolve_widget_ref(root_in);
    if (!root_p) return;
    const touchy_Widget &root = *root_p;
    if (widget_is_layout(root)) {
        apply_layout(parent, root);
        widget_build_children(parent, root);
        if (is_ref && g_pending_refs.size() > pre) {
            ActiveRef &back = g_pending_refs.back();
            back.parent     = parent;
            back.root       = parent;  // layout layer roots directly into `parent`
            back.outer      = &root_in;
            back.layer_root = true;
            back.id         = std::string(root_in.id);
            back.path       = std::string(root_in.kind.widget_ref.path);
        }
        return;
    }
    if (root.which_kind == 0) {
        // Proto3 default — empty widget.
        return;
    }
    lv_obj_t *obj = widget_build(parent, root);
    if (!obj) return;
    apply_styles(obj, root);
    apply_animations(obj, root);
    apply_rect(obj, root, /*absolute_layout=*/true);
    if (root.centered) lv_obj_center(obj);
    if (is_ref && g_pending_refs.size() > pre) {
        ActiveRef &back = g_pending_refs.back();
        back.parent          = parent;
        back.root            = obj;
        back.outer           = &root_in;
        back.layer_root      = true;
        back.absolute_layout = true;
        back.id              = std::string(root_in.id);
        back.path            = std::string(root_in.kind.widget_ref.path);
    }
}

// ---------------------------------------------------------------------------
// Stage 54 — public WidgetRef holder lifecycle.
// ---------------------------------------------------------------------------

void widget_refs_reset_pending()
{
    g_pending_refs.clear();
    g_ref_expansion.clear();
}

void widget_refs_commit()
{
    g_active_refs = std::move(g_pending_refs);
    g_pending_refs.clear();
    g_ref_expansion.clear();
}

size_t widget_refs_active_count()
{
    return g_active_refs.size();
}

const char *widget_refs_active_path(size_t i)
{
    if (i >= g_active_refs.size()) return "";
    return g_active_refs[i].path.c_str();
}

const char *widget_refs_current_path(const char *target_id)
{
    if (!target_id) return nullptr;
    for (const auto &ref : g_active_refs) {
        if (ref.id == target_id) return ref.path.c_str();
    }
    return nullptr;
}

bool widget_refs_change(const char *target_id, const char *new_path)
{
    if (!target_id || !new_path) return false;
    // Locate the addressable (outermost) ref by id.
    ActiveRef *match = nullptr;
    size_t     match_idx = 0;
    for (size_t i = 0; i < g_active_refs.size(); i++) {
        ActiveRef &r = g_active_refs[i];
        if (r.id == target_id && r.parent && r.outer) {
            match = &r;
            match_idx = i;
            break;
        }
    }
    if (!match) {
        ESP_LOGW(TAG, "widget_refs_change: no active ref with id '%s'", target_id);
        return false;
    }
    // Snapshot context before we mutate state.
    lv_obj_t                  *parent           = match->parent;
    const touchy_Widget       *outer            = match->outer;
    const bool                 layer_root       = match->layer_root;
    const bool                 grid_cell        = match->grid_cell;
    const bool                 absolute_layout  = match->absolute_layout;

    // Tear down the old LVGL subtree.
    if (layer_root && match->root == parent) {
        // Layout-layer expansion built directly into `parent` —
        // remove every child but keep the layer object itself.
        lv_obj_clean(parent);
    } else {
        lv_obj_delete(match->root);
    }

    // Replace the outer widget's widget_ref.path so the rebuild reads
    // the new file. Per widgets.options the field is an inline
    // fixed-size char buffer (max_size=96), so we can snprintf
    // directly into it — no heap fuss.
    snprintf(const_cast<char *>(outer->kind.widget_ref.path),
             sizeof(outer->kind.widget_ref.path),
             "%s", new_path);

    // Drop the old ref entry (its holder chain is no longer reachable
    // from any live LVGL object) and stage the rebuild.
    g_active_refs.erase(g_active_refs.begin() + match_idx);
    widget_refs_reset_pending();

    if (layer_root) {
        widget_build_layer(parent, *outer);
    } else {
        // Mirror widget_build_children's single-slot expansion.
        const size_t pre = g_pending_refs.size();
        const touchy_Widget *w = resolve_widget_ref(*outer);
        if (!w) {
            ESP_LOGW(TAG, "widget_refs_change: resolve failed for '%s'", new_path);
            widget_refs_reset_pending();
            return false;
        }
        lv_obj_t *obj = widget_build(parent, *w);
        if (obj) {
            apply_styles(obj, *w);
            apply_animations(obj, *w);
            // Stage 57 — placement comes from the outer widget_ref
            // node (its `placement.cell` / `placement.rect`), not
            // from the referenced inner widget.
            if (grid_cell) {
                apply_grid_cell(obj, *outer);
            } else {
                apply_rect(obj, *outer, absolute_layout);
            }
            if (w->centered) lv_obj_center(obj);
            if (g_pending_refs.size() > pre) {
                ActiveRef &back = g_pending_refs.back();
                back.parent          = parent;
                back.root            = obj;
                back.outer           = outer;
                back.layer_root      = false;
                back.grid_cell       = grid_cell;
                back.absolute_layout = absolute_layout;
                back.id              = std::string(outer->id);
                back.path            = new_path;
            }
        }
    }

    // Splice newly-pending refs into the active list (preserving the
    // sibling refs we left untouched).
    for (auto &nr : g_pending_refs) {
        g_active_refs.push_back(std::move(nr));
    }
    g_pending_refs.clear();
    g_ref_expansion.clear();
    return true;
}
