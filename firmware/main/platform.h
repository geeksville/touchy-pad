// SPDX-License-Identifier: GPL-3.0-or-later
//
// Per-board capability descriptor (Stage 65).
//
// Some host-visible behaviour depends on what the *board* can physically do
// rather than on what the firmware was compiled with — e.g. whether the
// touch panel reports more than one simultaneous contact (capacitive GT911
// vs. resistive XPT2046), or whether the chip has a native USB-OTG port at
// all. `fill_board_info()` ships these flags to the host so the companion
// app / simulator can adapt (e.g. refuse multi-finger gestures on a
// single-touch panel) without hard-coding board names.
//
// Each board provides its own `platform_get()` in boards/<board>/board.cpp.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct Platform {
    // True when the touch panel can report >1 simultaneous touch point.
    bool is_multitouch;
    // True when the chip exposes a native USB-OTG port (HID + vendor-bulk
    // host transport). False on classic-ESP32 boards that reach the host
    // only through a UART bridge. Mirrors CONFIG_SOC_USB_OTG_SUPPORTED.
    bool has_usb;
};

// Returns this board's capability descriptor. Implemented per-board; the
// returned pointer is to static storage and never NULL.
const struct Platform *platform_get(void);

// True when the board has a touch panel at all. Prior boards all ship a
// touchscreen, so this has a *weak* default (in platform.cpp) that returns
// true — behaving like a virtual method whose base implementation is
// "touchable". Display-less LED-matrix boards (Stage LB1, e.g.
// jc_esp32p4_m3) provide a strong override returning false, which selects
// the touch-less built-in default screen and lets the host hide touch-only
// UI (reported over the host API as SysBoardInfoResponse.is_touchable).
bool platform_is_touchable(void);

// Stage 71 — stable device serial derived from the factory MAC
// (`esp_read_mac`), formatted "txxxxxxxxxxxx" (leading 't' + 12 lowercase
// hex digits, no separators). Computed once on first call and cached; the
// returned pointer is to static storage and never NULL. Reported over the
// host API (`SysBoardInfoResponse.serial`) and as the USB string-descriptor
// iSerialNumber so the OS and the vendor transport agree.
const char *platform_serial(void);

#ifdef __cplusplus
}
#endif
