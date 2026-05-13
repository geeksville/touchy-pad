# touchy-pad v2 — ESP-IDF port (experimental)

This is an experimental re-implementation of [`firmware/`](../firmware) (the
PlatformIO + Arduino-core version) using the **raw ESP-IDF toolchain**.

Target board: **Waveshare ESP32-S3-Touch-LCD-7B** (ESP32-S3-N16R8, 16 MB flash,
8 MB octal PSRAM, 800×480 ST7262 RGB LCD, GT911 capacitive touch, CH422G I²C
IO-expander for backlight / resets).

What it does today (parity with v1 Stage 10):

- Brings up the parallel-RGB LCD via `esp_lcd_panel_rgb`.
- Brings up the GT911 multi-touch controller via `esp_lcd_touch_gt911`.
- Drives LVGL through `esp_lvgl_port` (double-buffered, anti-tearing, RGB565).
- Enumerates over the USB-OTG port as a **USB-HID mouse** (TinyUSB).
- Implements `TrackpadWidget` with the same gesture rules as v1:
  - 1-finger tap → left click
  - 2-finger tap → right click
  - 3-finger tap → middle click
  - 1-finger drag → mouse move

USB VID/PID match v1 (`0x4403 / 0x1002`, manufacturer "Geeksville").

---

## Prerequisites

- **ESP-IDF v5.3 or later.** Follow the [Espressif "Get Started" guide](https://docs.espressif.com/projects/esp-idf/en/v5.3/esp32s3/get-started/index.html)
  to install the SDK and toolchain. After install, source the export script in
  every shell where you build:

  ```bash
  . $HOME/esp/esp-idf/export.sh    # path depends on your install location
  ```

- A connected Waveshare ESP32-S3-Touch-LCD-7B. The board has **two USB-C
  connectors**:
  - The one silkscreened **UART** is the CH343 USB-serial bridge — use this
    one to flash and to receive `ESP_LOGx` output.
  - The one silkscreened **USB** is the ESP32-S3 native USB-OTG port — once
    flashed, this is what shows up to the host as the HID mouse.

In a Linux dev-container the UART side typically enumerates as `/dev/ttyACM0`.

---

## One-time setup

```bash
cd v2
idf.py set-target esp32s3
```

This generates `sdkconfig` from `sdkconfig.defaults` (PSRAM octal-8M, flash
QIO-16M @ 120 MHz, TinyUSB HID enabled, custom partition table).

If you want to tweak Kconfig options interactively:

```bash
idf.py menuconfig
```

Component dependencies (LVGL, esp_lvgl_port, esp_lcd_touch_gt911,
esp_io_expander_ch422g, esp_tinyusb) are pulled automatically from the
[Espressif Component Registry](https://components.espressif.com) on the first
build via [`main/idf_component.yml`](main/idf_component.yml). You don't need
to clone or vendor them by hand.

---

## Build

```bash
idf.py build
```

The first build will download the managed components (look for
`Solving dependencies requirements` / files appearing under
`managed_components/`). Subsequent builds are incremental.

To start from a clean slate:

```bash
idf.py fullclean
```

---

## Flash & monitor

Plug the **UART** USB-C port into your computer.

```bash
idf.py -p /host/dev/ttyACM1 flash monitor
```

(`idf.py monitor` alone also works once flashed.) Exit the monitor with
`Ctrl+]`.

If flashing fails, put the board into download mode manually: hold **BOOT**,
tap **RESET**, release **BOOT**, then re-run the flash command.

Once flashing finishes, plug the **USB** USB-C port (the OTG one) into the
host computer you want the mouse to show up on. Run `lsusb` (or check your OS
"HID devices") — you should see a device with VID `0x4403` / PID `0x1002`.

---

## Debug

The ESP32-S3 has a built-in USB-Serial-JTAG bridge, so no external JTAG probe
is needed.

In one terminal, start OpenOCD bound to the board:

```bash
idf.py openocd
```

In a second terminal:

```bash
idf.py gdb
```

This will halt at `app_main` and let you single-step, set breakpoints, etc.

For a post-mortem core dump after a panic:

```bash
idf.py coredump-info
```

---

## Project layout

```
v2/
├── CMakeLists.txt            # ESP-IDF project root
├── sdkconfig.defaults        # PSRAM, flash, TinyUSB, LVGL defaults
├── partitions.csv            # 16 MB partition layout
└── main/
    ├── CMakeLists.txt
    ├── idf_component.yml     # Component-registry deps
    ├── board.h / board.cpp           # Waveshare pin map, I2C bus, CH422G init
    ├── display.h / display.cpp       # esp_lcd_panel_rgb + esp_lvgl_port
    ├── touch.h / touch.cpp           # GT911 init + LVGL indev hookup
    ├── usb_hid.h / usb_hid.cpp       # TinyUSB HID-mouse wrapper
    ├── trackpad_widget.h / .cpp      # Ported gesture logic from v1
    └── main.cpp                      # app_main entry point
```

---

## Known issues / caveats

- **Untested on hardware.** This port was authored from datasheets and the
  upstream `esp-arduino-libs/ESP32_Display_Panel` board file; bring-up on a
  real board may reveal pin / timing tweaks.
- **No CDC over the OTG port.** TinyUSB is configured HID-only so the host
  sees a pure mouse. Get logs via the UART USB-C connector.
- **LVGL pinned to v8.4** to match v1. If you want LVGL v9, bump the version
  in `main/idf_component.yml` and adjust the LVGL API calls accordingly.

---

## Why a v2?

See the top-level [TODO.md](../TODO.md) — this is an experiment in moving off
the PlatformIO/Arduino-core stack onto the upstream Espressif toolchain so we
get faster builds, native FreeRTOS APIs, and access to newer ESP-IDF features
(USB device stack, esp_lcd_touch, esp_lvgl_port, etc.).
