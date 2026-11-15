# Property overrides break after an in-place WidgetRef swap

**Status:** open — found during a fork-review (jellydn), reproduced by reading the
code. Firmware-only fix, **being implemented upstream by the maintainer** (no PR
requested from the reporter).

## Goal / summary

`Touchy.set_properties()` stops reaching widgets that live inside a page swapped
in by `show_user_screen()` / `ActionChangeWidgetRef`. Concretely: save a user
page whose body holds `label(id="clock")`, `show_user_screen("clock")`, then
`set_properties([build_property_override("clock", "text", "...")])` — the RPC
returns OK and nothing changes on the panel. Rewriting the `.pb` file works,
which is the workaround that hides the bug.

**Claim from the reporter, confirmed:** `widget_refs_change()` rebuilds the
page subtree but never calls `widget_property_build_commit()`. The new ids stay
in `s_pending` (so `find_active()` cannot see them) and `s_active` keeps
pointers to the LVGL objects the swap just deleted.

## Root cause (verified)

* `firmware/main/widgets/widget_builders.cpp:1359` `widget_refs_change()` is the
  Stage-57 in-place rebind used by both `ActionChangeWidgetRef`
  (`show_user_screen`) and the Stage-85 fast path
  `widget_refs_rebuild_by_path()` (`notify_file_changed`, i.e. any `.pb`/image
  rewrite).
* It calls `widget_refs_reset_pending()` (line 1457) →
  `widget_property_build_reset()` clears `s_pending`; the rebuild re-registers
  every widget id through `widget_property_register()`; then lines 1500-1505
  splice `g_pending_refs` into `g_active_refs` and return.
* **`widget_property_build_commit()` is called from exactly one place —
  `widget_refs_commit()` at line 1336.** Grep confirms no other caller. So for
  a partial rebuild:
  1. `s_pending` holds the swapped-in ids and is never promoted →
     `find_active()` misses them → a post-swap `set_property` silently no-ops
     (`widget_property.cpp:163`, nothing logged at INFO).
  2. `s_active` still holds `IdObj`s for objects destroyed by
     `lv_obj_delete()` / `lv_obj_clean()` inside the swap → `find_active()` can
     return a **dangling pointer** → `lv_obj_set_property()` /
     `lv_obj_invalidate()` on freed memory = latent use-after-free. It worsens
     with every page switch.

Why the workaround "works": `widget_property_register()` re-applies stored
overrides at *build* time, so any rebuild picks them up. Only the
set-after-the-swap path is broken. That also means the reporter's clock (rewrite
the page every second) only half-works — and pre-Stage-lb16 it was additionally
throttled to the port's 500 ms idle sleep.

**Trap:** the naive fix — just calling `widget_property_build_commit()` at the
end of `widget_refs_change()` — is wrong. That function does
`s_active = std::move(s_pending)`, i.e. it would *replace* the whole registry
with just the swapped subtree and drop every other live widget (the chrome's
prev/next buttons, other `WidgetRef`s). The merge has to mirror what
`widget_refs_change()` already does for `g_active_refs`.

## Approach

1. **Self-healing deletion.** In `widget_property_register()`, register an
   `LV_EVENT_DELETE` callback (once per object) that erases that object from
   `s_active` and `s_pending`. This removes the dangling-pointer class of bug
   for *every* teardown path (screen reload, ref swap, widget delete) instead of
   only the ref-change one, and it removes the need to replicate the Stage-85
   `descendant_idx` subtree walk in a second place.
2. **Merge, don't replace.** Add `widget_property_build_merge()` next to
   `widget_property_build_commit()` in
   `firmware/main/widgets/widget_property.{h,cpp}`: for each `s_pending` entry,
   drop any `s_active` entry with the same `id` (last write wins, so a rebuilt
   widget's newer object beats its dead predecessor) and push the pending entry,
   then `s_pending.clear()`. Keep `widget_property_build_commit()` as-is for the
   full screen load (`widget_refs_commit()`).
3. **Call it at the end of `widget_refs_change()`**, right after the
   `g_pending_refs` splice (line ~1503), next to the existing
   `g_pending_refs.clear()` / `g_ref_expansion.clear()`.
4. No wire change, no proto change. Nothing host-side.

## Files

| File | Change |
|---|---|
| `firmware/main/widgets/widget_property.cpp` | `s_active`/`s_pending` delete hook; `widget_property_build_merge()` |
| `firmware/main/widgets/widget_property.h` | declare `widget_property_build_merge()` + document the partial-rebuild contract |
| `firmware/main/widgets/widget_builders.cpp` | call the merge at the end of `widget_refs_change()` |

## Phases

1. Merge + delete hook (the functional fix).
2. Regression note in `docs/design.md` (a note under lb12/lb14, or a new stage
   entry) so the registry contract is written down.

## Testing strategy

Firmware has no unit tests, so:

* `just build-proto` + `just firmware-build` (default board `jc4827w543`).
* Hardware/scripted check: user page with `label(id="clock")` →
  `show_user_screen("clock")` → `set_properties(text)` every 200 ms → the label
  updates live **without** rewriting the `.pb`.
* Override-before-swap still works (sticky path through
  `widget_property_register()`).
* Churn test: alternate `show_user_screen` between two pages ~500× while
  setting a property on each — no reboot/crash (proves the dangling pointer is
  gone; pre-fix this is a UAF).
* Chrome widgets (prev/next) keep responding to their own overrides after a
  page swap (proves the merge did not replace the registry).

## Risks / open questions

* One extra LVGL event callback per *identified* widget. If that cost shows up
  in profiling, the alternative is to collect descendants before the teardown
  (the Stage-85 walk) — correct but easy to forget in the next teardown path.
* `Widget.id` is not guaranteed unique across a screen. The merge's
  last-write-wins rule matches the current `find_active()` "first match" only
  loosely; worth documenting.
* Does `lv_obj_delete()` fire `LV_EVENT_DELETE` for children, or only for the
  object being deleted? Verify against the vendored LVGL v9 before relying on
  the hook alone (if children do not fire it, keep collect-before-delete as a
  belt and braces).