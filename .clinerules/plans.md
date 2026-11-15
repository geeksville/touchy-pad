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

* `docs/plans/rust-ai-consumer-docs.md` — **documentation aimed at AI consumers**
  (**done — Stage lb23**): Rust has no AI-specific doc standard, so the work was
  to put usage recommendations where they are *served*. Adds
  `rust/touchy-pad/examples/` (compiled by `cargo test`, shipped in the crate),
  a "For AI agents" block in the crate docs, `# Errors` sections on every public
  fallible API (enforced by `clippy::missing_errors_doc`), `#[doc(alias)]` on the
  entry points, docs.rs built with the `serial` feature, and a repo
  `CHANGELOG.md`. No wire change; plan records why `doc(cfg)`/`missing_panics_doc`
  were left out.

* `docs/plans/crates-io-publishing.md` — **crates.io-ready Rust crate**
  (**done — Stage lb22**): the prost bindings are **checked in**
  (`rust/touchy-pad/src/proto/touchy.rs`, emitted by the new `publish = false`
  `rust/proto-gen` member, wired into `just rust-gen-proto` / `just build-proto`
  and drift-checked in `app-ci.yml`), so `touchy-pad`'s build script and its
  `prost-build`/`protoc` requirements are **gone** (a consumer builds the
  packaged crate with `PROTOC=/nonexistent`); `include` now ships `tests/**`; and
  the browse-only `tools/autopage` submodule was removed — its nested
  `autopage-recipes` pin was dead, which broke cargo `git =` deps and recursive
  clones. No wire change (`proto/*.proto` untouched); `release.yml` now publishes
  the crate on every GitHub release (token secret, or crates.io trusted
  publishing when that secret is absent).

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

* `docs/plans/widget-ref-property-overrides.md` — **done — Stage lb21**:
  `widget_refs_change()` never promoted the property registry after an in-place
  `WidgetRef` swap, so `set_properties` missed pages shown by
  `show_user_screen()` and `s_active` kept dangling pointers to deleted LVGL
  objects. Fix: an `LV_EVENT_DELETE` hook per identified widget (LVGL recurses
  into children) + `widget_property_build_merge()` (merge, never replace) called
  from `widget_refs_change()`, plus registering the swapped page's resolved root
  (a leaf-root page was never addressable). Firmware-only, no wire change;
  on-hardware check still to run.
* `docs/plans/fs-write-transaction-recovery.md` — **done — Stage lb20**: a
  `FileCloseCmd` with the wrong handle cleared the global write registry but left
  `FlashFs`'s `g_txn` set, so every later `F:` write failed with `IO_ERROR`
  until reset. Fix: `Fs::abortWrite()` (+ overrides), called on a handle
  mismatch, from `fs_abort_open_transaction()`, and before every `openWrite()`
  so an already-wedged board self-heals. Firmware-only, no wire change; replaces
  the reporter's host-side `R:` fallback. On-hardware repro still to run.

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