# Waveshare ESP32-S3-Matrix

A compact ESP32-S3 dev board with a built-in 8×8 WS2812B NeoPixel matrix.

## Installing

Just three commands needed.

```bash
# flash the firmware
touchy update --board esp32_s3_matrix 
# install runtime config files for this particular board
touchy init
touchy pref from-template esp32_s3_matrix
```

## Hardware highlights

* ESP32-S3FH4R2
  * 4 MB embedded Flash
  * 2 MB embedded PSRAM (Quad SPI)
* 8×8 WS2812B LED matrix on **GPIO 14**
* Native USB-OTG (custom protocol endpoints)
* **No dedicated UART** — host protocol runs over USB only
* WiFi/Bluetooth antenna is present on the board

Vendor docs: https://docs.waveshare.com/ESP32-S3-Matrix  
Schematic: https://files.waveshare.com/wiki/ESP32-S3-Matrix/ESP32-S3-Matrix-Sch.pdf

