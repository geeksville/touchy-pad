# Open / Known Issues

## Stage 59 — Animated object leaves draw artifacts on LVGL slider's inactive track

**Symptom:** When the Stage-59 `reddot` spacer animates across the "test"
demo page, small black/dark-grey vertical stripes are left behind on the
**right (inactive) portion of the `level` slider** after the dot passes
through. No artifacts appear on any other widget. The effect looks like
partial alpha-blending: the darker inactive-track region of the slider is
visually corrupted, while the brighter active-track region on the left is
unaffected.

**Root cause hypothesis:** LVGL's `lv_arc` / `lv_slider` widget draws its
inactive track with a partially-transparent (semi-opaque) grey overlay on
top of the background. When `lv_obj_set_local_style_prop` changes
`LV_STYLE_X` (or `LV_STYLE_WIDTH`) on the animated object, LVGL marks the
animated object's **new** bounding box as dirty but may not correctly
re-invalidate the **old** bounding box through the slider's child widgets.
The slider's inactive track is drawn with `bg_opa < 255`, so when only the
dirty region is repainted the blending accumulates — the slider region is
re-composited onto whatever is already in the draw buffer rather than being
repainted from scratch over a clean background. The artefact only manifests
where a semi-transparent draw primitive (the inactive track) overlaps the
animated object's previous position.

**What was tried:**
- Moving `lv_obj_remove_style_all()` into `build_spacer()` to stop a local
  `bg_opa=LV_OPA_TRANSP` style from overriding the user-applied `bg_color`
  style (this fixed the dot being invisible; unrelated to the artifact).
- Considered calling `lv_obj_invalidate(ctx->obj)` **before**
  `lv_obj_set_local_style_prop(…)` in `anim_style_exec_cb` to cover the
  old bounding box, and/or invalidating the parent container to force a
  full repaint of the overlapping region. Neither was implemented before
  work was paused.

**Suggested fix directions:**
1. In `anim_style_exec_cb` (`firmware/main/widgets/widget_animations.cpp`),
   add `lv_obj_invalidate(ctx->obj)` **before** the style-property change
   for geometry props (`LV_STYLE_X`, `LV_STYLE_Y`, `LV_STYLE_WIDTH`,
   `LV_STYLE_HEIGHT`). This forces the old area to be marked dirty before
   the object moves.
2. If (1) is insufficient (because LVGL's layer-order repaint still
   blends the slider's semi-transparent track on a stale buffer), also
   call `lv_obj_invalidate(lv_obj_get_parent(ctx->obj))` to force the
   entire parent container to repaint from scratch each frame.  This is
   heavier but eliminates any stale-buffer compositing.
3. Longer term: switch geometry-property animation from
   `lv_obj_set_local_style_prop` to direct setters (`lv_obj_set_x`,
   `lv_obj_set_width`, …), which go through LVGL's position-update path
   and correctly invalidate both old and new areas.

**Severity:** Minor cosmetic — only visible where a semi-transparent widget
overlaps the animated object's path.  No functional impact.
