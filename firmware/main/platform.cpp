// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stage 71 — board-agnostic device identity helpers.
//
// `platform_get()` is provided per-board (boards/<board>/board.cpp); the
// serial helper here is common to every board because it derives purely
// from the chip's factory MAC, which every ESP target exposes through
// `esp_read_mac()`.

#include "platform.h"

#include "esp_mac.h"

#include <stdio.h>

const char *platform_serial(void)
{
    // "t" + 12 hex digits + NUL = 14 bytes. Computed once and cached so the
    // USB descriptor and the host-API response always return the identical
    // pointer/value.
    static char serial[16];
    if (serial[0] != '\0') {
        return serial;
    }

    uint8_t mac[6] = {0};
    // ESP_MAC_BASE is the factory-burned base MAC; stable across reboots
    // and unique per device.
    esp_read_mac(mac, ESP_MAC_BASE);
    snprintf(serial, sizeof(serial), "t%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return serial;
}

// Weak "base class" implementation: every board with a touch panel inherits
// this and returns true. Touch-less boards (Stage LB1 LED-matrix boards)
// override it with a strong definition in their boards/<board>/board.cpp.
__attribute__((weak)) bool platform_is_touchable(void)
{
    return true;
}
