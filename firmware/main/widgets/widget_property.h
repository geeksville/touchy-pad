// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stage lb12 — runtime widget property overrides.
//
// A host `SetPropertyCmd` overrides one LVGL property (by name or raw
// lv_prop_id_t) on the widget whose `Widget.id` matches `widget_id`.
// Overrides are session-scoped (RAM only, never persisted) and *sticky*:
// they re-apply whenever a widget with that id is (re)built, and apply
// immediately if the widget is already on screen. A `SetPropertyCmd` whose
// `value` oneof is unset removes the override for that (widget, property).
//
// Thread-safety: every entry point touches LVGL state and the shared
// tables under the LVGL port lock. `widget_property_set()` acquires the
// lock itself (it is called from the host_api dispatcher task); the
// build-time hooks are called from the screen loader, which already holds
// the lock.

#pragma once

#include "lvgl.h"
#include "touchy.pb.h"

// Apply (or, when the value oneof is unset, remove) a session override
// from a host SetPropertyCmd. If a widget with `cmd.widget_id` is currently
// on screen the change is applied immediately; otherwise it is remembered
// and applied when that widget is next built. Returns false only when the
// command is malformed (no property name/id) or the property/value could
// not be applied to a live widget — never because the widget is absent.
bool widget_property_set(const touchy_SetPropertyCmd &cmd);

// Screen-build hooks (called under the LVGL lock by the screen loader):
//   * widget_property_build_reset()   — start a fresh pending id→obj map.
//   * widget_property_register(id,obj) — record a built widget and apply
//     any matching sticky override to it right away.
//   * widget_property_build_commit()  — promote the pending map to active
//     once the new screen has replaced the old.
void widget_property_build_reset();
void widget_property_register(const char *id, lv_obj_t *obj);
void widget_property_build_commit();
