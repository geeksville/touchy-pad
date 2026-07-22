# Waveshare ESP32-S3-Matrix

A compact ESP32-S3 dev board with a built-in 8×8 WS2812B NeoPixel matrix.

## Hardware highlights

* ESP32-S3FH4R2
  * 4 MB embedded Flash
  * 2 MB embedded PSRAM (Quad SPI)
* 8×8 WS2812B LED matrix on **GPIO 14**
* Native USB-OTG (CDC-ACM + custom vendor endpoints)
* **No dedicated UART** — host protocol runs over USB only
* WiFi/Bluetooth antenna is present on the board

Vendor docs: https://docs.waveshare.com/ESP32-S3-Matrix  
Schematic: https://files.waveshare.com/wiki/ESP32-S3-Matrix/ESP32-S3-Matrix-Sch.pdf

## Stage matrix1 implementation plan

1. **Firmware board support** — add `firmware/boards/esp32_s3_matrix/` adapted from `esp32_s3_devkitc_1`:
   * `target` = `esp32s3`
   * `sdkconfig.defaults`: 4 MB flash, 2 MB quad PSRAM, no touch, vendor-USB protocol only, no hardware UART, log over proto
   * `board/CMakeLists.txt`, `idf_component.yml`, `board_pins.h`, `board.cpp`, `touch.cpp`
   * Reuse the shared `boards/common/leds/` LED matrix driver
2. **LED panel template** — add `app/src/touchy_pad/assets/templates/esp32_s3_matrix.json` with one 8×8 chain on GPIO 14.
3. **Docs/metadata** — update this README and list the board in the shared LED driver comments.

After flashing, provision the panel with:

```bash
touchy pref from-template esp32_s3_matrix
```
