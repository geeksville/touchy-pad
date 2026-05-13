// SPDX-License-Identifier: Apache-2.0
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

#ifdef __cplusplus
}
#endif
