# Plan: host-driven repaint latency (LVGL timer wake-ups)

**Status: DONE — both phases landed** (Stage lb16, `docs/design.md`). Phase 1 =
property batches, Phase 2 = the general bring-up registration. The file is kept
as the record of the diagnosis and the reasoning behind the choices.

## Goal / summary

Make a host-driven UI change appear on the panel *promptly*, instead of at the
next LVGL slice the device happens to wake up for.

This started as a bug report against `tools/googly-vr`: it pushed a
`SetPropertiesCmd` batch at ~10 fps and the eyes moved at ~2 fps. The batches
were received and applied correctly — they were just not drawn.

## Root cause (measured in the pinned sources)

On an idle panel **every** LVGL timer is paused, and a host-side invalidation
cannot wake the LVGL task:

1. `lv_display_refr_timer()` pauses its own refresh timer at the top of each
   pass (`lvgl/src/core/lv_refr.c`) and only a new invalidation un-pauses it.
2. Interrupt-driven touch panels (every GT911 board here has an INT line)
   are put in `LV_INDEV_MODE_EVENT` by `lvgl_port_add_touch()`, and LVGL pauses
   the indev read timer after each read (`lvgl/src/indev/lv_indev.c`) — touches
   arrive via `lvgl_port_task_wake(LVGL_PORT_EVENT_TOUCH)`.
3. So `lv_timer_handler()` sees no un-paused timer, returns
   `LV_NO_TIMER_READY`, and esp_lvgl_port substitutes its `task_max_sleep_ms`
   (**500 ms**, `ESP_LVGL_PORT_INIT_CONFIG()`) — the LVGL task sleeps up to half
   a second per pass.
4. An invalidation *does* un-pause the refresh timer, but `lv_timer_resume()`
   only notifies the port through `lv_timer_handler_set_resume_cb()`, and
   **esp_lvgl_port never registers one**. Nothing wakes the sleeping task.

A touch repaints fine (the interrupt wakes the task), which is why this went
unnoticed; only host-driven updates were affected.

## Implementation

### Phase 1 — property batches (Stage lb16) — DONE

`firmware/main/widgets/widget_property.cpp`:
`widget_property_set_batch()` tracks whether an entry landed on a widget that is
currently on screen (`find_active()` hit + `apply_to_obj()` success) and, if so:

* `lv_obj_invalidate()` the widget (also un-pauses the refresh timer),
* `lv_timer_resume()` + `lv_timer_ready()` the display's refresh timer so the
  pass is due now rather than `period` ms after the last one,
* after releasing the port lock, `lvgl_port_task_wake(LVGL_PORT_EVENT_USER, nullptr)`.

Scope notes: no wake for a batch that changes nothing visible (staging overrides
for an unloaded screen, or pure removals — those affect the *next* build).
Deliberately **not** `lv_refr_now()` from the dispatcher: that would render and
flush (~15-25 ms) on the host-API task, delay RPCs and the Stage 64.1
log/event poll, and busy-wait on `disp->flushing` (no `flush_wait_cb` is set).
No wire change, no `ProtocolVersion` bump, no simulator mirror (the sim has no
LVGL, no refresh timer and no task).


### Phase 2 — register LVGL's timer-resume callback once at bring-up — DONE

One registration at display bring-up fixes the class of bug, not just the one
call site — landed in `firmware/main/display.cpp::Display::post_init()`:

```cpp
// after hw_init() brought the LVGL port up (shared by every board + headless)
lv_timer_handler_set_resume_cb(
    [](void *) { lvgl_port_task_wake(LVGL_PORT_EVENT_USER, nullptr); }, nullptr);
```

Then any invalidation from any task wakes the LVGL task, bounding *all*
host-driven repaints by the 33 ms `LV_DEF_REFR_PERIOD` instead of 500 ms — a
`Screen_Load`, a `FileWrite`+`FileClose` image rewrite, `Run_Actions`, etc.

**Why it is not in Phase 1:** it is a global behaviour change for every host and
every board (more LVGL-task wake-ups; any repaint now happens at the refresh
period rather than lazily). It was kept as a separate decision on purpose and
took its own verdict — hence the two commits. It is also where
`Display::post_init()` (previously an empty `#if 0` stub) finally gets a real job
for all boards at once; a board overriding it must call the base first (noted in
`display.h`).

**Checked before landing:** `lv_timer_handler_set_resume_cb()` is public API in
the pinned LVGL (`src/misc/lv_timer.h`), and the callback is invoked from a
task/port-lock context (via `lv_inv_area`), so touching an event group is safe.
Setting the same event bit twice (callback + Phase 1's explicit wake) is free.

## Files & components

| Side | File | Phase |
|---|---|---|
| `firmware/main/` | `widgets/widget_property.{h,cpp}` | 1 (done) |
| `firmware/main/` | `display.cpp` (`Display::post_init()`) + `display.h` comment | 2 (done) |
| docs | `docs/design.md` (stage section), `AGENTS.md`, `docs/host-api.md` | 1 + 2 (done) |

No `proto/`, `app/` or `rust/` change in either phase — this is device-render
timing, invisible on the wire.

## Testing strategy

Firmware has no unit tests, so:

* `just firmware-build` green (Phase 1 confirmed; run again for Phase 2).
* Hardware: googly-vr against a real pad — eyes must move at the host's rate
  (`--period 100`), not in ~2 Hz steps. A useful instrument is the built-in
  `ForceRender` checkbox / an `fps_widget` screen: with Phase 1 the fps should
  follow the batch rate; with Phase 2, a host `Screen_Load` should also be
  visibly quicker than the old ~500 ms worst case.
* Regression: an idle panel must still sleep its task (no busy loop) — watch
  that CPU idle does not collapse when nothing is animating; touch and
  `ScreenWakeCmd` behaviour must be unchanged.
* Simulator: nothing to mirror (no LVGL in the sim), and no host test can
  observe device-side refresh timing.

## Risks / open questions

* **Wake-up churn (Phase 2).** Every invalidation wakes the LVGL task; a host
  that invalidates at 100+ Hz would keep the task busy. Mitigated by the fact
  that rendering still happens at the refresh period and `lv_throttled` gates
  the heavy paths.
* **Is 500 ms ever *desirable*?** It is the port's idle-sleep setting. Phase 2
  does not remove it (the task still sleeps when nothing invalidates); it only
  stops it from delaying a repaint that is already pending.
* **Other idle-panel consequences** of the "every timer is paused" state (e.g.
  LVGL animations started from a host command) are out of scope here but share
  the same root cause; if a host ever drives `AnimTrack`s, re-check this plan.
