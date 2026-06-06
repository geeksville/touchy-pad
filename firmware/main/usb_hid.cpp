// SPDX-License-Identifier: GPL-3.0-or-later

#include "usb_hid.h"
#include "tc_tag.h"
#include "host_api.h"
#include "platform.h"        // platform_serial()
#include "proto/touchy.pb.h"  // touchy_Constants_USB_VID / USB_PID

#include "esp_log.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#if CFG_TUD_CDC
#include "tinyusb_cdc_acm.h"
#include "tinyusb_console.h"
#endif
#include "class/hid/hid_device.h"
#include "class/vendor/vendor_device.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = TOUCHY_TAG("usb_hid");

// Report IDs used on the composite HID interface.
enum {
    REPORT_ID_MOUSE    = 1,
    REPORT_ID_KEYBOARD = 2,
};

// ----- HID report descriptor -----
// Composite mouse + keyboard on a single HID interface, distinguished by
// report ID:
//   * 1 = boot-protocol mouse (5-byte payload)
//   * 2 = boot-protocol keyboard (8-byte payload)
// Single-interface saves an endpoint and is the common TinyUSB pattern.
static const uint8_t s_hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_MOUSE   (HID_REPORT_ID(REPORT_ID_MOUSE)),
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(REPORT_ID_KEYBOARD)),
};

// Required by tinyusb for HID descriptor queries.
extern "C" uint8_t const *tud_hid_descriptor_report_cb(uint8_t /*instance*/)
{
    return s_hid_report_descriptor;
}

extern "C" uint16_t tud_hid_get_report_cb(uint8_t /*instance*/, uint8_t /*report_id*/,
                                          hid_report_type_t /*report_type*/,
                                          uint8_t * /*buffer*/, uint16_t /*reqlen*/)
{
    return 0;
}

extern "C" void tud_hid_set_report_cb(uint8_t /*instance*/, uint8_t /*report_id*/,
                                      hid_report_type_t /*report_type*/,
                                      uint8_t const * /*buffer*/, uint16_t /*bufsize*/)
{
}

// ----- String / device descriptors -----
// Index 3 (Serial) is filled in at init from platform_serial() so the USB
// iSerialNumber matches SysBoardInfoResponse.serial (Stage 71).
static const char *s_string_desc[7] = {
    (const char[]){0x09, 0x04},   // 0: supported language (English)
    "Geeksville",                  // 1: Manufacturer
    "Touchy-Pad",                  // 2: Product
    "000001",                      // 3: Serial (replaced in usb_hid_init)
    "Touchy-Pad CDC",              // 4: CDC interface
    "Touchy-Pad HID",              // 5: HID interface
    "Touchy-Pad HostAPI",          // 6: Vendor interface (host_api)
};

static const tusb_desc_device_t s_device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    // Miscellaneous Device class with IAD is required for composite CDC+HID.
    .bDeviceClass       = 0xEF,
    .bDeviceSubClass    = 0x02,
    .bDeviceProtocol    = 0x01,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = touchy_Constants_USB_VID,
    .idProduct          = touchy_Constants_USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

// Composite HID mouse + custom vendor (host_api), optionally with
// CDC-ACM prepended when `CONFIG_TINYUSB_CDC_ENABLED=y` (see Stage 64.2).
// CDC uses two interfaces (control + data) and three endpoints.
// HID uses one interface and one endpoint.
// Vendor uses one interface with two bulk endpoints (command OUT,
// response IN). The interrupt-IN event endpoint described in
// docs/host-api.md is reserved for a future stage; the host transport
// treats it as optional.
enum {
#if CFG_TUD_CDC
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
#endif
    ITF_NUM_HID,
    ITF_NUM_VENDOR,
    ITF_NUM_TOTAL
};

