// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stage 64.1 — tunnel ESP-IDF log lines to the host via the existing
// host_api protobuf channel. The dispatcher in host_api.cpp pops
// pending records here (via log_proto_pop) and returns them as
// Response.log_record payloads from EventConsumeCmd polls.
//
// Compile-time gated by CONFIG_TOUCHY_LOG_OVER_PROTO. When disabled,
// log_proto_start() is a no-op and log_proto_pop() always reports
// "queue empty" so host_api.cpp falls through to RESULT_NOT_FOUND.

#pragma once

#include "sdkconfig.h"
#include "touchy.pb.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialise the log queue and hook esp_log_set_vprintf() so subsequent
// ESP_LOG* / printf calls are also queued for the host. Idempotent.
void log_proto_start(void);

// Pop the next pending log record into *out. Returns true on success,
// false if the queue is empty. Non-blocking, callable only from a
// task context (not from an ISR).
//
// On success *out is fully populated, including any folded num_dropped
// counter from records discarded since the previous successful pop.
bool log_proto_pop(touchy_LogRecord *out);

#ifdef __cplusplus
}
#endif
