// SPDX-License-Identifier: GPL-3.0-or-later
//
// TempFs — the logical 'T:' ("temp") transient drive (stage 87).
//
// `T:` is a *logical* drive for host-generated, throwaway assets
// (dynamic images, the host image cache). It does not own storage of
// its own; it resolves to whichever transient backing store the board
// has:
//
//   * Boards with PSRAM  →  RamFs (the same volatile, wear-free PSRAM
//                           ramdisk that backs 'R:'). `temp_is_flash()`
//                           returns false.
//   * Boards without PSRAM (e.g. classic-ESP32 CYD, ~520 KB SRAM)
//                        →  FlashFs (LittleFS). The internal-RAM RamFs
//                           fallback is far too small for image assets,
//                           so we spill to flash instead.
//                           `temp_is_flash()` returns true.
//
// Callers (host-side `ImageSource`, Rust `ImageCache`) always address
// the drive as `T:...` and never branch on the board: the device owns
// the ramdisk-vs-flash decision. `temp_is_flash()` is surfaced over the
// host API only as an advisory hint (so the host can throttle high-
// frequency refreshes / warn about flash wear).
//
// A read-only LVGL 'T:' filesystem driver is registered so widgets can
// `lv_image_set_src("T:host/dyn/1.bin")` exactly like 'R:'/'F:'.

#pragma once

#include "fs.h"

// The concrete filesystem backing the 'T:' drive on this board. Never
// null. Chosen once on first call based on whether PSRAM is present.
Fs *temp_fs_backing();

// True when the 'T:' drive is backed by flash (no-PSRAM boards) rather
// than a PSRAM ramdisk. Reported to the host as
// SysBoardInfoResponse.temp_is_flash.
bool temp_is_flash();

// Register the read-only LVGL 'T:' filesystem driver. Must be called
// after lv_init() (lv_init resets LVGL's FS driver list), alongside
// fs_register_lvgl_drivers().
void temp_fs_register_lvgl_driver();
