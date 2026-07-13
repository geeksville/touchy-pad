# touchy-pad — ESP-IDF firmware

ESP-IDF firmware with first-class support for
multiple ESP32 / ESP32-S3 touch-LCD development boards.

Each board declares its own IDF chip target in a one-line
`boards/<BOARD>/target` file (e.g. `esp32s3` or `esp32`), so the build
system supports a mix of native-USB (ESP32-S3) and UART-only
(classic ESP32) boards.

## Supported boards

| `BOARD=` value          | Chip    | Hardware                                                                 | Display                              | Touch | Host link | Status      |
|-------------------------|---------|--------------------------------------------------------------------------|--------------------------------------|-------|-----------|-------------|
| `waveshare_s3_lcd_7b`   | esp32s3 | [Waveshare ESP32-S3-Touch-LCD-7B](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-7B) (N16R8, 16 MB flash, 8 MB OPI PSRAM) | 800×480 ST7262 16-bit parallel RGB   | GT911 | USB | Builds; tested incomplete |
| `jc4827w543`            | esp32s3 | "JC4827W543" 4.3-inch ESP32-S3 board (4 MB flash, 8 MB OPI PSRAM)         | 480×272 NV3041A 4-line QSPI IPS      | GT911 | USB | Builds;      |
| `esp32_2432s028rv3`     | esp32   | "CYD2USB" ESP32-2432S028R v3 (classic ESP32, 4 MB flash, **no PSRAM**)    | 320×240 ST7789 SPI                   | XPT2046 (resistive, single-touch) | UART (CH340 `/dev/ttyUSB*`) | Builds; HW bring-up |
| `esp32_2432s024`        | esp32   | "CYD2USB" ESP32-2432S024 2.4" (classic ESP32, 4 MB flash, **no PSRAM**)   | 320×240 ILI9341 SPI                  | XPT2046 (resistive, single-touch) | UART (CH340 `/dev/ttyUSB*`) | Builds; HW bring-up |

The default is `jc4827w543`. The CYD boards have no native USB: the protobuf
protocol rides UART0 @ 115200 and there is no HID emulation. The CYD family
(`esp32_2432s028rv3`, `esp32_2432s024`, …) shares one set of C++ sources in
`boards/cyd_common/`; each board contributes only its `board_pins.h`. The
display controller (ST7789 vs ILI9341) is selected at compile time from
`board_pins.h`.

What it does today (parity with v1 Stage 10):

- Brings up the panel (RGB parallel or QSPI depending on board).
- Brings up the touch controller and feeds events into LVGL.
- Drives LVGL through `esp_lvgl_port`, RGB565.
- On native-USB (ESP32-S3) boards, enumerates over the USB-OTG port as a
  **USB-HID mouse** (TinyUSB, VID/PID = `0x303A / 0x8369`). On UART-only
  boards (classic ESP32) HID is unavailable and the host link is the
  protobuf protocol over UART0.
- Implements `TrackpadWidget` with the v1 gesture rules:
  - 1-finger tap → left click
  - 2-finger tap → right click
  - 3-finger tap → middle click
  - 1-finger drag → mouse move

---

## Project layout

