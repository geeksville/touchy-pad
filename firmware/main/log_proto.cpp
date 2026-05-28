// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stage 64.1 — implementation of log_proto.h. See log_proto.h for the
// public API and the design.md Stage 64.1 plan for the higher-level
// rationale.

#include "log_proto.h"

#include "sdkconfig.h"

#if CONFIG_TOUCHY_LOG_OVER_PROTO

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/portmacro.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <atomic>

// Capacity of the in-flight log ring. ~200 bytes per record × 32 ≈
// 6.4 KiB of heap; comfortably small even on the ESP32 boards we
// target in Stage 64.2.
#define LOG_PROTO_QUEUE_DEPTH 32

// Max formatted line we accept. Matches touchy_LogRecord.message[]
// (160) minus one byte for the truncation marker the wire ellipsis
// uses. Lines longer than this are truncated at the boundary.
#define LOG_PROTO_MSG_MAX (sizeof(((touchy_LogRecord *)0)->message))
#define LOG_PROTO_TAG_MAX (sizeof(((touchy_LogRecord *)0)->tag))

static QueueHandle_t s_queue;                   // touchy_LogRecord items
static vprintf_like_t s_prev_vprintf;           // chain to UART/CDC

// Re-entrancy guard: set on entry to s_vprintf, cleared on exit. If a
// log emission triggers a downstream log emission (e.g. an ESP_LOGE
// fires inside the queue send path or the host_api vendor-write code),
// the inner call short-circuits and bumps s_pending_dropped instead of
// recursing. `__thread` gives us per-task storage on xtensa/riscv
// ESP-IDF builds without fighting for a FreeRTOS TLS slot.
static __thread bool s_in_emit;

// Cumulative count of records discarded since the last successful
// enqueue. Folded into the next surviving record's num_dropped field.
// Accessed from arbitrary task / ISR contexts so atomic is required.
static std::atomic<uint32_t> s_pending_dropped;

// Translate an ESP-IDF level letter (the first character ESP_LOG
// prepends to every line: 'E', 'W', 'I', 'D', 'V') to our wire
// LogPriority enum. Anything else maps to TRACE (the default).
static touchy_LogPriority level_from_letter(char letter)
{
    switch (letter) {
    case 'E': return touchy_LogPriority_LOG_PRIORITY_ERROR;
    case 'W': return touchy_LogPriority_LOG_PRIORITY_WARN;
    case 'I': return touchy_LogPriority_LOG_PRIORITY_INFO;
    case 'D': return touchy_LogPriority_LOG_PRIORITY_DEBUG;
    case 'V': return touchy_LogPriority_LOG_PRIORITY_TRACE;
    default:  return touchy_LogPriority_LOG_PRIORITY_TRACE;
    }
}

// Parse the leading "X (timestamp) TAG: " prefix that ESP-IDF prepends
// to every ESP_LOG line and extract (priority, tag, body). `line` is
// modified in place: trailing '\n' and ANSI colour escapes are
// stripped, and the returned `body` pointer points inside `line`.
//
// If the prefix doesn't match (e.g. a bare printf), priority defaults
// to TRACE, tag is empty, and body == line.
static void parse_esp_log_prefix(char *line,
                                 touchy_LogPriority *out_prio,
                                 const char **out_tag,
                                 const char **out_body)
{
    *out_prio = touchy_LogPriority_LOG_PRIORITY_TRACE;
    *out_tag  = "";
    *out_body = line;

    // Skip a leading ANSI colour escape ("\x1b[0;3Xm") if present;
    // ESP-IDF emits these when CONFIG_LOG_COLORS=y.
    char *p = line;
    if (p[0] == '\x1b' && p[1] == '[') {
        char *end = strchr(p, 'm');
        if (end) p = end + 1;
    }

    // Expect "X (timestamp) TAG: body".
    if (!p[0] || p[1] != ' ' || p[2] != '(') return;

    char letter = p[0];
    char *rparen = strchr(p + 3, ')');
    if (!rparen || rparen[1] != ' ') return;

    char *tag_start = rparen + 2;
    char *colon = strstr(tag_start, ": ");
    if (!colon) return;

    *colon = '\0';
    *out_prio = level_from_letter(letter);
    *out_tag  = tag_start;
    *out_body = colon + 2;
}

