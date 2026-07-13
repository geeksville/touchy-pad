// SPDX-License-Identifier: GPL-3.0-or-later
//
// Touchy-Pad custom USB host-API endpoint.
//
// Implements the device side of the protocol documented in
// docs/host-api.md and defined in proto/touchy.proto:
//
//   * Command  (host -> device, bulk OUT) : `touchy.Command` envelopes
//   * Response (device -> host, bulk IN)  : `touchy.Response` envelopes
//   * Event    (device -> host, interrupt IN, future)
//
// Wire framing is a u32 LE length prefix followed by the protobuf payload.
// Stage 13 implements just `Sys_Version_Get`; other commands respond with
// `RESULT_NOT_SUPPORTED` until subsequent stages fill them in.

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Start the host-API dispatcher task(s). Called once from app_main() after
// the transports are configured (USB brought up via usb_hid_init() on chips
// with native USB-OTG, and/or the serial UART). Selects its byte-stream
// link(s) by build config: the vendor bulk pair on native-USB chips
// (CONFIG_SOC_USB_OTG_SUPPORTED), and/or a serial port when
// CONFIG_TOUCHY_PROTO_OVER_SERIAL is set. Each link's task blocks on its
// stream and services commands sequentially.
void host_api_start(void);

// Hook for TinyUSB's tud_vendor_rx_cb — woken when bytes arrive on the
// command OUT endpoint. Defined in host_api.cpp; declared here so usb_hid.cpp
// can forward the callback without pulling in the dispatcher internals.
void host_api_on_rx(void);

// Hook for TinyUSB's tud_cdc_rx_cb — woken when bytes arrive on the
// USB-CDC ACM port that carries the protocol when
// CONFIG_TOUCHY_PROTO_OVER_SERIAL is set. No-op stub otherwise.
void host_api_on_cdc_rx(void);

// Enqueue an LvEvent for the host to fetch via EventConsumeCmd. Triggers
// a one-shot "event ready" mailbox notification on the interrupt-IN
// endpoint (the host then drains via repeated EventConsumeCmd until
// RESULT_NOT_FOUND). Drops oldest events when the queue is full.
//
// Forward-declared via the nanopb-generated struct so screens.cpp doesn't
// need to pull in pb_encode.h.
struct _touchy_LvEvent;
void host_api_post_event(const struct _touchy_LvEvent *evt);

#ifdef __cplusplus
}
#endif
