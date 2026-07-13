// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB-CDC ACM host-API link (Stage LB5, extracted from host_api.cpp).
//
// Rides a TinyUSB CDC-ACM port. Compiled only when the board requests it
// (CONFIG_TOUCHY_PROTO_OVER_CDCACM) and the chip has native USB-OTG with
// the CDC class enabled (CONFIG_SOC_USB_OTG_SUPPORTED &&
// CONFIG_TINYUSB_CDC_COUNT). The port must carry only protocol frames — the
// console must not be routed to it (see usb_hid.cpp).

#pragma once

#include "sdkconfig.h"

#if CONFIG_TOUCHY_PROTO_OVER_CDCACM && CONFIG_SOC_USB_OTG_SUPPORTED && CONFIG_TINYUSB_CDC_COUNT

#include "host_api_link.h"

struct SerialLink : HostApiLink {
    const char *name() const override { return "cdcacm"; }
    bool connected() override;
    size_t read_some(uint8_t *dst, size_t max) override;
    bool write_all(const uint8_t *p, size_t n) override;
    void flush() override;
};

// Process-wide singleton (the dispatcher and the USB ISR share it).
HostApiLink *serial_link_instance();

#endif  // CONFIG_TOUCHY_PROTO_OVER_CDCACM && CONFIG_SOC_USB_OTG_SUPPORTED && CONFIG_TINYUSB_CDC_COUNT
