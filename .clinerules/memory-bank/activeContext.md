# Active Context — current focus

**Last updated:** 2026-09-29 (googly-vr stage 5 + Stage lb15 + the USB
enumeration fix — "a running app never sees a re-plugged pad").

## Where the project is right now

* `main` @ `22d3f97` ("try cline" — the user committed the memory bank after it
  was initialized), `VERSION` `0.3.4` / build `21`.
* Host suite green: **259 passed, 2 skipped** via `just app-test` (~20 s, no
  hardware needed). The 2 skips are `test_touchydeck.py` cases gated on
  `register_controllers_factory`. `tools/googly-vr` is green too (**23 passed**
  via its own `just test`, `just lint` clean).

### The USB enumeration bug (2026-09-29, reported by the user against googly-vr)

* **Symptom:** a long-lived host app (`googly-vr`) connected fine when the pad
  was attached at launch, but after an unplug/re-plug — or if the pad was
  attached *after* launch — it reported `No Touchy-Pad device with VID=0x303a
  PID=0x8369 found` forever, while `dmesg`/sysfs showed the device and a fresh
  process found it instantly.
* **Root cause (measured with the new `just usb-diag` on libusb 1.0.27):**
  libusb caches the device list **per libusb context**, and pyusb keeps **one
  context per process** (`usb.backend.libusb1.get_backend()` is a module-level
  singleton). The diag output was decisive — same moment, same pad:
  `this process: total=21 touchy=NOT FOUND` vs
  `fresh process: total=22 touchy=['005/036']`.
* **Fix:** new shared helper `app/src/touchy_pad/_usb.py::find_usb_devices()`
  (enumerate via the process-wide context; only if that is empty, retry once
  with a **fresh** libusb context — pinning it to the returned devices so it
  isn't collected while a handle is open). Wired into `api/_transport.py`
  (`UsbTransport` → all of `touchy_open`, the CLI, `TouchyDeck`…),
  `api/device.py::touchy_get_pad_ids`, and `update.py::_bootloader_visible`'s
  non-Linux fallback. New tests: `app/tests/test_usb_find.py` (8 cases, pure
  fakes → Windows/macOS CI safe).
* **Second, environment-level wrinkle (documented, not a code bug):** the dev
  container's `/dev/bus/usb` is a start-up snapshot, so a later-attached pad can
  have a node only under `/host/dev/bus/usb`. The pre-existing
  `_install_host_dev_fallback()` handles it (verified: with **no** container node
  for the pad, `touchy_open()` still returned a usable transport — libusb_open
  fails `NO_DEVICE` (19), which its retry list already covers). I briefly added
  `NOT_FOUND` to that list on a wrong premise and reverted it after measuring
  the actual errno (19, not 2).
* **New tooling:** `bin/usb-diag.py` + a `just usb-diag` recipe — loops,
  printing (a) the live sysfs view with that device's node in
  `/dev/bus/usb` **and** `/host/dev/bus/usb`, (b) this process's raw pyusb
  enumeration, (c) a **fresh** process's, (d) what the shipping
  `find_usb_devices` sees, and (e) an open+board-info probe. That (b)-vs-(d)
  contrast is what makes the caching bug visible at a glance.
* **Docs:** `docs/open-issues.md` (full FIXED write-up + a low-severity
  side-observation about one stale-frame read), `AGENTS.md` (gotcha bullet:
  never call bare `usb.core.find` in long-lived code).
* **Still to confirm by the user:** the end-to-end replug recovery of
  `just run` with the fix in place (the mechanism is proven live; the app-level
  confirmation needs hands on hardware).
* **This session (2026-09-29): `tools/googly-vr` stage 5** — plan written first
  (`tools/googly-vr/docs/plans/stage5.md`), then implemented:
  * **`src/googly_vr/cli.py` rewritten around an event-driven loop.** `--host`
    now defaults to `127.0.0.1` (loopback only; `0.0.0.0` is the explicit
    opt-in). `_Hub` gained a `threading.Event` so the loop *parks* on an OSC
    update; `_open_pad()` / `_PadSearch` / `_render_loop` replace the old 10 Hz
    `time.sleep` loop, and a touchy-pad connection is attempted **only as a side
    effect of an update** (rate-limited to one attempt / 5 s). Nothing is fatal —
    no pad, a headless (`0x0`) pad or an incompatible firmware are retry reasons
    printed at most once per 5 s; the process exits only on ctrl-c, so a pad
    attached later is picked up. `--period` still caps frames at ~10 fps.
  * **`tests/test_cli.py` (new, 10 cases)** — loopback default + `--host`
    override, no device I/O while OSC is silent, 5 s rate limiting, a pad
    appearing later, headless/incompatible pads never exiting, frame failure
    closing the pad and restarting the search, the `--period` ceiling, ctrl-c,
    and the real `_Hub` wakeup. Seams: `cli._Hub`/`cli.connect`/
    `cli.EyeRenderer` are patched and time is injected (`cli._now`,
    `_PadSearch(clock=)`).
  * Docs: `README.md` (quickstart + commands table) and `docs/plans/general.md`
    (Stage-2 bind wording, no longer "0.0.0.0 so EyeTrackVR is drop-in").
* **This session: parent-repo Stage lb15** (the plan's item 3, motivated by
  googly-vr): `firmware/main/api/host_api.cpp`'s `set_properties` case now calls
  `backlight_wake()` when the batch is non-empty, so a host animating the panel
  is display *activity* and no longer blanks the display mid-animation
  (`docs/design.md` Stage lb15, `AGENTS.md`, `docs/host-api.md`,
  `docs/python-api.md`). **No wire change** (versions stay 28 / 14 / 9) and no
  simulator mirror (the sim has no backlight/auto-off). `just firmware-build`
  **green** — `host_api.cpp` recompiled clean after the edit.
* Uncommitted work in the tree: this session's googly-vr + firmware + docs
  changes, plus the earlier (2026-09-28) doc sweep / `touchy --port` fix and the
  `.clinerules/` files. Nothing has been committed (never auto-commit) — the
  user decides.

### Earlier session (2026-09-28) — doc sweep + a real CLI bug fix

* docs consistency sweep — `AGENTS.md`, `docs/design.md` (Stage 100 → DONE),
  `docs/hardware.md`, `docs/python-api.md`,
  `docs/hardware/lightbar/design.md`, `firmware/README.md` (full rewrite),
  `firmware/main/api/host_api.h` comments.
* a **real bug fix**: `app/src/touchy_pad/cli.py` imported a misspelled
  module (`api._transponrt_serial`), breaking `touchy --port <dev>`
  entirely; plus the new regression suite `app/tests/test_cli.py`.
* `.clinerules/` — four rule files copied in from the *starbash* project and
  adapted to this repo's layout: `collaboration.md` (the no-commit boundary
  now names the `tools/StreamController` submodule + `tools/streamdeck-probe`
  / `rust/`), `plans.md` (**plans live in `docs/plans/`**, created by the
  first plan), `searching.md` (semble MCP `repo` paths are relative to `app/`,
  so the firmware is `../firmware/main`), and `terminal.md` (which
  `just`/`touchy` commands block and why). `memory-bank.md` and
  `memory-bank/` were already repo-specific and are unchanged. Every `repo`
  example in `searching.md` was re-verified against the live MCP server.
