this file is design plans for lightbar firmware

## stage LB1 — first LED-matrix board (`jc_esp32p4_m3`)

**Status: implemented.** Host-side proto/CLI/sim + firmware board, LED
`Panel`/`LEDPanel`, LVGL LED display driver, weak `platform_is_touchable()`
default with the board's strong override, and the touch-less swatch default
screen are all in place. Firmware compilation on real ESP-IDF hardware is
not yet verified in this environment.

Bring up the first display-less, touch-less board: a Guition
`JC-ESP32P4-M3` module driving a single 8x32 WS2812B LED matrix as an
LVGL display, reachable from the host over native USB. This is the
foundation for the lightbar; multi-panel tiling, multi-lane output, and
runtime-configurable panel counts are **explicitly deferred** to later
stages.

### Target hardware
- **Module:** Guition `JC-ESP32P4-M3` (`esp32p4nrw32`) — dual-CPU
  (ESP32-P4 main + ESP32-C6 for WiFi, held in reset for now), 32 MB
  PSRAM, 16 MB flash, native USB-OTG. ESP-IDF already knows this chip
  (`set-target esp32p4`), so no custom chip support is needed.
- **Display:** one 8x32 WS2812B matrix (256 LEDs) on **GPIO 20**.
- **No LCD, no touch panel.**

### Definition of done
One panel renders the touch-less 3-square default screen, the board
enumerates over USB and answers `board-info` / screen uploads, brightness
works, and the host correctly sees "not touchable". No tiling / multi-panel
work in this stage.

### Work items

1. **Board scaffold** — create `firmware/boards/jc_esp32p4_m3/` mirroring
   the existing `elecrow_p4_lcd_7` layout:
   - `target` → `esp32p4`
   - `sdkconfig.defaults` (PSRAM enabled, native USB, `led_strip` deps)
   - `board/board_pins.h` — panel GPIO (`BOARD_LED_PANEL_GPIO = 20`),
     panel geometry (`8x32`), C6-reset GPIO, backlight-less markers.
   - `board/board.cpp` — `board_init()` (hold C6 in reset; no I2C/touch
     bus), `platform_get()` → `{ is_multitouch = false, has_usb = true,
     is_touchable = false }`, and `backlight_set()` mapping brightness to
     a global LED-strip brightness scalar (0 = off … 100 = max).
   - `board/CMakeLists.txt` + `idf_component.yml` pulling
     `espressif/led_strip` (>= 3.0.3).

2. **`Panel` / `LEDPanel` abstraction** — new class pair under
   `firmware/main/` (or `firmware/boards/common/`):
   - `Panel` — abstract base describing a rectangular pixel surface
     (width, height, `set_pixel(x, y, rgb)` / `flush()`).
   - `LEDPanel : Panel` — one instance per physical matrix, owns one
     `led_strip` handle bound to a GPIO, configured for **RMT + DMA**
     (`espressif/led_strip`, `with_dma = true`). Maps logical `(x, y)` to
     the physical serpentine index (see `lightbar.md`): even rows
     left-to-right, odd rows reversed — `idx = y*32 + (y&1 ? 31-x : x)`.
   - LB1 constructs exactly **one** `LEDPanel` on GPIO 20; the
     variable-count / multi-lane cases are designed-for but not built.

3. **LVGL display driver** — a per-board `display_init()` that:
   - Creates a single **32 wide × 8 tall** `lv_display` in
     `LV_COLOR_FORMAT_RGB565` (consistent with all other boards; global
     `LV_COLOR_DEPTH` unchanged).
   - Flush callback reads the RGB565 partial buffer, expands each pixel to
     RGB888, applies **gamma correction**, writes it into the `LEDPanel`
     via the serpentine map, then `flush()`es the strip and calls
     `lv_display_flush_ready()`.
   - No `touch_init()` — return `nullptr`; the trackpad/indev path stays
     unused.

4. **`platform_is_touchable()`** — add a `bool platform_is_touchable(void)`
   accessor (declared in `firmware/main/platform.h`) with a **weak** default
   implementation in `firmware/main/platform.cpp` that returns `true` —
   behaving like a virtual method whose base returns "touchable", so no
   existing board needs editing. `jc_esp32p4_m3` provides a **strong**
   override returning `false`.
   - Surface it over the host API: add `is_touchable` to
     `SysBoardInfoResponse` (proto + `fill_board_info` in `host_api.cpp`,
     bump `ProtocolVersion`), and print it in `touchy board-info`
     (Python + Rust clients).

5. **Touch-less default screen** — make the fallback screen depend on
   touch capability:
   - `proto/gen_default_screen.py` learns a touch-less variant: a new
     `build_setup_screen()`-style builder (in
     `touchy_pad.api.screens`) that lays out, left-to-right, a **4×4 red**,
     a **6×6 green**, and an **8×8 blue** square (sized to sit inside the
     8-pixel-tall panel). Emits a second JSON
     (`proto/default_screen_touchless.json`).
   - `proto/embed_screen_json.py` / the Justfile emit **both** symbols
     into `firmware/main/default_screen_pb.h`
     (`default_screen_pb_*` and `default_screen_touchless_pb_*`).
   - `firmware/main/screens.cpp` selects the variant at **build time**
     from the board's known touch capability (compile-time flag, e.g.
     `CONFIG_TOUCHY_NO_TOUCH`), so the unused variant costs nothing.
   - The simulator reads the appropriate JSON to match.

