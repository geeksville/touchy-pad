// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stage lb24 — rich text via LVGL span groups.
// See docs/plans/span-rich-text.md.
//
// Two entry points, one implementation:
//
//   * `build_span_group()` — the explicit `Widget.kind.span_group` kind:
//     per-run text colour and font, `overflow`, `max_lines`, `text_align`.
//
//   * `build_recolor_span_group()` — the compatibility path for
//     `Label.recolor`. The wire format (`#RRGGBB text#` in `Label.text`, and the
//     same markup in a runtime `text` override) is unchanged; what changed is
//     the engine. LVGL's own in-string recolour parser resets its command state
//     on every wrapped line and every draw pass, so a coloured run that straddles
//     an automatic line break is mis-measured (the line overruns) and drawn twice
//     (the seam glyphs overlap). Rendering the same markup as a span group has no
//     in-string command state at all, so the defect cannot occur.
//
// Both paths mark the object they create, so a runtime `text` property override
// — the one-RPC-per-frame live path — re-parses the markup into the group's span
// list (`span_group_apply_text_override`, called from widget_property.cpp).

#pragma once

#include "lvgl.h"
#include "touchy.pb.h"
#include "widgets.pb.h"

// Build the LVGL object for `Widget.kind.span_group`. Returns nullptr when
// LV_USE_SPAN is off (a build without LVGL's span widget), so callers can fall
// back to a plain label instead of rendering nothing.
lv_obj_t *build_span_group(lv_obj_t *parent, const touchy_Widget &w);

// Build the span group that implements `Label.recolor`. The label's `text` is
// LVGL recolour markup and is parsed into spans here; `text_align` carries over
// via the shared style path, and `long_mode` selects the overflow behaviour
// (WRAP / CLIP -> clip, DOTS -> ellipsis). Returns nullptr when a plain label
// must be used instead: either LV_USE_SPAN is off, or the long mode is
// SCROLL / SCROLL_CIRCULAR (a span group cannot marquee — and an unwrapped
// scroll line is not affected by the LVGL parser defect anyway).
lv_obj_t *build_recolor_span_group(lv_obj_t *parent, const touchy_Widget &w);

// Re-parse `markup` into `group`'s span list, replacing whatever was there.
// Text outside a `#RRGGBB …#` run inherits the group's own text colour, matching
// LVGL's label-recolour semantics. An opener needs exactly six hex digits
// followed by a space (the space is consumed); an unmatched `#` stays literal.
void span_group_set_markup(lv_obj_t *group, const char *markup);

// Handle a `text` property override addressed at one of our markup-backed span
// groups: "re-parse this markup into the span list". Returns true when the entry
// was ours — i.e. it named `text` with a string value on a span group we built —
// so widget_property.cpp can treat it as applied. Returns false for any other
// object or property, leaving the normal LVGL property path in charge.
bool span_group_apply_text_override(lv_obj_t *obj,
                                   const touchy_SetPropertyCmd &cmd);
