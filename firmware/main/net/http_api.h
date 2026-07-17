// SPDX-License-Identifier: GPL-3.0-or-later
//
// Touchy-Pad HTTP(S) command API (Stage lb8 + lb9 mTLS).
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
//   * mtls == false : plaintext HTTP on port 80.
//   * mtls == true  : HTTPS with **mutual TLS** on port 443. The server
//                     cert/key and the client-verification CA are read
//                     from files on the F: filesystem (see http_api.cpp).
//                     A client must present a certificate signed by that
//                     CA or the TLS handshake is rejected. The plaintext
//                     port is NOT started in this mode (no downgrade path).
//
// Call http_api_stop() before restarting with a different mode. Returns
// ESP_OK (0) on success, non-zero on failure (e.g. missing certs).
int  http_api_start(bool mtls);

// Stop whichever server is running (no-op if none).
void http_api_stop(void);

#ifdef __cplusplus
}
#endif
