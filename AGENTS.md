# touchy-pad — AI Agent Guide

## Project overview
Open-source multitouch USB touchpad with a built-in customizable screen (ESP32-S3 based).
Supports touchpad mode (USB HID mouse/multitouch) and a configurable button-matrix/macro mode.
Optional haptics via TI DRV2605L. Optional Stream-controller compatible API.

## Key locations
| Path | Purpose |
|------|---------|
| `firmware/` | PlatformIO C++ firmware (the only build artifact in this repo) |
| `firmware/src/main.cpp` | Application entry point |
| `firmware/platformio.ini` | Board, environment, port settings |
| `docs/design.md` | Development stage spec — **read this before adding features** |
| `TODO.md` | Outstanding work items |
| `README.md` | Hardware options and library links |

## Target hardware
**Primary target:** Waveshare ESP32-S3-Touch-LCD-7  
- Docs: https://docs.waveshare.com/ESP32-S3-Touch-LCD-7  
- Wiki: https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-7B  
- PlatformIO board: `4d_systems_esp32s3_gen4_r8n16` (current placeholder; update when switching to Waveshare)

## Build system — PlatformIO
All firmware work uses PlatformIO (not Arduino IDE, not CMake).

```bash
# Build
cd firmware && pio run -e 4d_systems_esp32s3_gen4_r8n16

# Upload + monitor (device on /dev/ttyACM0)
pio run --target upload --target monitor -e 4d_systems_esp32s3_gen4_r8n16

# Serial monitor only
pio device monitor --port /dev/ttyACM0
```

To pin the serial port permanently, set in `platformio.ini`:
```ini
upload_port = /dev/ttyACM0
monitor_port = /dev/ttyACM0
```
(Currently commented out — uncomment when device is attached.)

## Key libraries to use
| Purpose | Library |
|---------|---------|
| LCD + LVGL on Waveshare board | `iamfaraz/Waveshare_ST7262_LVGL` |
| GUI / rendering | LVGL (via the above wrapper) |
| USB HID mouse | `arduino-libraries/Mouse` |
| BLE mouse (alternative) | `leollo98/ESP32 BLE Mouse With Precision Scroll` |
| Haptics | TI DRV2605L (Adafruit library) |
| Host config protocol | nanopb (protobuf for embedded) |

## Development stage status (from docs/design.md)
- **Stage 0** — stub app builds and runs (prints "hello world" over serial). **Target: implement next.**
- **Stage 1** — "hello world" on the LCD via LVGL.
- **Stage 10** — USB HID mouse device.
- **Stage 11** — Multitouch gestures (tap=click, drag=move/scroll).
- **Stage 20** — USB HID keyboard + `lv_buttonmatrix` button grid.
- **Stage 21** — Host-configurable layouts via protobuf over USB + Python CLI/library.
- **Stage 30** — Linux host simulator + GDB plugin debugging.

Always check which stage is currently implemented before adding code. Update `docs/design.md` after completing a stage.

## Coding conventions
- Language: **C++**, Arduino-ish API via PlatformIO framework.
- Use LVGL primitives for all display output (no direct framebuffer writes).
- Keep `main.cpp` thin; add new subsystems as separate `.cpp`/`.h` files in `firmware/src/`.
- No external build scripts; everything goes through `platformio.ini`.

## Architecture notes
- The board exposes a USB port that should enumerate as **both** a HID mouse and HID keyboard (composite device).
- Touchpad multitouch data comes from the GT911 touch controller over I²C.
- Haptics are on a separate I²C bus via DRV2605L.
- A reserved row of on-screen buttons (top or bottom, user-selectable) acts as a physical/virtual control strip separate from the touchpad area.
- A lasercut/3D-printed overlay template physically delineates those buttons from the touchpad region.
