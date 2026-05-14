// SPDX-License-Identifier: Apache-2.0

#include "usb_hid.h"

#include "esp_log.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "class/hid/hid_device.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "usb_hid";

// ----- HID report descriptor -----
// We only need a single boot-style mouse interface.
static const uint8_t s_hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_MOUSE()
};

// Required by tinyusb when CDC is disabled but HID is enabled.
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
static const char *s_string_desc[5] = {
    (const char[]){0x09, 0x04},   // 0: supported language (English)
    "Geeksville",                  // 1: Manufacturer
    "Touchy-Pad",                  // 2: Product
    "000001",                      // 3: Serial
    "Touchy-Pad HID",              // 4: HID interface
};

static const tusb_desc_device_t s_device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x4403,    // matches v1
    .idProduct          = 0x1002,    // matches v1
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

// Single HID interface, IN endpoint 0x81, 8-byte FS poll rate 10 ms.
enum { ITF_NUM_HID = 0, ITF_NUM_TOTAL };
#define EPNUM_HID 0x81
#define CFG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)

static const uint8_t s_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CFG_TOTAL_LEN, 0, 100),
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, 4, HID_ITF_PROTOCOL_NONE,
                       sizeof(s_hid_report_descriptor),
                       EPNUM_HID, 8, 10),
};

extern "C" void usb_hid_init(void)
{
    ESP_LOGI(TAG, "Starting TinyUSB (HID mouse, VID:PID = 0x%04x:0x%04x)",
             s_device_desc.idVendor, s_device_desc.idProduct);

    // esp_tinyusb 2.x: start from the target-default config (full-speed on
    // ESP32-S3), then override only the descriptor pointers we need.
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device             = &s_device_desc;
    tusb_cfg.descriptor.string             = s_string_desc;
    tusb_cfg.descriptor.string_count       = sizeof(s_string_desc) / sizeof(s_string_desc[0]);
    tusb_cfg.descriptor.full_speed_config  = s_config_desc;
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

#if 0
    // After the bootloader ran USB-Serial/JTAG (303a:1001) the host has seen
    // a disconnect.  Force a clean re-enumeration by briefly pulling D+ low
    // so the host registers a new connect event.  Use a 500 ms hold-off so
    // the host port fully settles before we assert the new device.
    vTaskDelay(pdMS_TO_TICKS(20));
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));
    tud_connect();
    ESP_LOGI(TAG, "USB reconnect pulse sent");
#endif
}

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
    tud_hid_mouse_report(0, s_button_state, dx, dy, 0, 0);
}

extern "C" void usb_hid_mouse_scroll(int8_t vertical, int8_t horizontal)
{
    if (!wait_ready()) return;
    tud_hid_mouse_report(0, s_button_state, 0, 0, vertical, horizontal);
}

extern "C" void usb_hid_mouse_click(uint8_t button)
{
    if (!wait_ready()) return;
    s_button_state = button;
    tud_hid_mouse_report(0, s_button_state, 0, 0, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    s_button_state = 0;
    tud_hid_mouse_report(0, s_button_state, 0, 0, 0, 0);
}
