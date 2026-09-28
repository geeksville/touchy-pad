# touchy-pad — ESP-IDF firmware

ESP-IDF (v6.x) firmware for a family of ESP32-S3 / ESP32-P4 / classic-ESP32
boards, each with a touch LCD, an LED matrix, or no display at all. One
board-agnostic app lives in `main/`; every board supplies a `board` component
(panel + touch bring-up, pin map, backlight) under `boards/<board>/`.

Each board declares its IDF chip target in a one-line `boards/<BOARD>/target`
file (`esp32` / `esp32s3` / `esp32p4`), so the build supports native-USB and
UART-only boards side by side. USB capability is keyed off
`CONFIG_SOC_USB_OTG_SUPPORTED` — never a custom flag.

> **Building:** prefer the repo-root `just` recipes — `just firmware-build`,
> `just firmware-reconfigure <board>`, `just flash` — which source ESP-IDF for
> you and pass `-DBOARD=`. The raw `idf.py` equivalents appear below for people
> working inside a shell that has already sourced `export.sh`.

## Supported boards

| `BOARD=` value | Chip | Hardware | Display | Touch | Host link | Notes |
|---|---|---|---|---|---|---|
| `jc4827w543` | esp32s3 | JC4827W543 4.3" (4 MB QSPI flash, 8 MB OPI PSRAM) | 480×272 NV3041A QSPI IPS | GT911 | USB | **Default `BOARD`**, best-supported; QSPI capped at 32 MHz |
| `jc4827w543r` | esp32s3 | JC4827W543 **resistive** variant | 480×272 NV3041A QSPI | XPT2046 (single-touch) | USB | Builds |
| `waveshare_s3_lcd_7b` | esp32s3 | Waveshare ESP32-S3-Touch-LCD-7B (16 MB flash, 8 MB OPI PSRAM) | 800×480 ST7262 RGB | GT911 | USB | CH422G IO expander |
| `matouch_43` | esp32s3 | MakerFabs MaTouch 4.3" (16 MB flash, 8 MB octal PSRAM) | 800×480 RGB-16 | GT911 | USB | |
| `squixl` | esp32s3 | SQUiXL by Unexpected Maker (16 MB flash, 8 MB octal PSRAM) | 480×480 ST7701S RGB | GT911 | USB | LCA9555 expander, bit-banged ST7701S init |
| `elecrow_s3_lcd_7` | esp32s3 | Elecrow CrowPanel 7" (regular v2/v3) | 800×480 RGB | GT911 | **UART0** | OTG pins not wired to USB-C ⇒ no HID |
| `elecrow_s3_lcd_7_adv` | esp32s3 | Same board, Advance v1.0/v1.2 | 800×480 RGB | GT911 | **UART0** | Reuses `elecrow_s3_lcd_7/board/*` sources; RTC probe at boot |
| `elecrow_p4_lcd_7` | esp32p4 | Elecrow CrowPanel Advanced 7" P4 (16 MB flash, 32 MB PSRAM) | 1024×600 MIPI-DSI (EK79007) | GT911 | USB (High-Speed) | USB-only: `CONFIG_TOUCHY_WIFI` is off on P4 |
| `esp32_2432s028rv3` | esp32 | "CYD2USB" ESP32-2432S028R v3 (4 MB flash, **no PSRAM**) | 320×240 ST7789 SPI | XPT2046 (single-touch) | **UART0** (CH340) | Touch MISO pin still unconfirmed |
| `esp32_2432s024` | esp32 | "CYD2USB" ESP32-2432S024 2.4" (4 MB flash, **no PSRAM**) | 320×240 ILI9341 SPI | XPT2046 (single-touch) | **UART0** (CH340) | Shares `boards/cyd_common/` |
| `esp32_s3_matrix` | esp32s3 | Waveshare ESP32-S3-Matrix (8×8 WS2812B on GPIO 14) | LED matrix | — | USB | Geometry from the persisted `BoardConfig`; headless until configured |
| `esp32_s3_devkitc_1` | esp32s3 | Adafruit ESP32-S3 Feather LED-matrix | LED matrix | — | USB | Console on USB-Serial-JTAG |
| `jc_esp32p4_m3` | esp32p4 | Guition JC-ESP32P4-M3 LED-matrix | LED matrix | — | USB | **PSRAM init currently fails** — see [its README](boards/jc_esp32p4_m3/README.md) |

Boards with no native USB cannot emulate a USB mouse/keyboard: the protobuf
protocol rides their hardware UART (UART0 @ 115200, through the CH340 bridge)
and `board-info` reports `has_usb=false`. Resistive XPT2046 panels are
single-touch, so they report `is_multitouch=false`; the LED-matrix boards have
no touch at all and report both false.