### Deferred (future lightbar stages)
- Variable panel count / config-driven GPIO list.
- Multiple `Panel`s tiled into one `lv_display`, or multiple
  `lv_display`s.
- Multi-lane / PARLIO parallel output (4 lanes × 4 modules, 16 panels).
- 12V power, fan, OTA — tracked in `lightbar.md`.

## stage LB2 — 2nd LED-matrix board (`esp32-s3-devkitc-1`)

**Status: implemented (firmware not yet compiled on real ESP-IDF in this
environment).** Shared `Panel`/`LEDPanel` code was relocated from
`firmware/boards/common/` to `firmware/main/leds/` (compiled by the board
component, not `main`, so the `espressif/led_strip` dependency stays
board-scoped); `jc_esp32p4_m3` was repointed at the new location. The new
`firmware/boards/esp32-s3-devkitc-1/` board (target `esp32s3`, LED on
GPIO 4) reuses the LB1 display driver, touch-less no-op touch, brightness
path, and touch-less default screen unchanged.

The ESP32-P4 `jc_esp32p4_m3` board from stage LB1 does not yet boot on
real hardware, so it is parked (unworking). The goal of this stage is to
prove the LB1 software stack — `Panel`/`LEDPanel`, the LVGL LED display
driver, and the touch-less default screen — on **real hardware** using a
mainstream, well-supported ESP32-S3 module, even while the custom P4
board is stuck.

Bring up a second display-less, touch-less LED board: an Adafruit
ESP32-S3 Feather driving a single 8x32 WS2812B matrix as an LVGL display,
reachable over native USB. It reuses the LB1 `LEDPanel` and touch-less
default screen unchanged — only the board scaffold and pin map are new.
Multi-panel tiling / multi-lane output stay deferred (see below).

### Target hardware
- **Module:** Adafruit ESP32-S3 Feather (N16R8 variant — 16 MB flash,
  8 MB PSRAM), native USB-OTG. ESP-IDF already supports this chip
  (`set-target esp32s3`), so no custom chip support is needed. See
  `docs/hardware/esp32-s3-devkitc-1/README.md`.
- **Display:** one 8x32 WS2812B matrix (256 LEDs) on **GPIO 4**.
- **No LCD, no touch panel.**

### Definition of done
The Feather flashes and boots on real hardware, enumerates over USB,
answers `board-info` (reporting `is_multitouch = false`, `has_usb =
true`, `is_touchable = false`), accepts screen uploads, renders the
touch-less 3-square default screen on the matrix, and honours brightness.
No tiling / multi-panel work in this stage.

### Work items

1. **Relocate the shared LED code** — move the `Panel` / `LEDPanel`
   abstraction (`panel.h`, `led_panel.{h,cpp}`) from
   `firmware/boards/common/` into a common subdir of `firmware/main/`
   (e.g. `firmware/main/leds/`) so it is a first-class, board-independent
   subsystem rather than living under `boards/`. Update the include paths
   and CMake references in `jc_esp32p4_m3`'s `board/CMakeLists.txt` (and
   the new Feather board) to point at the new location, and rebuild LB1
   to confirm no regression.

2. **Board scaffold** — create `firmware/boards/esp32-s3-devkitc-1/`,
   mirroring the S3 boards (e.g. `waveshare_s3_lcd_7b`) for the
   chip/USB/PSRAM setup and `jc_esp32p4_m3` for the LED-display half:
   - `target` → `esp32s3`
   - `sdkconfig.defaults` — PSRAM (8 MB) enabled, native USB, `led_strip`
     deps; touch-less flag (`CONFIG_TOUCHY_NO_TOUCH=y`, same knob LB1
     uses to select the touch-less default screen).
   - `board/board_pins.h` — `BOARD_LED_PANEL_GPIO = GPIO_NUM_4`, panel
     geometry `32x8`, backlight-less markers. No I2C / touch / LCD pins.
   - `board/board.cpp` — `board_init()` (no I2C / touch bus),
     `platform_get()` → `{ is_multitouch = false, has_usb = true }`,
     a strong `platform_is_touchable()` override returning `false`, and
     `backlight_set()` mapping brightness to the LED-strip brightness
     scalar (0 = off … 100 = max) — reusing the LB1 pattern.
   - `board/display.cpp` — the same LED LVGL `display_init()` as LB1
     (single 32×8 `lv_display`, RGB565→RGB888 + gamma, serpentine map via
     `LEDPanel`, no `touch_init()`), constructing one `LEDPanel` on
     GPIO 4.
   - `board/CMakeLists.txt` + `idf_component.yml` pulling
     `espressif/led_strip` (>= 3.0.3) and referencing the relocated
     shared LED sources.

3. **Reconfigure + build plumbing** — confirm `just firmware-reconfigure
   esp32-s3-devkitc-1` reads the `target` file and runs `set-target
   esp32s3`, and that `just firmware-build` / `just flash` work for the
   new board. Add an `sdkconfig.esp32-s3-devkitc-1` snapshot if the repo
   commits per-board sdkconfigs.

