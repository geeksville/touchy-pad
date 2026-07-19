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

## stage lb7: `Display` class abstraction

**Status: implemented + firmware-built for every board (LED + all LCD
variants, all three target chips).** `Display` ABC
(`firmware/main/display.{h,cpp}`) with `init()` → `hw_init()` +
`post_init()` (dim-blue background via the LVGL-v9 active-screen style,
since `lv_disp_set_bg_color` is gone) and `raw()`; a board-agnostic
`HeadlessDisplay`; a board factory `Display *display_create(void)`. The
old `extern "C" display_init()` and `main.cpp`'s `display_init_headless()`
are gone; `main.cpp` owns a `Display*`. Every board's `display.cpp` now
defines a local `namespace { class …Display : public Display; }` whose
`hw_init()` holds the previous bring-up, plus `display_create()`. The LED
family uses the shared `LEDMatrixDisplay` in
`firmware/boards/common/leds/`. **Deviation from the plan below:** the
`LCDPanelDisplay` shared base was dropped as adding no value — LCD board
subclasses inherit `Display` directly.

Replace the free `lv_display_t *display_init(void)` C seam — implemented
eight different ways across the boards (`firmware/boards/common/leds/
led_display.cpp` for the LED family, plus seven per-board LCD
`display.cpp` files) — with a small C++ `Display` class hierarchy. Each
board provides a concrete subclass; `main.cpp` owns the instance and
drives its lifecycle. This turns today's copy-pasted bring-up into a
shared base with a single overridable hardware hook, and folds the
existing headless path into the same hierarchy.

### Background

* Today every board's `board` component exports
  `extern "C" lv_display_t *display_init(void)`. `main.cpp` calls it,
  falls back to a private `display_init_headless()` on `nullptr`, then
  calls `touch_init(disp)`. The returned handle is otherwise unused
  (LVGL tracks the default display internally).
* The eight implementations share a lot of boilerplate — `lvgl_port_init`,
  `lv_display_create`, `lv_display_set_color_format`,
  `lv_display_set_buffers`, `lv_display_set_flush_cb` — and differ only in
  the panel/bus bring-up (LED strip vs RGB-DPI vs SPI vs QSPI) and the
  flush callback.
* `firmware/main/display.h` is the seam header (currently just the C
  `display_init()` prototype). `firmware/main/main.cpp` holds
  `display_init_headless()` (gated on `CONFIG_TOUCHY_NO_DISPLAY` /
  `CONFIG_TOUCHY_HEADLESS_*`).

### Decisions (locked in)

* **Convert every board this stage** — the LED family (`LEDMatrixDisplay`)
  **and** all seven LCD `display.cpp` variants. No board keeps the old C
  `display_init()`.
* **Replace the C seam.** `main.cpp` owns a `Display*` and calls
  `init()` / `raw()` directly; the free `display_init()` function is
  removed. The board→main seam becomes a board-provided **factory**
  `Display *display_create(void)` (declared in `display.h`) that returns
  the board's concrete subclass.
* **Shared `post_init()` blue background on every display** — LED and LCD
  alike. `post_init()` is virtual so a board can override, but the base
  behaviour (dim blue `lv_disp_set_bg_color`) applies to all by default.
* **`LCDPanelDisplay` is a shared base**, not a leaf: it carries the
  common LVGL wiring, and a board that needs to tweak LCD bring-up
  subclasses it and overrides the relevant virtual (adding new virtual
  hooks as needed). LED boards get `LEDMatrixDisplay`. The headless path
  becomes a first-class `HeadlessDisplay` subclass.

### The `Display` hierarchy

```cpp
// firmware/main/display.h
class Display {
public:
    virtual ~Display() = default;

    // Bring the panel up: hw_init() then (on success) post_init().
    // Returns false if the hardware display could not be created.
    bool init();

    // The LVGL display handle created by hw_init(), or nullptr if init()
    // failed / hasn't run.
    lv_display_t *raw() const { return m_disp; }

protected:
    // Board/panel-specific bring-up. Must create and return an
    // lv_display_t* (stored into m_disp by init()), or nullptr on failure.
    virtual lv_display_t *hw_init() = 0;

    // Post-bring-up tweaks common to all displays. Base sets a dim blue
    // background via lv_disp_set_bg_color(); override to customise.
    virtual void post_init();

    lv_display_t *m_disp = nullptr;
};

// Board factory — one strong definition per board component.
Display *display_create(void);
```

`bool Display::init()` (in a new `firmware/main/display.cpp`):
```cpp
bool Display::init() {
    m_disp = hw_init();
    if (!m_disp) return false;
    post_init();
    return true;
}
void Display::post_init() {
    // Dim blue so a blank screen / bg-behind-widgets is clearly "on".
    lv_disp_set_bg_color(m_disp, lv_color_hex(0x000020));
}
```

### Definition of done

* `firmware/main/display.h` declares the `Display` ABC + the
  `display_create()` factory; `firmware/main/display.cpp` implements
  `init()` / `post_init()`. No `extern "C" display_init()` remains.
* Each board component defines exactly one `display_create()` returning
  its concrete subclass:
  - LED boards (`esp32_s3_devkitc_1`, `jc_esp32p4_m3`) →
    `LEDMatrixDisplay` (in `firmware/boards/common/leds/`).
  - The seven LCD boards → an `LCDPanelDisplay` subclass each (a shared
    `LCDPanelDisplay` base holds the common LVGL wiring; per-board
    subclasses implement `hw_init()`).
* `main.cpp` owns `Display *g_display = display_create();` calls
  `g_display->init()`, uses `g_display->raw()` for `touch_init()` and the
  rest, and on failure constructs a `HeadlessDisplay` instead. The old
  `display_init_headless()` free function is gone.
* Every board still builds (`just firmware-build` for a representative
  LED + LCD board) and the display comes up with the dim-blue background.

### Work items

1. **`Display` ABC + factory** — rewrite `firmware/main/display.h` with
   the class above (keep the `#include "lvgl.h"`; drop the `extern "C"`
   block). Add `firmware/main/display.cpp` (new entry in
   `firmware/main/CMakeLists.txt` `SRCS`) implementing `init()` +
   `post_init()`.

