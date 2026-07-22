this is a fairly standard esp32s3 board - similar to esp_s3_devkitc_1 except with these changes:

* ESP32-S3FH4R2 cpu. 
  * F / H4: 4 MB embedded Flash memory.
  * R2: 2 MB embedded PSRAM (Quad SPI).
* an 8x8 neopixel array attached to gpio 14
* no dedicated uart - all comms to PC are via usb cdcacm and our own custom endpoints
* no wifi antenna

vendor hw docs https://docs.waveshare.com/ESP32-S3-Matrix 
schematic https://files.waveshare.com/wiki/ESP32-S3-Matrix/ESP32-S3-Matrix-Sch.pdf

todo

## stage matrix1: support matrix board

* add support for this board in firmware/boards - similar to existing esp32 boards
* add a app/src/touchy_pad/assets/templates/esp32_s32_matrix.json template for the led panel