### Shared board code

* `boards/cyd_common/` — the whole "CYD" family (`esp32_2432s028rv3`,
  `esp32_2432s024`, more coming) compiles these *same* sources; each board
  directory contributes only its `board/board_pins.h`, which selects the panel
  driver (`BOARD_LCD_CONTROLLER_ST7789` vs `BOARD_LCD_CONTROLLER_ILI9341`).
  No symlinks.
* `boards/common/backlight_pwm.{h,cpp}` — the shared LEDC/PWM backlight driver
  used by every board whose backlight is a plain GPIO (Stage 94). Boards whose
  backlight sits behind an IO expander/MCU keep a local `backlight_set()` that
  quantises the 0–100 level.
* `boards/common/leds/` — the LED-panel (`LEDPanel`) plus chain (`LEDChain`)
  driver for LED-matrix boards, including per-panel wiring flags and tiling.
* `boards/elecrow_s3_lcd_7_adv/board/CMakeLists.txt` — compiles the regular
  Elecrow sources from a sibling directory; that's the pattern for a second
  board id that differs only in `sdkconfig.defaults`/pins.

## What it does today

* **Rendering** — LVGL v9.5 through `esp_lvgl_port` (RGB565). The app is
  layout-agnostic: the host sends a protobuf `Screen`/`Widget` tree and the
  firmware materialises it.
* **Touch** — capacitive GT911 multitouch or resistive XPT2046 single-touch,
  fed into LVGL as an indev, plus a raw multitouch snapshot the trackpad
  widget uses for multi-finger gestures.
* **USB composite device** (native-USB boards) — HID mouse + HID keyboard
  (report IDs 1/2) + HID Consumer Control (report ID 3, for volume/mute-style
  keys) + a vendor-specific bulk pair carrying the protobuf protocol.
  VID/PID `0x303A / 0x8369`. CDC-ACM is **off by default**
  (`CONFIG_TINYUSB_CDC_ENABLED=n`) to save endpoint slots.
* **Host protocol over several transports at once** (Stage LB5) — vendor bulk,
  a hardware UART (boards declaring `CONFIG_TOUCHY_HAS_PROTO_UART`), an
  optional USB-CDC ACM port, and WiFi HTTP(S). One dispatcher task per
  available link; responses return on the originating link.
* **Device logs come back over the protocol** — `ESP_LOG` output is tunneled
  (`CONFIG_TOUCHY_LOG_OVER_PROTO`, Stage 64.1) instead of only a serial
  console; `CONFIG_TOUCHY_LOG_TO_UART` mirrors it to UART0 as well. A core
  dump from a panic is decoded and logged on the next boot
  (`coredump_report.cpp`), and failed heap allocations log with
  free/largest-block figures.
* **Trackpad widget with the full gesture set** — 1/2/3-finger clicks,
  1-finger drag, 2-finger scroll, 4-direction swipes, pinch-zoom, twist
  rotate, and press-and-hold/release (drag-and-drop). Every gesture is bound
  to an editable `Action` list, so it emits USB HID (or host events) with no
  host software attached; macros can also hang off buttons, sliders and
  toggles.
* **Screens and widgets** — protobuf screens in `F:host/s/`, user pages in
  `F:host/uscr/` paged through the default chrome's `widget_ref(id="page")`,
  `widget_ref` file indirection, flex/grid/absolute layouts with opt-in
  `grow_x`/`grow_y` sizing, styles + state styles + transitions + animations,
  and runtime property overrides (`SetPropertyCmd`).
* **Filesystems** — `F:` LittleFS (persistent), `R:` PSRAM ramdisk, and the
  `T:` transient drive (PSRAM ramdisk where available, else a flash scratch
  area) for throwaway assets such as dynamic images and cached icons.
* **Preferences** — a persisted `PreferencesFile`: backlight level and sleep
  timeout, minimum log level, boot delay, LED panel/chain configuration, and
  the WiFi `NetworkConfig` (merged per sub-field, applied live).
* **LED matrices** — `LEDPanel`/`LEDChain` render LVGL onto chains of up to
  four tiled WS2812B panels whose geometry/wiring come from the persisted
  `BoardConfig`; an unconfigured board comes up headless.
* **WiFi + mTLS** (WiFi-capable chips) — `POST /touchy/api/v1/command`
  accepts binary protobuf or JSON, over plaintext HTTP or HTTPS with mutual
  TLS once certificates are provisioned.
