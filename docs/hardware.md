# Supported hardware

Currently this project supports two board types, but ali-express seems to have many similar cheap boards of different resolutions.  I'm happy to help anyone add a new supported board to this project - it is quite easy.  (Ping me if you want to do this and I'll flesh out these instructions).

Currently the following two boards are supported

## JC4827W543

![cyd](images/jc4827w543.jpg)

**jc4827w543 IS THE CURRENTLY 'BEST' BOARD TO PURCHASE**
This board costs just $15 USD, but has full multi-touch capacitive touchscreen with lots of RAM and FLASH!

The JC4827W543 (often sold under names like Guition or Sunton) is an ESP32-S3 board paired with a 4.3" 480x272 RGB display (using an NV3041A or ST3401A controller) and usually a GT911 capacitive touch chip. For detailed hw docs see [here](https://github.com/profi-max/JC4827W543_4.3inch_ESP32S3_board).

Here's a suitable [aliexpress search](https://www.aliexpress.us/w/wholesale-JC4827W543.html?spm=a2g0o.home.search.0).  Make sure to select sellers that sell a fair number of units and have decent reviews.  Also make sure to purchase one with **CAPACITIVE** touch.  For instance [this one](https://www.aliexpress.us/item/3256806543063048.htm).

### NV3041A QSPI clock ceiling

The NV3041A datasheet quotes ~40 MHz QSPI but on this particular board the
practical maximum is **32 MHz** — anything above that produces visible
corruption / dropped pixels. `BOARD_LCD_QSPI_CLK_HZ` in
[firmware/boards/jc4827w543/board/board_pins.h](../firmware/boards/jc4827w543/board/board_pins.h)
is set accordingly. At 32 MHz QSPI the raw wire bandwidth is ~16 MB/s,
which caps a full-frame RGB565 redraw (480×272×2 ≈ 261 KB) at roughly
~60 FPS regardless of how the host-side code is structured. 

## ESP32-2432S028Rv3: 2.8" resistive cheap-yellow-display variant (Also called "ESP32-2432S028R v3" or CYD2USB)

### WARNING: Not yet supported

FIXME once home with access to a multimeter to find where MISO is attached to
XPT2046.  Supposedly it is 39, but tried 12 and not there either.  Need to ohm it out. Until then both of the CYD boards are unsupported.  For the time being I'm leaving the test/debug code in touch.cpp.

> **touchy-pad support: implemented (Stage 65).** Board id
> `esp32_2432s028rv3` (`firmware/boards/esp32_2432s028rv3/`, IDF target
> `esp32`). Flash and talk to it over the CH340 UART on `/dev/ttyUSB*`
> (not `/dev/ttyACM*`) — the proprietary protobuf protocol runs over
> UART0 at 115200, device logs ride the Stage 64.1 `LogRecord` tunnel.
> No native USB ⇒ no HID mouse/keyboard. No PSRAM, but the `R:` ramdisk
> and image assets still work (RamFs falls back to internal SRAM,
> bounded by ~520 KB). The resistive XPT2046 is single-touch, so the
> device reports `is_multitouch=false` / `has_usb=false` in board-info.
>
> **Driver settings exposed in `board/board_pins.h`** (flip on real
> hardware if the panel looks wrong): the ST7789 is configured BGR +
> colour-inversion with SWAP_XY/MIRROR_X, and the backlight defaults to
> GPIO 21. The XPT2046 needs per-unit touch calibration. These are the
> "resolve on hardware" items from the Stage 65 plan.

### Core Specifications / GPIOs / Chips

USB connection to host is via a CH341 UART (USB VID=1a86, PID=7523)
Display resolution 320x240.
The SoC: ESP32-D0WD-V3 (Dual-core Xtensa 32-bit LX6 microprocessor running at 240 MHz).
Storage: 4 MB of integrated SPI flash.
Memory: 520 KB internal SRAM (Again, zero PSRAM on this specific WROOM module).
NOTE: This it NOT based on an ESP32-S3, so no native USB port support. i.e. can't emulate USB mouse or keyboard.

USB ports: this board contains both a USB-C and a USB-Micro port but they are electrically connected. USE ONLY ONE OF THEM.

| Component | Specification |
| :--- | :--- |
| **Display Controller** | ILI9341 or probably ST7789 (Newer v3 boards use ST7789) |
| **Touch Controller** | XPT2046 (Resistive) |
| **PSRAM** | 0 MB (Relies on 520KB internal SRAM) |
| **Flash** | 4 MB SPI Flash |

---

### SPI Pin Assignments

#### Display (ILI9341 / ST7789)
| Function | GPIO |
| :--- | :--- |
| **MOSI** | 13 |
| **MISO** | 12 |
| **SCK** | 14 |
| **CS** | 15 |
| **DC / RS** | 2 |
| **Backlight** | Probably 27, but might be 21 |

