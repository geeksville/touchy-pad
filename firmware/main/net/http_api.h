// SPDX-License-Identifier: GPL-3.0-or-later
//
// Touchy-Pad HTTP(S) command API (Stage lb8).
//
// Serves the protobuf Command/Response protocol over a single endpoint:
//
//   POST /touchy/api/v1/command
//   Content-Type: application/protobuf
//   <bare serialized touchy.Command>            (no MAGIC|LEN|CRC8 framing)
//
// The response body is a bare serialized touchy.Response. The handler
// reuses host_api_dispatch_serialized() so every command behaves exactly
// as it does over USB / UART.
//
// Only compiled when CONFIG_TOUCHY_WIFI is enabled (implies a WiFi-capable
// chip). Started by the network subsystem once WiFi has an IP.

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Start the command API server.
//
//   * https == false : plaintext HTTP on port 80.
//   * https == true  : TLS-PSK HTTPS on port 443; `psk_hex` is the shared
//                      key as a hex string. The plaintext port is NOT
//                      started in this mode (no downgrade path).
//
// Idempotent-ish: call http_api_stop() before restarting with a different
// mode. Returns ESP_OK (0) on success.
int  http_api_start(bool https, const char *psk_hex);

// Stop whichever server is running (no-op if none).
void http_api_stop(void);

#ifdef __cplusplus
}
#endif
