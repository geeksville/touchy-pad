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

## FIXED (2026-11-04) — a killed host desynchronises the next session (stale reply in the device FIFO)

**Symptom:** the *first* RPC of a fresh session dies with
`TouchyError: device returned NOT_FOUND (3)` — typically `touchy_open()`'s
`sys_board_info_get()`. In practice `voicebot listen --display pad` logged
`no touchy-pad to show this on (device returned NOT_FOUND (3))` at startup and
`found a touchy-pad but can't drive it (…)` on the next message, even though the
pad was attached and working.

**Root cause (measured on `esp32_s3_matrix`, protocol 14):** the vendor bulk pair
answers **strictly one frame per request** — the firmware queues its reply
whether or not the host ever reads it. A host that dies mid-RPC (Ctrl-C during
the event poll, a killed `voicebot`, …) therefore leaves that reply in the
device's bulk-IN FIFO, and **every later reply is one command behind**. Six
consecutive `sysBoardInfoGet` replies showed it exactly:

```
0 code: 3 payload: None bytes: 2        # ← the leftover frame
1 code: 0 payload: sys_board_info …     # ← actually the reply to command 0
2 code: 0 payload: sys_board_info …     # ← actually the reply to command 1
…
```

`TouchyClient.open()` *did* call `drain_pending()`, but that makes it worse: the
drain's own `EventConsume` reply becomes the new head, so the **next** command is
guaranteed to read a stale `NOT_FOUND`. Each *failed* `touchy_open()` also
orphans its own board-info reply, so the state persisted across sessions. (This
is the "side observation" the 2026-09-29 investigation recorded as low severity
and unreproducible — it is reproducible, and it self-perpetuates.)

**Fix (`app/src/touchy_pad/api/client.py`):**

* **`TouchyClient._flush_stale_frames()`** — called at open: read with a short
  (40 ms) timeout until the FIFO is empty, discarding whatever was left. The
  frame decoder keeps *partial* bytes across calls, so a genuine reply arriving
  mid-flush is never corrupted, and an already-clean FIFO costs one 40 ms read.
* **`TouchyClient.close()`** now takes `_rpc_lock` before closing the transport,
  so an in-flight exchange finishes (and consumes its reply) instead of
  orphaning it — that stops *new* stale frames being created on a clean close.

**Verified:** on the `esp32_s3_matrix` pad, `touchy_open()` + a full
`screen_save`/`screen_load` cycle now succeeds repeatedly (previously ~50 %, and
worse as the backlog grew). The fix is transport-agnostic and protects every host
client (the `touchy` CLI, `TouchyDeck`, googly-vr, voicebot). A host can still
create a stale frame by being hard-killed mid-RPC; `_flush_stale_frames()`
absorbs it on the next open.

**Tests:** `app/tests/test_client.py::test_flush_stale_frames_*`.

**Follow-up (2026-11-17) — the flush is not always enough, so the RPCs now
recover.** `TouchyClient.open()`'s flush uses a 40 ms read timeout, so a reply
that arrives late (device busy in LittleFS/LVGL) survives it; and `touchy_open()`
does not go through `TouchyClient.open()` at all, so nothing is flushed on that
path. A leftover then lands in front of *any* later reply:

* a leftover `NOT_FOUND` (the usual empty-`EventConsume` answer) read by
  `FileOpenWrite` raised `TouchyError: device returned NOT_FOUND (3)` even though
  the device had opened the handle — and because the open sat outside
  `file_save()`'s cleanup `try`, no `FileClose(commit=False)` was sent, leaking a
  device-side write transaction per failed attempt (`fs_open_write: stale
  transaction detected …`);
* a leftover `OK` with no payload read by `SysBoardInfoGet` parsed as the
  *default* `SysBoardInfoResponse`, so `touchy_open()` returned a phantom 0×0
  board (`protocol_version == 0`).

Fixed by making the two RPCs self-correcting: `file_save()` flushes the stale
frame(s) and retries once on `NOT_FOUND` / a payload-less `OK` (a genuine open
failure is `IO_ERROR`), `file_open_write()` rejects a reply with no handle, and
`touchy_open()` flushes + retries a default board reply (twice → a clear error).
`_flush_stale_frames()` also holds the RPC lock now, so a flush can't steal an
in-flight reply. Rust got the same flush-and-retry in
`Touchy::file_write_raw()`. Plan: `docs/plans/host-desync-recovery.md`; tests:
`app/tests/test_client.py` (`*_stale_*`) and `app/tests/test_api.py`
(`test_touchy_open_*board_info*`).