* **Headless fallback** — if panel bring-up fails, `HeadlessDisplay` keeps the
  protocol alive so the device can still be configured and updated.

## Project layout

```
firmware/
├── CMakeLists.txt              # selects BOARD, layers sdkconfig.defaults
├── version.cmake               # reads ../VERSION + git state → build/version.h
├── sdkconfig.defaults          # shared defaults (CPU, console, USB, LVGL, logs)
├── README.md                   # ← this file
├── partitions/                 # shared partition tables: 4M / 8M / 16M .csv
├── boards/
│   ├── common/                 # code shared between boards
│   │   ├── backlight_pwm.{h,cpp}   # PWM/LEDC backlight (Stage 94)
│   │   └── leds/                   # LEDPanel + LEDChain driver (lb6/lb10)
│   ├── cyd_common/             # shared C++ for the whole "CYD" family
│   │   ├── board.cpp           # platform_get() {multitouch:false, usb:false}
│   │   ├── display.cpp         # ST7789/ILI9341 SPI + esp_lvgl_port
│   │   └── touch.cpp           # XPT2046 resistive + LVGL indev
│   ├── jc4827w543/             # 4.3" NV3041A QSPI board (the default BOARD)
│   │   ├── target              # one-line IDF chip: "esp32s3"
│   │   ├── sdkconfig.defaults  # PSRAM-OPI, 4 MB QSPI flash, partition table
│   │   └── board/              # ← ESP-IDF component named `board`
│   │       ├── CMakeLists.txt
│   │       ├── idf_component.yml
│   │       ├── board.cpp       # shared-I2C bus init only
│   │       ├── board_pins.h    # GPIO map (private)
│   │       ├── display.cpp     # NV3041A + LVGL flush_cb
│   │       ├── nv3041a.{h,c}   # standalone QSPI panel driver
│   │       └── touch.cpp       # GT911 + LVGL indev
│   ├── esp32_2432s028rv3/      # classic-ESP32 CYD2USB 2.8", ST7789
│   │   ├── target              # "esp32"
│   │   ├── sdkconfig.defaults  # 4 MB flash, proto-over-UART0, no PSRAM
│   │   └── board/              # CMakeLists.txt compiles ../../cyd_common/*.cpp
│   │                           # idf_component.yml, board_pins.h (ST7789)
│   ├── esp32_2432s024/         # classic-ESP32 CYD2USB 2.4", ILI9341
│   ├── elecrow_s3_lcd_7/       # 7" RGB;  elecrow_s3_lcd_7_adv/ reuses it
│   ├── elecrow_p4_lcd_7/       # ESP32-P4 + 1024×600 MIPI-DSI
│   ├── squixl/  matouch_43/  waveshare_s3_lcd_7b/  jc4827w543r/
│   ├── esp32_s3_matrix/  esp32_s3_devkitc_1/  jc_esp32p4_m3/   # LED matrices
│   └── <board>/                # one dir per board: target + sdkconfig.defaults
│                               #   + board/ (a component named `board`)
└── main/                       # board-agnostic app code
    ├── CMakeLists.txt          # REQUIRES board, esp_lcd, joltwallet__littlefs, …
    ├── idf_component.yml       # managed deps (lvgl 9.5, esp_lvgl_port, tinyusb…)
    ├── main.cpp                # app_main entry point (thin)
    ├── board.h / display.h / touch.h  # the board-facing seam
    ├── platform.{h,cpp}        # platform_get() capability probe
    ├── prefs.{h,cpp}           # persisted PreferencesFile (Stage 82)
    ├── screens.{h,cpp}         # screen registry, load, file-change notify
    ├── display.cpp             # Display ABC + HeadlessDisplay
    ├── backlight.{h,cpp}       # auto-sleep timer + level
    ├── macros.{h,cpp}          # device-side USB-HID macro runner
    ├── log_proto.{h,cpp}       # ESP_LOG → host tunnel (Stage 64.1)
    ├── api/                    # host protocol: dispatcher + one file per link
    ├── widgets/                # builders, actions, styles, animations, trackpad
    ├── fs/                     # F: LittleFS, R: ramdisk, T: transient
    ├── net/                    # WiFi + HTTP(S) command API (CONFIG_TOUCHY_WIFI)
    └── proto/                  # generated .pb.{c,h} (gitignored)
```

### How the multi-board build works

1. The top-level `CMakeLists.txt` reads the `BOARD` CMake cache variable
   (default `jc4827w543`).
