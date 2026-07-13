// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB-CDC ACM host-API link — see serial_link.h.

#include "serial_link.h"

#if CONFIG_TOUCHY_PROTO_OVER_CDCACM && CONFIG_SOC_USB_OTG_SUPPORTED && CONFIG_TINYUSB_CDC_COUNT

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tusb.h"

bool SerialLink::connected()
{
    // tud_cdc_connected() tracks DTR; pyserial asserts it on open.
    return tud_cdc_connected();
}

size_t SerialLink::read_some(uint8_t *dst, size_t max)
{
    if (!tud_cdc_available()) return 0;
    return tud_cdc_read(dst, max);
}

bool SerialLink::write_all(const uint8_t *p, size_t n)
{
    size_t sent = 0;
    while (sent < n) {
        uint32_t w = tud_cdc_write(p + sent, n - sent);
        if (w == 0) {
            if (!tud_cdc_connected()) return false;
            tud_cdc_write_flush();
            vTaskDelay(1);
            continue;
        }
        sent += w;
    }
    return true;
}

void SerialLink::flush()
{
    tud_cdc_write_flush();
}

HostApiLink *serial_link_instance()
{
    static SerialLink s_link;
    return &s_link;
}

#endif  // CONFIG_TOUCHY_PROTO_OVER_CDCACM && CONFIG_SOC_USB_OTG_SUPPORTED && CONFIG_TINYUSB_CDC_COUNT
