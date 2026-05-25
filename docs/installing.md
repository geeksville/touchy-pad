# Installing touchy
This shows a typical setup for installing/upgrading the touchy firmware.

```bash
> touchy update
✓ detected running device: board=jc4827w543 firmware=0.2.1
╭─────────────────────────────── Manual step required ────────────────────────────────╮
│ Hold the BOOT button on the device, then unplug and re-plug it via USB. Keep BOOT   │
│ held until the device re-enumerates.                                                │
│                                                                                     │
│ Waiting for the ESP32-S3 bootloader to appear...                                    │
╰─────────────────────────────────────────────────────────────────────────────────────╯
✓ bootloader detected.
About to flash firmware for board jc4827w543.
⚠  Flashing the wrong board image can damage hardware.
Continue? [Y/n]: y
Downloading https://github.com/geeksville/touchy-pad/releases/latest/download/jc4827w543.bin
downloading ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ 885.2/885.2 kB 2.0 MB/s 0:00:00
✓ downloaded 885,248 bytes
using serial port /host/dev/ttyACM1
running: esptool -p /host/dev/ttyACM1 -b 460800 --before default-reset --after hard-reset --chip esp32s3 write-flash 0x0 /tmp/touchy-update-15cp1m1m/jc4827w543.bin
esptool v5.2.0
Connected to ESP32-S3 on /host/dev/ttyACM1:
Chip type:          ESP32-S3 (QFN56) (revision v0.2)
Features:           Wi-Fi, BT 5 (LE), Dual Core + LP Core, 240MHz, Embedded PSRAM 8MB (AP_3v3)
Crystal frequency:  40MHz
USB mode:           USB-Serial/JTAG
MAC:                a4:cb:8f:ec:1c:e8

Stub flasher running.
Changing baud rate to 460800...
Changed.

Configuring flash size...
Flash will be erased from 0x00000000 to 0x000d8fff...
Wrote 885248 bytes (543800 compressed) at 0x00000000 in 4.9 seconds (1437.6 kbit/s).
Hash of data verified.

Hard resetting via RTS pin...
╭──────────────────────────────────────────────────────────────────╮
│ ✓ Update succeeded.                                              │
│ Please unplug and replug your Touchy-Pad to run the application. │
╰──────────────────────────────────────────────────────────────────╯
```