4. **Reuse, don't re-implement** — the touch-less default screen
   (`proto/default_screen_touchless.json`, `default_screen_touchless_pb_*`
   in `firmware/main/default_screen_pb.h`) and the `is_touchable` host-API
   surfacing from LB1 are reused as-is; no proto, CLI, or simulator
   changes are expected in this stage.

### Deferred (future lightbar stages)
- Variable panel count / config-driven GPIO list.
- Multiple `Panel`s tiled into one `lv_display`, or multiple
  `lv_display`s.
- Multi-lane / PARLIO parallel output (4 lanes × 4 modules, 16 panels).
- 12V power, fan, OTA — tracked in `lightbar.md`.

## stage LB3 — label `long_mode` + scrolling welcome text

Add an LVGL long-mode selector to the `Label` widget, wire it through
every layer of the stack (proto → Python DSL → firmware builder → sim →
Rust mirror), and use it on the touch-less default screen so the
8×32 LED panel shows a continuously scrolling "Welcome to touchypad"
marquee over the bouncing shapes.

### Background

LVGL's `lv_label_long_mode_t` enum
([docs](https://lvgl.io/docs/open/main/widgets/label#long-modes))
controls what happens when label text overflows its fixed bounding box.
The installed LVGL (`firmware/managed_components/lvgl__lvgl`) defines:

```c
typedef enum {
    LV_LABEL_LONG_MODE_WRAP,             // 0  (default)
    LV_LABEL_LONG_MODE_DOTS,             // 1
    LV_LABEL_LONG_MODE_SCROLL,           // 2  (back-and-forth)
    LV_LABEL_LONG_MODE_SCROLL_CIRCULAR,  // 3  (continuous one-way)
    LV_LABEL_LONG_MODE_CLIP,             // 4
} lv_label_long_mode_t;
```

The proto enum values are chosen to match 1:1 so the firmware can cast
directly (same pattern already used for `TextAlign` ↔ `lv_text_align_t`).
`WRAP = 0` is the proto3 default, so existing labels that don't set
`long_mode` keep today's behaviour — **no wire-incompatible break**.

### Definition of done

* `Label` carries an optional `LongMode long_mode` field; the Python
  `label()` DSL, the firmware `build_label`, the Qt sim, and the Rust
  proto mirror all honour it.
* `build_setup_screen_touchless()` overlays a full-screen
  `LongMode.SCROLL_CIRCULAR` label reading
  `"Welcome to touchypad. This is a test"` above the three bouncers.
* `just build-proto` regenerates the Python `_proto` bindings, the C
  nanopb headers, and the Rust `prost` bindings cleanly; `just app-test`
  passes with a new test covering the field round-trip and the
  touch-less welcome label.

### Assumptions

* **Font:** the 8-pixel-tall panel can only legibly render an 8px font;
  the only sizes compiled today are Montserrat **16** and **30**
  (`firmware/sdkconfig.defaults`). This stage enables
  `CONFIG_LV_FONT_MONTSERRAT_8=y` in the shared `sdkconfig.defaults`
  (small flash cost, benefits every board) and sets `font_size=8` on the
  welcome label. *(If 8px reads too chunky on hardware, Montserrat 10 is
  the fallback — but 10px on an 8px display clips, so 8 is the safe
  first choice.)*
* **Label sizing under `absolute()`:** `grow_x`/`grow_y` are documented
  as ignored under an absolute parent (`AGENTS.md` Stage 72), and
  `SCROLL_CIRCULAR` *requires* a fixed width narrower than the text to
  activate. So the welcome label gets an explicit
  `rect(x=0, y=0, w=width, h=height)` to fill the 32×8 panel — this is
  the concrete mechanism behind the design's "grow the x/y size to fill
  the screen". `grow_x=1, grow_y=1` is also set for documentation /
  forward-compatibility.
* **Text colour:** white (`0xFFFFFF`) so it's bright on the WS2812B
  matrix over the dim `0x40`-tier bouncer colours.
* **Z-order:** "over the bouncing shapes" → the label is appended *after*
  the three bouncers (LVGL draws later children on top).

### Work items

1. **Proto — new `LongMode` enum + `Label.long_mode` field**
   (`proto/widgets.proto` *and* the mirror at
   `rust/touchy-pad/proto/widgets.proto`):

   ```proto
   // Long-mode behaviour for a label whose width/height is fixed.
   // Values match lv_label_long_mode_t 1:1 — the firmware casts directly.
   enum LongMode {
       WRAP            = 0;  // LV_LABEL_LONG_MODE_WRAP  (default)
       DOTS            = 1;  // LV_LABEL_LONG_MODE_DOTS
       SCROLL          = 2;  // LV_LABEL_LONG_MODE_SCROLL
       SCROLL_CIRCULAR = 3;  // LV_LABEL_LONG_MODE_SCROLL_CIRCULAR
       CLIP            = 4;  // LV_LABEL_LONG_MODE_CLIP
   }
   ```

   Add to `message Label` (next free tag):
   ```proto
   LongMode long_mode = 4;
   ```
   Bump `Widget.Version.CURRENT` 25 → 26 (house convention: bump on any
   widget-message change, even additive).

2. **Regenerate bindings** — `just build-proto` (Python `_proto` + C
   nanopb `widgets.pb.{h,c}`). The Rust `prost` bindings rebuild on next
   `cargo build` from the mirrored `.proto`.

3. **Python DSL** (`app/src/touchy_pad/api/screens.py`):
   * Add a `LongMode` re-export (alias of `_proto.Label.LongMode`,
     exactly like the existing `TextAlign` alias) at the top of the
     module with the other enum re-exports.
   * `label(...)` gains `long_mode: int = LongMode.WRAP` and writes
     `w.label.long_mode = long_mode` only when the caller passes a
     non-default value (keep the default path untouched so existing
     screens are byte-identical).
   * Update the `label()` docstring to describe each mode and note that
     `SCROLL` / `SCROLL_CIRCULAR` need a fixed width (set via `rect` or a
     sizing parent) to take effect.

4. **Firmware builder** (`firmware/main/widgets/widget_builders.cpp`,
   `build_label`): after the existing `text_align` block, add:

   ```cpp
   if (w.kind.label.long_mode != touchy_LongMode_WRAP) {
       lv_label_set_long_mode(lbl,
           (lv_label_long_mode_t)w.kind.label.long_mode);
   }
   ```
   `WRAP` (0) is skipped so the LVGL default path is unchanged. Extend
   the `ESP_LOGI` line to include `long_mode=%d`.

5. **Enable Montserrat 8** — add `CONFIG_LV_FONT_MONTSERRAT_8=y` to
   `firmware/sdkconfig.defaults` (alongside the existing 16 / 30 lines).
   No per-board override needed.

6. **Touch-less welcome label** — in
   `build_setup_screen_touchless()` (`app/src/touchy_pad/api/screens.py`),
   after the three `_bouncer(...)` calls and **before** `return screen`,
   append:

   ```python
   welcome = label(
       "welcome",
       text="Welcome to touchypad. This is a test",
       font_size=8,
       long_mode=LongMode.SCROLL_CIRCULAR,
       rect=rect(x=0, y=0, w=width, h=height),
       style=style(text_color=0xFFFFFF),
   )
   grow(welcome, x=1, y=1)   # documentation / forward-compat
   screen += welcome
   ```

   The explicit `rect(w=width, h=height)` is what actually fills the
   panel under the absolute layout *and* gives `SCROLL_CIRCULAR` the
   fixed box it needs to scroll within.

7. **Regenerate embedded screen** — `just gen-default-screen`
   (→ `proto/default_screen_touchless.json`) then
   `just build-default-screen` (→
   `firmware/main/default_screen_pb.h`). The welcome label now rides
   inside `default_screen_touchless_pb_*`.

8. **Qt sim** (`app/src/touchy_pad/sim/widgets.py`, the `kind == "label"`
   branch): map `SCROLL` / `SCROLL_CIRCULAR` to a rough equivalent —
   `QtWidgets.QLabel` has no built-in marquee, so wrap the text in a
   `QScrollArea` with horizontal scrollbar off, or (simplest) just let
   Qt clip (`CLIP`-like) and log a debug note. The sim is advisory; the
   exact animation fidelity is not required. At minimum, don't crash on
   the new field.

9. **Rust** — no hand-written label code exists; the mirrored proto +
   `prost` rebuild covers it. If `rust/touchy-pad` has a screen-builder
   helper that constructs `Label`, pass `long_mode` through there too
   (grep confirms there is none today).

10. **Tests** (`app/tests/test_screens.py`):
    * Extend the existing label round-trip test to set
      `long_mode=LongMode.SCROLL_CIRCULAR` and assert it survives
      `Screen.to_proto()` → bytes → parse.
    * Add a `build_setup_screen_touchless()` assertion that the decoded
      screen contains a `label` child with the welcome text,
      `long_mode == SCROLL_CIRCULAR`, and `font_size == 8`.

### Deferred / out of scope

* Per-label scroll speed / animation customisation (LVGL's
  `lv_style_set_anim` template) — not needed for the marquee.
* Custom bitmap font tuned for the 5×8 LED grid — Montserrat 8 is the
  pragmatic first step; revisit if legibility is poor on hardware.
* `DOTS` mode's in-place buffer edit caveat (LVGL warns against
  `_set_text_static` with ROM strings under `DOTS`) — the firmware
  already copies label text via `lv_label_set_text`, so this is a
  non-issue, but worth a one-line code comment in `build_label`.

## stage lb4: better prefs

extend the python cli with new subcommands under the existing "pref"

* json-get: use the protocol to read the Preferences protobuf from the device and emit it to stdout as json
* json-set: read prefs json from stdin and set them on the device.  it is okay for optional fields to be missing from the json.

## stage lb5: multi interface api access

**Status: implemented.** Firmware compiles for both a native-USB board
(`esp32-s3-devkitc-1`: vendor-USB + UART links) and a no-USB board
(`esp32_2432s028rv3`: UART-only), verified with `just firmware-build`.
The host-API code moved to `firmware/main/api/`
(`host_api.{cpp,h}` + `host_api_link.h` base + `vendor_link` / `serial_link`
/ `uart_link` file pairs); `host_api_start()` now registers every available
link into `s_links[]` and spawns a task each, with `s_active_link` tracking
the most-recently-used link. The single `CONFIG_TOUCHY_PROTO_OVER_SERIAL`
tri-state was replaced by three independent flags
(`PROTO_OVER_VENDORUSB` = y, `PROTO_OVER_UART` = y, `PROTO_OVER_CDCACM` = n),
each only instantiated when the board also has the backing hardware
(`CONFIG_SOC_USB_OTG_SUPPORTED`, `+CONFIG_TINYUSB_CDC_COUNT`, or a
board-declared `CONFIG_TOUCHY_HAS_PROTO_UART`). The CYD boards + elecrow_s3
migrated to `HAS_PROTO_UART=y` (UART0); the Feather now runs vendor-USB **and**
UART0 with its console routed to USB-Serial-JTAG. `usb_hid.cpp` CDC gating
moved to `PROTO_OVER_CDCACM`. `TCPLink` remains future work.

Today the firmware runs the host-API dispatcher over **exactly one**
transport, chosen at build time by mutually-exclusive `#if` guards in
`firmware/main/host_api.cpp` (`VendorLink` = USB vendor bulk, `SerialLink`
= USB-CDC ACM, or `UartLink` = hardware UART). This stage generalises that
to an **array of `HostApiLink` instances** so a single board can expose the
protocol over several transports at once and a client may connect over any
of them. It also relocates the host-API code into its own
`firmware/main/api/` subdirectory and splits the per-transport link classes
into their own files. A future `TCPLink` (WiFi) is the motivating next
consumer of this array, but is **out of scope** here.

### Background

* `host_api_start()` currently spawns one `host_api_task` per compiled-in
  link; each task owns its own rx/tx scratch buffers + wake semaphore and
  loops `read_frame → dispatch → write_frame` independently. Because a
  Response is written back on the *same* link its Command arrived on,
  request/response already routes correctly per-link with no extra work.
* Asynchronous **events** are the only output that isn't naturally
  request-scoped: `host_api_post_event()` enqueues onto the shared
  `s_evt_queue`, and the host drains it by polling `EventConsumeCmd`.
  Whichever link the host polls on receives the drained events, so under
  the "one active client at a time" simplifying assumption this also works
  — but the USB "event ready" interrupt-IN mailbox (`host_api_on_rx`) is
  hard-wired to the vendor link. Routing that notification to the
  most-recently-used link is the one genuinely new behaviour.
* The transport guards are mutually exclusive by construction:
  `UartLink` is gated `#if CONFIG_TOUCHY_PROTO_OVER_SERIAL &&
  (!CONFIG_SOC_USB_OTG_SUPPORTED || !CONFIG_TINYUSB_CDC_COUNT)`, so a
  native-USB board can never compile the hardware-UART link. The Feather
  (`esp32s3`, has USB-OTG) is the first board that wants *both* USB and a
  hardware UART live simultaneously. This stage replaces the single
  `CONFIG_TOUCHY_PROTO_OVER_SERIAL` tri-state with **three independent
  per-transport flags** — `PROTO_OVER_VENDORUSB` (default `y`),
  `PROTO_OVER_UART` (default `y`), `PROTO_OVER_CDCACM` (default `n`) — see
  work item 5.

### Definition of done

* `firmware/main/api/` holds the relocated host-API code
  (`host_api.{cpp,h}`) plus one file per transport
  (`vendor_link.{h,cpp}`, `serial_link.{h,cpp}`, `uart_link.{h,cpp}`) and
  a shared `host_api_link.h` base; `main` builds cleanly with the new
  include paths and every existing includer of `host_api.h`
  (`main.cpp`, `usb_hid.cpp`, `widgets/widget_actions.cpp`) still resolves.
* `host_api_start()` registers **all** available links into an array and
  spawns a dispatcher task per link; a board with two live transports
  answers `board-info` / uploads / events over **either** one.
* "Last used wins": a Command arriving on link *B* while link *A* was
  previously active atomically re-points the event mailbox / any unsolicited
  output at *B*; responses continue to go back on the originating link.
* Three independent flags — `CONFIG_TOUCHY_PROTO_OVER_VENDORUSB` (default
  `y`), `CONFIG_TOUCHY_PROTO_OVER_UART` (default `y`), and
  `CONFIG_TOUCHY_PROTO_OVER_CDCACM` (default `n`) — each gate one link, and
  each link is only *instantiated* when the board also has the backing
  hardware. On the Feather this yields a live vendor-USB link **and** a
  live hardware-UART link, and both enumerate.
* No wire-protocol change — this is a firmware-internal refactor plus a
  board config change, so no `ProtocolVersion` bump and no host-side edits.


### Work items

1. **Relocate into `firmware/main/api/`** — `git mv` (or create + delete)
   `host_api.cpp` and `host_api.h` into a new `api/` subdir. Update:
   - `firmware/main/CMakeLists.txt`: change the `SRCS` entry
     `"host_api.cpp"` → `"api/host_api.cpp"` (plus the new link `.cpp`s
     below), and add `"api"` to `INCLUDE_DIRS` so `#include "host_api.h"`
     keeps working for the three current includers without touching them.
   - Confirm `main.cpp`, `usb_hid.cpp`, and `widgets/widget_actions.cpp`
     still compile (include path preserved, so no edit expected).

2. **Extract the link classes into their own files.** Split out of
   `host_api.cpp`:
   - `api/host_api_link.h` — the abstract `HostApiLink` base (rx/tx
     buffers, `rx_sem`, `name()/connected()/read_some()/write_all()/
     flush()/wait_rx()`), transport-independent.
   - `api/vendor_link.{h,cpp}` — `VendorLink` (TinyUSB vendor bulk),
     guarded `#if CONFIG_SOC_USB_OTG_SUPPORTED`.
   - `api/serial_link.{h,cpp}` — `SerialLink` (USB-CDC ACM).
   - `api/uart_link.{h,cpp}` — `UartLink` + its `uart_rx_pump_task` /
     `uart_link_init()` + the `HOST_API_UART_NUM` / `HOST_API_UART_BAUD`
     knobs.
   The framing helpers (`crc8_update`, `read_frame`, `write_frame`,
   `link_read_exact`, `link_sync_magic`) and `host_api_task` stay in
   `host_api.cpp` and operate on `HostApiLink*` as they do now.

3. **Link registry + per-link dispatch.** Replace the three static
   link singletons with a small registry: a fixed-capacity
   `HostApiLink *s_links[N]` (or `std::vector`) populated in
   `host_api_start()` from whichever links the build config enables, then
   `for (link : s_links) xTaskCreatePinnedToCore(host_api_task, …, link,
   …)`. The task body is unchanged; only construction moves to a loop.

4. **"Last used wins" active-link tracking.** Add a
   `std::atomic<HostApiLink*> s_active_link`. In `host_api_task`, set
   `s_active_link = link` immediately after a frame decodes successfully
   (i.e. this link just carried a Command). Route the unsolicited event
   mailbox notification to `s_active_link` instead of the hard-wired
   vendor link:
   - Generalise `host_api_post_event()` — after enqueuing, poke the
     current `s_active_link`'s wake path (its `rx_sem`, or, on USB, the
     interrupt-IN mailbox) so the active client learns events are
     waiting. Since events are still drained by `EventConsumeCmd` polling
     on that same link, responses/events stay coherent.
   - `host_api_on_rx` / `host_api_on_cdc_rx` keep waking their specific
     link's `rx_sem` (they fire from the USB ISR for their own endpoint);
     no active-link logic needed on the *input* side.