2. It points `EXTRA_COMPONENT_DIRS` at `boards/<BOARD>/` so ESP-IDF picks up
   the **`board`** component nested inside (`boards/<BOARD>/board/`). Every
   board provides a component named literally `board`, so `main/` can list
   `board` in its `REQUIRES` and stay entirely board-agnostic.
3. `SDKCONFIG_DEFAULTS` is a two-element list: the common `sdkconfig.defaults`
   first, then `boards/<BOARD>/sdkconfig.defaults` on top — so board-specific
   knobs (flash size, PSRAM mode, partition CSV, pixel clock) override the
   common defaults.
4. The generated config is written to `sdkconfig.<BOARD>` so switching boards
   never clobbers your previous settings. (Both files are gitignored.)
5. The IDF chip target comes from `boards/<BOARD>/target`; `just
   firmware-reconfigure <board>` reads it and runs
   `idf.py -DBOARD=<board> set-target <chip>`.
6. Each board also gets `-DTOUCHY_BOARD_<board>=1` and
   `-DTOUCHY_BOARD_NAME="<board>"` if code ever needs to branch on it.

> **Never edit `sdkconfig.<BOARD>` directly.** It is regenerated by the build
> system and hand edits are discarded. To persist a Kconfig flag:
> * **shared across all boards** → `firmware/sdkconfig.defaults`
> * **one board only** → `firmware/boards/<BOARD>/sdkconfig.defaults`
>
> Both files are committed and are the canonical place for build config.

### Adding a new board

1. Create `boards/<name>/board/` with a `CMakeLists.txt` that registers a
   component called `board`, implementing `board_init()`, a `Display`
   subclass returned from `display_create()`, and `touch_init()` (signatures
   in `main/board.h`, `main/display.h`, `main/touch.h`).
2. Add `boards/<name>/target` — a one-line file naming the IDF chip
   (`esp32`, `esp32s3`, `esp32p4`). `just firmware-reconfigure <name>` reads
   it.
3. Add `boards/<name>/sdkconfig.defaults` with flash/PSRAM/partition
   overrides, referencing the matching `partitions/<size>.csv` (add a new CSV
   if the flash size is new).
4. Add board-specific managed components to
   `boards/<name>/board/idf_component.yml`.
5. Build with `just firmware-reconfigure <name> && just firmware-build`. The
   `-DBOARD` is required on `set-target`, and when you switch to a different
   *chip* you must first `rm -f firmware/sdkconfig firmware/sdkconfig.<old>`.
6. Add a row to the board table above and to
   [`../docs/hardware.md`](../docs/hardware.md).

> **CYD family:** for another "Cheap Yellow Display" variant, do not write
> fresh `board.cpp`/`display.cpp`/`touch.cpp` — reuse `boards/cyd_common/`:
> have `board/CMakeLists.txt` compile `../../cyd_common/*.cpp` (with
> `PRIV_INCLUDE_DIRS "."` so the board's own `board_pins.h` is found first)
> and contribute only `board_pins.h`. Select the panel driver there with
> `BOARD_LCD_CONTROLLER_ST7789` or `BOARD_LCD_CONTROLLER_ILI9341`. See
> `esp32_2432s028rv3` and `esp32_2432s024` for the two-file pattern.

> **LED-matrix board:** reuse `boards/common/leds/` via an `LEDMatrixDisplay`,
> have `backlight_set()` forward to the matrix brightness, and leave the panel
> geometry to the persisted `BoardConfig` — don't hard-code it in
> `board_pins.h`.

## Prerequisites

- **ESP-IDF v6.x** — the devcontainer pins **v6.0.2**; CI builds with v6.0.1.
- One connected board from the table above.
- Toolchains for the chips you build: classic-ESP32 / ESP32-S3 / ESP32-P4 all
  need their own toolchain, e.g. `./install.sh esp32,esp32s3,esp32p4`.

In every shell where you invoke `idf.py` by hand:

```bash
. $HOME/esp/esp-idf/export.sh    # path depends on your install location
```

## Build / flash

With `just` (recommended — it sources ESP-IDF for you):

```bash
just firmware-reconfigure jc4827w543   # select a board + its chip
just firmware-build                    # → firmware/build/touchy_pad_v2.elf (+ .bin)
just flash                             # build + flash over the board's UART
just hw-543                            # shortcut: reconfigure for jc4827w543
just merge-bin                         # single merged image at 0x0 (touchy update)
```

Equivalents inside a sourced ESP-IDF shell (`firmware/`):

```bash
idf.py -DBOARD=jc4827w543 set-target esp32s3    # persists BOARD in CMakeCache
idf.py build
idf.py -p /dev/ttyACM0 flash monitor            # UART USB-C port, NOT the OTG one
```

After flashing a native-USB board, plug its **OTG** USB-C port into the host:
VID `0x303A` / PID `0x8369` ("Touchy-Pad") shows up as a HID mouse plus a
vendor interface carrying the protocol — `touchy board-info` should answer.
UART-only boards are reached through their USB-UART bridge at 115200; the
Python client auto-discovers CH340-attached boards (Stage 83), or you can
point at the port explicitly.

### Switching boards

```bash
rm -f firmware/sdkconfig firmware/sdkconfig.<old-board>   # required on chip change
just firmware-reconfigure <new-board>
just firmware-build
```

`sdkconfig.<board>` files coexist on disk, so menuconfig tweaks survive
bouncing between boards.

## Console and logging

- The primary console is **UART0** (`CONFIG_ESP_CONSOLE_UART_DEFAULT=y`):
  available at boot, before USB enumerates. The USB-Serial/JTAG controller is
  disabled in `sdkconfig.defaults` because it shares GPIO 19/20 with TinyUSB.
- The intended way to watch logs is the **protocol log tunnel** (Stage 64.1):
  every `ESP_LOG` line is queued and delivered to the host over the command
  channel, so `touchy --debug board-info` (or any `--listen` run) prints them.
  `CONFIG_TOUCHY_LOG_TO_UART` additionally mirrors them to UART0 where that's
  convenient, and a panic's core dump is decoded + logged on the next boot.
- To get a **USB CDC-ACM console** back for a debugging session, set
  `CONFIG_TINYUSB_CDC_ENABLED=y` in that board's `sdkconfig.defaults` (and
  optionally `CONFIG_TOUCHY_PROTO_OVER_CDCACM=y` to also serve the protocol
  over it). It costs a USB-OTG endpoint slot, which is why it's off by default.

