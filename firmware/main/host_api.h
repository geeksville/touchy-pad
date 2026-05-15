// SPDX-License-Identifier: Apache-2.0
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

// Start the host-API dispatcher task. Called once from usb_hid_init() after
// TinyUSB is up. The task blocks on the vendor bulk OUT endpoint and
// services commands sequentially.
void host_api_start(void);

// Hook for TinyUSB's tud_vendor_rx_cb — woken when bytes arrive on the
// command OUT endpoint. Defined in host_api.cpp; declared here so usb_hid.cpp
// can forward the callback without pulling in the dispatcher internals.
void host_api_on_rx(void);

#ifdef __cplusplus
}
#endif
