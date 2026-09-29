# Progress — what works, what's left, decision history

**Snapshot (2026-09-29):** `main` @ `22d3f97` ("try cline" — the
user committed the memory bank), `VERSION` `0.3.4`/build `21`. Host suite:
**259 passed, 2 skipped** (`just app-test`, ~20 s); `tools/googly-vr`'s own
suite **23 passed** (`just test`) with `just lint` clean; `just firmware-build`
green for `jc4827w543`. This session implemented **googly-vr stage 5** (loopback
OSC + lazy/never-fatal pad search), the parent-repo **Stage lb15** (host
property writes reset the display auto-off timer) and a **USB enumeration fix**
(a running app never saw a re-plugged pad) — see the sections below.

### Documentation/consistency fixes in this session (2026-09-28)

* `docs/design.md` Stage 100 — status flipped from "planned/in-progress" to
  **DONE** with a "What was implemented" section (it shipped in `e37848f`).
* `AGENTS.md` — dropped the stale `Screen.Version.CURRENT == 5` claim (no such
  enum since Stage 56; the version is on the root `Widget`) and replaced the
  7-board intro list with all 13 board dirs.
* `docs/python-api.md` — fixed three broken examples: `Screen.Version.CURRENT`
  / `Screen(name=…)` in the raw-protobuf example (`screen_save` also needs
  `name=` for a raw `Screen`), `screen_load("home")` → path-based loads, and
  `pad.run_actions(...)` → `pad.client.run_actions(...)` /
  `pad.show_user_screen(...)` (there is no `Touchy.run_actions`).
* `docs/hardware.md` — CYD 2.8"/2.4" sections no longer say "Not yet
  supported"; the Elecrow S3 section uses the current UART-link flags instead
  of the retired `CONFIG_TOUCHY_PROTO_OVER_SERIAL`; the P4 section no longer
  claims WiFi is unimplemented (it is; this board just has no usable radio).
* `docs/hardware/lightbar/design.md` — removed obsolete "firmware not compiled
  on real ESP-IDF" caveats (LB1/LB2/lb6/lb8/lb10), corrected lb2's stale
  `firmware/main/leds/` + `esp32-s3-devkitc-1` paths, dropped lb8's removed
  `tls_psk` flags, and turned lb9's leftover second "Status: planning" block
  into rationale prose.
* `firmware/README.md` — rewritten end to end: accurate 13-board table (with
  the CYD touch caveat and the parked P4 LED board), current architecture
  (`Display` ABC, `api/`+`widgets/`+`fs/`+`net/`, `boards/common/`), `just`
  recipes first, console/logging truth (log tunnel, CDC off by default), and
  current known issues.
* `firmware/main/api/host_api.h` — comments updated to the three real
  transport flags (`VENDORUSB`/`CDCACM`/`UART`) instead of the retired
  `CONFIG_TOUCHY_PROTO_OVER_SERIAL`.
* **Real bug fixed:** `app/src/touchy_pad/cli.py` imported
  `api._transponrt_serial` (typo), so `touchy --port <dev>` raised
  `ModuleNotFoundError` every time. Now `_transport_serial`, covered by the new
  `app/tests/test_cli.py` (the test fails with the typo and passes with the
  fix).

## Wire-format versions in force

`Widget.Version.CURRENT == 28` · `SysBoardInfoResponse.ProtocolVersion.CURRENT == 14`
· `PreferencesFile.Version.CURRENT == 9` · (no `Screen.Version` exists — the
version rides the root `Widget`).

## What works (shipped, all marked DONE in `docs/design.md`)

**Foundation:** 0–24.4 (HID mouse, protobufs, custom USB protocol, filesystem,
host-driven screens, button behaviours, nanopb cleanup, trackpad widget, backlight
auto-sleep, images, styles/transitions/transform, image scale/rotation, layers,
layout cleanup, animations).

**Core 50–60:** 50.1 StreamDeck reverse-engineering tool, 50.2 `TouchyDeck` shim,
51 filesystem cleanup (drive-prefixed paths), 52 image mmap, 53 native image
formats, 54 widget-in-file (`widget_ref`), 55 minimal redraws, 56 widget tweak,
57 `ActionChangeWidgetRef` (replacing `ActionSwitchScreen`), 58 assigned
VID/PID `0x303A/0x8369`, 59 animation improvements, 60 streamdeck-probe +
image registry.