5. **Replace the tri-state guard with three independent flags.** Make
   link availability additive rather than exclusive so USB + hardware
   UART (and optionally USB-CDC) can coexist. Retire
   `CONFIG_TOUCHY_PROTO_OVER_SERIAL` in favour of three per-transport
   Kconfig bools in `firmware/main/Kconfig.projbuild`:
   - `CONFIG_TOUCHY_PROTO_OVER_VENDORUSB` — **default `y`** — the USB
     vendor-bulk `VendorLink`.
   - `CONFIG_TOUCHY_PROTO_OVER_UART` — **default `y`** — the hardware
     `UartLink`, with its own `UART_NUM` / baud / pin config.
   - `CONFIG_TOUCHY_PROTO_OVER_CDCACM` — **default `n`** — the USB-CDC ACM
     `SerialLink` (the old `PROTO_OVER_SERIAL` behaviour; off by default
     because most boards use the CDC port for the console instead).

   The flag only *enables* a link; the link is **instantiated only when
   the board also has the backing hardware**, so a flag defaulting `y`
   never forces a link a board can't support:
   - `VendorLink` needs `CONFIG_SOC_USB_OTG_SUPPORTED`.
   - `SerialLink` needs `CONFIG_SOC_USB_OTG_SUPPORTED && CONFIG_TINYUSB_CDC_COUNT`.
   - `UartLink` needs a board-declared protocol `UART_NUM` / pins (a board
     with none simply doesn't build it even with the flag `y`).

   Concretely: `#if CONFIG_TOUCHY_PROTO_OVER_VENDORUSB &&
   CONFIG_SOC_USB_OTG_SUPPORTED` around `VendorLink`, and likewise for the
   other two — the flag AND the hardware capability must both hold. This
   also means the default config (`y`/`y`/`n`) "just works" per board with
   zero per-board overrides: a native-USB board gets vendor-USB, a no-USB
   board silently drops it and keeps UART.
   - **Console conflict:** the hardware `UartLink` defaults to `UART_NUM_0`,
     which is also the IDF console UART. On a board enabling both USB and
     UART, route the IDF console off the protocol UART
     (`CONFIG_ESP_CONSOLE_*`, or point `UartLink` at a spare `UART_NUM`)
     so log bytes don't interleave with protocol frames. Capture the
     chosen pins in the board's `board_pins.h` / `sdkconfig.defaults`.

6. **Feather board config.** With the new defaults the Feather already
   gets vendor-USB (`y` + USB-OTG) **and** the hardware UART (`y`), so no
   flag flip is strictly required — but pin the intent in
   `firmware/boards/esp32-s3-devkitc-1/sdkconfig.defaults` (explicit
   `CONFIG_TOUCHY_PROTO_OVER_UART=y` plus the `UART_NUM` / baud /
   console-routing settings, and `CONFIG_TOUCHY_PROTO_OVER_CDCACM` left
   off). Verify with `just firmware-reconfigure esp32-s3-devkitc-1` +
   `just firmware-build` that both links start (two `host_api dispatcher
   started (…)` log lines) and that `touchy board-info` answers over each.

7. **Docs + memory + migration.** Update `AGENTS.md` / `CLAUDE.md`'s
   transport section (which currently states the serial/USB paths are
   mutually exclusive and that `# CONFIG_X is not set` disables a link) to
   describe the multi-link array and the three independent flags. Sweep the
   codebase + every `sdkconfig.defaults` for the retired
   `CONFIG_TOUCHY_PROTO_OVER_SERIAL` and migrate each occurrence to the
   appropriate new flag (`PROTO_OVER_CDCACM` for the boards that used it
   for USB-CDC, `PROTO_OVER_UART` for the no-USB CYD boards). No
   `docs/host-api.md` wire changes.


### Deferred / out of scope

* **`TCPLink` (WiFi transport).** The array is the enabler; the actual
  TCP/socket link, its lifecycle, and WiFi bring-up land in a later stage.
* **Concurrent multi-client** use (two hosts driving the device at once).
  The "last used wins" assumption explicitly means only one active client
  at a time; arbitration / per-client event queues are not built here.
* **Per-link flow-control / backpressure** tuning beyond what the existing
  single-link path already does.

## stage lb6: runtime LED-panel config via `BoardConfig`

**Status: implemented (host side; firmware not compiled on real ESP-IDF in
this environment).** Proto (`Panel`/`Display`/`BoardConfig` +
`PreferencesFile.board_config`, `Version` 5→6), nanopb caps
(`max_count:1`), firmware merge/persist + proto-free `led_panel_config()`
accessor, `led_display.cpp` reading it (headless when unset), removal of
the `BOARD_LED_PANEL_*` macros, the `pref from-template` CLI (+ shared
`_apply_prefs_json` helper), the bundled
`assets/templates/led-32x8.json`, tests, and docs are all in place.
`just app-test` + `just app-lint` pass.

Move the LED-matrix hardware description off compile-time C macros and
into a persisted, host-writable protobuf. Today each LED board hard-codes
its panel in `board_pins.h`:

```c
#define BOARD_LED_PANEL_GPIO   GPIO_NUM_4
#define BOARD_LED_PANEL_W      32
#define BOARD_LED_PANEL_H      8
```

After this stage an LED board owns **no** built-in panel geometry: the
GPIO and dimensions come from a new `BoardConfig` message stored inside
the device's `PreferencesFile`, provisioned by the host CLI from a JSON
template. This is the first step toward the runtime-configurable,
multi-panel lightbar; multi-display / multi-panel tiling stays deferred.

### Background

* `firmware/boards/common/leds/led_display.cpp` builds its single `LEDPanel` from
  the `BOARD_LED_PANEL_*` macros at compile time (`display_init()`), and
  `fill_board_info()` in `firmware/main/api/host_api.cpp` reports display
  size straight off the live `lv_display`.
* Boot order already favours us: `main.cpp` runs `fs_init()` →
  `Prefs::instance().begin()` **before** `board_init()` / `display_init()`,
  so `display_init()` can read the loaded preferences to decide what panel
  (if any) to construct.
* Only the two LED boards (`esp32_s3_devkitc_1`, `jc_esp32p4_m3`) use the
  `BOARD_LED_PANEL_*` macros and the `firmware/boards/common/leds/` display driver.
  LCD/touch boards use a different `display_init()` and are untouched here.

### Decisions (locked in)

* **`BoardConfig` lives on `PreferencesFile`** — a new
  `optional BoardConfig board_config` field, persisted and merged through
  the existing Stage 82 partial-update path, not a separate file.
* **No compile-time default / no first-boot seed.** A fresh LED board has
  empty prefs → no `Panel` → no LED display. It comes up **headless**
  (`board-info` reports `display_width == display_height == 0`) until the
  user pushes a template with `touchy pref from-template …`. This keeps
  the firmware config-driven with a single source of truth.
* **The `BOARD_LED_PANEL_*` macros are removed entirely.** LED boards ship
  no built-in geometry; all panel config arrives over the wire.
* **Scope: LED-panel boards only.** `BoardConfig.displays` is empty on
  LCD/touch boards and their display pipeline is unchanged. The one-display
  assumption is preserved (see work item 1).

### Definition of done

* New `BoardConfig` / `Display` / `Panel` messages exist; `PreferencesFile`
  carries an `optional BoardConfig board_config`, `Version.CURRENT` bumps
  6 → 7, and `SetPreferencesCmd` can write it (device merges + persists,
  applied on **next boot**, not live).
* `firmware/boards/common/leds/led_display.cpp` constructs its `LEDPanel` from
  `prefs.board_config.displays[0].panels[0]` (gpio / width / height); with
  no panel configured it stands up headless and `board-info` reports
  `0×0`. No `BOARD_LED_PANEL_*` macros remain anywhere in the tree.
* `touchy pref from-template <name>` installs
  `app/src/touchy_pad/assets/<name>.json` as a partial preferences update,
  sharing the JSON→proto plumbing with `pref json-set`; a shipped
  `led-32x8.json` reproduces today's Feather config.
* `just build-proto` regenerates Python `_proto`, C nanopb, and Rust
  `prost` bindings cleanly; `just app-test` passes with a template
  round-trip test; the two LED boards build via `just firmware-build`.

### Work items

1. **Proto — `BoardConfig` + nested messages**
   (`proto/preferences.proto` and the Rust mirror
   `rust/touchy-pad/proto/preferences.proto`):

   ```proto
   // One physical LED matrix on a single data GPIO.
   message Panel {
       uint32 width  = 1;   // logical pixel columns
       uint32 height = 2;   // logical pixel rows
       uint32 gpio   = 3;   // data GPIO number
   }

   // One LVGL display surface. For now exactly one Panel (see options).
   message Display {
       repeated Panel panels = 1;
   }

   // Build/hardware description programmed onto the device. For now just
   // an array of at most one Display.
   message BoardConfig {
       repeated Display displays = 1;
   }
   ```

   Add to `PreferencesFile` (next free tag is 7):
   ```proto
   optional BoardConfig board_config = 7;   // Stage lb6
   ```
   Bump the `Version` enum `V5/CURRENT=5` → `V6 = 6; CURRENT = 6`.
   *(House convention: bump on any prefs-schema change.)*

2. **nanopb options** (`proto/preferences.options`) — cap the repeated
   fields so nanopb can stack-allocate (preserving the one-display
   assumption):
   ```
   touchy.BoardConfig.displays  max_count:1
   touchy.Display.panels        max_count:1
   ```
   These are `FT_STATIC` fixed arrays — no heap, matching the existing
   prefs style.

3. **Regenerate bindings** — `just build-proto` (Python `_proto` + C
   nanopb `preferences.pb.{h,c}`); Rust `prost` rebuilds from the mirror.

4. **Firmware — merge + persist** (`firmware/main/prefs.{h,cpp}`):
   * Store `board_config` in the `Prefs` singleton; include it in
     `to_proto()` and merge it in `apply_partial()` when present. Unlike
     the live-effect prefs (backlight, log level), `board_config` has **no
     side effect** — it is only read at the next `display_init()`. Add a
     `const touchy_BoardConfig &board_config() const` accessor (or
     `has_panel()` + `panel()` convenience getters).

5. **Firmware — LED display reads prefs**
   (`firmware/boards/common/leds/led_display.cpp`):
   * Delete the `BOARD_LED_PANEL_W/H/GPIO` `constexpr` reads. Instead pull
     `displays[0].panels[0]` from `Prefs::instance()`. If there is no
     display/panel, log an info line and return `nullptr` (headless) — the
     rest of the stack already tolerates a null display (`display_init`
     failure path, `fill_board_info` reports 0×0).
   * Guard against absurd sizes (e.g. `width*height == 0` or over a sane
     max) so a bad template can't OOM the draw-buffer malloc.

6. **Remove the macros** — delete `BOARD_LED_PANEL_GPIO/_W/_H` from
   `firmware/boards/esp32_s3_devkitc_1/board/board_pins.h` and
   `firmware/boards/jc_esp32p4_m3/board/board_pins.h`, plus any stray
   references (`grep BOARD_LED_PANEL`). The board files keep only truly
   board-fixed pins (C6-reset, etc.).

7. **Python — `pref from-template`** (`app/src/touchy_pad/cli.py`):
   * Add `@pref.command("from-template")` taking a `<name>` argument. It
     loads `app/src/touchy_pad/assets/<name>.json`
     (via `importlib.resources`), parses it into a `PreferencesFile` with
     `google.protobuf.json_format.Parse`, clears the device-owned
     `file_version`, and calls `c.set_preferences(prefs)` — refactor the
     JSON→proto→`set_preferences` core out of `pref json-set` into a shared
     helper both commands call.
   * List available templates in the help / on unknown name (glob the
     assets dir for `*.json`).

8. **Ship `led-32x8.json`** — new asset
   `app/src/touchy_pad/assets/led-32x8.json` encoding the Feather's old
   config:
   ```json
   {
     "fileVersion": "V6",
     "boardConfig": { "displays": [ { "panels": [
       { "width": 32, "height": 8, "gpio": 4 }
     ] } ] }
   }
   ```
   *(`fileVersion` is the `json-set`/`from-template` validity canary and is
   stripped before send.)* Register `*.json` as package data in
   `app/pyproject.toml` if the existing asset glob doesn't already include
   it.

9. **Rust mirror** (`rust/touchy-pad`) — the regenerated `prost` bindings
   cover the new messages; add a `Touchy::set_board_config(...)` helper
   only if a caller needs it (grep — none expected this stage, so this is
   optional/skip).

10. **Tests** (`app/tests/`):
    * A template round-trip: load `led-32x8.json`, parse to
      `PreferencesFile`, assert `board_config.displays[0].panels[0]` is
      `{32, 8, 4}` and that it survives proto serialise → parse.
    * A `from-template` CLI test against the simulator/fake client
      asserting the sent `SetPreferencesCmd` carries the panel and omits
      `file_version`.

11. **Docs + memory** — note in `AGENTS.md`/`CLAUDE.md` that LED-panel
    geometry is now a runtime pref (`BoardConfig` on `PreferencesFile`,
    applied next boot; fresh board is headless until `pref from-template`)
    and update the `PreferencesFile.Version.CURRENT == 6` line.

### Deferred / out of scope

* Multiple `Display`s, or multiple `Panel`s per display (tiling) — the
  `max_count:1` caps hold the one-display assumption; lifting them is a
  later lightbar stage.
* Live re-configuration (rebuilding the LVGL display when `board_config`
  changes without a reboot).
* Moving **LCD/touch** board geometry into `BoardConfig`.
* Panel wiring/serpentine/lane descriptors, colour order, gamma, and
  brightness curves — still compile-time / firmware-side for now.