// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stage 59 — declarative LVGL animations driven by `Widget.animations[]`.
//
// `apply_animations()` walks the repeated `Animation` field on a built
// widget, creates one `lv_anim_t` per `AnimTrack`, and starts them all
// in parallel. The supporting `AnimCtx` / `WidgetAnimations` structs are
// freed on `LV_EVENT_DELETE` so animations stop and memory is released
// when the widget is destroyed.

#pragma once

#include "lvgl.h"
#include "widgets.pb.h"

// No-op if `w.animations_count == 0`.
void apply_animations(lv_obj_t *obj, const touchy_Widget &w);
