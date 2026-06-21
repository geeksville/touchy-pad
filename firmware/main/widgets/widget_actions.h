// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stage 20.2 / 24 — LVGL widget → Action wiring.
//
// Each interactive widget in a touchy.Screen can carry a list of Actions
// per LVGL event slot (CLICKED, VALUE_CHANGED, ...). `widget_attach_actions`
// hooks one of those slots up to LVGL: when the event fires it walks the
// list and runs each action.
//
// Action dispatch:
//   * ActionMacro   → run HID macro (macros_run).
//   * ActionHost    → forward LvEvent to host_api with the widget id +
//                     packed value.
//   * ActionDevice  → on-device behaviours (Stage 24: screen switching).
//
// User-data stored on the LVGL object is heap-allocated and freed via an
// LV_EVENT_DELETE callback that the widget builders don't have to think
// about.

#pragma once

#include "lvgl.h"
#include "macros.h"
#include "touchy.pb.h"
#include "widgets.pb.h"
#include "pb.h"

// Populate the value-bearing field of `evt` (the oneof in touchy.LvEvent)
// from the widget's current LVGL state. Pluggable per widget kind so the
// same dispatch path serves buttons (no value), sliders (int), switches
// (bool), etc.
using widget_value_fn = void (*)(lv_obj_t *obj, touchy_LvEvent *evt);

void widget_value_none(lv_obj_t *obj, touchy_LvEvent *evt);
void widget_value_slider(lv_obj_t *obj, touchy_LvEvent *evt);
void widget_value_switch(lv_obj_t *obj, touchy_LvEvent *evt);

// Attach `actions[]` (owned by the active screen's decoded protobuf —
// must outlive `obj`) to the LVGL event `code` on `obj`. The internal
// slot copy of `widget_id` is bounded by the proto cap (32 bytes incl.
// terminator). On widget deletion the slot is freed automatically.
//
// No-op when `actions_count == 0` — no event handlers are registered,
// keeping the LVGL event-list short for purely-decorative widgets.
void widget_attach_actions(lv_obj_t *obj,
                           const char *widget_id,
                           const touchy_Action *actions,
                           pb_size_t actions_count,
                           lv_event_code_t code,
                           widget_value_fn set_value);

// Stage 71 — run a single Action exactly as if a widget had triggered it.
// Shared by the LVGL widget event callback and the host-driven
// RunActionsCmd path. `obj` may be NULL and `widget_id` may be "" for
// host-sourced actions (the host `set_value` is then `widget_value_none`,
// which ignores `obj`). Must be called with the LVGL lock held.
void widget_run_action(const touchy_Action &act,
                       lv_obj_t *obj,
                       const char *widget_id,
                       lv_event_code_t code,
                       widget_value_fn set_value);

// Stage 71 — run a host-supplied list of Actions (RunActionsCmd), as if a
// local widget had fired them. Takes the LVGL lock internally, so call it
// from any task (e.g. the host_api dispatcher). Each action runs with
// `obj = NULL`, `widget_id = ""`, `code = LV_EVENT_CLICKED` and
// `widget_value_none`.
void widget_run_actions(const touchy_Action *actions, pb_size_t actions_count);

// Stage 90 — run a list of Actions *synchronously and inline* on the
// calling task, for the trackpad's high-frequency on_move / on_scroll
// gestures. `ActionMacro`s run via `macros_run_inline` (no inter-step
// delay); `move_ctx` (may be NULL) supplies the ambient delta for `Move`
// steps with unset dx/dy. Host / device actions run as usual. The caller
// must already hold the LVGL lock (the trackpad event path does).
void widget_run_actions_inline(const touchy_Action *actions,
                               pb_size_t actions_count,
                               const MacroMoveCtx *move_ctx);
