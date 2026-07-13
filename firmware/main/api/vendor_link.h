// SPDX-License-Identifier: GPL-3.0-or-later
//
// USB vendor-bulk host-API link (Stage LB5, extracted from host_api.cpp).
//
// Rides the TinyUSB vendor bulk endpoint pair. Compiled only when the
// board both requests it (CONFIG_TOUCHY_PROTO_OVER_VENDORUSB) and has a
// native USB-OTG controller (CONFIG_SOC_USB_OTG_SUPPORTED).

#pragma once

#include "sdkconfig.h"

#if CONFIG_TOUCHY_PROTO_OVER_VENDORUSB && CONFIG_SOC_USB_OTG_SUPPORTED

#include "host_api_link.h"

struct VendorLink : HostApiLink {
    const char *name() const override { return "vendor"; }
    bool connected() override;
    size_t read_some(uint8_t *dst, size_t max) override;
    bool write_all(const uint8_t *p, size_t n) override;
    void flush() override;
};

// Process-wide singleton (the dispatcher and the USB ISR share it).
HostApiLink *vendor_link_instance();

#endif  // CONFIG_TOUCHY_PROTO_OVER_VENDORUSB && CONFIG_SOC_USB_OTG_SUPPORTED
