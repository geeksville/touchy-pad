by https://github.com/chmouel from https://github.com/geeksville/touchy-pad/issues/14

## Problem

The JC4827W543 4.3" ESP32-S3 HMI board from Guition/JCZN ships in at least two factory hardware revisions under the **exact same product label and silkscreen**:

| Variant | Touch Controller | Factory Image | Working with current Touchy-Pad? |
|---------|-----------------|---------------|----------------------------------|
| `C_I` (capacitive) | GT911 over I2C | `JC4827W543C_I.bin` | Yes |
| `R_I` (resistive) | XPT2046 over SPI | `JC4827W543R_I.bin` | **No** |

Flashing the current `jc4827w543` firmware onto a resistive unit causes touch to fail silently. The display works, but no touch events are generated. Serial logs show:

```
[E][Wire.cpp:542] requestFrom(): i2cRead returned Error
```

This is because the firmware attempts to initialise a GT911 at I2C address `0x5D`, which does not exist on the resistive board variant.

---

## Diagnosis Steps Taken

1. Flashed the official `jc4827w543` Touchy-Pad releases `v0.2.6`, `v0.2.10`, `v0.2.11`, `v0.2.12` — display works, touch fails on all of them.
2. Restored the manufacturer factory image `JC4827W543C_I.bin` — display works, touch still fails.
3. Restored the manufacturer factory image `JC4827W543R_I.bin` — **both display and touch work correctly**.
4. Confirmed the board is the resistive variant by checking the manufacturer's Arduino demo source (`touch.h`), which conditionally defines XPT2046 pins for the R variant.

---

## Fix

Add a new board target `jc4827w543r` that reuses the existing NV3041A QSPI display driver unchanged, but replaces the GT911 I2C touch driver with the `atanisoft/esp_lcd_touch_xpt2046` SPI driver.

### Verified XPT2046 pin map (from manufacturer Arduino demo source):

| Signal | GPIO |
|--------|------|
| SPI Host | `SPI3_HOST` |
| SCK | `GPIO 12` |
| MISO | `GPIO 13` |
| MOSI | `GPIO 11` |
| CS | `GPIO 38` |
| PENIRQ/INT | `GPIO 3` |
| Clock speed | `2 MHz` |

### Files changed:

- `firmware/boards/jc4827w543r/` — new board folder (patch attached)
- `app/src/touchy_pad/update.py` — add `jc4827w543r` to `SUPPORTED_BOARDS`
- `.github/workflows/build-firmware.yml` — add `jc4827w543r` to CI matrix

### Notes for the reviewer:

- `display.cpp`, `nv3041a.c`, and `nv3041a.h` are **identical copies** from `jc4827w543`. You may prefer to share them via symlink or a common path rather than duplicating.
- `touch_init()` uses `swap_xy=0`, `mirror_x=0`, `mirror_y=0`. These were **not calibrated** — they may need tuning depending on orientation.
- `board_get_i2c_bus()` returns `nullptr` since there is no I2C bus on this variant.
