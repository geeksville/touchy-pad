# Progress — what works, what's left, decision history

**Snapshot (2026-09-28):** `main` @ `40a133e`, `VERSION` `0.3.4`/build `21`,
working tree clean (only `.clinerules/` untracked). Host test suite:
**241 passed, 2 skipped** (`just app-test`). Firmware not rebuilt this session.

## Wire-format versions in force

`Widget.Version.CURRENT == 28` · `SysBoardInfoResponse.ProtocolVersion.CURRENT == 13`
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
4. **JSON network API is intentionally partial** — `setProperty`,
   `sysBoardInfoGet`, `screenWake`, `getPreferences`, `sysRebootBootloader`,
   `eventConsume` only; `setPreferences`/`runActions` are protobuf-only; an
   unknown command key returns HTTP 400.
5. **Docs drift found 2026-09-28:** (a) `docs/design.md` Stage 100 says
   "planned/in-progress" although it is implemented; (b) `AGENTS.md` claims
   `Screen.Version.CURRENT == 5`, but no such enum exists in `proto/` any more —
   the version lives on the root `Widget`. Neither is a code bug, but both
   mislead contributors/agents. Fixing them is cheap and recommended.
6. **`firmware/README.md` is partly stale** (LVGL-v8 pin, HID-only/no-CDC
   description, 4-board table) versus the 13 board dirs that exist today;
   `docs/hardware.md` is also behind for the CYD boards.
7. **Windows CI has no libusb** — discovery paths must degrade to a clear error
   (`NoBackendError`), not a traceback.

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
