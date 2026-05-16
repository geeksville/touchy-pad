// SPDX-License-Identifier: Apache-2.0
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

#ifdef __cplusplus
extern "C" {
#endif

// One-time setup. Safe to call more than once.
void screens_init(void);

// Inspect a freshly-written file at `path` (relative to /from_host/, i.e.
// no leading slash and no "F:" drive letter — e.g. "screens/home.pb").
// For "screens/*.pb" files, decode and cache the screen so a subsequent
// `screens_load(name)` can instantiate it. For other files, returns true
// without doing anything (the host_api_save path already wrote them to
// the FS so image/font loaders can resolve them via "F:" paths).
bool screens_register_from_file(const char *path);

// Switch the active LVGL screen to a previously-registered screen.
// Returns false if no screen by that name has been registered, or if
// rendering failed.
bool screens_load(const char *name);

// Discard every cached screen. Called by host_api when handling
// FileResetCmd so the device state matches the post-wipe filesystem.
void screens_clear(void);

#ifdef __cplusplus
}
#endif
