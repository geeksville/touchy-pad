// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stage lb13 — canonical protobuf-JSON <-> nanopb bridge for the network
// command endpoint (net/http_api.cpp). Lets a plain HTTP client (curl,
// browser fetch, …) drive the device without protobuf tooling.
//
// nanopb has no reflection, so the mapping is hand-written per message.
// json_to_command() covers setProperty plus the simple scalar commands;
// response_to_json() renders the top-level code + the sys_board_info
// payload. Field names / enum values follow canonical proto3 JSON
// (lowerCamelCase, enum names, default/zero fields omitted).

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "touchy.pb.h"

#ifdef __cplusplus
extern "C" {
#endif

// Parse a canonical protobuf-JSON object (`json`, `len` bytes) into `out`.
// Returns false on malformed JSON or an unsupported/unknown command key;
// on failure *err (if non-null) points at a short static message. On
// success `out` is fully initialised (touchy_Command_init_zero + the one
// command populated).
bool json_to_command(const char *json, size_t len, touchy_Command *out,
                     const char **err);

// Render `resp` as a canonical protobuf-JSON string into a freshly
// malloc'd, NUL-terminated buffer (caller free()s). Returns nullptr on OOM.
char *response_to_json(const touchy_Response *resp);

#ifdef __cplusplus
}
#endif