```
firmware/
├── CMakeLists.txt              # selects BOARD, layers sdkconfig.defaults
├── sdkconfig.defaults          # shared defaults (USB/USJ/LVGL/log)
├── README.md                   # ← this file
├── partitions/                 # shared partition tables, one per flash size
│   ├── 4M.csv                  # 4 MB layout (jc4827w543)
│   └── 16M.csv                 # 16 MB layout (waveshare_s3_lcd_7b)
├── boards/
│   ├── waveshare_s3_lcd_7b/    # 7" RGB-parallel Waveshare board
│   │   ├── sdkconfig.defaults  # PSRAM-OPI, 16 MB QIO flash, partition table
│   │   └── board/              # ← ESP-IDF component named `board`
│   │       ├── CMakeLists.txt
│   │       ├── idf_component.yml
│   │       ├── board.cpp       # CH422G IO expander, I2C, resets, backlight
│   │       ├── board_pins.h    # GPIO map (private)
│   │       ├── display.cpp     # esp_lcd_panel_rgb + esp_lvgl_port
│   │       └── touch.cpp       # GT911 + LVGL indev
│   └── jc4827w543/             # 4.3" NV3041A QSPI board
│       ├── target              # one-line IDF chip: "esp32s3"
│       ├── sdkconfig.defaults  # PSRAM-OPI, 4 MB QIO flash, partition table
│       └── board/              # ← ESP-IDF component named `board`
│           ├── CMakeLists.txt
│           ├── idf_component.yml
│           ├── board.cpp       # shared-I2C bus init only
│           ├── board_pins.h    # GPIO map (private)
│           ├── display.cpp     # NV3041A + LVGL flush_cb
│           ├── nv3041a.{h,c}   # standalone QSPI panel driver
│           └── touch.cpp       # GT911 + LVGL indev
│   ├── cyd_common/             # shared C++ for the whole "CYD" family
│   │   ├── board.cpp           # platform_get() {multitouch:false, usb:false}
│   │   ├── display.cpp         # ST7789/ILI9341 SPI + esp_lvgl_port
│   │   └── touch.cpp           # XPT2046 resistive + LVGL indev
│   ├── esp32_2432s028rv3/      # classic-ESP32 CYD2USB 2.8", ST7789
│   │   ├── target              # one-line IDF chip: "esp32"
│   │   ├── sdkconfig.defaults  # 4 MB flash, proto-over-UART0, no PSRAM
│   │   └── board/              # ← ESP-IDF component named `board`
│   │       ├── CMakeLists.txt     # compiles ../../cyd_common/*.cpp
│   │       ├── idf_component.yml  # atanisoft/esp_lcd_touch_xpt2046
│   │       └── board_pins.h    # GPIO map + BOARD_LCD_CONTROLLER_ST7789
│   └── esp32_2432s024/         # classic-ESP32 CYD2USB 2.4", ILI9341
│       ├── target              # one-line IDF chip: "esp32"
│       ├── sdkconfig.defaults  # 4 MB flash, proto-over-UART0, no PSRAM
│       └── board/              # ← ESP-IDF component named `board`
│           ├── CMakeLists.txt     # compiles ../../cyd_common/*.cpp
│           ├── idf_component.yml  # esp_lcd_ili9341 + esp_lcd_touch_xpt2046
│           └── board_pins.h    # GPIO map + BOARD_LCD_CONTROLLER_ILI9341
└── main/                       # board-agnostic app code
    ├── CMakeLists.txt          # REQUIRES board
    ├── idf_component.yml       # common deps (lvgl, esp_lvgl_port, tinyusb, ...)
    ├── board.h                 # public API: board_init(), board_get_i2c_bus()
    ├── display.h               # public API: Display ABC + display_create()
    ├── touch.h                 # public API: esp_lcd_touch_handle_t touch_init()
    ├── main.cpp                # app_main entry point
    ├── usb_hid.{h,cpp}         # TinyUSB HID-mouse wrapper
    └── trackpad_widget.{h,cpp} # gesture engine, LVGL UI
```

### How the multi-board build works

1. The top-level `CMakeLists.txt` reads the `BOARD` CMake cache variable
   (defaults to `waveshare_s3_lcd_7b`).
2. It points `EXTRA_COMPONENT_DIRS` at `boards/<BOARD>/` so ESP-IDF picks up
   the **`board`** component nested inside (`boards/<BOARD>/board/`). Every
   board provides a component named literally `board`, so `main/` can simply
   list `board` in its `REQUIRES` and stay completely board-agnostic.
3. `SDKCONFIG_DEFAULTS` is set to a 2-element list: the common
   `sdkconfig.defaults` first, then `boards/<BOARD>/sdkconfig.defaults` on
   top — so board-specific knobs (flash size, PSRAM mode, partition CSV,
   pixel clock) override the common defaults.
4. The generated `sdkconfig` is named `sdkconfig.<BOARD>` so switching
   boards never clobbers your previous config. (These are `.gitignore`d.)
5. The compile flag `-DTOUCHY_BOARD_<board>=1` is exposed to code if it
   ever needs to `#ifdef`.

> **Never edit `sdkconfig.<BOARD>` directly.** That file is fully
> regenerated by the build system on every `idf.py build` run; any hand
> edits will be silently discarded. To persist a Kconfig flag:
> * **Shared across all boards** → `firmware/sdkconfig.defaults`
> * **One board only** → `firmware/boards/<BOARD>/sdkconfig.defaults`
>
> Both files are committed to git and are the canonical place for build
> configuration.

### Adding a new board

1. Create `boards/<name>/board/` with a `CMakeLists.txt` that registers a
   component called `board` and implements `board_init()`, a `Display`
   subclass returned from `display_create()`, and `touch_init()`
   (signatures in `main/board.h`, `main/display.h`, `main/touch.h`).
2. Add `boards/<name>/target` — a one-line file naming the IDF chip
   (`esp32s3`, `esp32`, …). `just firmware-reconfigure <name>` reads it.