#### Touchscreen (XPT2046)
| Function | GPIO |
| :--- | :--- |
| **MOSI** | 32 |
| **MISO** | 39 |
| **CLK** | 25 |
| **CS** | 33 |
| **IRQ** | 36 |

#### MicroSD Card
| Function | GPIO |
| :--- | :--- |
| **MOSI** | 23 |
| **MISO** | 19 |
| **CLK** | 18 |
| **CS** | 5 |

---

### Onboard Peripherals

| Peripheral | GPIO | Notes |
| :--- | :--- | :--- |
| **RGB LED - Red** | 4 | Active Low |
| **RGB LED - Green** | 16 | Active Low |
| **RGB LED - Blue** | 17 | Active Low |
| **Audio Out** | 26 | I2S Speaker |
| **LDR** | 34 | Analog input (Light Sensor) |
| **Boot Button** | 0 | |

---

### Available Expansion Pins (JST Connectors)

| GPIO | Capability | Suggested Use |
| :--- | :--- | :--- |
| **22** | Free (I/O) | I2C SCL |
| **21** | Free (I/O) | I2C SDA |
| **35** | Input Only | Analog/Digital Read (No internal pull-ups) |

> **Note:** The display backlight might be hardwired to GPIO 21. Reusing GPIO 21 for general I/O or I2C SDA will cause the screen to strobe during data transmission.

## ESP32-2432S024: The 2.4" version of CYD2USB (also called ESP32-2432S024)

### WARNING: Not yet supported

FIXME once home with access to a multimeter to find where MISO is attached to
XPT2046.  Supposedly it is 39, but tried 12 and not there either.  Need to ohm it out. Until then both of the CYD boards are unsupported.  For the time being I'm leaving the test/debug code in touch.cpp.

### Hardware config

More pinout information here: https://github.com/F1ATB/ESP32-2432S028-2432S024-2432S032-JC2432W328

But in short: it should be mostly the [same](hardware.md) as the ESP32-028 except it uses a ILI9341 display controller.  Try to share as much as possible (via sym links or better directory structure) with the 028v3 variant.

> **Status: supported** (`BOARD=esp32_2432s024`, IDF target `esp32`). Builds
> green; on-hardware bring-up of colour/orientation/touch-calibration still
> pending. The whole CYD family now shares one C++ implementation in
> `firmware/boards/cyd_common/` (no symlinks); each board contributes only its
> `board_pins.h`, which selects the panel driver via
> `BOARD_LCD_CONTROLLER_ILI9341` (024) or `BOARD_LCD_CONTROLLER_ST7789` (028).

### Misc tips

Fix the Colors: The ST7789 panels used on the CYD2USB frequently have their color channels wired backward. If your UI renders with swapped colors (e.g., red appears blue), you must add this to your configuration header:
#define TFT_RGB_ORDER TFT_BGR

Inversion: If the screen looks like a film negative, flip the inversion flag:
#define TFT_INVERSION_ON (or OFF, depending on the default state).

## Waveshare 7 inch

![waveshare](images/waveshare-7.jpg)

