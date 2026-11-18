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

* `docs/plans/host-desync-recovery.md` — **done — host library (Python + Rust),
  no wire change**: the companion to Stage lb20, reported by voicebot. A host
  hard-killed mid-RPC leaves a reply in the device FIFO, so the *next* RPC reads
  it — `FileOpenWrite` then saw a stale `NOT_FOUND` (raising, and leaking a write
  transaction because the handle was opened outside `file_save()`'s cleanup
  `try`), and `SysBoardInfoGet` saw a foreign `OK` that parsed as a phantom 0×0
  board. Fix: `file_save()` flushes + retries once on `NOT_FOUND`/a payload-less
  `OK` (`_DesyncedReply`), `file_open_write()` validates its reply,
  `touchy_open()` flushes + retries a default board reply via
  `_read_board_info()`, `_write_lock` serialises whole write sequences, and the
  Rust `file_write_raw()` gets the same flush+retry (`Client::flush_stale_frames`).
  On-hardware 20×`kill -9` loop still to run.

* `docs/plans/span-rich-text.md` — **done — Stage lb24**: a voicebot-reported
  **LVGL** defect — the label recolour parser resets per line / per draw pass, so
  a `#RRGGBB …#` run that straddles a wrapping break loses its colour, swallows
  the next word and draws the seam twice (LVGL #4599 / #8547). **`Label.recolor`
  kept its exact wire/API** and the *firmware* stopped calling
  `lv_label_set_recolor()`: `build_label` now delegates to
  `build_recolor_span_group()` (`firmware/main/widgets/widget_span.{h,cpp}`),
  which builds an LVGL **spangroup** with the markup parsed device-side (so the
  live `set_properties(id, "text", markup)` path kept working, unchanged and
  one-RPC-per-frame). New `Span` / `SpanGroup` + `Widget.kind.span_group = 34` +
  `span()` / `span_group()` / `parse_recolor_markup()` are the recommended
  rich-text API; `SCROLL` / `SCROLL_CIRCULAR` labels deliberately keep the old
  path (unaffected, and a spangroup cannot marquee). `Widget.Version` 30 → 31;
  new demo/regression page `pages/span.py` (page cycle is now span → test →
  trackpad). Verified: 324 host tests, `just app-lint`, `just firmware-build`,
  plus an end-to-end sim run with a live override; **on-hardware wrap check still
  to run**. Detailed notes: `docs/hardware/lightbar/design.md` → "stage lb24".

* `docs/plans/thought-bubbles.md` — **proposed (decision pending)** — chat-style
  "thought bubbles" for voicebot: a rounded rectangle with **three rounded corners and one
  sharp one**, text centred. Today nothing in the stack can express it — LVGL 9.5 has a single
  uniform `LV_STYLE_RADIUS` and `Style.radius` is the only wire knob — so the plan records the
  three zero-firmware workarounds that work right now (rounded `button` + corner-covering
  `spacer`; a host-rendered bubble image + a `label`; four-rounded-corners) and proposes
  **Route A**: per-corner radii on `Style` (`radius_tl/tr/br/bl`) drawn by a new
  `widget_shape.cpp` composite (union of 2 bands + 4 corner discs, with a 2-task fast path for
  one sharp corner), `LayoutFlex.main_align/cross_align` (`lv_obj_set_flex_align`) so a bubble
  can centre its text vertically, two local-style fixes in `widget_builders.cpp` (layout
  containers and labels currently force local styles that beat host styles), a host-side
  `bubble()` DSL helper, a Qt per-corner-radius sim mirror, and `Widget.Version`
  31 → 32 (stage **`lb25`** — renumbered: lb24 was taken by `span-rich-text.md`,
  which landed 2026-11-18).
  Alternatives Route B (a `Bubble` widget kind) / Route C (a bubble-capable `Label`) are
  documented with their trade-offs (firmware-owned text loses the `set_properties("text")`
  path; bespoke vertical text centring).

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