**Hosts & transport 61–72:** 61 Rust API (`touchy-pad` crate), 62 OpenDeck plugin,
63 wire-accurate simulator, 64.1 device-log tunnel (`LogRecord` in the
`EventConsume` response), 64.2 CDC-ACM optional, 64.3 framed protocol over
serial/TCP (one decoder per side), 64.4 follow-ups, 65 CYD `esp32_2432s028rv3`
support + `platform_get()` capabilities, 65.1 `esp32_2432s024` + shared
`cyd_common/`, 67 inline `host_action(on_event=…)` callbacks with auto codes
(`0x10000`+), 68 screen-switch cleanup (`host/s/`, default chrome
`host/s/default.pb`, `host/uscr/` + `widget_ref(id="page")`), 70 OpenDeck
enumeration fix, 71 user-screen repaint + unified enumeration + device serials +
`RunActionsCmd`, 72 opt-in `grow_x`/`grow_y` sizing.

**Features 80–95:** 80 GIF support, 81 board-info RAM/flash numbers, 82 partial
preferences (`SetPreferencesCmd` + `min_log_level`/`boot_delay_s`), 83 (per
design.md), 84 SQUiXL board, 85 Rust image cache, 86 in-place `ImageButton` slot
repaint, 87 dynamic images + `T:` transient drive, 88 MaTouch 4.3", 90 trackpad
gestures as Actions (`on_left/right/middle_click`, `on_move`, `on_scroll`),
91 single-finger swipes, 92 pinch zoom, 93 twist/rotate + HID Consumer-Control
report + `consumer_key`, 94 unified PWM backlight + `backlight_level` pref,
95 press-and-hold / hold-release (`tap_time`, new `tap_distance`).

