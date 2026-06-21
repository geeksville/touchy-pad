// SPDX-License-Identifier: GPL-3.0-or-later
//
// No-USB HID stubs.
//
// Compiled in place of usb_hid.cpp for boards without native USB-OTG
// (chips where CONFIG_SOC_USB_OTG_SUPPORTED is unset, e.g. the classic
// ESP32 on the esp32_2432s028rv3 / CYD2USB board, whose only host link is a
// CH340 UART bridge). Such boards cannot emulate a USB HID mouse/keyboard at
// all, so the emitter functions become no-ops. The macro runner (macros.cpp)
// and the trackpad widget (widgets/trackpad_widget.cpp) call these
// unconditionally; keeping the symbols here lets the app link unchanged.
//
// Per the project convention (see AGENTS.md), we log once at WARN rather
// than crashing, so a host that pushes a HID macro to a no-USB board gets a
// clear diagnostic instead of a hard failure.

#include "usb_hid.h"
#include "tc_tag.h"

#include "esp_log.h"

static const char *TAG = TOUCHY_TAG("usb_hid");

// Emit a single warning the first time any HID emitter is invoked on a
// board that has no USB. Further calls are silent so a busy trackpad can't
// flood the log.
static void warn_no_usb_once(void)
{
    static bool warned = false;
    if (!warned) {
        warned = true;
        ESP_LOGW(TAG,
                 "USB HID not available on this board; ignoring HID output "
                 "(mouse/keyboard/macros have no effect).");
    }
}

extern "C" void usb_hid_init(void)
{
    // No USB peripheral on this board. host_api_start() is invoked directly
    // from app_main() (see main.cpp) so the serial transport still comes up.
    ESP_LOGI(TAG, "No native USB on this board; HID disabled.");
}

extern "C" void usb_hid_mouse_move(int8_t /*dx*/, int8_t /*dy*/)
{
    warn_no_usb_once();
}

extern "C" void usb_hid_mouse_scroll(int8_t /*vertical*/, int8_t /*horizontal*/)
{
    warn_no_usb_once();
}

extern "C" void usb_hid_mouse_click(uint8_t /*button*/)
{
    warn_no_usb_once();
}

extern "C" void usb_hid_mouse_buttons(uint8_t /*buttons*/, int8_t /*dx*/,
                                      int8_t /*dy*/, int8_t /*wheel*/)
{
    warn_no_usb_once();
}

extern "C" void usb_hid_keyboard_report(uint8_t /*modifiers*/,
                                        const uint8_t /*keycodes*/[6])
{
    warn_no_usb_once();
}

extern "C" void usb_hid_consumer_control(uint16_t /*usage*/)
{
    warn_no_usb_once();
}