// Endpoint addresses. High bit (0x80) = IN (device -> host). Each
// EP *number* (low 4 bits) can host one IN and one OUT independently,
// so e.g. 0x02 and 0x82 share number 2 but are separate physical
// endpoints. The ESP32-S3 USB-OTG controller is full-speed and has 5
// IN + 5 OUT endpoints total (EP0 + 4 user). Allocations are packed
// so the freed EP slot when CDC is off can be re-used by a future
// interrupt-IN event mailbox.
enum {
#if CFG_TUD_CDC
    EPNUM_CDC_NOTIF  = 0x81,
    EPNUM_CDC_OUT    = 0x02,
    EPNUM_CDC_IN     = 0x82,
    EPNUM_HID        = 0x83,
    EPNUM_VENDOR_OUT = 0x04,
    EPNUM_VENDOR_IN  = 0x84,
#else
    // CDC disabled: shift HID + Vendor down so they occupy EPs 1..2
    // (leaving 3..4 free for future use).
    EPNUM_HID        = 0x81,
    EPNUM_VENDOR_OUT = 0x02,
    EPNUM_VENDOR_IN  = 0x82,
#endif
};

#define CFG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN \
                       + (CFG_TUD_CDC ? TUD_CDC_DESC_LEN : 0) \
                       + TUD_HID_DESC_LEN \
                       + TUD_VENDOR_DESC_LEN)

// Full-speed configuration (used by FS-only controllers and as the FS
// alternate config on HS-capable devices like the ESP32-P4).
static const uint8_t s_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CFG_TOTAL_LEN, 0, 100),
#if CFG_TUD_CDC
    // CDC-ACM: notification EP, bulk OUT, bulk IN, 64-byte packet size.
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
#endif
    // HID composite (mouse + keyboard): IN EP, 16-byte max packet, 10 ms poll.
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, 5, HID_ITF_PROTOCOL_NONE,
                       sizeof(s_hid_report_descriptor),
                       EPNUM_HID, 16, 10),
    // Vendor (host_api): bulk OUT (command) + bulk IN (response).
    TUD_VENDOR_DESCRIPTOR(ITF_NUM_VENDOR, 6, EPNUM_VENDOR_OUT, EPNUM_VENDOR_IN, 64),
};

#if CONFIG_IDF_TARGET_ESP32P4
// High-speed configuration for the P4's USB 2.0 HS controller.
// USB 2.0 spec requires HS bulk endpoints to use 512-byte max packet size.
// Descriptor length is identical to the FS config (wMaxPacketSize is a field
// value, not part of the descriptor structure length).
static const uint8_t s_hs_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CFG_TOTAL_LEN, 0, 100),
#if CFG_TUD_CDC
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, 512),
#endif
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, 5, HID_ITF_PROTOCOL_NONE,
                       sizeof(s_hid_report_descriptor),
                       EPNUM_HID, 16, 10),
    TUD_VENDOR_DESCRIPTOR(ITF_NUM_VENDOR, 6, EPNUM_VENDOR_OUT, EPNUM_VENDOR_IN, 512),
};

// Device Qualifier: required for HS-capable devices to describe the device
// as it would appear at full speed (queried by a HS host during enumeration).
static const tusb_desc_device_qualifier_t s_device_qualifier = {
    .bLength            = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType    = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0xEF,
    .bDeviceSubClass    = 0x02,
    .bDeviceProtocol    = 0x01,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 0x01,
    .bReserved          = 0x00,
};
#endif  // CONFIG_IDF_TARGET_ESP32P4

