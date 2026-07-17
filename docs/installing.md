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
MAC:                a4:...:e8

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

## Configuring WiFi

If you want your touchypad to be accessible by wifi:

```bash
touchy pref wifi-set-ssid "my-network"
touchy pref wifi-set-psk  "my-passphrase"
```

By default the network API is **plaintext HTTP** — anyone on the LAN can
drive the device. To lock it down, provision **mutual TLS (mTLS)** over the
trusted USB (or UART) link:

```bash
touchy pref provision-mtls              # uses the device's mDNS hostname
```

## Connecting a wifi touchy

Point any `touchy` CLI subcommand at the endpoint with `--url`:

```bash
# plaintext (before provisioning)
touchy --url http://touchypad_ab12.local board-info

# mutual TLS (after provisioning) — credentials are loaded automatically
touchy --url https://touchypad_ab12.local board-info
```

`--url` may also be supplied via the `TOUCHY_URL` environment variable.