2. **`HeadlessDisplay`** — move `display_init_headless()` out of
   `main.cpp` into a `HeadlessDisplay : Display` (its `hw_init()` is the
   current headless body). Put it somewhere board-agnostic
   (`firmware/main/display.cpp` or a `headless_display.{h,cpp}`) since it
   depends only on `CONFIG_TOUCHY_HEADLESS_*`.

3. **`LEDMatrixDisplay`** — convert
   `firmware/boards/common/leds/led_display.cpp`'s `display_init()` into
   `LEDMatrixDisplay::hw_init()`; keep the Stage lb6 `led_panel_config()`
   read, the geometry guard, and the gamma/serpentine flush. Provide the
   board factory `display_create()` for the two LED boards returning
   `new LEDMatrixDisplay(...)` (the shared source can define
   `display_create()` since both LED boards want the same subclass).

4. **`LCDPanelDisplay` base + per-board subclasses** — add a shared
   `LCDPanelDisplay` (e.g. `firmware/boards/common/lcd_panel_display.{h,cpp}`)
   holding the boilerplate common to the LCD boards (LVGL display create,
   colour format, buffer alloc, flush-cb registration). Convert each of
   the seven LCD `display.cpp` files
   (`waveshare_s3_lcd_7b`, `elecrow_s3_lcd_7`, `elecrow_p4_lcd_7`,
   `cyd_common`, `matouch_43`, `jc4827w543`, `squixl`) into an
   `LCDPanelDisplay` subclass implementing `hw_init()` (the panel/bus
   bring-up) and, where a board diverges, overriding a virtual (add new
   virtuals to the base as the concrete conversions reveal the seams —
   e.g. a `flush()` hook, an `orientation()` hook). Each board's
   `display_create()` returns its subclass.

5. **`main.cpp` lifecycle** — replace the `display_init()` /
   `display_init_headless()` dance with:
   ```cpp
   Display *disp = display_create();
   if (!disp->init()) { delete disp; disp = new HeadlessDisplay(); disp->init(); }
   lv_display_t *raw = disp->raw();
   ...
   tp = touch_init(raw);
   ```
   Keep the `CONFIG_TOUCHY_NO_DISPLAY` branch (construct `HeadlessDisplay`
   directly). Stash `disp` in a file-scope owner so it outlives boot.