extern "C" void usb_hid_init(void)
{
    // Stage 71: report the MAC-derived serial as USB iSerialNumber so it
    // matches SysBoardInfoResponse.serial. platform_serial() returns a
    // pointer to cached static storage, valid for the program lifetime.
    s_string_desc[3] = platform_serial();

    ESP_LOGI(TAG, "Starting TinyUSB (%sHID + vendor, VID:PID = 0x%04x:0x%04x, serial %s)",
             CFG_TUD_CDC ? "CDC-ACM + " : "",
             s_device_desc.idVendor, s_device_desc.idProduct, s_string_desc[3]);

    // esp_tinyusb 2.x: start from the target-default config (full-speed on
    // ESP32-S3), then override only the descriptor pointers we need.
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device             = &s_device_desc;
    tusb_cfg.descriptor.string             = s_string_desc;
    tusb_cfg.descriptor.string_count       = sizeof(s_string_desc) / sizeof(s_string_desc[0]);
    tusb_cfg.descriptor.full_speed_config  = s_config_desc;
#if CONFIG_IDF_TARGET_ESP32P4
    tusb_cfg.descriptor.high_speed_config  = s_hs_config_desc;
    tusb_cfg.descriptor.qualifier          = &s_device_qualifier;
#endif
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

#if CFG_TUD_CDC
#if CONFIG_TOUCHY_PROTO_OVER_SERIAL
    // Stage 64.3: the CDC-ACM port carries the protobuf protocol, not log
    // text. Use the raw TinyUSB CDC API (tud_cdc_*) directly from
    // host_api.cpp; deliberately skip tinyusb_cdcacm_init / the console
    // hookup so esp_log output is never written onto the protocol port
    // (it would corrupt frames). Logs reach the host via the Stage 64.1
    // LogRecord tunnel instead.
#else
    // If the call returns ESP_ERR_INVALID_STATE, ESP_ERROR_CHECK aborts, and
    // the chip reboots in a loop — preventing USB enumeration.
    const tinyusb_config_cdcacm_t cdc_cfg = {
        .cdc_port                     = TINYUSB_CDC_ACM_0,
        .callback_rx                  = nullptr,
        .callback_rx_wanted_char      = nullptr,
        .callback_line_state_changed  = nullptr,
        .callback_line_coding_changed = nullptr,
    };
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&cdc_cfg));

    // Mirror esp_log output to the CDC-ACM interface.  Logs emitted before
    // the host opens the port are silently dropped (UART0 still has them).
    ESP_ERROR_CHECK(tinyusb_console_init(ITF_NUM_CDC));
#endif
#endif
}

// TinyUSB calls this from its USB task whenever the vendor OUT endpoint
// receives bytes. We just nudge the host_api dispatcher; it does the
// actual draining via tud_vendor_read().
extern "C" void tud_vendor_rx_cb(uint8_t /*itf*/, uint8_t const * /*buffer*/,
                                 uint16_t /*bufsize*/)
{
    host_api_on_rx();
}

#if CFG_TUD_CDC && CONFIG_TOUCHY_PROTO_OVER_SERIAL
// Stage 64.3: bytes arrived on the CDC port that carries the protocol.
// Nudge the serial host_api dispatcher (see host_api.cpp).
extern "C" void tud_cdc_rx_cb(uint8_t /*itf*/)
{
    host_api_on_cdc_rx();
}
#endif

// Helper to wait briefly for the host to enumerate / accept the next report.
static bool wait_ready(void)
{
    for (int i = 0; i < 50; i++) {
        if (tud_mounted() && tud_hid_ready()) return true;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return false;
}

static uint8_t s_button_state = 0;

extern "C" void usb_hid_mouse_move(int8_t dx, int8_t dy)
{
    if (!wait_ready()) return;
    tud_hid_mouse_report(REPORT_ID_MOUSE, s_button_state, dx, dy, 0, 0);
}

extern "C" void usb_hid_mouse_scroll(int8_t vertical, int8_t horizontal)
{
    if (!wait_ready()) return;
    tud_hid_mouse_report(REPORT_ID_MOUSE, s_button_state, 0, 0, vertical, horizontal);
}

extern "C" void usb_hid_mouse_click(uint8_t button)
{
    if (!wait_ready()) return;
    s_button_state = button;
    tud_hid_mouse_report(REPORT_ID_MOUSE, s_button_state, 0, 0, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    s_button_state = 0;
    tud_hid_mouse_report(REPORT_ID_MOUSE, s_button_state, 0, 0, 0, 0);
}

// ---------------------------------------------------------------------------
// Stage 16 — raw mouse + keyboard helpers used by macros.cpp.
//
// These differ from the click/move helpers above in that the caller owns
// the button-state / key-state bookkeeping: a macro typically does
//   button_down(LEFT); move(...); button_up(LEFT);
// and we don't want hidden side-state to interfere with the macro runner.
// ---------------------------------------------------------------------------

extern "C" void usb_hid_mouse_buttons(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel)
{
    if (!wait_ready()) return;
    s_button_state = buttons;
    tud_hid_mouse_report(REPORT_ID_MOUSE, buttons, dx, dy, wheel, 0);
}

extern "C" void usb_hid_keyboard_report(uint8_t modifiers, const uint8_t keycodes[6])
{
    if (!wait_ready()) return;
    uint8_t kc[6] = {0};
    if (keycodes) memcpy(kc, keycodes, 6);
    tud_hid_keyboard_report(REPORT_ID_KEYBOARD, modifiers, kc);
}
