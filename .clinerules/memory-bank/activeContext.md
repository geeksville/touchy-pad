# Active Context — current focus

**Last updated:** 2026-09-28 (memory bank initialized from a clean checkout).

## Where the project is right now

* `main` @ `40a133e` ("feat(justfile): add hw-543 recipe for jc4827w543 devboard
  setup"), `VERSION` `0.3.4` / build `21`, tree clean except the new
  `.clinerules/` directory created by this memory-bank init.
* Host suite green: **241 passed, 2 skipped** via `just app-test` (~20 s, no
  hardware needed). The 2 skips are `test_touchydeck.py` cases gated on
  `register_controllers_factory` being unavailable.
* Firmware was **not** rebuilt in this session (no board attached). Treat
  `just firmware-build` as unverified-here; CI is host-only too.
* No in-flight feature work is pending in the working tree — this is a
  documentation/metadata state, not a code state.

## Recent changes (last ~20 commits, newest first)

| Commit | Summary |
|---|---|
| `40a133e` | `justfile`: add `hw-543` recipe for jc4827w543 devboard setup |
| `214ce47` | fix(firmware): pin `esp_lvgl_port` below 2.9.0 and track `dependencies.lock` |
| `eba0cad` | merge PR #19 (jc4827w543 GT911 init fix) |
| `5f3b02d` | bump `cryptography` 45.0.7→50.0.1, fix `test-interactive` command |
| `78540fc` | jc4827w543: fix intermittent GT911 init crash from a floating INT pin |
| `5e5d515` | "bounce smaller shapes" — touch-less fallback animation polish |
| `9678fd3` | don't create USB HID endpoints on devices with no touch |
| `8e39524` | add `esp32_s3_matrix` support (LED matrix board) |
| `38dec84` | also accept JSON in addition to protobufs (Stage lb13) |
| `e302b4a` | dynamic property setting works — `touchy property set welcome text "i like cats"` (lb12) |
| `fdf0430` | add a two-panel vertically tiled layout (lb10) |
| `7fa3f19` | resolution-independent animations (lb11) |
| `206115f` | multi-panel support (lb10) |
| `e37848f` | **Stage 100** — StreamController plugin works again (image cache + user screen) |
| `cad6d66` | mTLS HTTPS works (lb9) |
| `81803e4` | WiFi instructions (lb8) |

Themes: the `lb*` stage line (multi-transport host API → LED board config →
`Display` seam → WiFi/HTTP → mTLS → LED chains → resolution-independent
animation → live property overrides → JSON endpoint) plus board-support fixes
(GT911 INT pin, HID-less devices, S3 matrix).

## Next steps (ordered by cost/benefit)

1. **Fix the two documentation drifts** (cheap; prevents future confusion):
   * `docs/design.md` Stage 100 → mark DONE (it is implemented in `e37848f`:
     `api/image_cache.py`, `Touchy.set_image_button_slot`,
     `screens.set_image_button_slot_action`, rewritten `touchydeck/`, and
     `app/tests/test_image_cache.py` all exist and pass).
   * `AGENTS.md` → drop/replace the `Screen.Version.CURRENT == 5` claim; the
     version lives on the root `Widget` (currently 28).
2. **CYD touch bring-up** (blocking real use of the two classic-ESP32 boards):
   determine the XPT2046 MISO / touch wiring with a multimeter, finish
   calibration, then update `docs/hardware.md`.
3. **Un-rot `firmware/README.md`** (still claims an LVGL v8 pin, HID-only USB
   with no CDC, and lists only 4 boards) and refresh the CYD sections of
   `docs/hardware.md`.
4. Pick up `docs/TODO.md`: system-info/Steam pages for autopage, the user-widget
   roadmap (`docs/user-widgets.md`), StreamController support, hardware guide.
5. **Memory-bank habit:** after each landed stage, update `progress.md`
   (status + version tables) and `activeContext.md` (focus + next steps), and
   record new version bumps / reversals in `systemPatterns.md` + `progress.md`.

## Active decisions & considerations

* Wire-format versions must stay in sync across three schemas —
  `Widget` 28, `ProtocolVersion` 13, `PreferencesFile` 9. Adding an enum value
  to an existing action payload does **not** need a `ProtocolVersion` bump;
  adding a command does.
* Backwards-compat policy: new gesture/feature behaviour must be **opt-in via
  field presence** (a master-enable field) and **additive** (never suppress
  existing behaviour). This is why Stages 90-95 did not change the default
  trackpad's behaviour.
* The device stays a renderer; user-facing features land as proto + host DSL
  changes, with firmware additions only where physically necessary.
* Simulator changes must mirror every wire-visible feature (including
  `setProperty` WARN+OK and `T:` drive semantics), and new sim behaviour needs a
  pytest.
* USB protocol work must never assume libusb exists (Windows CI).
* `just`-only for builds/tests; **no auto-commit/push** — the user commits.
* Docs are part of the deliverable: a stage isn't done until `docs/design.md`
  (and `AGENTS.md` when boards/architecture change) reflect it.

## Preferences, patterns and insights (carry forward)

* **Always read the relevant `docs/design.md` stage section before changing
  anything** — it holds the rationale, the locked decisions and the acceptance
  criteria reviewers will check.
* Keep `AGENTS.md`/`CLAUDE.md` truthful: it's the highest-traffic document for
  humans and agents, and it currently carries at least one stale claim.
* Regenerate, never hand-edit: `app/src/touchy_pad/_proto/*`,
  `firmware/main/proto/*`, `firmware/main/default_screen_pb.h`,
  `proto/default_screen.json`. `_proto/*` is not committed, so a fresh checkout
  needs `just build-proto` (every `app-*` recipe does it for you).
* Test with `just app-test`: it builds the proto bindings first, so it catches
  schema/generator mismatches a bare `pytest` would miss.
* High-frequency device paths (gesture frames) stay allocation-free and
  integer-only — the swipe angle test precomputes a `tanf`→fixed-point scalar at
  construction precisely to avoid float trig per frame.
* Every gesture fire logs a one-line `ESP_LOGI` (`swipe …`, `zoom …`, `twist …`)
  — that's how these features are validated on hardware, since the unit tests
  can't observe them.
* Adding a board: `target` + `sdkconfig.defaults` + `board/` component; reuse
  `cyd_common/` or `boards/common/` when it fits, then
  `just firmware-reconfigure <board> && just firmware-build`.
* Invariants to preserve: one framing decoder per side, drive-prefixed paths,
  `PbMessage<T>` RAII, `lvgl_port_lock` around LVGL calls from other tasks,
  `logging.getLogger(__name__)` with child loggers for noisy RPC traces.
* Documentation is dense and mostly accurate — prefer reading
  `docs/design.md` + `AGENTS.md` over re-deriving behaviour from code, but
  verify claims that look version-specific (see the two drifts above).
