// SPDX-License-Identifier: GPL-3.0-or-later
//
// Hardware-UART host-API link (Stage LB5, extracted from host_api.cpp).
//
// Rides a hardware UART for boards with no usable native USB (e.g. the
// classic-ESP32 CYD boards, reached through their CH340 USB-UART bridge).
// Compiled only when the board requests it (CONFIG_TOUCHY_PROTO_OVER_UART)
// *and* declares it wires a dedicated protocol UART
// (CONFIG_TOUCHY_HAS_PROTO_UART, which supplies CONFIG_TOUCHY_PROTO_UART_NUM
// / CONFIG_TOUCHY_PROTO_UART_BAUD). Keep the IDF console off this UART so
// its log text never interleaves with the binary protocol frames.

#pragma once

#include "sdkconfig.h"

#if CONFIG_TOUCHY_PROTO_OVER_UART && CONFIG_TOUCHY_HAS_PROTO_UART

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "host_api_link.h"

struct UartLink : HostApiLink {
    QueueHandle_t evt_queue = nullptr;

    const char *name() const override { return "uart"; }
    bool init() override;  // install the UART driver + rx pump task
    // A UART has no link-up notion; the host is "connected" whenever bytes
    // can flow. Report true so the dispatcher always services frames.
    bool connected() override { return true; }
    size_t read_some(uint8_t *dst, size_t max) override;
    bool write_all(const uint8_t *p, size_t n) override;
    void flush() override;
};

// Process-wide singleton.
HostApiLink *uart_link_instance();

#endif  // CONFIG_TOUCHY_PROTO_OVER_UART && CONFIG_TOUCHY_HAS_PROTO_UART