6. **CMake** — every board's `board/CMakeLists.txt` already compiles its
   `display.cpp`; update the LCD boards to also pull the shared
   `../../common/lcd_panel_display.cpp` (mirroring how the LED boards pull
   `common/leds/led_display.cpp`), and confirm `Display` (declared in
   main via `REQUIRES main` from Stage lb6's fix) is on the include path.

7. **Docs + memory** — update `firmware/README.md` (the
   `display.h : lv_disp_t *display_init(void)` line) and add an AGENTS.md /
   CLAUDE.md note describing the `Display` ABC, the `display_create()`
   factory seam, `HeadlessDisplay`, and the shared blue-bg `post_init()`.

### Deferred / out of scope

* Multi-display support (`main.cpp` still owns exactly one `Display`).
* Merging `touch_init()` into `Display` — touch stays its own seam for
  now (a `Display` may or may not have an indev).
* Reworking the LVGL flush/port internals or buffer strategy beyond
  moving the existing per-board code into subclasses.
* Any behaviour change to the panels themselves — this is a structural
  refactor; the only intended runtime change is the shared dim-blue
  background.

## lb8: expose protobuf API over HTTP/HTTPS sockets

**Status: implemented (host + firmware code written; firmware not compiled
on real ESP-IDF in this environment).** Proto (`NetworkConfig` +
`PreferencesFile.network`, `Version` 6→7), nanopb string caps, firmware
`net/network.{h,cpp}` + `net/http_api.{h,cpp}` (gated on `CONFIG_TOUCHY_WIFI`,
default `y` on WiFi chips), the reusable `host_api_dispatch_serialized()`
seam, prefs per-sub-field merge + live `network_apply()`, the boot call,
CMake/Kconfig/sdkconfig wiring, the host `HttpTransport` +
`touchy_open(url=, tls_psk=)` + CLI `--url`/`--tls-psk` +
`pref wifi-set-ssid`/`wifi-set-psk`, the simulator's plaintext-HTTP server
on 8083, and tests are all in place. `just app-test` + `just app-lint` pass.

Give the device an optional WiFi network presence and a request/response
HTTP(S) API so a host can drive the same protobuf `Command`/`Response`
protocol over the network — no USB/UART cable required. This is the
natural next consumer of the Stage lb5 multi-link `HostApiLink` array (a
`TCPLink` was explicitly deferred there) but is implemented here as an
HTTP request handler layered on the existing command dispatcher rather
than a raw framed link, because the transport is one-shot request/response
(`POST … → Response`) instead of a persistent byte stream.

### Background

* The wire protocol is normally self-synchronising frames over a byte
  stream (`MAGIC | LEN | payload | CRC8`). HTTP already delimits the
  request body, so the HTTP transport carries a **bare, unframed**
  serialized `Command` in the POST body and a bare serialized `Response`
  in the reply body — no MAGIC/LEN/CRC8 (HTTP `Content-Length` + TCP
  already provide framing and integrity). The single dispatch seam
  (`host_api` command handler on the device;
  `SimDevice.handle_command(payload) -> bytes` in the simulator) is reused
  unchanged; only the framing wrapper differs per transport.
* Preferences already carry all persisted device config
  (`PreferencesFile`, merged via the Stage 82 partial path in
  `Prefs::apply_partial`) and are read at boot **before** `board_init()`.
  WiFi credentials therefore live naturally on `PreferencesFile`, and
  network bring-up hangs off the same apply path as every other pref.
* `main.cpp` boot order: `fs_init()` → `Prefs::begin()` → `board_init()` →
  `display_init()` → `host_api_start()`. Network bring-up + the HTTP
  server start after `Prefs::begin()` so they can read the config, gated
  on WiFi being configured.
* Events remain **poll-based** (`EventConsumeCmd`) exactly as on every
  other transport — each HTTP POST is an independent command, and a host
  that wants events simply POSTs `EventConsumeCmd` in a loop.

### Decisions (locked in)

* **One combined stage** (not split): NetworkConfig + WiFi/mDNS + the
  HTTP/HTTPS server + the host client + docs all land together.
* **`NetworkConfig` lives on `PreferencesFile`** as
  `optional NetworkConfig network = 8`, persisted/merged through the
  existing partial path, `Version.CURRENT` bumps 6 → 7. WiFi
  bring-up/teardown is a **live side effect** in `apply_partial` (like
  backlight/log-level), so `pref json-set` can join a network without a
  reboot.
* **Secrets stored plaintext** in `PreferencesFile` on flash, same as any
  other pref (documented as such). No redaction of `wifi_psk` /
  `tls_psk_key` in this stage — the flash is already fully host-readable
  over the protocol.
* **TLS-PSK is full-stack this stage** — device (`esp_https_server` with
  a PSK hint/key) **and** the Python client. Python PSK requires
  `ssl.SSLContext` PSK callbacks (**Python 3.13+**); on older Pythons the
  HTTPS-PSK client path raises a clear "requires Python 3.13+" error while
  the plain-HTTP path keeps working.
* **`tls_psk_key` gates security:** when set, the device serves **only**
  HTTPS (PSK-secured) and the plaintext HTTP port is disabled; when unset,
  the device serves **only** plaintext HTTP. (Never both — avoids a
  downgrade path.)
* **Simulator: bare HTTP only**, on a fixed port **8083**, reusing its
  in-process `SimDevice.handle_command`. No TLS in the sim.
* **Host selector is URL-style**, mirroring `TOUCHY_SIM_URL`: a
  `http://host[:port]` / `https://host[:port]` string selects the HTTP
  transport, plus a `--tls-psk KEY` flag (and `touchy_open(url=...,
  tls_psk=...)`) for the HTTPS-PSK case.

### Ports

* Device plaintext HTTP: **80**. Device HTTPS: **443**.
* Simulator plaintext HTTP: **8083** (fixed; no TLS).
* mDNS advertises the chosen scheme/port under the resolved hostname.

### Definition of done

* `PreferencesFile` carries `optional NetworkConfig network = 8`
  (`wifi_ssid`, `wifi_psk`, `hostname`, `tls_psk_key`), `Version.CURRENT
  == 7`; `just build-proto` regenerates Python `_proto`, C nanopb, and
  Rust `prost` cleanly.
* When `wifi_ssid`/`wifi_psk` are set, the device joins the network,
  brings up mDNS as `<hostname>` (defaulting to `touchypad_<serial-suffix>`
  when `hostname` is unset), and starts the HTTP **or** HTTPS server
  (HTTPS iff `tls_psk_key` set). With no WiFi config the device behaves
  exactly as today (no radio, no server).
* `POST /touchy/api/v1/command` with `Content-Type: application/protobuf`
  and a serialized `Command` body returns a serialized `Response` body;
  the same request works against the simulator on `http://<host>:8083`.
* `touchy --url http://<host> board-info` (and the HTTPS-PSK variant with
  `--tls-psk`) round-trips against a real device and the simulator;
  `touchy_open(url=..., tls_psk=...)` exposes the same from the API.
* `just app-test` passes with new HTTP-transport + NetworkConfig
  round-trip tests; docs/readmes updated.

### Work items

1. **Proto — `NetworkConfig` + `PreferencesFile.network`**
   (`proto/preferences.proto` and the Rust mirror
   `rust/touchy-pad/proto/preferences.proto`):

   ```proto
   // Optional WiFi + network-API configuration. When wifi_ssid/wifi_psk
   // are set the device joins that network and starts the HTTP(S) API.
   message NetworkConfig {
       optional string wifi_ssid  = 1;   // 2.4 GHz SSID to join
       optional string wifi_psk   = 2;   // WPA2 passphrase (plaintext)
       optional string hostname   = 3;   // mDNS name; unset → touchypad_<serial>
       optional string tls_psk_key = 4;  // hex/base64 PSK; set ⇒ HTTPS-only
   }
   ```

   Add to `PreferencesFile` (next free tag is 8):
   ```proto
   optional NetworkConfig network = 8;   // Stage lb8
   ```
   Bump the `Version` enum: add `V7 = 7;` and move `CURRENT = 7;`.

2. **nanopb options** (`proto/preferences.options`) — bound the strings so
   nanopb can stack-allocate (matching the existing `FT_STATIC` prefs
   style):
   ```
   touchy.NetworkConfig.wifi_ssid   max_size:33
   touchy.NetworkConfig.wifi_psk    max_size:64
   touchy.NetworkConfig.hostname    max_size:32
   touchy.NetworkConfig.tls_psk_key max_size:64
   ```

3. **Regenerate bindings** — `just build-proto` (Python `_proto` + C
   nanopb `preferences.pb.{h,c}`); Rust `prost` rebuilds from the mirror.

4. **Firmware — network subsystem** (new
   `firmware/main/net/network.{h,cpp}`):
   * `network_apply(const touchy_NetworkConfig &)` — idempotent: if the
     SSID/PSK differ from the currently-joined network, (re)connect;
     when SSID is cleared, disconnect and stop the servers. Uses the
     IDF `esp_wifi` STA APIs + `esp_netif`.
   * mDNS via the `espressif/mdns` managed component: advertise
     `_touchy._tcp` under the resolved hostname. Default hostname is
     `touchypad_<suffix>` where `<suffix>` is the low bytes of the board
     serial (reuse whatever `sys_board_info` uses for the USB serial
     string).
   * On WiFi-got-IP, start the HTTP server (see item 5); on disconnect,
     stop it and retry the join with backoff.
   * Gated on `CONFIG_TOUCHY_WIFI` (default `y` on chips with WiFi:
     esp32s3/esp32; the esp32p4 has no radio → default `n` / compiled
     out). No-radio boards compile the whole subsystem out.

5. **Firmware — HTTP(S) API server** (new
   `firmware/main/net/http_api.{h,cpp}`):
   * Register a single `POST /touchy/api/v1/command` URI handler on
     `esp_http_server` (plaintext) or `esp_https_server` (TLS-PSK).
   * Handler: read the full body (a bare serialized `Command`, no
     MAGIC/LEN/CRC8), call the **shared** command dispatcher already used
     by every `HostApiLink` (factor the per-command switch in
     `api/host_api.cpp` into a reusable
     `host_api_dispatch(const uint8_t*, size_t, ...) -> serialized Response`
     if it isn't already callable standalone), and write the serialized
     `Response` back with `Content-Type: application/protobuf`.
   * **Scheme selection:** `tls_psk_key` set → start `esp_https_server`
     with the PSK hint/key, port 443, and do **not** start the plaintext
     server; unset → start plaintext `esp_http_server` on port 80.
   * Because a POST is a full command exchange, this reuses the existing
     dispatch seam and needs no `HostApiLink` streaming machinery. Events
     are still served by the host POSTing `EventConsumeCmd`.

6. **Firmware — prefs wiring** (`firmware/main/prefs.{h,cpp}`):
   * Store `network` in the `Prefs` singleton; include in `to_proto()`,
     merge in `apply_partial()`, and (unlike `board_config`) fire the
     **live** side effect `network_apply(network)` so `pref json-set`
     joins/leaves a network without a reboot. Add a
     `const touchy_NetworkConfig &network() const` accessor.

7. **Firmware — boot + CMake** (`firmware/main/main.cpp`,
   `firmware/main/CMakeLists.txt`):
   * After `Prefs::begin()` (and after `board_init()` so the netif stack
     is ready), call `network_apply(Prefs::instance().network())` under
     `#if CONFIG_TOUCHY_WIFI`.
   * Add `net/network.cpp` + `net/http_api.cpp` to `SRCS`, `"net"` to
     `INCLUDE_DIRS`, and `REQUIRES esp_wifi esp_http_server
     esp_https_server mdns esp-tls`.
   * New `CONFIG_TOUCHY_WIFI` bool in `Kconfig.projbuild` (default `y`
     when `CONFIG_SOC_WIFI_SUPPORTED`, else `n`).

8. **Host — HTTP transport** (new
   `app/src/touchy_pad/api/_transport_http.py`):
   * `HttpTransport(Transport)` posting a **bare** serialized `Command`
     to `<base>/touchy/api/v1/command` and returning the bare `Response`
     body. Unlike `_StreamFramedTransport` there is no frame decoder —
     override the command/response methods directly (or add a thin
     `_UnframedTransport` base) since HTTP delimits the payload.
   * Plain HTTP uses `http.client`/`urllib`. HTTPS-PSK builds an
     `ssl.SSLContext` with `set_psk_client_callback` (**Python 3.13+**);
     on older Pythons raise `TransportError("HTTPS-PSK requires Python
     3.13+")`.
   * A `parse_api_url()` helper accepting `http://host[:port]` /
     `https://host[:port]` (defaults 80/443), analogous to
     `parse_sim_url`.

9. **Host — `touchy_open` + CLI selector**
   (`app/src/touchy_pad/api/device.py`, `cli.py`):
   * `touchy_open(url=None, tls_psk=None, ...)`: when `url` starts with
     `http://` / `https://`, build an `HttpTransport` (before USB/UART
     enumeration, mirroring the `TOUCHY_SIM_URL` short-circuit). `https://`
     with no `tls_psk` is an error; `tls_psk` with `http://` is an error.
   * CLI: global `--url URL` and `--tls-psk KEY` options; when `--url` is
     given, all subcommands (`board-info`, `pref …`, `screen …`) talk to
     that endpoint. Also honour a `TOUCHY_URL` env var for parity with
     `TOUCHY_SIM_URL`.

10. **Simulator — bare HTTP server** (`app/src/touchy_pad/sim/`):
    * Add an HTTP endpoint (a `http.server.ThreadingHTTPServer` on port
      **8083**, or extend `SimServer`) exposing
      `POST /touchy/api/v1/command` that calls
      `SimDevice.handle_command(body) -> bytes`. No TLS.
    * Start it alongside the existing TCP `SimServer` when the simulator
      runs (guarded so a port clash is a warning, not a crash).

11. **Rust mirror** (`rust/touchy-pad`) — the regenerated `prost`
    bindings cover `NetworkConfig`. Add a `Touchy::set_network(...)`
    helper and an HTTP transport (`transport_http.rs`, `reqwest` behind an
    `http` feature) only if a caller needs it — otherwise defer (grep;
    likely optional this stage).

12. **Tests** (`app/tests/`):
    * `NetworkConfig` round-trip: build a `PreferencesFile` with a
      `network` block, assert it survives serialize → parse and that
      `file_version` handling is unchanged.
    * `HttpTransport` against the sim's 8083 endpoint (or a local
      `ThreadingHTTPServer` wrapping a `SimDevice`): `board-info`
      round-trips; `pref json-set` with a `network` block reaches the
      device.
    * A `--url http://…` CLI test against the sim HTTP endpoint.
    * HTTPS-PSK: on Python < 3.13 assert the clear "requires 3.13+"
      error; on 3.13+ a loopback PSK round-trip if feasible in CI
      (otherwise skip with a reason).

13. **Docs + memory** — new `docs/network-api.md` (endpoint, framing
    difference vs. the byte-stream transports, TLS-PSK setup, mDNS
    naming, port table); update `docs/host-api.md`, `docs/companion-app.md`,
    the CLI reference, and `README`s; add an `AGENTS.md`/`CLAUDE.md` note
    covering `NetworkConfig`, `PreferencesFile.Version.CURRENT == 7`, the
    HTTP transport's unframed payload, the `--url`/`--tls-psk` selectors,
    and the sim's 8083 endpoint.

### Deferred / out of scope

* **Persistent `TCPLink`** (a framed byte-stream link registered in the
  Stage lb5 `HostApiLink` array) — the HTTP request/response server
  covers the near-term need; a streaming socket link with push events is
  future work.
* **WiFi provisioning UX** (SoftAP/BLE onboarding, captive portal) —
  credentials arrive via `pref json-set` / `from-template` over an
  existing USB/UART link for now.
* **WiFi AP mode**, static IP, enterprise/EAP auth, and multi-AP roaming.
* **Certificate-based TLS** (server certs / mutual TLS) — only TLS-PSK is
  supported this stage.
* **Server-pushed events** (WebSocket / SSE / long-poll) — events stay
  `EventConsumeCmd`-polled.
* **Secret redaction / at-rest encryption** of `wifi_psk` / `tls_psk_key`.
* Simulator HTTPS/TLS — the sim is plaintext-HTTP-only.

## lb9: secure the network API with mutual TLS (mTLS)

**Status: implemented (host + firmware; firmware builds via `just
firmware-build`).** `NetworkConfig.tls_psk_key` was removed (tag 4
reserved; `PreferencesFile.Version` 7→8). mTLS certs are uploaded as files
(`F:tls/server.crt`, `F:tls/server.key`, `F:tls/client_ca.crt`) via the
normal FileWrite API by `touchy pref provision-mtls`, which generates a
one-shot EC P-256 CA + device + client certs with `cryptography`
(`app/src/touchy_pad/api/mtls.py`), pushes the device trio over USB, and
saves the host client cert/key + CA under the user config dir. Firmware
`net/http_api.cpp` reads those PEMs and, when present, starts
`esp_https_server` with `servercert`/`prvtkey_pem`/`cacert_pem` (→
`MBEDTLS_SSL_VERIFY_REQUIRED`, i.e. client-cert required) on 443 and skips
the plaintext port; `net/network.cpp` picks HTTPS-vs-HTTP by cert-file
presence. Host `HttpTransport` builds the mTLS `ssl.SSLContext`
(`load_cert_chain` + CA verify, `check_hostname=False`); the lb8
`--tls-psk` flag is gone. `just app-test` + `just app-lint` pass. Decisions
locked from discussion: single client cert, long-lived certs (re-provision
to rotate/revoke), USB-only recovery, sim stays plaintext, plaintext until
provisioned then mTLS-only, `cryptography` a hard dependency, certs stored
as files (option B).

**Status: planning / for discussion.** This supersedes the TLS-PSK idea
from lb8, which is a dead end: ESP-IDF 6.0.2's `esp_https_server` does not
expose `psk_hint_key` on its public `httpd_ssl_config_t`, so there is no
way to stand up a PSK-authenticated HTTPS server from application code
(the PSK field exists only on the lower-level `esp_tls_cfg_server_t`, which
`esp_https_server` does not let us reach). Certificate-based mutual TLS
**is** fully supported on 6.0.2 and gives us the same "only clients we
provisioned can connect" guarantee, so we pivot to that.

### What we're trying to achieve

Right now the lb8 network API is plaintext HTTP: anyone who can reach the
device on the LAN can drive it. We want: **after the user provisions the
device once (over the trusted USB cable), the device only accepts HTTPS
connections from clients that hold a credential we handed out, and the
client also refuses to talk to any device that isn't ours.** No passwords,
no public Certificate Authorities, no cloud.

### mTLS in one paragraph (for the security-noob embedded dev)

Ordinary "https://" (one-way TLS) is how your browser talks to a bank: the
**server** proves its identity with a certificate signed by a public
Certificate Authority (CA) your OS already trusts, and the client stays
anonymous. **Mutual** TLS (mTLS) adds the second direction: the **client**
must *also* present a certificate, and the server checks it. For a private
gadget we don't want public CAs at all — instead we run our **own** tiny
one-off CA. Think of the CA as a rubber stamp: we mint one stamp, use it to
sign exactly two ID cards (one for the device, one for the host client),
then we could even throw the stamp away. At connection time:

* the **device** trusts "anything my CA stamped" and checks the client's
  ID card against it → strangers without a stamped card are rejected at the
  TLS handshake, before any HTTP is parsed;
* the **client** trusts the same CA and checks the device's ID card → it
  won't send commands to an impostor device.

Concretely there are three PEM blobs and two private keys:

| Blob                | Lives on | Purpose                                        |
|---------------------|----------|------------------------------------------------|
| CA certificate      | both     | the "rubber stamp" both sides verify against   |
| server cert + key   | device   | the device's ID card (+ its secret half)       |
| client cert + key   | host     | the host's ID card (+ its secret half)         |

The device never needs the CA's *private* key (only the host, at
provisioning time, to sign cards). The device stores: its own server
cert+key, and the CA cert (to verify clients). The host stores: its own
client cert+key, and the CA cert (to verify the device).

