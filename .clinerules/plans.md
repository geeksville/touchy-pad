# Plans

Implementation and design plans live in `docs/plans/*.md` and are tracked in git.
(Note the plural `docs/` — this is the project's *documentation* tree, not the
mkdocs site: `app/mkdocs.yml` builds the published Python API docs from
`app/docs/`, so plans written here are never published.)

## Rules

- When I create or edit a plan, I MUST save it as `docs/plans/<topic>.md`
  (kebab-case, e.g. `trackpad-gestures.md`). Create the directory if it does
  not exist yet.
- Always persist plans to that directory so they can eventually be committed —
  never leave a plan only in chat or in a temporary location.
- One plan per file. Update the existing file for the topic instead of creating
  a near-duplicate; use dated filenames only when an explicit supersession is
  intended.
- Plans are implementation-oriented. Each should include: goal/summary,
  approach, library/technology choices with rationale, file & component layout,
  a phased implementation sequence, testing strategy, and risks/open questions.
- Touchy-Pad specifics to cover in a plan: **which side(s)** it touches
  (`firmware/main/`, `app/src/touchy_pad/`, `rust/`, `proto/`), and whether it
  moves a wire version (`Widget.Version`, `SysBoardInfoResponse.ProtocolVersion`,
  `PreferencesFile.Version`) or is additive — plus the `just` recipes
  (`just build-proto`, `just app-test`, `just firmware-build`) that validate it.
- When a plan drives active work, reference it from the memory bank
  (`activeContext.md`).
- Do not delete a plan; mark it superseded and link the replacement.

## Existing plans

* `docs/plans/label-scroll-speed.md` — **custom label scrolling speed**
  (**done — Stage lb19**): `Label.scroll_duration_ms = 6` (ms/lap, applied
  verbatim) **and** `Label.scroll_speed_px_s = 7` (px/s 10…10000, resolved by
  LVGL against the real text length) → `lv_obj_set_style_anim_duration()` in
  `build_label`; `Widget.Version` 29→30. Host: `label(...,
  scroll_duration_ms=)` / `label(..., scroll_speed_px_s=)` +
  `SCROLL_SPEED_MIN/MAX_PX_S`, and `touchy property set … --int` so the runtime
  override path can retime a live label (`anim_duration`). LVGL's default
  (**40 px/s**, lap clamped to 300 ms…10 s, 300 ms bounce pause at each end) is
  now documented in `docs/python-api.md` → "Scrolling speed". No `rust/` or
  simulator change. Full notes in §9 of the plan.

* `docs/plans/host-driven-repaint-latency.md` — LVGL refresh wake-ups: **Stage
  lb16** — phase 1 (property batches repaint immediately) + phase 2 (the general
  fix: `lv_timer_handler_set_resume_cb()` registered at display bring-up, so
  *every* host-driven repaint is bounded by the 33 ms refresh period instead of
  the port's 500 ms idle sleep). Both **done**.
* `docs/plans/led-brightness.md` — LED-matrix brightness: reuse the Stage-94
  `backlight_level` pref to dim the whole surface (mechanism already landed via
  the `LEDChain` brightness scalar, which scales images too), and make LED
  boards default to **10 %** when the pref is unset. **Done — Stage lb18** (the
  LED default landed; `platform_is_led_matrix()` + `Prefs` explicit-flag).

Docs that are *not* plans but are easy to confuse with them:

| Doc | What it is |
|-----|------------|
| `docs/design.md` | Authoritative stage-by-stage history — "what stage are we on". Update it when a stage finishes. |
| `docs/TODO.md` | Loose feature backlog / wish list (checkboxes), not a design. |
| `docs/open-issues.md` | Known bugs and rough edges. |
| `docs/*.md` | Shipped user/developer docs (`host-api.md`, `ui.md`, `simulator.md`, …). |
| `.clinerules/memory-bank/` | Cline's cross-session context — not project documentation. |

When a plan is added, list it here with a one-line summary and cross-reference
it from `activeContext.md`; when it is superseded, keep the file and link the
replacement rather than deleting it.