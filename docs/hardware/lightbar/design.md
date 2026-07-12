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

## stage LB2 — 2nd LED-matrix board (`feather_esp32_s3`)

**Status: implemented (firmware not yet compiled on real ESP-IDF in this
environment).** Shared `Panel`/`LEDPanel` code was relocated from
`firmware/boards/common/` to `firmware/main/leds/` (compiled by the board
component, not `main`, so the `espressif/led_strip` dependency stays
board-scoped); `jc_esp32p4_m3` was repointed at the new location. The new
`firmware/boards/feather_esp32_s3/` board (target `esp32s3`, LED on
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
  `docs/hardware/feather_esp32_s3/README.md`.
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

2. **Board scaffold** — create `firmware/boards/feather_esp32_s3/`,
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
   feather_esp32_s3` reads the `target` file and runs `set-target
   esp32s3`, and that `just firmware-build` / `just flash` work for the
   new board. Add an `sdkconfig.feather_esp32_s3` snapshot if the repo
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