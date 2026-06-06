// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stage 64.1 — implementation of log_proto.h. See log_proto.h for the
// public API and the design.md Stage 64.1 plan for the higher-level
// rationale.

#include "log_proto.h"
#include "tc_tag.h"

#include "sdkconfig.h"

#if CONFIG_TOUCHY_LOG_OVER_PROTO

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/portmacro.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>

// Capacity of the in-flight log ring. ~200 bytes per record × 32 ≈
// 6.4 KiB of heap; comfortably small even on the ESP32 boards we
// target in Stage 64.2.
#define LOG_PROTO_QUEUE_DEPTH 32

// Stack buffer for the raw vsnprintf output in s_vprintf (includes the
// full "X (ts) TAG: body" ESP-IDF prefix). Lines longer than this are
// truncated. `message` is now FT_STATIC (a fixed-size inline char[] in
// touchy_LogRecord), so the parsed body is copied straight into the
// record with no heap activity — see the Stage 64.1 note in
// proto/touchy.options for why malloc() must never run in this hook.
#define LOG_PROTO_LINE_BUF 256

static QueueHandle_t s_queue;                   // touchy_LogRecord items
static vprintf_like_t s_prev_vprintf;           // chain to UART/CDC

// Boot-stability gate. Until log_proto_enable() flips this true (late in
// boot, after all driver/flash/display bring-up), s_vprintf forwards to
// UART only and skips ALL of: the thread-local re-entrancy guard,
// vsnprintf, and the FreeRTOS queue. Early-boot init logs from contexts
// with interrupts disabled, where those operations are unsafe and a DEBUG
// log flood intermittently crashes the device. Atomic: read from arbitrary
// task contexts, written once from app_main.
static std::atomic<bool> s_ready{false};

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

// Stage 82 — minimum priority that gets queued for the host. Records
// strictly below this are dropped silently (intentional filtering, so we
// do NOT bump s_pending_dropped for them). Default ERROR; the prefs
// subsystem overrides it on boot and on SetPreferencesCmd. Atomic because
// it's read from arbitrary task contexts inside s_vprintf.
static std::atomic<int> s_min_level{touchy_LogPriority_LOG_PRIORITY_ERROR};

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
// into it on success. Bumps the dropped counter on failure. The record
// (including its inline message buffer) is copied by value into the
// queue — there is no heap ownership to track.
static void enqueue(const touchy_LogRecord *rec)
{
    touchy_LogRecord copy = *rec;
    uint32_t dropped = s_pending_dropped.exchange(0);
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

    // Boot-stability gate: until the tunnel is explicitly enabled late in
    // boot, do nothing beyond the UART chain above. This keeps the unsafe
    // queue / thread-local / vsnprintf work out of early-boot init paths
    // that run with interrupts disabled.
    if (!s_ready.load(std::memory_order_relaxed)) {
        return written;
    }

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

    char line[LOG_PROTO_LINE_BUF];   // raw vsnprintf output including ESP prefix
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

    // Stage 82 — drop anything below the host's requested threshold.
    // Additionally, DEBUG/TRACE records from tags that don't carry the
    // "tc-" prefix (i.e. third-party ESP-IDF / driver noise) are always
    // suppressed regardless of the configured min_log_level. This prevents
    // a DEBUG pref from flooding the tunnel with system internals and is the
    // root fix for the intermittent early-boot crash: system drivers emit
    // heavy DEBUG traffic during bring-up; by the time our hook is active
    // (post-enable) those logs are already past the dangerous init window,
    // but filtering them here keeps the queue healthy.
    // Intentional filtering — do NOT bump s_pending_dropped.
    int min_level = s_min_level.load(std::memory_order_relaxed);
    bool is_touchy = (strncmp(tag, "tc-", 3) == 0);
    int effective_min = (!is_touchy && min_level < touchy_LogPriority_LOG_PRIORITY_INFO)
                        ? (int)touchy_LogPriority_LOG_PRIORITY_INFO
                        : min_level;
    if ((int)prio < effective_min) {
        in_emit_clear();
        return written;
    }

    // Copy the parsed body straight into the record's inline buffer
    // (FT_STATIC). No malloc here — this hook can run with interrupts
    // disabled during early boot, where heap calls are illegal.
    touchy_LogRecord rec = touchy_LogRecord_init_zero;
    rec.priority     = prio;
    rec.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
    snprintf(rec.tag,     sizeof(rec.tag),     "%s", tag);
    snprintf(rec.message, sizeof(rec.message), "%s", body);

    enqueue(&rec);

    in_emit_clear();
    return written;
}

extern "C" void log_proto_set_min_level(touchy_LogPriority level)
{
    s_min_level.store((int)level, std::memory_order_relaxed);
}

extern "C" void log_proto_enable(void)
{
    // Install our vprintf hook only now, late in boot. Until this point
    // the ESP-IDF default vprintf runs directly with none of our code in
    // the path (no chain indirection, no atomic load, no queue/TLS work),
    // so the early-boot init paths that log with interrupts disabled stay
    // on the stock console path that ESP-IDF is known to tolerate.
    if (s_ready.exchange(true, std::memory_order_relaxed)) return;
    if (!s_queue) return;
    s_prev_vprintf = esp_log_set_vprintf(s_vprintf);
}

extern "C" void log_proto_start(void)
{
    // Create the queue early so log_proto_pop()/flush are safe to call,
    // but do NOT install the vprintf hook yet — that happens in
    // log_proto_enable() once boot is past the unsafe early-init phase.
    if (s_queue) return;
    s_queue = xQueueCreate(LOG_PROTO_QUEUE_DEPTH, sizeof(touchy_LogRecord));
}

extern "C" bool log_proto_pop(touchy_LogRecord *out)
{
    if (!s_queue || !out) return false;
    return xQueueReceive(s_queue, out, 0) == pdTRUE;
}

extern "C" bool touchy_logs_flush(uint32_t timeout_ms)
{
    // Nothing to flush if the tunnel never started.
    if (!s_queue) return true;

    const TickType_t start = xTaskGetTickCount();
    const TickType_t budget = pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        // "Consumed by host" == popped off the queue by the host_api
        // dispatcher in response to an event_consume poll. Once the
        // queue is empty every record has been handed to the encoder
        // and written to the wire.
        if (uxQueueMessagesWaiting(s_queue) == 0) return true;

        if ((TickType_t)(xTaskGetTickCount() - start) >= budget) {
            return false;   // host not draining fast enough
        }
        // Yield long enough for the host's ~20 Hz poll loop to make
        // progress without busy-spinning.
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

#else // CONFIG_TOUCHY_LOG_OVER_PROTO

extern "C" void log_proto_start(void) {}
extern "C" void log_proto_enable(void) {}
extern "C" bool log_proto_pop(touchy_LogRecord *out) { (void)out; return false; }
extern "C" bool touchy_logs_flush(uint32_t timeout_ms) { (void)timeout_ms; return true; }
extern "C" void log_proto_set_min_level(touchy_LogPriority level) { (void)level; }

#endif // CONFIG_TOUCHY_LOG_OVER_PROTO
