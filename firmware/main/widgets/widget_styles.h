// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stage 15 / 20.2 — LVGL style + transition application.
//
// `apply_styles()` reads the `touchy.Style` messages attached to a
// `touchy.Widget`, builds matching `lv_style_t` instances on the heap,
// attaches them to the LVGL object with `lv_obj_add_style`, and arranges
// for their cleanup on `LV_EVENT_DELETE`.
//
// Split out of screens.cpp during the Stage 24 refactor.

#pragma once

#include "lvgl.h"
#include "widgets.pb.h"

#include <cstdint>

// LVGL colour from packed 0x00RRGGBB. Shared with the widget builders
// (e.g. trackpad colour configuration) so it lives in the styles
// header rather than in a private translation-unit copy.
static inline lv_color_t color_from_u32(uint32_t rgb)
{
    return lv_color_make((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

// Walk `w.styles[]`, instantiate `lv_style_t`s + optional
// `lv_style_transition_dsc_t`s, attach them to `obj`, and register a
// destroy callback that releases everything on widget deletion.
//
// Safe to call with a widget that has no styles — it's a no-op.
void apply_styles(lv_obj_t *obj, const touchy_Widget &w);

// Stage 59 — wire-stable enum → LVGL translators, exposed for reuse by
// `widget_animations.cpp`. Unknown values fall back sensibly
// (`LV_STYLE_PROP_INV` / `lv_anim_path_linear`).
lv_style_prop_t   lv_prop_from_proto(touchy_StyleProp p);
lv_anim_path_cb_t lv_path_from_proto(touchy_AnimPath  p);
