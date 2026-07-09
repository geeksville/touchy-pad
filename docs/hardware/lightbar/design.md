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

