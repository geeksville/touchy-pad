// SPDX-License-Identifier: GPL-3.0-or-later
//
// Touchy-Pad host-uploaded screen registry — see screens.h.
//
// Each `.pb` file under `<drive>:host/s/` is a serialised
// `touchy.Screen` (see proto/touchy.proto). After a successful
// FileClose we cache the raw encoded bytes keyed by the full
// drive-prefixed path; on ScreenLoad we decode and walk the message,
// dispatching widgets to LVGL's C API. Decoded structures are large
// (~16 KB; nanopb generates fixed-size arrays sized for our worst
// case) so we never keep one resident longer than a single load call.
//
// Per-widget construction, action wiring, style application and layout
// math live in the sibling translation units widget_builders.cpp,
// widget_actions.cpp, widget_styles.cpp and screen_layout.cpp. This
// file owns:
//   * the cached raw-bytes registry (drive-prefixed path → encoded Screen),
//   * decoding + life-cycle of the currently-active decoded Screen,
//   * boot-time autoload + ScreenLoad dispatch + restore-last-screen,
//   * the small public C API in screens.h.

#include "screens.h"
#include "tc_tag.h"

#include "debug.h"
#include "default_screen_pb.h"
#include "fs.h"
#include "lv_throttled.h"
#include "prefs.h"
#include "protobuf.h"
#include "touchy.pb.h"
#include "widget_builders.h"
#include "widgets.pb.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "pb_decode.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <string>
#include <vector>

static const char *TAG = TOUCHY_TAG("screens");

// Touch controller handle, set once at boot by main.cpp via
// screens_set_touch(). Trackpad widgets need it to recover multi-finger
// snapshots that LVGL's single-point indev doesn't carry. Reached from
// widget_builders.cpp via screens_get_touch().
static esp_lcd_touch_handle_t s_touch_handle = nullptr;

extern "C" void screens_set_touch(esp_lcd_touch_handle_t handle)
{
    s_touch_handle = handle;
}

extern "C" esp_lcd_touch_handle_t screens_get_touch(void)
{
    return s_touch_handle;
}

// ---------------------------------------------------------------------------
// Cache: drive-prefixed path (e.g. "F:host/s/home.pb")
//        -> encoded touchy.Screen bytes.
// ---------------------------------------------------------------------------

