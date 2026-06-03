// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stage 15 / 20.1 / 24.1 / 24.2 — layout-widget configuration + child placement.
//
// Two scopes:
//   * `apply_layout()` configures the LVGL layout manager (flex / grid
//     / absolute) on an LVGL container from a layout-widget
//     (`touchy_Widget` whose `which_kind` is one of the
//     `touchy_Widget_layout_*_tag` values). Used by the screen loader
//     to configure the active LVGL screen / persistent layer object,
//     and by nested layout widgets to configure their own wrapper
//     `lv_obj_t`.
//   * `apply_rect()` / `apply_grid_cell()` place an individual widget
//     within its parent container's layout, mirroring the
//     `Widget.placement` oneof.

#pragma once

#include "lvgl.h"
#include "widgets.pb.h"
#include "touchy.pb.h"

// True iff `w.which_kind` is one of the layout-widget kinds
// (LayoutAbsolute / LayoutFlex / LayoutGrid).
bool widget_is_layout(const touchy_Widget &w);

// Configure the LVGL layout manager attached to `parent` from a
// layout-widget (flex / grid / absolute). Idempotent — safe to call
// once per container build. No-op when `w` is not a layout widget.
void apply_layout(lv_obj_t *parent, const touchy_Widget &w);

// Apply `Widget.placement.rect` to `obj`. When `absolute_layout` is
// true (parent has no layout manager) we also write the x/y position;
// under a flex/grid layout the parent owns position so we only set
// the size. Zero/negative width or height becomes LV_SIZE_CONTENT.
//
// Stage 72 — cross-axis / main-axis "grow to fill" is driven by
// `Widget.grow_x` / `Widget.grow_y` (not the Rect). Under a flex
// parent the main axis maps to `lv_obj_set_flex_grow` and the cross
// axis to `lv_pct(100)`; see the field doc in widgets.proto.
void apply_rect(lv_obj_t *obj, const touchy_Widget &w, bool absolute_layout,
                const touchy_Widget *parent_layout = nullptr);

// Place `obj` in its parent grid cell using `Widget.placement.cell` (or
// the (0,0) cell with span 1 when no cell is set). All values clamp to
// non-negative; `*_span` defaults to 1. Stage 72 — `Widget.grow_x` /
// `grow_y` select per-axis LV_GRID_ALIGN_STRETCH (grow > 0) vs
// LV_GRID_ALIGN_CENTER (content-sized, the default).
void apply_grid_cell(lv_obj_t *obj, const touchy_Widget &w);