**`lb*` line (recent):** lb5 multi-interface host API (vendor-USB + CDC-ACM +
UART links, "last used wins"), lb6 runtime LED-panel config via `BoardConfig`,
lb7 `Display` class seam (`display_create()` per board + `HeadlessDisplay`),
lb8 protobuf API over WiFi HTTP (+ mDNS, sim server on 8083), lb9 mutual-TLS
security (`touchy pref provision-mtls`, HTTPS on 443, plaintext skipped when
certs present), lb10 tiled LED panel chains + per-panel wiring flags
(`PanelChain`, `tile_by_row`, snaking/major/flip flags), lb11
resolution-independent animations (`AnimTrack.start_inverted`/`end_inverted`,
touch-less fallback rewritten around it), lb12 runtime widget property overrides
(`SetPropertyCmd`, session-scoped + sticky), lb13 protobuf-JSON on the network
endpoint, **lb14 batched `SetPropertiesCmd` *replacing* `set_property`**
(wire break, `ProtocolVersion` 14; sim honours a documented property subset
`x`/`y`/`w`/`h`/`bg_color`/`text` at the proto level — sticky + lossless
removal), driven by the new `tools/googly-vr` submodule (animated googly
eyes: `sim-eyes` OSC broadcaster + `googly-vr` renderer, ~10 fps, one
coalesced batch per frame; plan in `tools/googly-vr/docs/plans/general.md`), and
**lb15 host property writes reset the display auto-off timer** (one
`backlight_wake()` in `host_api.cpp`'s `set_properties` case — no wire change,
no sim mirror; motivated by googly-vr's animation blanking the panel).

### googly-vr stage 5 + Stage lb15 (2026-09-29)

Plan first (`tools/googly-vr/docs/plans/stage5.md`), then implemented and
verified:

* **googly-vr `--host` is loopback-only by default** (`127.0.0.1`; `0.0.0.0` is
  the explicit opt-in) — the OSC socket drives what the panel shows, so it
  should not be LAN-reachable by accident.
* **The render loop is event-driven and never exits on its own.**
  `_Hub` carries a `threading.Event` set by every OSC update; the loop parks in
  `hub.wait()` and only *tries* to connect to a touchy-pad as a side effect of
  an update (`_PadSearch`, rate-limited to one attempt / 5 s). A missing,
  unplugged, headless (`0x0`) or incompatible-firmware pad is a retry reason
  printed at most once per 5 s — never fatal — so a pad attached later is picked
  up; ctrl-c is the only exit. `--period` (100 ms) still caps frames at ~10 fps
  even if a real tracker pushes 100+ Hz. Idle cost = the OSC thread's blocked
  `recvfrom` + one parked `Event.wait()` (no timer).
* **Tests:** new `tests/test_cli.py` (10 cases) covering the loopback default,
  "no device I/O while OSC is silent", the 5 s rate limit, a pad appearing
  later, headless/incompatible never exiting, a mid-run frame failure closing
  the pad and restarting the search, the `--period` ceiling, ctrl-c, and the
  real `_Hub` wakeup. `tools/googly-vr`: `just test` = **23 passed**,
  `just lint` clean.
* **Parent repo:** `just app-test` = **251 passed, 2 skipped** (unchanged —
  no Python/Rust surface was touched); `just firmware-build` **green** for
  jc4827w543, with `host_api.cpp` recompiled clean.
* **Still open:** the on-hardware confirmation that the panel stays lit while
  googly-vr animates a 5 s auto-off timeout (needs a board).

### USB enumeration fix (2026-09-29) — "a running app never sees a re-plugged pad"

* Reported by the user testing googly-vr's new reconnect loop: `just run` works
  if the pad is attached at launch, but a pad attached later (or after an
  unplug/re-plug) is never found — `No Touchy-Pad device with VID=0x303a
  PID=0x8369 found`, forever — despite `dmesg` and a fresh process seeing it.
* **Root cause, measured**: libusb **1.0.27** caches the device list **per
  libusb context**, and pyusb keeps **one context per process**
  (`usb.backend.libusb1.get_backend()` is a module-level singleton). Proof from
  the new diagnostic (`just usb-diag`), same instant: `this process: total=21
  touchy=NOT FOUND` vs `fresh process: total=22 touchy=['005/036']`.
* **Fix**: `app/src/touchy_pad/_usb.py::find_usb_devices()` — first enumerate
  through the process-wide context (unchanged fast path), and only when that is
  empty retry once with a **fresh** libusb context (`libusb_init`), pinning that
  context to the returned devices so it can't be collected while a handle is
  open. Used by `UsbTransport` (so `touchy_open`/CLI/`TouchyDeck`),
  `touchy_get_pad_ids` and `_bootloader_visible`'s non-Linux fallback. 8 new
  tests in `app/tests/test_usb_find.py` (pure fakes; Windows/macOS CI safe).
* **Environment wrinkle (documented, no code change):** the dev container's
  `/dev/bus/usb` is captured at container start, so a later-attached pad may
  have a node only under `/host/dev/bus/usb` — handled by the pre-existing
  `_install_host_dev_fallback()` (verified: with no container node at all,
  `touchy_open()` still worked; libusb_open fails `NO_DEVICE`=19, already in its
  retry list — an added `NOT_FOUND` guess was measured to be wrong and reverted).
* **New tool**: `bin/usb-diag.py` + `just usb-diag` — prints the live sysfs view
  (with the device's node in `/dev/bus/usb` *and* `/host/dev/bus/usb`), this
  process's raw pyusb enumeration, a fresh process's, what the shipping
  `find_usb_devices` sees, and an open+board-info probe.
* Docs: `docs/open-issues.md` (FIXED write-up), `AGENTS.md` (never call bare
  `usb.core.find` in long-lived code).
Note a **real jc4827w543 device is attached** to this devcontainer running
protocol-13 firmware — it needs a re-flash before the batched commands work
on hardware.
POST endpoint (`Content-Type` selects JSON vs binary; `bin/set-property.sh`).

**Stage 100 — Python image cache + TouchyDeck user-screen port: implemented**
(commit `e37848f` "update StreamController plugin to work again (stage 100)";
`api/image_cache.py`, `Touchy.set_image_button_slot`,
`screens.set_image_button_slot_action`, rewritten `touchydeck/layout.py` &
`deck.py`, `app/tests/test_image_cache.py`) even though `docs/design.md` still
labels it *"planned/in-progress"*.

## What's left / next candidates

From `docs/TODO.md` (open items):

* Fix touch on the really cheap CYD boards (XPT2046 MISO pin unknown — needs a
  multimeter; both CYD boards ship "implemented but unverified on hardware").
* Autopage + Steam background + system-info pages (CPU %, GPU %, temp, frame
  rate, graph).
* Finish the [user widgets](../../docs/user-widgets.md) roadmap (host-rendered dynamic
  content).
* Built-in StreamController support (probably via the mock-device proof of
  concept).
* A "which hardware / why" guide; change the recommended board in
  `hardware.md`; explain 3D-printing/knob plans; write a hackster.io article;
  post to r/esp32.
* Alpha-n wishlist: StreamDeck-like knobs/dials with gesture overlays;
  gesture-based screen switching instead of top buttons; multitouch HID
  digitizer; user-screen "builder" wizard; USB security model (restrict
  untrusted hosts changing macro behaviour); no-touch device support; more
  boards; e-ink; stylus/paintbrush; WiFi/TCP API for signage; haptics; tactile
  overlay/case; auto-generated per-app pages; possibly skip StreamController and
  go straight from YAML → Python helper; StreamDeck background-graphic API;
  IO_MUX SPI routing to try 80 MHz panel clocks.

Deferred by design (from stage docs):

* Stage 100 follow-up: wire `set_brightness` to the Stage-94 backlight pref
  (today it stays best-effort wake/sleep only).
* Stage 63's remaining simulator-fidelity gaps.
* Tuning constants for swipe/zoom/twist/hold are validated on
  `jc4827w543`-class hardware only.

## Known issues

1. **Animated object leaves artifacts on a slider's inactive track**
   (`docs/open-issues.md`, Stage 59). Semi-transparent slider inactive track +
   `lv_obj_set_local_style_prop` geometry animation = stale-buffer blending.
   Candidate fixes: `lv_obj_invalidate(ctx->obj)` before the style change, or
   invalidate the parent, or switch to direct setters. Cosmetic; no functional
   impact. **Still open.**
2. **CYD touch unverified** — the resistive XPT2046 MISO GPIO is unconfirmed
   (`docs/hardware.md` says 39 was tried; needs ohm-ing out), so touch is not
   proven on either CYD board.
3. **Simulator serves plaintext HTTP only** (port 8083); real mTLS exists in
   firmware only.
4. **JSON network API is intentionally partial** — `setProperties`,
   `sysBoardInfoGet`, `screenWake`, `getPreferences`, `sysRebootBootloader`,
   `eventConsume` only; `setPreferences`/`runActions` are protobuf-only; an
   unknown command key returns HTTP 400.
5. **`jc_esp32p4_m3` (Guition P4 LED-matrix board) doesn't boot** — PSRAM init
   fails before `app_main` (loops on MSPI DQS phase); parked. Use
   `esp32_s3_devkitc_1` (same LED stack, mainstream silicon) instead.
6. **Windows CI has no libusb** — discovery paths must degrade to a clear error
   (`NoBackendError`), not a traceback.

*(Resolved 2026-09-28: the Stage-100 status drift, the `Screen.Version` claim,
the stale `firmware/README.md`/`docs/hardware.md` sections and the
`touchy --port` import typo were all fixed — see the "Documentation/consistency
fixes" section above. Firmware/board docs still to refresh: the CYD sections of
`firmware/README.md`'s table are accurate, but `docs/hardware.md` keeps a lot
of historical CYD pin-hunting detail that is only useful for the pending
multimeter work.)*

## Evolution of key decisions (reversals worth remembering)

| Decision today | What it replaced / why |
|---|---|
| ESP-IDF + CMake | PlatformIO (Stage 0 tooling) — abandoned early |
| LVGL **v9** | v8.4 (kept for v1 parity, long since bumped) |
| `grow_x`/`grow_y` opt-in sizing | implicit child stretching / implicit COLUMN cross-fill (Stage 72) |
| protobuf screens | LVGL XML (`docs/why-not-xml.md` still explains the reasoning) |
| Drive-prefixed paths `F:`/`R:`/`T:` | unprefixed/rebased paths (Stage 51, `T:` added Stage 87) |
| Three independent transport Kconfig flags | single `CONFIG_TOUCHY_PROTO_OVER_SERIAL` tri-state (Stage LB5) |
| mTLS certs | TLS-PSK (abandoned: IDF 6.0.2's `esp_https_server` doesn't expose `psk_hint_key`; `tls_psk_key` tag reserved) |
| `SetPreferencesCmd` (partial merge) | per-setting `ScreenLoadCmd`/`ScreenSleepTimeoutCmd` (Stage 82) |
| `Move` (dx/dy optional) | `MouseMove` (+wheel) — Stage 90, so macros can consume ambient deltas |
| `ImageButton` slot swap | Stage 85's stub-rewrite repaint (deleted the held button, lost keyUp, raced) |
| `tap_time` / `tap_distance` | `tap_max_ms` / `TAP_MAX_MOVE` (Stage 95; pure renames + a new field) |
| Content-addressed image caches on `T:` | per-key full file rewrites + whole-grid reload (Stages 85–87, 100) |
| `host/s/default.pb` chrome + `host/uscr/` pages | per-screen standalone grids + `ScreenLoadCmd` ping-pong (Stages 68, 100) |
| One default screen built by `proto/gen_default_screen.py` | hand-maintained C fallback screen (Stage 68) |
| `Display` ABC + `display_create()` | free `extern "C" display_init()` + `main.cpp` special cases (Stage lb7) |
