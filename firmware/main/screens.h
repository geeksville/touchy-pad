// SPDX-License-Identifier: Apache-2.0
//
// Touchy-Pad host-uploaded screen/component registry (stage 15).
//
// Bridges files saved under /from_host/ via the host_api FileSave command
// to LVGL's runtime XML loader (LV_USE_XML / lv_xml_component_register_*).
// Initialisation order is: Fs::begin() → screens_init() → host_api_start().

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// One-time setup: calls lv_xml_init(). Safe to call more than once.
void screens_init(void);

// Register a single file with LVGL, dispatching on filename suffix:
//   *.xml      → lv_xml_component_register_from_file("F:<path>")
//   other      → no-op (the file is still written to the FS and reachable
//                via "F:" paths for image/font loaders to find on demand).
// `path` is the on-device path *without* the "from_host/" prefix or the
// "F:" LVGL drive letter (e.g. "screens/home.xml"). Returns true if the
// registration was attempted and succeeded, false otherwise.
bool screens_register_from_file(const char *path);

// Switch the currently displayed screen to a previously-registered
// component name (the basename without .xml). Returns true on success.
bool screens_load(const char *name);

#ifdef __cplusplus
}
#endif
