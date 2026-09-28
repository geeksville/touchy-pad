# Product Context — why touchy-pad exists

## The problem

1. **StreamDeck-class hardware is expensive and closed.** An Elgato StreamDeck
   with 15 keys costs >$100; a 32-key XL costs ~$250. Meanwhile ESP32-S3 boards
   with a capacitive multitouch LCD (e.g. the 4.3" JC4827W543) cost **$15**.
2. **Laptop touchpads are hit-or-miss**, and a desktop has no touchpad at all.
   A $15 panel can be a bigger, better, gesture-capable touchpad for
   Mac/Linux/Windows/Android — with a screen built in for on-the-fly config.
3. **Embedded displays need embedded programming.** Hobbyists who want a custom
   status panel / macro pad / signage surface normally have to write and flash
   firmware and re-flash on every change. touchy-pad inverts that: the device is
   a *dumb renderer* and the host (Python, Rust, or anything speaking
   protobuf/JSON over USB or HTTPS) owns the UI, so iterating on a screen is a
   script run, not a flash cycle.
4. **Gestures/macros must survive without a companion app.** A macro keypad
   that stops working when your helper process dies is a toy. touchy-pad
   compiles macros and gestures into on-device `Action` lists that emit real USB
   HID events.

## Who it's for

| Persona | What they get |
|---|---|
| StreamDeck/OpenDeck user | `touchy-opendeck` plugin (~$15 StreamDeck clone; per-key images cached on-device for fast repaint) |
| StreamController user | `touchy_pad.touchydeck.TouchyDeck` monkey-patch that makes touchy-pads enumerate as StreamDecks |
| Desktop user, no software | Out-of-the-box USB HID touchpad: 1/2/3-finger clicks, drag, scroll, swipe, pinch-zoom, twist-rotate, press-and-hold drag-and-drop |
| Python hobbyist | `touchy_pad.api` DSL (`screens.button()`, `trackpad()`, `slider()`, …), inline `host_action(on_event=callable)` callbacks, dynamic images (`ImageSource`) |
| Rust developer | async `touchy-pad` crate (+ `touchy-demo`), same proto surface |
| Maker / signage | WiFi + mTLS-secured `POST /touchy/api/v1/command` endpoint speaking protobuf **or** JSON, so a browser/curl/`bin/set-property.sh` can drive the panel |
| LED-matrix tinkerer | Board-config-driven chains of WS2812B matrices (`led-32x8.json`, `led-96x8-chain.json`, tiled arbitrarily) rendered by LVGL |

## How it works (user-facing mental model)

```
   host script / OpenDeck / StreamController           $15 ESP32 board
   ┌───────────────────────────────────────┐      ┌──────────────────────────┐
   │ touchy_pad.api DSL  →  Screen protobuf│ USB  │ LVGL renders the widget  │
   │ macros/actions      →  Action lists   │─────▶│ tree you sent;           │
   │ ImageCache          →  hashed assets  │      │ the user touches it;     │
   │                                       │◀─────│ events + USB HID come back│
   └───────────────────────────────────────┘      └──────────────────────────┘
```

* Upload a screen once → the device stores it under `F:host/s/*.pb`; the
  prev/next chrome (`F:host/s/default.pb`) pages through user page bodies in
  `F:host/uscr/*.pb` via a `widget_ref(id="page")`.
* Macros/gestures are `Action` lists **on** the widget, so they keep working
  with no host process attached.
* With a host app running, it can also push per-widget property overrides
  (`SetPropertyCmd`), stream device logs back over the same RPC channel, and
  swap `ImageButton` image slots in place (`ActionChangeWidgetRef`
  RELEASED/PRESSED) for flicker-free repaints.

## User-experience goals

* **"Premium feel."** Multitouch gestures must not fight each other: taps vs.
  drags vs. swipes vs. pinch vs. twist are separated by tuned
  time/distance/angle thresholds, and every gesture feature is *opt-in* (a
  master "presence" field enables it) so default behaviour is byte-for-byte
  unchanged and costs zero per-frame CPU when unused.
* **Zero-friction install.** `pip install touchy-pad && touchy update` should
  identify the board, download the right firmware and flash it (with prompts);
  `touchy init` provisions chrome + a trackpad page on a fresh device.
* **Works offline.** No account, no server. USB cable, or (optionally) your own
  WiFi network with self-signed mTLS certs you provision yourself.
* **Fast iteration loop.** `touchy --sim` / `--sim-gui` renders a screen on the
  desktop with no hardware; the sim speaks the same wire protocol as the device.
* **Honest failure.** Friendly errors for missing udev rules / libusb / old
  firmware; device logs tunnel back to the host logger
  (`touchy_pad.device`) so `--debug` shows firmware `ESP_LOG` output inline.
* **Cheap hardware, no soldering.** Every supported board is a finished
  consumer devkit; plug in USB and go. Cases/3D printing optional.

## Feature inventory (as of HEAD)

* **Touchpad:** 1/2/3+-finger click, drag/move, 2-finger scroll, 1-finger
  swipes (4 directions, repeat-while-travelling), 2-finger pinch-zoom in/out,
  2-finger twist CW/CCW, press-and-hold + hold-release (drag-and-drop) — all
  bound to editable `Action` lists with sensible Python DSL defaults.
* **Widgets:** `button`, `image_button` (two image slots, in-place repaint),
  `label`, `slider`, `arc`, `toggle`, `checkbox`, `image` (static / PIL / bytes
  / dynamic `ImageSource`), `log_line`, `fps`, `force_render`, `trackpad`,
  `spacer`, `widget_ref`, plus flex (`row`/`col`), `grid`+`cell` and `absolute`
  layouts with opt-in `grow_x`/`grow_y` sizing, styles, state styles,
  transitions and animations (resolution-independent via
  `start_inverted`/`end_inverted`).
* **Host-facing:** filesystem API (`F:`/`R:`/`T:` drives), screen save/load,
  user screens, `RunActionsCmd`, `SetPreferencesCmd` (partial updates),
  `SetPropertyCmd` (live LVGL property overrides), board info (RAM/flash/log),
  firmware update over USB, mTLS provisioning, network API, simulator.

## Docs map for product questions

`README.md` (pitch + video), `docs/hardware.md` (which board to buy),
`docs/installing.md`, `docs/ui.md`, `docs/user-widgets.md` (roadmap for
host-driven user widgets), `docs/opendeck*.md`, `docs/simulator.md`,
`docs/python-api.md`, `docs/rust-api.md`, `docs/performance.md`,
`docs/led-art.md`, `docs/why-not-xml.md` (why protobuf instead of LVGL XML).
