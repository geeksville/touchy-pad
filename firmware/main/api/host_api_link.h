// SPDX-License-Identifier: GPL-3.0-or-later
//
// Abstract host-API transport link (Stage LB5).
//
// A HostApiLink is the byte stream the host_api dispatcher reads Commands
// from and writes Responses to. There is one concrete subclass per physical
// interface — VendorLink (USB vendor bulk), SerialLink (USB-CDC ACM) and
// UartLink (hardware UART), each in its own file in this directory. The
// dispatcher runs one task per link and never aliases state between links:
// every instance owns its rx/tx scratch buffers and a wake semaphore.
//
// Stage LB5 generalised the old build-time single-link selection into an
// array of links (see host_api.cpp): a board may expose the protocol over
// several interfaces at once and a client connects over whichever it likes.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

// Largest serialised Command we accept / Response we emit. With streaming
// writes (Stage 51) every FileWriteCmd carries at most ~4 KiB of payload,
// so one USB-HS bulk transfer's worth plus nanopb framing is plenty.
#define HOST_API_RX_MAX  (5 * 1024)
#define HOST_API_TX_MAX  (4 * 1024)

struct HostApiLink {
    SemaphoreHandle_t rx_sem = nullptr;
    uint8_t rx_buf[HOST_API_RX_MAX];
    uint8_t tx_buf[HOST_API_TX_MAX];

    virtual ~HostApiLink() {}

    // Short human-readable link name, also used as the dispatcher task name.
    virtual const char *name() const = 0;

    // One-time driver bring-up, called after rx_sem is created and before
    // the dispatcher task starts. Default no-op; UartLink installs its
    // UART driver + rx pump here. Return false to abort registering the
    // link.
    virtual bool init() { return true; }

    virtual bool connected() = 0;
    // Non-blocking: copy up to `max` available bytes into `dst`, return count.
    virtual size_t read_some(uint8_t *dst, size_t max) = 0;
    // Blocking write of exactly `n` bytes; false if the link drops.
    virtual bool write_all(const uint8_t *p, size_t n) = 0;
    virtual void flush() {}

    // Block up to `ms` waiting for the rx wake semaphore (or just sleep if
    // this link has none).
    void wait_rx(uint32_t ms) {
        if (rx_sem) xSemaphoreTake(rx_sem, pdMS_TO_TICKS(ms));
        else        vTaskDelay(pdMS_TO_TICKS(ms));
    }

    // Wake this link's dispatcher task from an ISR (rx bytes available).
    void wake_from_isr() {
        if (!rx_sem) return;
        BaseType_t hpw = pdFALSE;
        xSemaphoreGiveFromISR(rx_sem, &hpw);
        portYIELD_FROM_ISR(hpw);
    }
};