## Debug

- The built-in USB-Serial-JTAG bridge is disabled in the default config, so
  in-system debugging over the OTG port is not available (boards that need it,
  e.g. `esp32_s3_devkitc_1`, override the console back to USB-Serial-JTAG).
  Use an external JTAG probe (ESP-Prog) with `idf.py openocd` + `idf.py gdb`.
- Core dumps (saved to the `coredump` flash partition) can be decoded with
  `idf.py coredump-info`; the firmware also auto-decodes a saved dump on the
  next boot and logs the summary over the protocol.
- Every failed heap allocation logs an ERROR with free/largest-block figures
  (`main.cpp`'s `heap_caps_register_failed_alloc_callback`), so OOM and
  fragmentation are distinguishable.
- `CONFIG_FREERTOS_USE_TRACE_FACILITY=y` is on, so the device can dump
  per-task stack/CPU info on demand.

## Known issues / caveats

- **CYD touch is unproven.** Both classic-ESP32 CYD boards build and run, but
  the XPT2046's MISO line is still unknown (39 and 12 both failed), so touch
  has not been verified on real hardware. The display half works.
- **`jc_esp32p4_m3` doesn't boot yet** — PSRAM init fails before `app_main`
  (see its board README). Use `esp32_s3_devkitc_1` (the same LED stack on
  mainstream silicon) instead.
- **`jc4827w543` QSPI tops out at 32 MHz** on that board despite the NV3041A
  datasheet quoting ~40 MHz; above 32 MHz you get visible pixel corruption.
- **`espressif/esp_lvgl_port` is pinned `>=2.8.0,<2.9.0`.** 2.9.0 points the
  MIPI-DSI (esp32p4) avoid-tearing callback at `on_frame_buf_complete` for
  every IDF ≥ 5.5, but IDF 6.0.x still declares `on_refresh_done` only, so the
  `elecrow_p4_lcd_7` build fails to compile. Relax the pin once the project's
  IDF carries the renamed field.
- **No unit tests for firmware.** All automated tests are host-side
  (`app/tests/`, `just app-test`); bring-up and gesture tuning are validated on
  hardware, which is why gesture code logs one line per fire
  (`swipe …`, `zoom …`, `twist …`).
- **Windows/macOS hosts** are supported by the host tools, but the firmware
  build is normally done from Linux or the devcontainer.

## Related docs

- [Host protocol](../docs/host-api.md) — framing, commands, filesystem paths.
- [Network API](../docs/network-api.md) — WiFi + HTTP(S)/mTLS endpoint.
- [Design history](../docs/design.md) — the authoritative stage-by-stage record.
- [Dev setup](../docs/development.md), [hardware guide](../docs/hardware.md).
- [AGENTS.md](../AGENTS.md) — orientation for AI agents/contributors.
