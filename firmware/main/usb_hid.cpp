// SPDX-License-Identifier: Apache-2.0

#include "usb_hid.h"

#include "esp_log.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_console.h"
#include "class/hid/hid_device.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "usb_hid";

// ----- HID report descriptor -----
// We only need a single boot-style mouse interface.
static const uint8_t s_hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_MOUSE()
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
static const char *s_string_desc[6] = {
    (const char[]){0x09, 0x04},   // 0: supported language (English)
    "Geeksville",                  // 1: Manufacturer
    "Touchy-Pad",                  // 2: Product
    "000001",                      // 3: Serial
    "Touchy-Pad CDC",              // 4: CDC interface
    "Touchy-Pad HID",              // 5: HID interface
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
    .idVendor           = 0x4403,    // matches v1
    .idProduct          = 0x1002,    // matches v1
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

// Composite CDC-ACM + HID mouse.
// CDC uses two interfaces (control + data) and three endpoints.
// HID uses one interface and one endpoint.
enum {
#if CFG_TUD_CDC 
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
#endif   
    ITF_NUM_HID,
    ITF_NUM_TOTAL
};
#define EPNUM_CDC_NOTIF  0x81
#define EPNUM_CDC_OUT    0x02
#define EPNUM_CDC_IN     0x82
#define EPNUM_HID        (0x81 + ITF_NUM_HID)

#define CFG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + (CFG_TUD_CDC ? TUD_CDC_DESC_LEN : 0) + TUD_HID_DESC_LEN)

static const uint8_t s_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CFG_TOTAL_LEN, 0, 100),
#if CFG_TUD_CDC    
    // CDC-ACM: notification EP, bulk OUT, bulk IN, 64-byte packet size.
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
#endif
    // HID mouse: IN EP, 8-byte max packet, 10 ms poll interval.
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, 5, HID_ITF_PROTOCOL_NONE,
                       sizeof(s_hid_report_descriptor),
                       EPNUM_HID, 8, 10),
};

extern "C" void usb_hid_init(void)
{
    ESP_LOGI(TAG, "Starting TinyUSB (CDC-ACM + HID mouse, VID:PID = 0x%04x:0x%04x)",
             s_device_desc.idVendor, s_device_desc.idProduct);

    // esp_tinyusb 2.x: start from the target-default config (full-speed on
    // ESP32-S3), then override only the descriptor pointers we need.
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device             = &s_device_desc;
    tusb_cfg.descriptor.string             = s_string_desc;
    tusb_cfg.descriptor.string_count       = sizeof(s_string_desc) / sizeof(s_string_desc[0]);
    tusb_cfg.descriptor.full_speed_config  = s_config_desc;
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    // If the call returns ESP_ERR_INVALID_STATE, ESP_ERROR_CHECK aborts, and
    // the chip reboots in a loop — preventing USB enumeration.
    const tinyusb_config_cdcacm_t cdc_cfg = {
        .cdc_port                     = TINYUSB_CDC_ACM_0,
        .callback_rx                  = nullptr,
        .callback_rx_wanted_char      = nullptr,
        .callback_line_state_changed  = nullptr,
        .callback_line_coding_changed = nullptr,
    };
#if CFG_TUD_CDC     
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&cdc_cfg));

    // Mirror esp_log output to the CDC-ACM interface.  Logs emitted before
    // the host opens the port are silently dropped (UART0 still has them).
    ESP_ERROR_CHECK(tinyusb_console_init(ITF_NUM_CDC));
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