* That session did not rebuild the firmware; **this** session did
  (`just firmware-build` green for jc4827w543 after the lb15 edit).

## Recent changes (newest first)

| Commit | Summary |
|---|---|
| `22d3f97` | "try cline" — commits the initialized `.clinerules/memory-bank/` |
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

1. **Review/commit the uncommitted work** (see "Where the project is right
   now"): this session's googly-vr stage 5 (`cli.py` + `tests/test_cli.py` +
   README/general.md in the submodule) and the parent-repo Stage lb15
   (`host_api.cpp` one-liner + four doc files), on top of the 2026-09-28 doc
   sweep and the `touchy --port` fix.
2. **CYD touch bring-up** (blocking real use of the two classic-ESP32 boards):
   buzz out the XPT2046 MISO GPIO with a multimeter, set it in
   `firmware/boards/esp32_2432s028rv3/board/board_pins.h` (and the 2.4"
   sibling), calibrate, then trim the historical pin-hunting prose out of
   `docs/hardware.md`.
3. **`jc_esp32p4_m3` PSRAM bring-up** — the Guition P4 LED board loops in MSPI
   DQS phase selection before `app_main`; needs scope/silicon-level debugging
   (its board README documents the log).
4. Pick up `docs/TODO.md`: system-info/Steam pages for autopage, the user-widget
   roadmap (`docs/user-widgets.md`), StreamController support, hardware guide.
5. **googly-vr (`tools/googly-vr/` submodule): stages 0–2 + E1/E2 landed earlier;
   stage 5 (loopback OSC + lazy/never-fatal pad search + `--period` cap) is
   IMPLEMENTED this session.** Remaining: (a) the **on-hardware** check for
   Stage lb15 — `touchy pref backlight-timeout 5`, run googly-vr against
   `sim-eyes`, and confirm the panel stays lit while the eyes move and still
   blanks ~5 s after the batches stop; (b) flash the attached jc4827w543 with
   protocol-14 firmware; (c) the plan's E3 (device-side lerp) stays speculative;
   (d) future stages = pupil-size shaping + the real EyeTrackVR hookup.
6. **Memory-bank habit:** after each landed stage, update `progress.md`
   (status + version tables) and `activeContext.md` (focus + next steps), and
   record new version bumps / reversals in `systemPatterns.md` + `progress.md`.
   Also re-check that `AGENTS.md`/`CLAUDE.md`, `firmware/README.md` and the
   board tables in `docs/hardware.md` still match reality — they were the
   sources of the drift fixed this session.

## Active decisions & considerations

* Wire-format versions must stay in sync across three schemas —
  `Widget` 28, `ProtocolVersion` 14, `PreferencesFile` 9. Adding an enum value
  to an existing action payload does **not** need a `ProtocolVersion` bump;
  adding a command does — and since lb14, *replacing* a command (the
  `set_properties` wire break) bumps it too, per the googly-vr owner's
  explicit no-backwards-compat-for-now stance.
* Backwards-compat policy: new gesture/feature behaviour must be **opt-in via
  field presence** (a master-enable field) and **additive** (never suppress
  existing behaviour). This is why Stages 90-95 did not change the default
  trackpad's behaviour.
* The device stays a renderer; user-facing features land as proto + host DSL
  changes, with firmware additions only where physically necessary.
* Simulator changes must mirror every wire-visible feature (lb14: the sim
  now *implements* `set_properties` for the documented subset
  `x`/`y`/`w`/`h`/`bg_color`/`text` at the proto level — sticky across
  reloads, lossless removal, WARN+OK for anything else), and new sim
  behaviour needs a pytest.
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
