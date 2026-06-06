// SPDX-License-Identifier: GPL-3.0-or-later
//
// Touchy-Pad host-uploaded screen registry (stage 15, updated in stage 51).
//
// Renders LVGL screens from protobuf-encoded `touchy.Screen` blobs that
// the host streams to the device. Screens may live on either of the
// device's two filesystems:
//
//   F:host/s/<name>.pb   — persistent flash
//   R:host/s/<name>.pb   — PSRAM (transient; lost on reboot)
//
// All public functions identify screens by their full drive-prefixed
// path. The host-side authoring DSL lives in
// `app/src/touchy_pad/screens.py`; the wire schema is in
// `proto/touchy.proto` / `proto/widgets.proto`. See
// `docs/why-not-xml.md` for the rationale behind protobuf over XML.
//
// Lifecycle (called from main.cpp):
//   1. fs_init()
//   2. screens_init()
//   3. host_api_start()
// Subsequently, every FileClose / ScreenLoad command routed by
// host_api.cpp calls the helpers below.

#pragma once

#include "esp_lcd_touch.h"

// Stage 68: on-disk location of screen-layout blobs. Screens live under
// `<drive>:host/s/` (moved from the old `host/screens/`); the canonical
// prev/next chrome the host's `screen init` writes is `default.pb`,
// which the firmware prefers as its boot screen when present. These are
// the C++ mirror of `touchy_pad.paths.SCREENS_DIR` / `DEFAULT_SCREEN_FILE`.
#define HOST_SCREENS_SUBDIR "host/s"
#define HOST_SCREENS_PREFIX "host/s/"
#define DEFAULT_SCREEN_FILE "default.pb"

#ifdef __cplusplus
extern "C" {
#endif

// One-time setup. Safe to call more than once. Scans the `host/s/`
// subtree on every registered filesystem (currently F: and R:) for
// host-uploaded `.pb` files and registers them; `host/s/default.pb` is
// preferred as the boot screen, else the first one found (see
// `screens_load`).
void screens_init(void);

// Provide the touch controller handle so Trackpad widgets inside loaded
// screens can read multi-finger data. Must be called once at boot after
// `touch_init()` returns; safe to call before `screens_init()`. Passing
// nullptr disables the multi-finger fallback (taps degrade to single
// finger via LVGL's indev).
void screens_set_touch(esp_lcd_touch_handle_t handle);

// Inspect a freshly-written file at the given drive-prefixed `path`
// (e.g. `"F:host/s/home.pb"`). For `*:host/s/*.pb` files,
// decode and cache the screen so a subsequent `screens_load(path)` can
// instantiate it. For other files, returns true without doing anything
// (the file is already on disk for LVGL's loaders to resolve via its
// drive-letter path).
bool screens_register_from_file(const char *path);

// Switch the active LVGL screen to a previously-registered screen.
// Passing NULL or "" loads the default screen — the first `.pb` file
// found during the boot scan (or the first registered since), or a
// built-in "No screens configured" fallback when nothing has been
// uploaded. Returns false if rendering failed or the path isn't
// registered.
bool screens_load(const char *path);

// Discard every cached screen. Called by host_api when handling
// FileDeleteCmd so the device state matches the post-wipe filesystem.
void screens_clear(void);

// Touch controller handle stored by `screens_set_touch`. Exposed so the
// Trackpad widget builder can recover multi-finger data without each
// new file having to know about the screens module's internals.
// Returns NULL until `screens_set_touch()` has been called.
esp_lcd_touch_handle_t screens_get_touch(void);

// Stage 24: drive-prefixed path of the currently-loaded screen, or ""
// before any successful `screens_load`. Used by the prefs subsystem to
// persist the last-viewed screen across reboots.
const char *screens_current_path(void);

// Stage 55: notify the screen system that a file at `path` has just
// been written (or deleted). When the currently-loaded screen
// references `path` — directly (`g_current_path == path`), via an
// `Image` / `ImageButton` asset, or via a `WidgetRef` — the active
// screen is reloaded so the new bytes take effect. Files that no
// active widget references are silently ignored: this keeps host
// uploads of unrelated assets from causing visible redraws.
//
// The actual reload is deferred onto the LVGL task via lv_async_call
// (FIFO order preserved across back-to-back calls): the caller (the
// host_api dispatch task) returns immediately and the visual update
// lands a frame later. This keeps host_api free to service
// event_consume polls (so the Stage 64.1 log tunnel keeps draining
// during a rebuild) and confines LVGL widget-tree mutation to the LVGL
// task. Safe to call from any task.
void screens_notify_file_changed(const char *path);

// Stage 80: called just before a host upload commits (renames the temp
// file over `path`). If an animated GIF on the active screen is
// rendering `path`, its decoder holds the source file open, which would
// make the atomic rename-over-destination fail with EBUSY. This releases
// that decoder's file handle so the commit can proceed; the follow-up
// screens_notify_file_changed() re-applies the source. No-op for
// non-GIF paths. Safe to call from any task; takes the LVGL port lock
// internally.
void screens_prepare_file_overwrite(const char *path);

#ifdef __cplusplus
}
#endif
