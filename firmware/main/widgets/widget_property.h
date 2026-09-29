// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stage lb12 — runtime widget property overrides. googly-vr / stage lb14
// batched them into `SetPropertiesCmd`.
//
// A host `SetPropertyCmd` (one entry of a `SetPropertiesCmd` batch)
// overrides one LVGL property (by name or raw lv_prop_id_t) on the widget
// whose `Widget.id` matches `widget_id`. Overrides are session-scoped (RAM
// only, never persisted) and *sticky*: they re-apply whenever a widget
// with that id is (re)built, and apply immediately if the widget is already
// on screen. An entry whose `value` oneof is unset removes the override
// for that (widget, property).
//
// Thread-safety: every entry point touches LVGL state and the shared
// tables under the LVGL port lock. `widget_property_set_batch()` acquires
// the lock itself (it is called from the host_api dispatcher task) and
// applies every entry under that one acquisition; the build-time hooks
// are called from the screen loader, which already holds the lock.

#pragma once

#include "lvgl.h"
#include "touchy.pb.h"

// Apply (or, for entries whose value oneof is unset, remove) a batch of
// session overrides from a host SetPropertiesCmd. Entries are applied in
// order under a single LVGL port lock acquisition — one RPC, one lock.
// If a widget with an entry's `widget_id` is currently on screen the
// change is applied immediately; otherwise it is remembered and applied
// when that widget is next built. Per-entry semantics match
// `SetPropertyCmd`'s exactly. Returns false only when some entry was
// malformed (no property name/id) or its property/value could not be
// applied to a live widget — never because a widget is absent, and a
// failing entry never aborts the rest of the batch.
//
// Stage lb16: when an entry actually changed a widget that is on screen the
// batch also forces the repaint to happen *now* — it marks that widget dirty,
// makes the display refresh timer due, and wakes the LVGL task — because the
// LVGL task otherwise sleeps up to 500 ms between passes on an idle,
// interrupt-driven panel (esp_lvgl_port never registers LVGL's timer-resume
// callback, so a bare invalidation cannot wake it).
bool widget_property_set_batch(const touchy_SetPropertiesCmd &cmds);

// Screen-build hooks (called under the LVGL lock by the screen loader):
//   * widget_property_build_reset()   — start a fresh pending id→obj map.
//   * widget_property_register(id,obj) — record a built widget and apply
//     any matching sticky override to it right away.
//   * widget_property_build_commit()  — promote the pending map to active
//     once the new screen has replaced the old.
void widget_property_build_reset();
void widget_property_register(const char *id, lv_obj_t *obj);
void widget_property_build_commit();