3. Add `boards/<name>/sdkconfig.defaults` with flash/PSRAM/partition
   overrides, referencing the appropriate `partitions/<SIZE>.csv` (add a new
   one to `firmware/partitions/` if the flash size differs).
4. Add any board-specific managed-component deps to
   `boards/<name>/board/idf_component.yml`.
5. Build with `just firmware-reconfigure <name> && just firmware-build`
   (or `idf.py -DBOARD=<name> set-target <chip> && idf.py build`). Note the
   `-DBOARD` is required on `set-target`, and `rm -f firmware/sdkconfig`
   first when switching to a different chip.
6. Add a row to the table at the top of this file.

> **CYD family:** if the new board is another "Cheap Yellow Display" variant,
> don't write fresh `board.cpp`/`display.cpp`/`touch.cpp` — reuse the shared
> sources in `boards/cyd_common/`. Have `board/CMakeLists.txt` compile
> `../../cyd_common/*.cpp` (with `PRIV_INCLUDE_DIRS "."` so the board's own
> `board_pins.h` is found first) and contribute only `board_pins.h`. Select
> the panel driver there via `BOARD_LCD_CONTROLLER_ST7789` or
> `BOARD_LCD_CONTROLLER_ILI9341`. See `esp32_2432s028rv3` (ST7789) and
> `esp32_2432s024` (ILI9341) for the two-file pattern.

---

## Prerequisites

- **ESP-IDF v5.3 or later** (tested with v6.0.1). For the classic-ESP32
  `esp32_2432s028rv3` board, install both toolchains: `./install.sh
  esp32,esp32s3`.
- A connected board from the table above.

In every shell where you build:

```bash
. $HOME/esp/esp-idf/export.sh    # path depends on your install location
```

---

## Build / flash

```bash
cd firmware

# Set the board once per checkout (writes BOARD to CMakeCache).
idf.py -DBOARD=waveshare_s3_lcd_7b set-target esp32s3       # 7" Waveshare
# or
idf.py -DBOARD=jc4827w543          set-target esp32s3       # 4.3" JC4827W543
# or (classic-ESP32 CYD boards)
idf.py -DBOARD=esp32_2432s028rv3   set-target esp32         # 2.8" CYD2USB (ST7789)
idf.py -DBOARD=esp32_2432s024      set-target esp32         # 2.4" CYD2USB (ILI9341)

# Build, flash, monitor (UART USB-C, not the OTG one).
idf.py build
idf.py -p /host/dev/ttyACM0 flash monitor
```

After flashing, plug the **OTG** USB-C port of the board into the host
computer. You should see VID `0x303A` / PID `0x8369` ("Touchy-Pad") show
up as a HID mouse.

### Switching boards

```bash
# Clean build dir is required to switch boards (CMakeCache pins BOARD).
rm -rf build sdkconfig.<old-board>
idf.py -DBOARD=<new-board> set-target esp32s3
idf.py build
```

The two `sdkconfig.<board>` files happily coexist on disk so you don't lose
menuconfig tweaks when bouncing between boards.

---

## efuse setting (one-time, recommended)

For reliable USB-OTG enumeration the built-in USB-Serial/JTAG controller on
GPIO 19/20 must be disabled — it otherwise tries to share those pins with
TinyUSB and "wins" briefly at boot. Burn it out permanently:

```bash
espefuse.py --port /dev/ttyACM0 burn_efuse DIS_USB_SERIAL_JTAG
```

> ⚠️  This efuse is **irreversible**. After burning, you can no longer
> flash via the native USB port or use it as a serial console; flashing
> must always go through the CH343 UART port. That's already how this
> project flashes, so this is generally what you want.

---

## Debug

The ESP32-S3's built-in USB-Serial-JTAG bridge is disabled by the efuse
above, so debugging through the OTG port is not possible. Use an external
JTAG probe (e.g. ESP-Prog) and `idf.py openocd` + `idf.py gdb` if you need
in-system debugging. Core dumps over UART work as usual:

```bash
idf.py coredump-info
```

---

## Known issues / caveats

- **jc4827w543 is untested on hardware.** The NV3041A QSPI driver was
  authored from the [Arduino_GFX reference](https://github.com/moononournation/Arduino_GFX/blob/master/src/display/Arduino_NV3041A.cpp);
  bring-up may need rotation/byte-swap / clock-speed tweaks.
- **No CDC over the OTG port.** TinyUSB is configured HID-only so the host
  sees a pure mouse. Get logs via the UART USB-C connector.
- **LVGL pinned to v8.4** to match v1. Bump
  [`main/idf_component.yml`](main/idf_component.yml) for v9.