### Why this works on ESP-IDF 6.0.2 (verified)

`httpd_ssl_config_t` (the `esp_https_server` config) exposes exactly the
fields we need: `servercert`/`servercert_len`, `prvtkey_pem`/`prvtkey_len`,
and `cacert_pem`/`cacert_len`. When `cacert_pem` is set, esp-tls calls
`mbedtls_ssl_conf_authmode(MBEDTLS_SSL_VERIFY_REQUIRED)` — i.e. the server
**requires and verifies** a client certificate signed by that CA. That is
mTLS, out of the box, no PSK needed. (Confirmed in
`components/esp-tls/esp_tls_mbedtls.c`.) On the host, Python's stdlib
`ssl` has done cert-based mTLS since forever — so, unlike the PSK path,
**no Python 3.13 requirement**.

### Provisioning flow (all over the trusted USB link)

1. Host CLI (`touchy pref provision-mtls`) generates, in memory, using the
   `cryptography` library: a throwaway CA, a device server cert+key, and a
   host client cert+key (all signed by the CA).
2. Over USB it writes the **device's** three items (server cert, server
   key, CA cert) to the device — see the storage decision below.
3. Locally it saves the **host's** items (client cert+key + CA cert) under
   `~/.config/touchy/mtls/<device-id>/` so later `touchy --url https://…`
   invocations authenticate automatically.
