// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB vendor-bulk host-API link — see vendor_link.h.

#include "vendor_link.h"

#if CONFIG_TOUCHY_PROTO_OVER_VENDORUSB && CONFIG_SOC_USB_OTG_SUPPORTED

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tusb.h"

bool VendorLink::connected()
{
    return tud_mounted();
}

size_t VendorLink::read_some(uint8_t *dst, size_t max)
{
    return tud_vendor_read(dst, max);
}

bool VendorLink::write_all(const uint8_t *p, size_t n)
{
    size_t sent = 0;
    while (sent < n) {
        uint32_t w = tud_vendor_write(p + sent, n - sent);
        if (w == 0) {
            if (!tud_mounted()) return false;
            tud_vendor_write_flush();
            vTaskDelay(1);
            continue;
        }
        sent += w;
    }
    return true;
}

void VendorLink::flush()
{
    tud_vendor_write_flush();
}

HostApiLink *vendor_link_instance()
{
    static VendorLink s_link;
    return &s_link;
}

#endif  // CONFIG_TOUCHY_PROTO_OVER_VENDORUSB && CONFIG_SOC_USB_OTG_SUPPORTED