static void strip_trailing_newline(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
    // Also drop any trailing ANSI colour reset ("\x1b[0m").
    if (n >= 4 && s[n - 4] == '\x1b' && s[n - 3] == '[' &&
        s[n - 2] == '0' && s[n - 1] == 'm') {
        s[n - 4] = '\0';
    }
}

static inline bool in_emit_set(void)
{
    if (s_in_emit) return true;
    s_in_emit = true;
    return false;
}

static inline void in_emit_clear(void)
{
    s_in_emit = false;
}

// Enqueue a fully-formed record, folding any pending dropped counter
// into it on success. Bumps the dropped counter on failure.
static void enqueue(const touchy_LogRecord *rec)
{
    touchy_LogRecord copy = *rec;
    uint32_t dropped = s_pending_dropped.exchange( 0);
    copy.num_dropped = dropped;

    if (xQueueSend(s_queue, &copy, 0) != pdTRUE) {
        // Queue full — drop this record and re-fold the dropped count
        // (plus 1 for the record we just lost) into the pending bucket.
        s_pending_dropped.fetch_add(dropped + 1);
    }
}

// esp_log_set_vprintf hook. Runs on whatever task called ESP_LOG*.
// Never blocks on the queue. ISR-context callers (ESP_EARLY_LOG /
// ets_printf paths) are deliberately not handled here; they continue
// to use the previous vprintf (UART) and never enqueue.
static int s_vprintf(const char *fmt, va_list ap)
{
    // Chain to the previous vprintf (UART / CDC) unconditionally when
    // configured, even before our re-entrancy check, so the dropped
    // log still appears on the physical console.
    int written = 0;
#if CONFIG_TOUCHY_LOG_TO_UART
    if (s_prev_vprintf) {
        va_list ap_copy;
        va_copy(ap_copy, ap);
        written = s_prev_vprintf(fmt, ap_copy);
        va_end(ap_copy);
    }
#endif

    // ISR context: cannot safely vsnprintf into our buffer (stack
    // size unknown) and cannot block on the queue. Bump the dropped
    // counter and bail.
    if (xPortInIsrContext()) {
        s_pending_dropped.fetch_add(1);
        return written;
    }

    if (!s_queue) return written;

    // Re-entrancy: if we're already inside s_vprintf on this task,
    // skip the enqueue so a log emission from the queue / protobuf
    // / vendor-write code path can't deadlock or recurse.
    if (in_emit_set()) {
        s_pending_dropped.fetch_add(1);
        return written;
    }

    char line[LOG_PROTO_MSG_MAX + 32];   // headroom for the ESP prefix
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    if (n < 0) {
        s_pending_dropped.fetch_add(1);
        in_emit_clear();
        return written;
    }
    strip_trailing_newline(line);

    touchy_LogPriority prio;
    const char *tag;
    const char *body;
    parse_esp_log_prefix(line, &prio, &tag, &body);

    touchy_LogRecord rec = touchy_LogRecord_init_zero;
    rec.priority     = prio;
    rec.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
    strncpy(rec.tag, tag, LOG_PROTO_TAG_MAX - 1);
    rec.tag[LOG_PROTO_TAG_MAX - 1] = '\0';
    // Truncate over-long bodies silently — anything past the wire
    // budget is dropped (Stage 64.1: keep records small to bound the
    // host RX/TX buffer cost).
    size_t body_len = strlen(body);
    if (body_len >= LOG_PROTO_MSG_MAX) {
        memcpy(rec.message, body, LOG_PROTO_MSG_MAX - 1);
        rec.message[LOG_PROTO_MSG_MAX - 1] = '\0';
    } else {
        memcpy(rec.message, body, body_len + 1);
    }

    enqueue(&rec);

    in_emit_clear();
    return written;
}

extern "C" void log_proto_start(void)
{
    if (s_queue) return;
    s_queue = xQueueCreate(LOG_PROTO_QUEUE_DEPTH, sizeof(touchy_LogRecord));
    if (!s_queue) return;
    s_prev_vprintf = esp_log_set_vprintf(s_vprintf);
}

extern "C" bool log_proto_pop(touchy_LogRecord *out)
{
    if (!s_queue || !out) return false;
    return xQueueReceive(s_queue, out, 0) == pdTRUE;
}

#else // CONFIG_TOUCHY_LOG_OVER_PROTO

extern "C" void log_proto_start(void) {}
extern "C" bool log_proto_pop(touchy_LogRecord *out) { (void)out; return false; }

#endif // CONFIG_TOUCHY_LOG_OVER_PROTO