4. The device, on its next network bring-up (or immediately, since prefs
   apply live), sees it now has a full cert set and starts **HTTPS with
   mTLS** on 443 instead of plaintext HTTP on 80.

After step 4 the plaintext port is gone; only mTLS clients get in.

### Key decisions (locked in)

* **mTLS, not server-only TLS.** Server-only TLS would encrypt traffic but
  let any LAN client issue commands. We want client authentication, which
  is the whole point.
* **Our own single-purpose CA, generated host-side per device.** No public
  CAs, no shared secret to leak. The CA private key never touches the
  device and can be discarded after provisioning (we don't need to mint
  more cards unless the user re-provisions).
* **`check_hostname = False` on the client, but full chain verification.**
  A gadget's IP/mDNS name changes; pinning a hostname is brittle. We still
  cryptographically verify the device cert was signed by our CA — that's
  the identity guarantee that matters here. (We can optionally stuff the
  serial/hostname into the cert's SAN for defence-in-depth later.)
* **`tls_psk_key` (lb8) is removed/repurposed.** It never worked; the
  firmware already stubs HTTPS out. We replace it with the cert material.
* **Provisioning only over USB/UART (a physically-trusted link).** We do
  not expose a "set my certs" path over the network itself (that would be
  a bootstrap hole). This matches lb8's "credentials arrive over an
  existing local link" stance.

### The one big open question — where do the certs live?

PEM certs are large (~600 B–2 KB each; ~4 KB total). Two options:

* **(A) In `NetworkConfig` as nanopb `FT_POINTER` (heap) `bytes` fields.**
  Most literally matches the user's "provision by writing preferences"
  ask. Downsides: the prefs encode/save buffer (currently a 512-byte stack
  buffer in `prefs.cpp`) must grow to ~8 KB, `PreferencesFile` gains heap
  fields (pb_release handling), and every unrelated prefs save now
  serialises ~4 KB to flash. `json-get`/`json-set` would dump big base64
  blobs.
* **(B) As three files on LittleFS via the existing FileWrite API**
  (`F:tls/server.crt`, `F:tls/server.key`, `F:tls/client_ca.crt`), with
  `NetworkConfig` holding just a small `bool https_mtls` (or we infer
  "enabled" from the files existing). Reuses the streaming file-upload
  machinery certs are naturally suited to, keeps prefs tiny, and the
  firmware reads the PEMs at network bring-up (fs_init runs before
  network_apply). Keys sit in the same flash as everything else — no worse
  than option A.

**Recommendation: (B).** Certs are files; treat them as files. The
"provision by writing preferences" intent is still satisfied — the *enable
flag* is a preference, and provisioning is still one CLI command over USB;
only the bulky blobs ride the file API instead of the prefs blob. Flag for
discussion.

### Other open questions to settle before coding

1. **Client-cert lifetime / revocation.** Simplest: certs are long-lived
   (e.g. 10 years) and "revocation" = re-provision (new CA, new cards,
   old ones stop verifying). Good enough? Or do we want per-host client
   certs so one laptop can be revoked without re-flashing the others? *** ok
2. **Multiple host clients.** One client cert shared across a user's
   machines, or a `provision-mtls --add-client` that mints extra cards 
   signed by the stored CA? (Requires keeping the CA key host-side.) *** no need for mult clients
3. **Losing the client creds = lockout.** If `~/.config/touchy/mtls/…` is
   deleted, the only recovery is re-provisioning over USB. Acceptable
   (USB is the trusted path anyway), but worth stating loudly. *** ok
4. **Simulator.** Keep the sim plaintext-HTTP-only (no TLS), as in lb8? *** yes
   (Recommend yes — the sim is a dev convenience, not a security surface.)
5. **Do we keep a plaintext HTTP option at all**, or is the network API
   HTTPS-mTLS-or-nothing once WiFi is on? (Recommend: plaintext until
   provisioned, mTLS-only after — same shape as lb8's psk gate.) *** yes
6. **`cryptography` as a host dependency.** Adds a compiled dependency to
   the `touchy` CLI. Acceptable, or gate it behind an extra
   (`pip install touchy-pad[mtls]`)? *** depend onf crypto always

### Sketch of the work (to firm up after the discussion)

* **Proto:** drop `NetworkConfig.tls_psk_key`; add either the cert `bytes`
  fields (option A) or a `bool https_mtls` (option B). Bump
  `PreferencesFile.Version`.
* **Host — provisioning:** `touchy pref provision-mtls` (generate CA +
  device + client certs with `cryptography`; push device certs over
  USB via FileWrite/prefs; save host certs locally). A
  `--add-client` variant later if we decide on multi-client.
* **Host — client:** teach `HttpTransport` to build an mTLS
  `ssl.SSLContext` (`load_cert_chain(client)` + `load_verify_locations(ca)`
  + `verify_mode=CERT_REQUIRED`, `check_hostname=False`), auto-loading the
  saved creds by device id; `--url https://…` "just works" after
  provisioning. Retire the `--tls-psk` flag.
* **Firmware:** in `net/http_api.cpp`, when the cert set is present, start
  `esp_https_server` with `servercert`/`prvtkey_pem`/`cacert_pem` set
  (→ mTLS required) on 443 and skip the plaintext server; read the PEMs
  from files (option B) or prefs (option A). `net/network.cpp` picks
  HTTPS-vs-HTTP based on the enable flag.
* **Sim:** unchanged (plaintext HTTP only).
* **Docs:** rewrite the TLS section of `docs/network-api.md` around mTLS +
  the provisioning command; update `AGENTS.md`/`CLAUDE.md`.

### Deferred / out of scope

* Public-CA / Let's-Encrypt style certs, ACME, OCSP/CRL revocation.
* Certificate rotation without re-provisioning.
* mTLS for the simulator.
* Storing the CA private key on the device (we deliberately never do this).
* Network-side provisioning (certs only ever arrive over USB/UART).

## stage lb10: tiled panels

**Status: implemented (host + firmware code written; firmware not compiled
on real ESP-IDF in this environment).** Proto reshape (`Panel` wiring flags
+ `gpio` removed, new `PanelChain`, `Display.panels`→`Display.chains`,
`Version` 8→9) + Rust mirror, nanopb caps (`Display.chains max_count:1`,
`PanelChain.panels max_count:4`), the `led_chain_config()` accessor, the
`LEDPanel` tile / `LEDChain` composite split (with an `inline`
`serpentine_index`), the tiled `led_display.cpp` build, migrated + new JSON
templates, and tests are all in place. `just build-proto`, `just app-test`,
and `just app-lint` pass. Generalise the Stage lb6 `BoardConfig` so a single
data GPIO can drive a *chain* of small LED matrices tiled into one larger
logical display, and promote the compile-time serpentine-wiring macros
(`LED_ROWS_SNAKED` / `LED_COLS_SNAKED` / `LED_ROW_MAJOR`) into per-`Panel`
protobuf flags (plus two new `cols_flipped` / `rows_flipped` flags). This
is the first stage that actually renders across more than one physical
matrix; the LVGL side still sees exactly one `Display`.

### Background

* Today `BoardConfig` is `displays[<=1] → Display.panels[<=1] → Panel{width,
  height, gpio}` — a single matrix on a single GPIO
  ([proto/preferences.proto](../../../proto/preferences.proto), capped in
  [proto/preferences.options](../../../proto/preferences.options#L8-L16)).
* The firmware reads it through the proto-free accessor
  `led_panel_config(int*w, int*h, int*gpio)` in
  [firmware/main/prefs.cpp](../../../firmware/main/prefs.cpp) (keeps nanopb
  out of the board component), and
  [firmware/boards/common/leds/led_display.cpp](../../../firmware/boards/common/leds/led_display.cpp)
  builds one `LEDPanel` from it (headless when unset).
* `LEDPanel::serpentine_index(x, y)`
  ([firmware/boards/common/leds/led_panel.cpp](../../../firmware/boards/common/leds/led_panel.cpp#L61-L79))
  maps a logical `(x, y)` to a physical LED index using three
  **compile-time** switches — and two of them are currently hard-wired in
  the `.cpp` (`LED_COLS_SNAKED` is `#define`d to 1 inline; `LED_ROWS_SNAKED`
  / `LED_ROW_MAJOR` are `#ifdef`-absent). No board actually sets them, so
  the wiring is effectively frozen. This stage makes them data.
* The two shipped templates
  ([led-32x8.json](../../../app/src/touchy_pad/assets/templates/led-32x8.json),
  [neopixel-1.json](../../../app/src/touchy_pad/assets/templates/neopixel-1.json))
  encode the old single-panel shape and must migrate.

### Decisions (locked in)

* **Chain, not recursion.** A `PanelChain` owns `repeated Panel panels`
  (max 4) plus the single `gpio` for the whole chain and a `tile_by_row`
  flag. The self-recursive `repeated PanelChain panels` in the original
  sketch was a typo. `gpio` moves off `Panel` onto `PanelChain`.
* **`Display` still holds exactly one `PanelChain`** (capped
  `max_count:1`). Multiple chains per display (and multiple displays) stay
  deferred — the near-term goal is one LVGL surface tiled from one chain.
* **Wiring flags are per-`Panel`** so a chain may mix differently-wired
  matrices. Five flags: `rows_snaked`, `cols_snaked`, `row_major`,
  `cols_flipped`, `rows_flipped`.
* **`cols_snaked` defaults *true*; the rest default *false*.** proto3
  bools default false, so `cols_snaked` is an `optional bool` whose
  **unset** state means `true` (present-but-`false` = explicitly straight
  columns). This preserves today's behaviour (`LED_COLS_SNAKED == 1`,
  others off) for a template that sets no flags. The other four are plain
  `bool` (default false).
* **`tile_by_row` defaults false → horizontal tiling.** 3×`{w:32,h:8}`
  tiled horizontally = a 96×8 display; `tile_by_row=true` = a 32×24
  display. Panels are laid out in **chain order** (panel 0 is the first
  matrix on the data line).
* **`PreferencesFile.Version` bumps 8 → 9** (house convention on any
  prefs-schema change); both templates migrate to the new shape.

### Proto shape (`proto/preferences.proto` + Rust mirror)

```proto
// One physical LED matrix, one link in a data-line chain. Origin (0,0) is
// the panel's top-left; the flags describe how that logical grid is wired
// to the physical LED order within this panel.
message Panel {
    uint32 width  = 1;              // logical pixel columns
    uint32 height = 2;              // logical pixel rows
    // gpio moved to PanelChain (tag 3 retired — see `reserved` below).

    bool          rows_snaked  = 4; // odd rows run right→left (default false)
    optional bool cols_snaked  = 5; // odd cols run bottom→top (UNSET ⇒ true)
    bool          row_major     = 6; // index = row*width+col (default false ⇒ col-major)
    bool          cols_flipped = 7; // mirror X across the whole panel
    bool          rows_flipped = 8; // mirror Y across the whole panel

    reserved 3;                      // was: uint32 gpio (moved to PanelChain)
}

// A chain of up to 4 panels sharing one data GPIO, tiled into one surface.
message PanelChain {
    repeated Panel panels = 1;       // chain order; max_count:4
    uint32   gpio         = 2;       // data GPIO for the whole chain
    bool     tile_by_row  = 3;       // false ⇒ tile horizontally (default)
}

// One LVGL display surface. For now exactly one PanelChain (capped in
// preferences.options); multi-chain / multi-display tiling stays deferred.
message Display {
    repeated PanelChain chains = 1;  // max_count:1
}

message BoardConfig {
    repeated Display displays = 1;   // max_count:1 (unchanged)
}
```

nanopb caps (`proto/preferences.options`): retire the old
`touchy.Display.panels` line; add
```
touchy.Display.chains       max_count:1
touchy.PanelChain.panels    max_count:4
```
(`touchy.BoardConfig.displays max_count:1` unchanged). All still
`FT_STATIC` — no heap.

### `serpentine_index` — data-driven, branch-friendly

Promote the three macros to `const` members set once in the `LEDPanel`
ctor (from the `Panel` proto), add the two flips, and keep the per-pixel
math a straight-line sequence of predictable branches on `const` bools so
the compiler can hoist/inline in the hot flush loop:

```cpp
// members, all const, initialised from the Panel proto in the ctor:
//   const bool _rows_snaked, _cols_snaked, _row_major, _cols_flipped, _rows_flipped;

int LEDPanel::serpentine_index(int x, int y) const
{
    // 1. whole-panel mirror (cheap, before any snake math).
    if (_cols_flipped) x = _width  - 1 - x;
    if (_rows_flipped) y = _height - 1 - y;

    // 2. row snaking: odd rows reversed left↔right.
    const int col = (_rows_snaked && (y & 1)) ? (_width - 1 - x) : x;

    // 3. column snaking: odd cols reversed top↔bottom.
    const int row = (_cols_snaked && (col & 1)) ? (_height - 1 - y) : y;

    // 4. major order.
    return _row_major ? (row * _width + col) : (col * _height + row);
}
```

The old inline `#define LED_COLS_SNAKED 1` and the `#ifdef LED_ROWS_SNAKED`
/ `#ifdef LED_ROW_MAJOR` blocks are deleted; the default
`Panel{}`-derived flags (`cols_snaked` unset⇒true, others false) reproduce
today's index exactly.

### Tiling model

A `PanelChain` becomes a composite the LED display owns: a vector of
`LEDPanel` sub-matrices, each with an `(x_off, y_off)` in the logical
surface and a `base` offset into the physical strip (running sum of prior
panels' pixel counts, since the data line visits panel 0 fully, then panel
1, …). Total logical size:

* `tile_by_row == false` (horizontal): `W = Σ panel.width`,
  `H = max panel.height`.
* `tile_by_row == true` (vertical): `W = max panel.width`,
  `H = Σ panel.height`.

`Panel::set_pixel(x, y, …)` on the composite picks the panel whose tile
range contains `(x, y)`, subtracts its offset, and writes
`strip[base + panel.serpentine_index(localx, localy)]`. Guard against
zero/absurd totals exactly as the Stage lb6 single-panel path does.

### Definition of done

* `Panel` carries the five wiring flags (gpio removed, tag 3 reserved);
  `PanelChain{panels[<=4], gpio, tile_by_row}` exists; `Display` holds
  `repeated PanelChain chains` (`max_count:1`); `PreferencesFile.Version
  == 9`. `just build-proto` regenerates Python `_proto`, C nanopb, and
  Rust `prost` cleanly.
* `LEDPanel::serpentine_index` is fully data-driven from `const` members;
  no `LED_*_SNAKED` / `LED_ROW_MAJOR` macros remain in the tree.
* The LED display builds a tiled composite from
  `board_config.displays[0].chains[0]`; a horizontal 3×`{32,8}` chain
  presents a 96×8 surface and `board-info` reports `96×8`. A single-panel
  chain behaves exactly as Stage lb6 (headless when unconfigured).
* Both shipped templates migrate to the `PanelChain` shape and still round
  trip; a new multi-panel example template ships.
* `just app-test` + `just app-lint` pass; the two LED boards build via
  `just firmware-build`.

### Work items

1. **Proto** — rewrite `Panel` (add flags, `reserved 3`), add
   `PanelChain`, change `Display.panels` → `Display.chains` in
   `proto/preferences.proto` **and** the Rust mirror
   `rust/touchy-pad/proto/preferences.proto`; bump the `Version` enum
   (`V9 = 9; CURRENT = 9;`).
2. **nanopb options** — swap the `Display.panels` cap for
   `Display.chains max_count:1` and add `PanelChain.panels max_count:4`.
3. **Regenerate** — `just build-proto` (Python + C nanopb + Rust prost).
4. **Accessor** — replace `led_panel_config(w,h,gpio)` in
   `firmware/main/prefs.cpp` with a richer proto-free descriptor that
   yields the chain: gpio, `tile_by_row`, and per-panel
   `{w, h, rows_snaked, cols_snaked, row_major, cols_flipped, rows_flipped}`
   (a small POD struct declared in `led_display.h`, keeping nanopb out of
   the board component). Handle `cols_snaked` presence (unset ⇒ true) here.
5. **`LEDPanel`** — add the five `const bool` flag members + ctor args;
   rewrite `serpentine_index` per the block above; delete the macros
   (`led_panel.cpp` / any `board_pins.h`).
6. **Composite / tiling** — teach
   `firmware/boards/common/leds/led_display.cpp` (`LEDMatrixDisplay::hw_init`)
   to build the vector of offset `LEDPanel`s from the chain, compute total
   dims from `tile_by_row`, size the LVGL draw buffer, and route
   `set_pixel` through the tile lookup + per-panel base offset. Keep the
   geometry/`malloc` guards.
7. **Templates** — migrate
   `app/src/touchy_pad/assets/templates/led-32x8.json` and
   `neopixel-1.json` to `{boardConfig:{displays:[{chains:[{gpio, panels:[…]}]}]}}`
   with `fileVersion: "V9"`; add a worked multi-panel example (e.g.
   `led-96x8-chain.json` = three 32×8 panels, `tileByRow:false`).
8. **Rust** — the regenerated prost covers it; add a
   `Touchy::set_board_config` helper only if a caller needs it (grep;
   likely skip).
9. **Simulator** — if the sim renders the LED display, mirror the tiling +
   flag math; otherwise note it as unaffected.
10. **Tests** (`app/tests/`) — template round-trip for the new shape;
    assert a 3-panel horizontal chain parses to the expected panels/gpio
    and survives serialise→parse; a `from-template` CLI test.
11. **Docs + memory** — update the `PreferencesFile.Version.CURRENT == 9`
    line and the `BoardConfig`/`Panel` description in
    `AGENTS.md`/`CLAUDE.md`; document the chain/tiling model and the
    per-panel wiring flags here.

### Deferred / out of scope

* Multiple `PanelChain`s per `Display`, and multiple `Display`s — the
  `max_count:1` caps hold the one-surface assumption.
* Non-uniform tile grids (2-D mosaics), per-panel rotation, and inter-panel
  gaps/margins — tiling is a single row or single column of panels.
* Colour order / gamma / brightness curves — unchanged from today.
* Live reconfiguration without a reboot (`board_config` is still read once
  at `display_init()`).