## Upstream LVGL — a label recolour run that crosses a wrapped line (worked around in stage lb24)

**Symptom (reported by `voicebot`, 2026-11, `jc4827w543`, LVGL 9.5.0):** a `label`
with `recolor = true` and a wrapping long mode (`WRAP` / `DOTS`) mis-renders the
line at a colour-run boundary when an automatic line break falls *inside* a
`#RRGGBB …#` run. The continuation line loses the run's colour, the **closing**
`#` is re-read as an *opening* command, the next word is swallowed as the colour
parameter, the line overruns the box by a word, and the seam glyphs are drawn
twice — a violet copy offset up/left plus a base-coloured copy at the baseline,
so the words collide. Text *outside* the run and lines that begin at a valid
opener are unaffected, which is why it took a while to spot.

**Root cause (LVGL, not touchy-pad):** the parser state is local to one
call/pass and is never advanced across them.

* `lv_text_get_next_line()` (`src/misc/lv_text.c`) creates
  `lv_text_cmd_state_t cmd_state = LV_TEXT_CMD_STATE_WAIT;` **per call**, while
  its caller passes `&txt[line_start]`. A line starting inside a run therefore
  starts in `WAIT`, so `lv_text_is_cmd()` treats the closing `#` as an opener:
  `#`, the next word and the following space all measure at **zero width** — and
  word wrap is all-or-nothing, so an extra word fits on the line.
* `lv_draw_label()` (`src/draw/lv_draw_label.c`) declares `recolor_cmd_state` /
  `is_first_space_after_cmd` *after* its "skip to the first visible line" loop, so
  any pass that starts at line > 0 (the `LV_LABEL_LONG_TXT_HINT` fast path a tall
  label takes, or a clipped/tiled draw) re-parses from mid-run: the seam word draws
  in the base colour, one word is dropped, the rest of the line shifts — and
  composited with a pass that started at line 0 you get the two offset copies.
* `lv_text_get_width()` has the same per-call reset, so **centre/right aligned**
  recoloured wrapped labels are affected too.

**Upstream:** the same long-standing defect as
[#4599](https://github.com/lvgl/lvgl/issues/4599) (v8) and
[#8547](https://github.com/lvgl/lvgl/issues/8547) (v9.3 — closed stale, fix PR
unmerged). `recolor` was even removed in early v9 on the grounds that it was "too
limited … can be replaced by `lv_span`", and `lv_span`/`lv_spangroup` remains
LVGL's supported rich-text path.

**Workaround (touchy-pad, stage lb24 — no upstream patch):** the firmware stopped
using LVGL's parser for wrapping labels. `build_label()` now delegates a
`recolor` label to `build_recolor_span_group()`
(`firmware/main/widgets/widget_span.{h,cpp}`), which builds an **LVGL spangroup**
and parses the very same `#RRGGBB text#` string into spans device-side — no
in-string command state to carry, so the defect cannot occur. The wire format,
the `label(recolor=True)` API and the runtime `text` override are unchanged, so
existing hosts (voicebot included) were fixed by a firmware update alone. New
code should prefer the explicit `span_group()` API. The two **scrolling** long
modes still use LVGL's parser deliberately: a span group cannot marquee, and a
scrolling label lays its text out as a single unwrapped line, where the state is
never split.

**A fix worth filing upstream:** carry the parser state across lines and draw
passes — have `lv_text_get_next_line()` / `lv_text_get_next_word()` take the
caller's `cmd_state` (or re-derive it from `txt[0..line_start)`); initialise
`recolor_cmd_state` in `lv_draw_label()` *before* the first-visible-line skip and
feed every skipped line through `lv_text_is_cmd()`; same for
`lv_text_get_width()`. Reference the closed-stale #8547 rather than opening a new
issue blind.

Full analysis, the minimal PC-simulator repro, the rejected alternatives and the
migration notes: `docs/plans/span-rich-text.md`;
`docs/hardware/lightbar/design.md` → "stage lb24".

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
