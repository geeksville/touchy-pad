// SPDX-License-Identifier: GPL-3.0-or-later
//
// Touchy-Pad host-uploaded screen registry (stage 15).
//
// Renders LVGL screens from protobuf-encoded `touchy.Screen` blobs that
// the host uploads via `FileSave("screens/<name>.pb", ...)`. The host-side
// authoring DSL lives in `app/src/touchy_pad/screens.py`; the wire schema
// is in `proto/touchy.proto`. See `docs/why-not-xml.md` for the rationale
// behind protobuf over XML.
//
// Lifecycle (called from main.cpp):
//   1. Fs::begin()
//   2. screens_init()
//   3. host_api_start()
// Subsequently, every FileSave / ScreenLoad command routed by host_api.cpp
// calls the helpers below.

#pragma once

#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

// One-time setup. Safe to call more than once. Scans
// /from_host/screens/ for any host-uploaded .pb files and registers
// them; the first one found becomes the default screen (see
// `screens_load`).
void screens_init(void);

// Provide the touch controller handle so Trackpad widgets inside loaded
// screens can read multi-finger data. Must be called once at boot after
// `touch_init()` returns; safe to call before `screens_init()`. Passing
// nullptr disables the multi-finger fallback (taps degrade to single
// finger via LVGL's indev).
void screens_set_touch(esp_lcd_touch_handle_t handle);

// Inspect a freshly-written file at `path` (relative to /from_host/, i.e.
// no leading slash and no "F:" drive letter — e.g. "screens/home.pb").
// For "screens/*.pb" files, decode and cache the screen so a subsequent
// `screens_load(name)` can instantiate it. For other files, returns true
// without doing anything (the host_api_save path already wrote them to
// the FS so image/font loaders can resolve them via "F:" paths).
bool screens_register_from_file(const char *path);

// Switch the active LVGL screen to a previously-registered screen.
// Passing NULL or "" loads the default screen — the first .pb file
// found under /from_host/screens/ at boot (or the first registered
// since), or a built-in "No screens configured" fallback when nothing
// has been uploaded. Returns false if rendering failed or the named
// screen isn't registered.
bool screens_load(const char *name);

// Discard every cached screen. Called by host_api when handling
// FileResetCmd so the device state matches the post-wipe filesystem.
void screens_clear(void);

#ifdef __cplusplus
}
#endif
