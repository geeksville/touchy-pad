# Open / Known Issues

## FIXED (2026-09-29) — a running host app never sees a re-plugged Touchy-Pad

**Symptom:** `googly-vr` (and any other long-lived client) connects fine if the
pad is attached when it starts, but after an unplug/re-plug — or if the pad was
*not* attached at start — it reports `No Touchy-Pad device with VID=0x303a
PID=0x8369 found` forever, even though `dmesg`/sysfs show the device and a fresh
`touchy …` process finds it immediately.

**Root cause (measured, libusb 1.0.27):** libusb caches the device list **per
libusb context**, and pyusb keeps exactly **one context per process**
(`usb.backend.libusb1.get_backend()` is a module-level singleton). A process
whose first enumeration happened before the device appeared therefore never sees
it again. Proven with `just usb-diag` while re-plugging:

```
sysfs(live): 5-1.1.4.3 -> node 005/036 (/dev:yes, /host/dev:yes)
this process:  total=21 touchy=NOT FOUND      # stale cached list
fresh process: total=22 touchy=['005/036']    # new context → sees it
```

**Fix:** `touchy_pad._usb.find_usb_devices()` — enumerate through the
process-wide context first (unchanged fast path) and, only when that comes up
empty, retry once with a **fresh** libusb context, which is always current. Used
by `UsbTransport` (so `touchy_open`, the CLI, `TouchyDeck`, the OpenDeck plugin's
Python path…), by `touchy_get_pad_ids`, and by `update._bootloader_visible`'s
non-Linux fallback. The fresh context is pinned to the returned devices so it
can't be collected while a handle is open.

**Related dev-container wrinkle (not a code bug):** in the VS Code dev container
the container's `/dev/bus/usb` is built at *start-up*, so a pad attached later
can have no node there while the host's live view (`/host/dev/bus/usb`) does.
That is what `_install_host_dev_fallback()` in `api/_transport.py` exists for —
it opens the `/host/dev` node and wraps it with `libusb_wrap_sys_device()`
(pre-existing; verified working: with no container node for the pad at all,
`touchy_open()` still returns a usable transport). If the container view ever
causes trouble, `sudo mount --bind /host/dev/bus/usb /dev/bus/usb` makes it live.

**Diagnostic tool:** `bin/usb-diag.py` / `just usb-diag` (loops, prints sysfs vs
this-process vs fresh-process enumeration, node presence in both views, and an
open+board-info probe).

**Side observation (low severity, not reproduced).** During the investigation,
one `touchy_open()` in a *just-closed* previous session's wake returned
`board_info` of `0x0`/empty-serial instead of `480x272`/`ta4cb8fec1ce8` — the
signature of reading a stale frame left in the device's bulk-IN FIFO by an
abrupt close (which is what `TouchyClient.drain_pending()` exists to clear; the
`touchy_open()` path doesn't call it). Three consecutive probes seconds later,
and the CLI, all read it correctly, so it was not reproducible on demand.
Worth remembering only because it self-heals by design in the new clients: a
bad first response shows up as a retry reason (googly-vr: `reports no display
(0x0)` → retry in 5 s) rather than a crash.

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
