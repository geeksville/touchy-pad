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
// The caller owns the heap-allocated out->message pointer; transfer it
// to a pb_release()-managed struct (e.g. a PbMessage<touchy_Response>
// field) or call free(out->message) when done.
bool log_proto_pop(touchy_LogRecord *out);

// Stage 82 — set the minimum log priority that gets queued for the host.
// Records strictly below `level` are dropped device-side (intentional
// filtering, not counted as loss). Called from the prefs subsystem on boot
// and whenever a SetPreferencesCmd changes min_log_level. No-op when the
// proto log tunnel is disabled.
void log_proto_set_min_level(touchy_LogPriority level);

// Block until the host has drained every queued log record (the queue
// is empty), or until timeout_ms elapses. Returns true if fully
// flushed, false on timeout. Returns true immediately when the proto
// tunnel is disabled or no records are pending.
//
// Use this sparingly, right after an ESP_LOG* line you cannot afford to
// lose, to guarantee it has crossed the wire before a risky operation
// (one that may corrupt the heap and reboot the device, taking the
// async log queue with it).
//
// IMPORTANT: log consumption is driven by the host polling
// event_consume, which is serviced on the host_api task. NEVER call
// this from the host_api dispatch task itself — that task would block
// here and stop draining the queue, guaranteeing a timeout. Call it
// from another task (e.g. the LVGL/UI task).
bool touchy_logs_flush(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
