// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stage 20.1 / 24 — per-kind widget construction.
//
// `widget_build()` is the single entry point used by the screen loader:
// it dispatches on `Widget.kind` and returns a freshly-created LVGL
// object (or nullptr for unknown/unsupported kinds). The dispatched
// builders do _not_ apply styles or layout — those are handled by the
// caller in screens.cpp after build returns, so the same code path can
// style every widget uniformly.

#pragma once

#include "lvgl.h"
#include "widgets.pb.h"

// Dispatch on `w.which_kind` and return a new LVGL object parented at
// `parent`, or nullptr if the kind is unknown / cannot be built (in
// which case the caller should skip the widget — apply_styles &
// apply_*layout are also skipped automatically when the return is null).
lv_obj_t *widget_build(lv_obj_t *parent, const touchy_Widget &w);

// Build every child of `container` (a layout widget — see
// `widget_is_layout`) into `parent`. Picks the right `Layout.children`
// list off `container.which_kind`, then for each child runs the full
// build → apply_styles → apply_placement → optional center sequence.
// Recursively descends into nested layout-widget children. No-op if
// `container` is not a layout widget. Caller must hold the LVGL lock.
void widget_build_children(lv_obj_t *parent, const touchy_Widget &container);

// Build (or rebuild) one LVGL layer from a decoded `touchy_Widget`.
// When the root is a layout widget we configure `parent`'s layout
// manager and instantiate its children directly into `parent`. When it
// is a leaf widget it becomes a child of `parent`. For an empty root
// (which_kind == 0) the call is a no-op.
// `parent` is typically an lv_screen or one of LVGL's persistent layer
// objects. Caller must hold the LVGL lock.
void widget_build_layer(lv_obj_t *parent, const touchy_Widget &root);

// ---------------------------------------------------------------------------
// Stage 54 — WidgetRef indirection.
//
// When a screen's widget tree contains a `Widget.widget_ref` node, the
// builder reads the referenced file (`{drive}:host/widgets/<name>.pb`)
// at build time and splices the decoded widget inline. The decoded
// holders must outlive the LVGL tree (their heap arrays back action
// slots, action steps, etc.), so the builder parks them in a
// "pending" vector while building and the screen loader commits them
// alongside the new active screen.
//
// Workflow (called by screens.cpp::load_decoded with LVGL lock held):
//   widget_refs_reset_pending();   // before any widget_build_layer call
//   ...widget_build_layer(...) calls accumulate refs into pending...
//   widget_refs_commit();          // after old screen is freed,
//                                  // alongside `g_active_screen` swap.
// On failure, simply call `widget_refs_reset_pending()` again — the
// pending vector is destroyed and nothing is exposed.
// ---------------------------------------------------------------------------

void widget_refs_reset_pending();
void widget_refs_commit();

// Stage 55 — number of currently-active WidgetRef holders and access
// to their decoded paths. Used by `screens_notify_file_changed` to
// decide whether a file overwrite affects the active screen via the
// indirection layer. `widget_refs_active_path(i)` returns the
// drive-prefixed path of the `i`-th active ref (the same string that
// appeared in the parent screen's `widget_ref.path` field).
size_t widget_refs_active_count();
const char *widget_refs_active_path(size_t i);

// ---------------------------------------------------------------------------
// Stage 57 — in-RAM WidgetRef rebinding.
//
// `widget_refs_current_path(target_id)` returns the path currently
// bound to the active outermost WidgetRef whose outer `Widget.id`
// equals `target_id`, or nullptr if no such ref is active.
//
// `widget_refs_change(target_id, new_path)` rebuilds that ref's
// subtree in place: it tears down the old LVGL objects, mutates the
// outer widget's `widget_ref.path` to `new_path`, and reruns the
// builder under the original parent with the original placement. The
// change is RAM-only — no file is rewritten. Returns false if no
// matching ref is active or the rebuild fails (in which case the
// caller should expect the old subtree to be gone; the slot will
// simply be empty until the next screen reload). Caller must hold
// the LVGL lock.
// ---------------------------------------------------------------------------

const char *widget_refs_current_path(const char *target_id);
bool        widget_refs_change(const char *target_id, const char *new_path);

// ---------------------------------------------------------------------------
// Stage 60 — image binding registry.
//
// Every `Image` widget and every configured `ImageButton` slot built
// into the active screen registers the wire path it tracks. When a
// file is overwritten via FileOpenWrite/Write/Close,
// `widget_image_registry_notify()` re-applies that file's bytes to
// every matching lv_image in place — no screen rebuild, so a button
// currently receiving a touch keeps its LVGL state machine intact and
// still emits its RELEASE / PRESS_LOST event when the finger lifts.
//
// Returns true if at least one binding matched the path; false if no
// active widget references it (in which case the caller can fall back
// to checking widget_refs / screen-pb paths).
//
// Caller must hold the LVGL lock.
// ---------------------------------------------------------------------------

bool widget_image_registry_notify(const char *wire_path);
