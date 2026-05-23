// SPDX-License-Identifier: GPL-3.0-or-later
//
// Touchy-Pad host-uploaded screen registry (stage 15, updated in stage 51).
//
// Renders LVGL screens from protobuf-encoded `touchy.Screen` blobs that
// the host streams to the device. Screens may live on either of the
// device's two filesystems:
//
//   F:host/screens/<name>.pb   — persistent flash
//   R:host/screens/<name>.pb   — PSRAM (transient; lost on reboot)
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

#ifdef __cplusplus
extern "C" {
#endif

// One-time setup. Safe to call more than once. Scans the `host/screens/`
// subtree on every registered filesystem (currently F: and R:) for
// host-uploaded `.pb` files and registers them; the first one found
// becomes the default screen (see `screens_load`).
void screens_init(void);

// Provide the touch controller handle so Trackpad widgets inside loaded
// screens can read multi-finger data. Must be called once at boot after
// `touch_init()` returns; safe to call before `screens_init()`. Passing
// nullptr disables the multi-finger fallback (taps degrade to single
// finger via LVGL's indev).
void screens_set_touch(esp_lcd_touch_handle_t handle);

// Inspect a freshly-written file at the given drive-prefixed `path`
// (e.g. `"F:host/screens/home.pb"`). For `*:host/screens/*.pb` files,
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

// Stage 24: jump to another registered screen by behaviour code.
//   * 0 = BY_PATH   — load `path`.
//   * 1 = NEXT      — advance one entry in registry iteration order.
//   * 2 = PREVIOUS  — step back one entry.
// NEXT/PREVIOUS wrap around at the registry ends; `path` is ignored.
// Returns false if no screen could be loaded (empty registry, unknown
// path, etc.).
bool screens_switch(int behavior, const char *path);

// Stage 24: drive-prefixed path of the currently-loaded screen, or ""
// before any successful `screens_load`. Used by the prefs subsystem to
// persist the last-viewed screen across reboots.
const char *screens_current_path(void);

#ifdef __cplusplus
}
#endif