namespace {

std::map<std::string, std::vector<uint8_t>> &registry()
{
    // Heap-allocated through static-init so we don't pay the cost on boards
    // that never use the screen system.
    static auto *m = new std::map<std::string, std::vector<uint8_t>>();
    return *m;
}

// Guards every access to registry() and g_default_screen_path. Stage 55's
// notify path now reloads the active screen on the LVGL task (deferred via
// lv_async_call), so screens_load() — which reads the registry — can run
// there concurrently with screens_register_from_file() writing it from the
// host_api dispatch task as the host streams in the next upload. Without
// this, a registry()[key].assign() reallocating a vector that screens_load
// is mid-decode on is a use-after-free. Plain (non-recursive) mutex: no
// path re-enters it while held.
std::mutex &registry_mutex()
{
    static auto *m = new std::mutex();
    return *m;
}

bool ends_with(const char *s, const char *suffix)
{
    size_t ls = strlen(s);
    size_t lx = strlen(suffix);
    if (lx > ls) return false;
    return strcasecmp(s + ls - lx, suffix) == 0;
}

// True iff `path` looks like a screen-bearing host upload, i.e.
// `<drive>:host/s/*.pb`. Used to filter both auto-discovery walks
// and post-FileClose registration attempts.
bool is_screen_path(const char *path)
{
    if (!path) return false;
    // Drive letter + colon prefix is mandatory.
    if (!path[0] || path[1] != ':') return false;
    const char *rest = path + 2;
    // Tolerate `F:/host/...` from older clients that include the slash.
    if (*rest == '/') rest++;
    if (strncmp(rest, HOST_SCREENS_PREFIX, strlen(HOST_SCREENS_PREFIX)) != 0)
        return false;
    return ends_with(path, ".pb");
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
// the widget action callback hold pointers into this struct, so we keep
// it alive until the next ScreenLoad replaces it. The PbMessage destructor
// walks the message via pb_release(), freeing every heap-allocated widget /
// action / step array along the way.
std::unique_ptr<ScreenMsg> g_active_screen;

// Drive-prefixed path of the registered screen the firmware autoloads
// on boot and when host code calls screens_load(NULL). Set by
// screens_init() to the first .pb file it discovers (in F: then R:
// order), and updated by screens_register_from_file() when that's
// still the first arrival.
std::string g_default_screen_path;

// Drive-prefixed path of the screen most recently passed to
// load_decoded(). Empty until the first successful load. Exposed via
// screens_current_path() and used by the prefs subsystem to persist
// last-viewed across reboots; also the anchor for ActionSwitchScreen
// NEXT/PREVIOUS traversals.
std::string g_current_path;

// Build (or rebuild) one LVGL layer from a decoded `touchy_Widget`
// (the value of `Screen.active` / `top` / `sys` / `bottom`).
//
// When the root widget is a layout-widget (`LayoutAbsolute/Flex/Grid`)
// we configure `parent`'s layout manager from it and instantiate its
// `Layout.children` directly into `parent` — there's no wrapping
// `lv_obj`. When the root is a leaf widget (rare, only useful for
// "single full-screen widget" layers) we build it as a normal child of
// `parent`.
// `parent` is the active screen object for `Screen.active`, or one of
// LVGL's persistent layer objects (`lv_layer_top()` / `lv_layer_sys()`
// / `lv_layer_bottom()`) for the other three. Caller must already hold
// the LVGL lock.
// build_layer() has moved to widgets/widget_builders.cpp as widget_build_layer().

// Render a freshly-decoded screen. Takes ownership of `holder` on
// success (moves it into `g_active_screen`); on failure the holder is
// destroyed by the caller's unique_ptr going out of scope.
bool load_decoded(std::unique_ptr<ScreenMsg> holder, const char *log_name)
{
    if (!holder) return false;
    const touchy_Screen &S = **holder;

    lvgl_port_lock(0);

    // Stage 54 — clear any WidgetRef holders left over from a previous
    // failed build before we start decoding refs for this screen.
    widget_refs_reset_pending();

    lv_obj_t *scr = lv_obj_create(NULL);
    if (!scr) {
        lvgl_port_unlock();
        ESP_LOGE(TAG, "lv_obj_create(NULL) returned NULL");
        return false;
    }

    // LVGL 9 creates screens with an opaque white background by default.
    // Use black so transparent widgets show dark rather than white.
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // Active layer always present — populates the new lv_screen.
    if (S.has_active) {
        widget_build_layer(scr, S.active);
    }

    // Persistent LVGL layers (top / sys / bottom) are only touched when
    // the incoming Screen carries them. LVGL does not clean these layers
    // on `lv_screen_load`, so an unset layer in the new screen means
    // "leave whatever's there alone". A *populated* layer means
    // "replace its contents wholesale" — we clean and rebuild.
    auto rebuild_persistent = [](lv_obj_t *layer, const touchy_Widget &W,
                                 const char *log_tag) {
        lv_obj_clean(layer);
        widget_build_layer(layer, W);
        ESP_LOGI(TAG, "rebuilt %s layer", log_tag);
    };
    if (S.has_bottom) rebuild_persistent(lv_layer_bottom(), S.bottom, "bottom");
    if (S.has_top)    rebuild_persistent(lv_layer_top(),    S.top,    "top");
    if (S.has_sys)    rebuild_persistent(lv_layer_sys(),    S.sys,    "sys");

    // Remember the previously-active screen so we can free it after the
    // swap. `lv_screen_load()` only changes which screen LVGL renders;
    // it does NOT delete the old one, so without this every ScreenLoad
    // leaks the entire previous LVGL subtree (every Style, every
    // animation, every widget's user-data — including the C++
    // `TrackpadWidget` whose `LV_EVENT_DELETE` callback would never
    // fire). Three or four switches were enough to exhaust the heap
    // and reboot the device.
    lv_obj_t *old_scr = lv_screen_active();
    lv_screen_load(scr);

    // Delete the old LVGL subtree BEFORE releasing the old decoded
    // proto: widget delete-callbacks (e.g. ActionMacro / ActionSwitch
    // bindings, the trackpad's deleter) still index into the previous
    // `g_active_screen`'s heap-allocated action arrays.
    if (old_scr && old_scr != scr) {
        lv_obj_delete(old_scr);
    }

    g_active_screen = std::move(holder);
    g_current_path = log_name ? log_name : "";
    // Stage 54 — old screen is gone; promote the pending WidgetRef
    // holders alongside the new active screen so their heap arrays
    // remain alive for action-slot pointers in the new LVGL tree.
    widget_refs_commit();

    // Stage 68 debug — force a layout pass and dump the geometry LVGL
    // actually computed for the screen and its immediate children, so we
    // can tell whether apply_rect's pct(100) sizing took effect (the
    // boot-time apply_rect logs get dropped under load).
    //
    // CRITICAL: keep this inside the SAME lock region as the screen swap
    // above. The previous code released the LVGL lock here (to persist
    // prefs to flash) and then re-acquired it for this dump while still
    // holding the raw `scr` pointer. During that unlocked window the
    // LVGL task could run a queued `lv_async_call` from
    // `screens_notify_file_changed` — i.e. another `screens_load` →
    // `load_decoded` that does `lv_obj_delete(old_scr)` on the very
    // screen we just made active. `scr` then dangled and
    // `lv_obj_update_layout(scr)` walked a freed object
    // (`lv_obj_get_screen` use-after-free), crashing the host_api task.
    lv_obj_update_layout(scr);
    ESP_LOGD(TAG, "geom scr -> w=%ld h=%ld", (long)lv_obj_get_width(scr),
             (long)lv_obj_get_height(scr));
    for (uint32_t i = 0; i < lv_obj_get_child_count(scr); i++) {
        lv_obj_t *c = lv_obj_get_child(scr, i);
        ESP_LOGD(TAG, "geom child[%lu] -> x=%ld y=%ld w=%ld h=%ld",
                 (unsigned long)i, (long)lv_obj_get_x(c), (long)lv_obj_get_y(c),
                 (long)lv_obj_get_width(c), (long)lv_obj_get_height(c));
    }
    lvgl_port_unlock();

    // Persist last-loaded so a reboot restores it. The built-in fallback
    // is given the sentinel path "<built-in>" by its caller; we skip
    // persisting that so a real screen, once loaded, always wins. This is
    // flash I/O, so do it AFTER releasing the LVGL lock above.
    if (!g_current_path.empty() && g_current_path[0] != '<') {
        Prefs::instance().set_current_screen(g_current_path);
    }

    ESP_LOGI(TAG, "loaded screen '%s'", log_name);

    dump_critical_info();
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

    // Auto-discovery: scan `<drive>:host/s/` on every registered
    // filesystem for any .pb files the host has previously uploaded and
    // register them. `host/s/default.pb` is preferred as the boot
    // default (screens_load(NULL) target); otherwise the first one we
    // find wins. Order is whatever the filesystem returns from list()
    // — for LittleFS this is the on-disk order, so it's stable across
    // reboots but not alphabetical. RamFs (R:) is scanned too even
    // though it's empty at boot, to keep the code symmetric and to pick
    // up any persistent boot-time seeds we might add later.
    auto scan_fs = [](Fs &fs) {
        char drive = fs.letter();
        fs.list(HOST_SCREENS_SUBDIR,
            [drive](const std::string &name, bool is_dir) {
                if (is_dir) return true;
                if (!ends_with(name.c_str(), ".pb")) return true;
                std::string full;
                full.reserve(name.size() + 16);
                full.push_back(drive);
                full.push_back(':');
                full.append(HOST_SCREENS_PREFIX);
                full.append(name);
                screens_register_from_file(full.c_str());
                return true;
            });
    };
    scan_fs(FlashFs::instance());
    scan_fs(RamFs::instance());

    if (g_default_screen_path.empty()) {
        ESP_LOGI(TAG, "screens registry initialised (no host screens; "
                      "built-in fallback will be used)");
    } else {
        ESP_LOGI(TAG, "screens registry initialised (default='%s')",
                 g_default_screen_path.c_str());
    }
}

bool screens_register_from_file(const char *path)
{
    if (!path || !*path) return false;

    // Only `<drive>:host/s/*.pb` files are layout descriptors;
    // everything else (images, fonts, ...) is on disk for LVGL's loaders
    // to pick up via drive-letter paths and needs no per-file
    // registration step.
    if (!is_screen_path(path)) {
        ESP_LOGD(TAG, "ignoring non-screen upload: %s", path);
        return true;
    }

    Fs *fs = nullptr;
    std::string rest;
    fs = fs_resolve(std::string(path), &rest);
    if (!fs) {
        ESP_LOGE(TAG, "register_from_file: cannot resolve '%s'", path);
        return false;
    }

    size_t len = 0;
    uint8_t *raw = fs->readBinary(rest, &len);
    if (!raw) {
        ESP_LOGE(TAG, "register_from_file: cannot read '%s'", path);
        return false;
    }

    // Decode just enough to check the version field before caching.
    // Stage 56: the wire-format version lives on the root `Widget`,
    // which for screen files means `screen.active.version`. A screen
    // without an `active` layer is malformed (every screen needs one)
    // and is treated as a version mismatch.
    {
        auto check = decode_screen(std::vector<uint8_t>(raw, raw + len));
        bool ok = check && (*check)->has_active &&
                  (*check)->active.version == touchy_Widget_Version_CURRENT;
        if (!ok) {
            int v = (check && (*check)->has_active)
                        ? (int)(*check)->active.version
                        : -1;
            ESP_LOGW(TAG, "screen '%s' has wrong version (%d) — deleting",
                     path, v);
            delete[] raw;
            fs->remove(rest);
            return false;
        }
    }

    std::string key(path);

    // Boot-default selection (Stage 68): the canonical prev/next chrome
    // `host/s/default.pb` always wins as the screens_load(NULL) target,
    // even if it's discovered after some other screen. Failing that, the
    // first screen to land becomes the default. Subsequent non-default
    // uploads don't reshuffle it (host can always pick by path).
    {
        std::lock_guard<std::mutex> guard(registry_mutex());
        registry()[key].assign(raw, raw + len);
        if (ends_with(key.c_str(), HOST_SCREENS_PREFIX DEFAULT_SCREEN_FILE)) {
            g_default_screen_path = key;
        } else if (g_default_screen_path.empty()) {
            g_default_screen_path = key;
        }
    }
    delete[] raw;

    ESP_LOGI(TAG, "registered screen '%s' (%u bytes)",
             path, (unsigned)len);
    return true;
}

bool screens_load(const char *path)
{
    // NULL or empty path means "show the default screen". The default is
    // the first registered screen, or — if nothing is registered — a
    // built-in fallback compiled in from proto/default_screen.json.
    if (!path || !*path) {
        std::string def;
        {
            std::lock_guard<std::mutex> guard(registry_mutex());
            def = g_default_screen_path;
        }
        if (!def.empty()) {
            return screens_load(def.c_str());
        }
        ESP_LOGI(TAG, "loading built-in default screen");
        std::vector<uint8_t> bytes(
#if CONFIG_TOUCHY_NO_TOUCH
            // Touch-less boards (Stage LB1 LED-matrix) get the colour-swatch
            // variant instead of the trackpad-based setup screen.
            default_screen_touchless_pb_data,
            default_screen_touchless_pb_data + default_screen_touchless_pb_len);
#else
            default_screen_pb_data,
            default_screen_pb_data + default_screen_pb_len);
#endif
        auto holder = decode_screen(bytes);
        if (!holder) {
            ESP_LOGE(TAG, "failed to decode built-in default screen");
            return false;
        }
        return load_decoded(std::move(holder), "<built-in>");
    }

    // Copy the encoded bytes out of the registry under the lock so the
    // decode below can't race a concurrent screens_register_from_file()
    // reallocating this entry's vector on the host_api task.
    std::vector<uint8_t> bytes;
    {
        std::lock_guard<std::mutex> guard(registry_mutex());
        auto it = registry().find(path);
        if (it == registry().end()) {
            ESP_LOGE(TAG, "screen '%s' not registered", path);
            return false;
        }
        bytes = it->second;
    }

    auto holder = decode_screen(bytes);
    if (!holder) {
        ESP_LOGE(TAG, "out of memory decoding screen '%s'", path);
        return false;
    }
    return load_decoded(std::move(holder), path);
}


const char *screens_current_path(void)
{
    return g_current_path.c_str();
}

void screens_clear(void)
{
    std::lock_guard<std::mutex> guard(registry_mutex());
    registry().clear();
    g_default_screen_path.clear();
    ESP_LOGI(TAG, "screen registry cleared");
}

void screens_notify_path_deleted(const char *path)
{
    if (!path || !*path) return;

    // Build an exact key and a directory prefix. A trailing '/' means
    // the host deleted a directory tree: match every registered screen
    // whose key starts with it. Otherwise match the file exactly and
    // also (defensively) anything under "<path>/".
    std::string exact(path);
    std::string prefix(path);
    if (!prefix.empty() && prefix.back() == '/') {
        exact.pop_back();          // "R:host/icache/" -> exact "R:host/icache"
    } else {
        prefix += '/';             // "F:host/s" -> prefix "F:host/s/"
    }

    std::lock_guard<std::mutex> guard(registry_mutex());
    auto &reg = registry();
    size_t removed = 0;
    for (auto it = reg.begin(); it != reg.end();) {
        const std::string &k = it->first;
        bool match = (k == exact) ||
                     (k.size() >= prefix.size() &&
                      k.compare(0, prefix.size(), prefix) == 0);
        if (match) {
            if (g_default_screen_path == k) g_default_screen_path.clear();
            it = reg.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    if (removed) {
        ESP_LOGI(TAG, "screens_notify_path_deleted: '%s' dropped %u registered screen(s)",
                 path, (unsigned)removed);
    } else {
        ESP_LOGD(TAG, "screens_notify_path_deleted: '%s' matched no registered screens",
                 path);
    }
}

// ---------------------------------------------------------------------------
// Stage 55 — file-change notification.
// ---------------------------------------------------------------------------
//
// The host sends a stream of FileWrite/FileClose commands as it pushes
// images, widgets, and screens. Most of those uploads are unrelated to
// whatever the device is currently displaying — e.g. uploading the next
// twenty TouchyDeck icons while a totally different screen is active.
// Re-reading and re-rendering the active screen on every commit
// produces a visible "flash" each time. Stage 55's contract:
//
//   * If the overwritten file IS the currently-loaded screen, or is
//     reachable through any `WidgetRef` resolved during that screen's
//     build, or is referenced by any `Image` / `ImageButton.released`
//     / `ImageButton.pressed` widget in the decoded tree, reload the
//     active screen so the new bytes take effect.
//   * Otherwise no-op.
//
// We accept the cost of a full reload for the affirmative case: it is
// simple, correct, and produces exactly one redraw. A per-image
// invalidation pass would be a future optimisation; the cache-drop
// + lv_obj_invalidate path needs an LVGL-object↔widget map we don't
// currently build, and the gain over a single rebuild is marginal.

namespace {

// Recursively scan `w` and any nested `Layout.children` looking for a
// widget that references `path`. Returns true on first hit.
bool widget_tree_references_path(const touchy_Widget &w,
                                 const std::string &path)
{
    switch (w.which_kind) {
    case touchy_Widget_image_tag:
        return path == w.kind.image.path;
    case touchy_Widget_image_button_tag: {
        const touchy_ImageButton &ib = w.kind.image_button;
        if (path == ib.released.path) return true;
        if (ib.has_pressed && path == ib.pressed.path) return true;
        return false;
    }
    case touchy_Widget_widget_ref_tag:
        // WidgetRef.path is also covered by widget_refs_active_path()
        // below, but check here too in case a brand-new ref appears in
        // a tree that hasn't been built yet (shouldn't happen for the
        // active screen, but cheap).
        return path == w.kind.widget_ref.path;
    case touchy_Widget_layout_flex_tag:
    case touchy_Widget_layout_grid_tag:
    case touchy_Widget_layout_absolute_tag: {
        const touchy_Layout *L = nullptr;
        if (w.which_kind == touchy_Widget_layout_flex_tag)
            L = &w.kind.layout_flex.layout;
        else if (w.which_kind == touchy_Widget_layout_grid_tag)
            L = &w.kind.layout_grid.layout;
        else
            L = &w.kind.layout_absolute.layout;
        for (pb_size_t i = 0; i < L->children_count; i++) {
            if (widget_tree_references_path(L->children[i], path)) return true;
        }
        return false;
    }
    default:
        return false;
    }
}

bool active_screen_references_path(const std::string &path)
{
    if (!g_active_screen) return false;
    const touchy_Screen &S = **g_active_screen;
    if (S.has_active && widget_tree_references_path(S.active, path)) return true;
    if (S.has_top    && widget_tree_references_path(S.top, path))    return true;
    if (S.has_sys    && widget_tree_references_path(S.sys, path))    return true;
    if (S.has_bottom && widget_tree_references_path(S.bottom, path)) return true;
    // Also check transitive widget_ref-loaded subtrees, since those
    // were already expanded inline above only when reachable from the
    // active screen — but the *source* paths live here. (A change to
    // the WidgetRef file itself must trigger a reload too.)
    for (size_t i = 0; i < widget_refs_active_count(); i++) {
        if (path == widget_refs_active_path(i)) return true;
    }
    return false;
}

}  // namespace

void screens_prepare_file_overwrite(const char *path)
{
    if (!path || !*path) return;
    ESP_LOGD(TAG, "prepare_file_overwrite: '%s' (lock+release gif)", path);
    // Timed lock take: this runs on the host_api dispatch task and
    // blocks until the LVGL task releases the port lock. If the LVGL
    // task is mid-reload, this can stall for the whole rebuild — which
    // surfaces host-side as a 2 s FileClose RPC timeout. Log when the
    // wait is long so we can pin the stall on lock contention.
    int64_t t0 = esp_timer_get_time();
    lvgl_port_lock(0);
    int64_t waited_us = esp_timer_get_time() - t0;
    if (waited_us >= 200000) {
        ESP_LOGW(TAG, "prepare_file_overwrite: '%s' waited %lldms for LVGL lock",
                 path, waited_us / 1000);
    }
    widget_image_registry_release_gif(path);
    lvgl_port_unlock();
}

// Stage 85 — coalesced full-reload request.
//
// A burst of file writes that each affect the active screen (e.g. the
// OpenDeck plugin rewriting N per-key WidgetRef stub files when a
// profile switches) would otherwise schedule N full screen rebuilds,
// one per FileClose. Each rebuild is expensive and they pile up on the
// LVGL task, starving the FileClose handler's screens_prepare_file_overwrite
// LVGL-lock take and the event_consume poll — which surfaces host-side
// as 2 s RPC timeouts and "stale transaction" errors mid-burst.
//
// Since a reload always rebuilds the *whole* current screen from the
// latest on-disk bytes, N reloads collapse to one: we only ever need
// the last one. request_active_reload() sets a pending flag and
// schedules a single deferred reload; further requests arriving before
// it runs are absorbed. Both the request and the reload run on the LVGL
// task (request from notify_file_changed_impl, reload from lv_async_call),
// so the plain bool needs no locking.
static bool s_reload_pending = false;

static void run_pending_reload()
{
    s_reload_pending = false;
    if (g_current_path.empty()) return;
    // Defensive copy because screens_load() may eventually mutate
    // g_current_path under the lock.
    std::string p = g_current_path;
    ESP_LOGI(TAG, "run_pending_reload: rebuilding active screen '%s'", p.c_str());
    screens_load(p.c_str());
}

static void request_active_reload(void)
{
    if (s_reload_pending) {
        ESP_LOGD(TAG, "request_active_reload: coalesced into pending reload");
        return;
    }
    s_reload_pending = true;
    // Defer through the shared throttle so a full reload can't pile onto
    // the LVGL lock back-to-back with the per-path in-place rebuilds it
    // shares the queue with (see lv_throttled.h). The s_reload_pending
    // flag still coalesces multiple requests into this single post.
    lv_throttled_post(run_pending_reload);
}

// The actual notify work. Runs on the LVGL task (scheduled via the
// shared lv_throttled_post queue from screens_notify_file_changed) so
// the in-place image reload and any full screen rebuild happen on the
// same thread that drives rendering — never on the host_api dispatch
// task, which must stay free to answer event_consume polls (and so
// drain the Stage 64.1 log tunnel). Already runs under the LVGL port
// lock held by lv_timer_handler; the inner lvgl_port_lock() calls below
// are
// harmless recursive takes.
static void notify_file_changed_impl(const char *path)
{
    if (!path || !*path) return;
    std::string key(path);

    // Stage 60 — fast path: if the overwritten file is only referenced
    // by Image / ImageButton widgets on the active screen, re-mmap and
    // re-apply just those lv_image sources in place. No screen
    // rebuild, so any widget currently receiving a touch keeps its
    // LVGL state machine and still emits a RELEASE event on
    // finger-up. Holds the LVGL lock since the registry walk mutates
    // widget state.
    {
        lvgl_port_lock(0);
        bool handled = widget_image_registry_notify(path);
        lvgl_port_unlock();
        if (handled) {
            ESP_LOGI(TAG, "notify_file_changed: '%s' updated in place via image registry",
                     path);
            return;
        }
    }

    // Stage 85 — second fast path: if the overwritten file is the
    // target of an active WidgetRef (an OpenDeck per-key stub, or the
    // chrome's `page` body), rebuild *just that ref's* subtree from the
    // fresh file instead of reloading the whole screen. This is both far
    // cheaper (one cell, not the entire chrome) and — crucially —
    // non-destructive: a full screens_load() re-decodes the screen file
    // from the registry and discards any prior change_widget_ref() page
    // switch, which would revert the display back to its default body.
    // Rebuilding the ref in place keeps the current page and repaints
    // only what changed. Holds the LVGL lock since it mutates the tree.
    {
        lvgl_port_lock(0);
        size_t rebuilt = widget_refs_rebuild_by_path(path);
        lvgl_port_unlock();
        if (rebuilt) {
            ESP_LOGI(TAG, "notify_file_changed: '%s' rebuilt %u widget_ref(s) in place",
                     path, (unsigned)rebuilt);
            return;
        }
    }

    // The active screen's own .pb file is the obvious case; check it
    // first so we don't have to traverse on a screen-file overwrite.
    // We also fall through to a full reload for changes that
    // reach the active screen via a WidgetRef — those carry whole
    // widget subtrees whose construction can't be patched in place.
    bool reload = (key == g_current_path) ||
                  active_screen_references_path(key);
    if (!reload) {
        ESP_LOGD(TAG, "notify_file_changed: '%s' is not referenced by active screen",
                 path);
        return;
    }
    ESP_LOGI(TAG, "notify_file_changed: '%s' affects active screen — reloading",
             path);
    // Re-load whatever's currently active. If the changed file is itself
    // the active screen, its cached bytes were just refreshed by
    // screens_register_from_file() ahead of this call, so the reload
    // picks up the new content. For asset/widget-ref changes,
    // re-running screens_load() rebuilds the LVGL tree against the
    // now-updated on-disk files (images mmap fresh; widget refs read
    // fresh).
    //
    // Stage 85: coalesce. A burst of stub writes each land here; rather
    // than rebuild once per file, schedule a single deferred reload
    // that picks up the final on-disk state. This keeps the LVGL task
    // from starving the FileClose handler / event_consume poll during a
    // profile switch's flurry of set_image calls.
    if (g_current_path.empty()) return;
    request_active_reload();
}

// Stage 85 — de-duplicated in-place rebuild queue.
//
// Every FileClose that overwrites an asset / WidgetRef stub schedules
// an in-place update on the LVGL task. A profile switch rewrites dozens
// of files back-to-back (e.g. 18 OpenDeck per-key stubs plus their
// image bins). The throttling that keeps such a burst from monopolising
// the LVGL port lock lives in the shared lv_throttled_post() utility
// (see lv_throttled.h) — it drains a few items per timer slice so the
// FileClose handler's screens_prepare_file_overwrite lock take always
// gets a window. Here we add the screens-specific concern on top: a
// per-path de-dup so repeated writes to the same file collapse into a
// single rebuild that reads the final on-disk bytes (which also removes
// the ordering race that previously lost a late stub write). The set is
// touched only on the LVGL task / under the port lock, so it needs no
// separate mutex.
static std::set<std::string> s_queued_paths;

static void enqueue_changed_path(std::string path)
{
    if (path.empty()) return;
    // De-dup: a path already queued will rebuild from the latest on-disk
    // bytes when its slice runs, so a second write before then is a
    // no-op.
    if (!s_queued_paths.insert(path).second) return;
    lv_throttled_post([path]() {
        // Erase BEFORE running so a write arriving during this rebuild
        // re-queues and rebuilds again (rather than being dropped).
        s_queued_paths.erase(path);
        int64_t t0 = esp_timer_get_time();
        notify_file_changed_impl(path.c_str());
        int64_t dt_us = esp_timer_get_time() - t0;
        if (dt_us >= 100000) {
            ESP_LOGW(TAG, "notify_file_changed: '%s' rebuild took %lldms",
                     path.c_str(), dt_us / 1000);
        }
    });
}

void screens_notify_file_changed(const char *path)
{
    if (!path || !*path) return;

    // Hop onto the LVGL task's serialization domain: the caller is the
    // host_api dispatch task (inside the FileClose handler). We take the
    // LVGL port lock only to enqueue (a cheap set-insert + maybe arming
    // the throttle timer); the actual widget-tree mutation runs later,
    // throttled, on the LVGL task via lv_throttled_post. This keeps the
    // FileClose handler from (a) blocking on a full rebuild — so it
    // stays free to service event_consume and drain the Stage 64.1 log
    // tunnel — and (b) mutating the LVGL tree from a non-LVGL task. The
    // brief lock take is safe from contention because the throttle
    // guarantees the LVGL task never holds the lock for long.
    lvgl_port_lock(0);
    enqueue_changed_path(path);
    lvgl_port_unlock();
}
