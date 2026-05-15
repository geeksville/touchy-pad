# touchy-pad — ESP-IDF firmware

ESP-IDF firmware with first-class support for
multiple ESP32-S3 touch-LCD development boards.

## Supported boards

| `BOARD=` value          | Hardware                                                                 | Display                              | Touch | Status      |
|-------------------------|--------------------------------------------------------------------------|--------------------------------------|-------|-------------|
| `waveshare_s3_lcd_7b`   | [Waveshare ESP32-S3-Touch-LCD-7B](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-7B) (N16R8, 16 MB flash, 8 MB OPI PSRAM) | 800×480 ST7262 16-bit parallel RGB   | GT911 | Builds; tested incomplete |
| `jc4827w543`            | "JC4827W543" 4.3-inch ESP32-S3 board (4 MB flash, 8 MB OPI PSRAM)         | 480×272 NV3041A 4-line QSPI IPS      | GT911 | Builds;      |

The default is `jc4827w543`.

What it does today (parity with v1 Stage 10):

- Brings up the panel (RGB parallel or QSPI depending on board).
- Brings up the touch controller and feeds events into LVGL.
- Drives LVGL through `esp_lvgl_port`, RGB565.
- Enumerates over the USB-OTG port as a **USB-HID mouse** (TinyUSB,
  VID/PID = `0x4403 / 0x1002`).
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
├── boards/
│   ├── waveshare_s3_lcd_7b/    # 7" RGB-parallel Waveshare board
│   │   ├── sdkconfig.defaults  # PSRAM-OPI, 16 MB QIO flash, partition table
│   │   ├── partitions.csv      # 16 MB layout
│   │   └── board/              # ← ESP-IDF component named `board`
│   │       ├── CMakeLists.txt
│   │       ├── idf_component.yml
│   │       ├── board.cpp       # CH422G IO expander, I2C, resets, backlight
│   │       ├── board_pins.h    # GPIO map (private)
│   │       ├── display.cpp     # esp_lcd_panel_rgb + esp_lvgl_port
│   │       └── touch.cpp       # GT911 + LVGL indev
│   └── jc4827w543/             # 4.3" NV3041A QSPI board
│       ├── sdkconfig.defaults  # PSRAM-OPI, 4 MB QIO flash, partition table
│       ├── partitions.csv      # 4 MB layout
│       └── board/              # ← ESP-IDF component named `board`
│           ├── CMakeLists.txt
│           ├── idf_component.yml
│           ├── board.cpp       # shared-I2C bus init only
│           ├── board_pins.h    # GPIO map (private)
│           ├── display.cpp     # NV3041A + LVGL flush_cb
│           ├── nv3041a.{h,c}   # standalone QSPI panel driver
│           └── touch.cpp       # GT911 + LVGL indev
└── main/                       # board-agnostic app code
    ├── CMakeLists.txt          # REQUIRES board
    ├── idf_component.yml       # common deps (lvgl, esp_lvgl_port, tinyusb, ...)
    ├── board.h                 # public API: board_init(), board_get_i2c_bus()
    ├── display.h               # public API: lv_disp_t *display_init(void)
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

### Adding a new board

1. Create `boards/<name>/board/` with a `CMakeLists.txt` that registers a
   component called `board` and implements `board_init()`, `display_init()`,
   and `touch_init()` (signatures in `main/board.h`, `main/display.h`,
   `main/touch.h`).
2. Add `boards/<name>/sdkconfig.defaults` with flash/PSRAM/partition
   overrides, plus a `partitions.csv` if the flash size differs.
3. Add any board-specific managed-component deps to
   `boards/<name>/board/idf_component.yml`.
4. Build with `idf.py -DBOARD=<name> set-target esp32s3 && idf.py build`.
5. Add a row to the table at the top of this file.

---

## Prerequisites

- **ESP-IDF v5.3 or later** (tested with v6.0.1).
- A connected ESP32-S3 board from the table above.

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

# Build, flash, monitor (UART USB-C, not the OTG one).
idf.py build
idf.py -p /host/dev/ttyACM0 flash monitor
```

After flashing, plug the **OTG** USB-C port of the board into the host
computer. You should see VID `0x4403` / PID `0x1002` ("Touchy-Pad") show
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
