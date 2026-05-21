// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stage 16 \u2014 ActionMacro runner.
//
// A macro is a list of HID keyboard / mouse steps (see `touchy.MacroStep`
// in proto/touchy.proto) replayed entirely on the device. Macros run on a
// dedicated low-priority FreeRTOS task so step delays do not block LVGL
// or the host_api dispatcher. Submissions are non-blocking: the queue
// drops the macro and logs a warning if full.

#pragma once

#include "widgets.pb.h"

#ifdef __cplusplus
extern "C" {
#endif

// Practical cap on actions per widget event slot. Matches the
// `max_count` in touchy.options for every *.on_click / *.on_change field.
#define TOUCHY_MAX_ACTIONS_PER_EVENT  8

// One-time setup. Spawns the runner task. Safe to call repeatedly.
void macros_init(void);

// Enqueue a deep copy of `macro` for replay. Returns false when the
// queue is full or the macro is rejected (e.g. zero steps).
bool macros_run(const touchy_ActionMacro *macro);

#ifdef __cplusplus
}
#endif