The [Waveshare 7 inch](https://docs.waveshare.com/ESP32-S3-Touch-LCD-7), eventually many other similar boards will be supported. For more information on this board see https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-7B. This board is slightly expensive but has a very high-res screen. Costs about $30 USD on aliexpress.

## Elecrow CrowPanel Advanced 7" ESP32-P4 (`elecrow_p4_lcd_7`)

The [Elecrow CrowPanel Advanced 7" ESP32-P4](https://www.elecrow.com/crowpanel-advanced-7inch-esp32-p4-hmi-ai-display-1024x600-ips-touch-screen-with-wifi-6-compatible-with-arduino-lvgl-micropython.html) runs the ESP32-P4 chip (dual-core Xtensa LX9 at 400 MHz) paired with a crisp 1024×600 MIPI-DSI display driven through an EK79007 bridge IC. 16 MB flash, 32 MB hex-PSRAM at 200 MHz. GT911 capacitive multitouch. Costs roughly $30–40 USD.

**USB HID works fully** — the P4 has a USB 2.0 High-Speed OTG controller (480 Mbps), and the firmware negotiates HS descriptors (512-byte bulk endpoints) automatically. This means you get the full mouse + keyboard HID experience, exactly like the jc4827w543.

The board also contains an ESP32-C6 WiFi companion chip, held in reset for now (WiFi is not yet implemented).

Notable quirks discovered during bring-up:
- The LDO channel 3 must be raised to 2.5 V before DSI bus init, or the PHY PLL never locks.
- ESP32-P4 v1.x silicon requires a few Kconfig workarounds (`REV_MIN_100`, `SPIRAM_SPEED_200M`, `MBEDTLS_HARDWARE_ECDSA=n`).

Board id: `elecrow_p4_lcd_7`, IDF target `esp32p4`.

## Elecrow CrowPanel 7" ESP32-S3 (`elecrow_s3_lcd_7` / `elecrow_s3_lcd_7_adv`)

Two variants of the Elecrow 7" ESP32-S3 board are supported: the [regular CrowPanel 7"](https://www.elecrow.com/esp32-display-7-inch-hmi-display-rgb-tft-lcd-touch-screen-support-lvgl.html) and the [CrowPanel Advance 7"](https://www.elecrow.com/crowpanel-advance-7-hmi-esp32-ai-display-800x480-ai-ips-touch-screen.html). Both use an ESP32-S3 with an 800×480 RGB panel and GT911 capacitive multitouch, and cost around $20–25 USD.

**Important limitation:** neither variant exposes the ESP32-S3's USB-OTG pins — the USB-C port is wired to the UART0 bridge only. As a result **USB HID is not available**; the protocol runs over serial (115200 baud, `CONFIG_TOUCHY_PROTO_OVER_SERIAL`) and `has_usb` is reported `false` to the host. If you need HID (mouse/keyboard emulation), choose a different board.

The firmware supports all hardware revisions under one build. At boot it probes I²C address `0x51` for the Advance-only RTC chip and selects the correct pin assignments and backlight controller automatically:

- **Regular (v2/v3):** PCA9557 IO expander for GT911 reset and backlight.
- **Advance (v1.0):** PCA9557 IO expander, different GPIO routing.
- **Advance (v1.2):** STC8H1K28 backlight controller instead of PCA9557.

Two board IDs exist because the Advance variant requires slightly different sdkconfig defaults, but they share all C++ source files. Board ids: `elecrow_s3_lcd_7` (regular) and `elecrow_s3_lcd_7_adv` (Advance), IDF target `esp32s3` for both.

# Typical links
(No endorsement of particular vendors is implied)

![sample hw](images/hw.png)

## Screen+hardware 
* https://www.aliexpress.us/item/3256808357526751.html
* https://shopee.tw/ESP32-P4-WIFI6-7%E8%8B%B1%E5%AF%B8%E4%BA%94%E9%BB%9E%E8%A7%B8%E6%8E%A7%E5%B1%8F%E9%96%8B%E7%99%BC%E6%9D%BF-1024%C3%97600%E5%88%86%E8%BE%A8%E7%8E%87-%E5%9F%BA%E6%96%BCESP32-P4%E5%92%8CESP32-C6-%E6%94%AF-i.871575797.40371503863
* https://shopee.tw/ESP32-S34.3%E5%AF%B85%E5%AF%B8LCD%E9%9B%BB%E5%AE%B9%E8%A7%B8%E6%91%B8%E5%B1%8FLVGL%E9%96%8B%E7%99%BC%E6%9D%BFIO%E6%93%B4%E5%B1%95-i.1578084179.44557771187?extraParams=%7B%22display_model_id%22%3A410687861211%2C%22model_selection_logic%22%3A3%7D&sp_atk=b636f0bc-1d2e-4669-8e72-0a2a2fb56695&xptdk=b636f0bc-1d2e-4669-8e72-0a2a2fb56695
* https://shopee.tw/-ESP32%E9%96%8B%E7%99%BC%E6%9D%BFWiFi%E8%97%8D%E7%89%992.8%E5%AF%B8240*320%E6%99%BA%E8%83%BD%E9%A1%AF%E7%A4%BA%E5%B1%8FTFT%E6%A8%A1%E5%A1%8A%E8%A7%B8%E6%91%B8%E8%9E%A2%E5%B9%95LVGL-i.265939604.43270466926?extraParams=%7B%22display_model_id%22%3A177318412563%2C%22model_selection_logic%22%3A3%7D&sp_atk=e2aa9b15-00cf-4c73-a053-7c6069bfe820&xptdk=e2aa9b15-00cf-4c73-a053-7c6069bfe820
* https://shopee.tw/ESP32-S3%E9%96%8B%E7%99%BC%E6%9D%BF%E5%B8%B65%E5%AF%B87%E5%AF%B8LCD%E5%9C%96%E5%BD%A2%E9%A1%AF%E7%A4%BA%E5%B1%8F%E9%9B%BB%E5%AE%B9%E5%B1%8FwifiMCU%E7%89%A9%E8%81%AF%E7%B6%B2-i.1725679736.56706409304

## More information

* [general info](https://www.espboards.dev/esp32/cyd-esp32-8048s050/) on these boards

## Haptics
We plan to support haptics in a future release (to provide nice button press 'feel')

Use this chip+motor TI DRV2605L https://www.adafruit.com/product/2305?srsltid=AfmBOopbcDSYZ7QqgK9jD2y2IHr5d2SBN0EGje71ejSgan9e44qDU4RJ 
https://www.aliexpress.us/w/wholesale-haptic-motor.html?spm=a2g0o.productlist.search.0

