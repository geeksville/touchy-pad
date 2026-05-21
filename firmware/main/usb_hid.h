// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

// USB HID button bitmasks (TinyUSB MOUSE_BUTTON_LEFT, etc).
#define HID_MOUSE_BTN_LEFT      0x01
#define HID_MOUSE_BTN_RIGHT     0x02
#define HID_MOUSE_BTN_MIDDLE    0x04

#ifdef __cplusplus
extern "C" {
#endif

// Brings up TinyUSB and starts the device task. Enumerates as a single HID
// mouse on the OTG USB-C port (VID/PID match v1 firmware).
void usb_hid_init(void);

// Send a relative mouse move (dx, dy in HID units) without changing buttons.
void usb_hid_mouse_move(int8_t dx, int8_t dy);

// Send a wheel scroll without changing buttons.
void usb_hid_mouse_scroll(int8_t vertical, int8_t horizontal);

// Press, then release, the given button (e.g. HID_MOUSE_BTN_LEFT). Sends two
// reports back to back.
void usb_hid_mouse_click(uint8_t button);

// Stage 16 — raw HID reports used by the macro runner. Caller owns all
// button / key state bookkeeping; these helpers just emit one report each.

// Send a mouse report with the given button mask plus optional delta and
// wheel. Subsequent calls inherit the button mask via internal state, so
// matching button-down / button-up pairs are required.
void usb_hid_mouse_buttons(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel);

// Send a boot-keyboard report: modifier byte + up to six simultaneously-
// pressed keycodes. Pass `keycodes = nullptr` (or an all-zero array) for
// the "all released" report. Modifier bits follow the standard HID layout
// (LCTRL = 0x01, LSHIFT = 0x02, LALT = 0x04, LGUI = 0x08, RCTRL = 0x10,
// RSHIFT = 0x20, RALT = 0x40, RGUI = 0x80).
void usb_hid_keyboard_report(uint8_t modifiers, const uint8_t keycodes[6]);

#ifdef __cplusplus
}
#endif
