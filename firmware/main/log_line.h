// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stage 18 — shared device log sink + LogLine.
//
// Any subsystem can post a one-line status string via
// `log_line_post(...)`. The most recent line is cached so newly
// created LogLines show context immediately; existing LogLines
// update synchronously. Posts are safe to make from any task that
// already holds the LVGL port lock (e.g. the LVGL event callback
// in TrackpadWidget); from other tasks the caller must take/release
// the lock around the post.

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// printf-style. Truncates to ~120 chars. Updates every registered
// LogLine and stores the line for future widget creations.
void log_line_post(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}

// Single-line label tied to the shared log sink. Construct as a child
// of a Screen, then size/style normally via the host DSL. The widget
// registers itself with the sink on construction and unregisters on
// LV_EVENT_DELETE.
class LogLine {
public:
    explicit LogLine(lv_obj_t *parent);

    lv_obj_t *obj() const { return _container; }

    // Internal: called by log_line_post via the registry.
    void _update(const char *line);

private:
    lv_obj_t *_container = nullptr;
    lv_obj_t *_label     = nullptr;

    static void _deleteCb(lv_event_t *e);
};

#endif  // __cplusplus
