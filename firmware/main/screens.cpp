// SPDX-License-Identifier: GPL-3.0-or-later
//
// Touchy-Pad host-uploaded screen registry — see screens.h.
//
// Each .pb file under /from_host/screens/ is a serialised `touchy.Screen`
// (see proto/touchy.proto). On FileSave we cache the raw encoded bytes
// keyed by filename stem; on ScreenLoad we decode and walk the message,
// dispatching widgets to LVGL's C API. Decoded structures are large
// (~16 KB; nanopb generates fixed-size arrays sized for our worst case)
// so we never keep one resident longer than a single load call.
//
// Per-widget construction, action wiring, style application and layout
// math live in the sibling translation units widget_builders.cpp,
// widget_actions.cpp, widget_styles.cpp and screen_layout.cpp. This
// file owns:
//   * the cached raw-bytes registry (filename stem → encoded Screen),
//   * decoding + life-cycle of the currently-active decoded Screen,
//   * boot-time autoload + ScreenLoad dispatch + restore-last-screen,
//   * the small public C API in screens.h.

#include "screens.h"

#include "debug.h"
#include "default_screen_pb.h"
#include "fs.h"
#include "prefs.h"
#include "protobuf.h"
#include "screen_layout.h"
#include "touchy.pb.h"
#include "widget_builders.h"
#include "widget_styles.h"
#include "widgets.pb.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "pb_decode.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <vector>

static const char *TAG = "screens";

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

// Name of the registered screen the firmware autoloads on boot and when
// host code calls screens_load(NULL). Set by screens_init() to the first
// .pb file it discovers under /from_host/screens/, and updated by
// screens_register_from_file() when that's still the first arrival.
std::string g_default_screen_name;

// Name of the screen most recently passed to load_decoded(). Empty until
// the first successful load. Exposed via screens_current_name() and used
// by the prefs subsystem to persist last-viewed across reboots; also the
// anchor for ActionSwitchScreen NEXT/PREVIOUS traversals.
std::string g_current_name;

// Build (or rebuild) one LVGL layer from a decoded `touchy_Widget`
// (the value of `Screen.active` / `top` / `sys` / `bottom`).
//
// When the root widget is a layout-widget (`LayoutAbsolute/Flex/Grid`)
// we configure `parent`'s layout manager from it and instantiate its
// `Layout.children` directly into `parent` — there's no wrapping
// `lv_obj`. When the root is a leaf widget (rare, only useful for
// "single full-screen widget" layers) we build it as a normal child of
// `parent`.
//
// `parent` is the active screen object for `Screen.active`, or one of
// LVGL's persistent layer objects (`lv_layer_top()` / `lv_layer_sys()`
// / `lv_layer_bottom()`) for the other three. Caller must already hold
// the LVGL lock.
void build_layer(lv_obj_t *parent, const touchy_Widget &root)
{
    if (widget_is_layout(root)) {
        apply_layout(parent, root);
        widget_build_children(parent, root);
        return;
    }
    if (root.which_kind == 0) {
        // Proto3 default — empty widget. Nothing to build (caller has
        // already cleaned the parent if needed).
        return;
    }
    lv_obj_t *obj = widget_build(parent, root);
    if (!obj) return;
    apply_styles(obj, root);
    apply_rect(obj, root, /*absolute_layout=*/true);
    if (root.centered) lv_obj_center(obj);
}

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

    // LVGL 9 creates screens with an opaque white background by default.
    // Use black so transparent widgets show dark rather than white.
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // Active layer always present — populates the new lv_screen.
    if (S.has_active) {
        build_layer(scr, S.active);
    }

    // Persistent LVGL layers (top / sys / bottom) are only touched when
    // the incoming Screen carries them. LVGL does not clean these layers
    // on `lv_screen_load`, so an unset layer in the new screen means
    // "leave whatever's there alone". A *populated* layer means
    // "replace its contents wholesale" — we clean and rebuild.
    auto rebuild_persistent = [](lv_obj_t *layer, const touchy_Widget &W,
                                 const char *log_tag) {
        lv_obj_clean(layer);
        build_layer(layer, W);
        ESP_LOGI(TAG, "rebuilt %s layer", log_tag);
    };
    if (S.has_bottom) rebuild_persistent(lv_layer_bottom(), S.bottom, "bottom");
    if (S.has_top)    rebuild_persistent(lv_layer_top(),    S.top,    "top");
    if (S.has_sys)    rebuild_persistent(lv_layer_sys(),    S.sys,    "sys");

    lv_screen_load(scr);
    // Replace the previously-active decoded Screen *after* loading the
    // new LVGL screen, so its widgets' delete-callbacks (which still
    // dereference the old action arrays) fire before the old struct is
    // freed by the unique_ptr reset.
    g_active_screen = std::move(holder);
    g_current_name = log_name ? log_name : "";
    lvgl_port_unlock();

    // Persist last-loaded so a reboot restores it. The built-in fallback
    // is given the sentinel name "<built-in>" by its caller; we skip
    // persisting that so a real screen, once loaded, always wins.
    if (!g_current_name.empty() && g_current_name[0] != '<') {
        Prefs::instance().set_current_screen(g_current_name);
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

    // Decode just enough to check the version field before caching.
    {
        auto check = decode_screen(std::vector<uint8_t>(raw, raw + len));
        if (!check ||
            (*check)->version != touchy_Screen_Version_CURRENT) {
            ESP_LOGW(TAG, "screen '%s' has wrong version (%d) — deleting",
                     stem.c_str(),
                     check ? (int)(*check)->version : -1);
            delete[] raw;
            Fs::instance().remove(fs_path);
            return false;
        }
    }

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

bool screens_switch(int behavior, const char *name)
{
    // BY_NAME == 0 (per ActionSwitchScreen.Behavior); just forward.
    if (behavior == 0) {
        return screens_load(name);
    }

    auto &reg = registry();
    if (reg.empty()) {
        ESP_LOGW(TAG, "screens_switch: registry empty");
        return false;
    }

    // Find the current screen in the registry's iteration order (stable
    // because std::map sorts by key). When `g_current_name` is empty or
    // refers to something that's no longer registered (e.g. after a
    // clear+re-upload) we anchor at begin() so NEXT advances to the 2nd
    // entry and PREVIOUS wraps to the last — matching what a user would
    // expect after the registry was rebuilt.
    auto it = g_current_name.empty() ? reg.end()
                                     : reg.find(g_current_name);
    if (it == reg.end()) it = reg.begin();

    if (behavior == 1) {           // NEXT
        ++it;
        if (it == reg.end()) it = reg.begin();
    } else if (behavior == 2) {    // PREVIOUS
        if (it == reg.begin()) it = reg.end();
        --it;
    } else {
        ESP_LOGW(TAG, "screens_switch: unknown behavior %d", behavior);
        return false;
    }
    return screens_load(it->first.c_str());
}

const char *screens_current_name(void)
{
    return g_current_name.c_str();
}

void screens_clear(void)
{
    registry().clear();
    g_default_screen_name.clear();
    ESP_LOGI(TAG, "screen registry cleared");
}
