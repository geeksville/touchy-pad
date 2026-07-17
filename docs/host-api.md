# Host API
This file is the design doc for the protocol used to talk between the device and the host PC.

## Legacy HID descriptors

Unrelated to the 'fancy' custom API, the device will also expose HID keyboard and mouse interfaces/endpoints.  These are used to provide button press and touchpad events to the PC even if our helper app is not being used.  

## Touchy Host API

We will provide a custom API on custom USB interface composed of two bulk endpoints.

* Command Endpoint (OUT from PC to device). A message pipe.   The host sends commands (as protocol buffers?).
* Response Endpoint (IN to the PC) reads back a response (one response per command)

Events (button presses, slider movements, etc.) are delivered by
host-side polling: the application repeatedly issues `Event_Consume`
until it returns `RESULT_NOT_FOUND`. The ESP32-S3 USB-OTG controller
only supports five concurrent IN endpoints (EP0 + CDC notif + CDC bulk
IN + HID + vendor bulk IN), so there is no budget for a dedicated
interrupt-IN event mailbox.

### Command messages

Files (screen layouts, images, fonts, or any other resource) are managed
via a small set of streaming commands. The device inspects the file
extension after the writing transaction is committed to decide how to
post-process it:

* `host/s/*.pb` — decoded as a `touchy.Screen` protobuf (see
  [Stage 15](../docs/why-not-xml.md)) and cached for `Screen_Load`.
  (Stage 68 — moved from the old `host/screens/`. The canonical
  prev/next chrome lives at `host/s/default.pb`, which the firmware
  prefers as its boot screen when present.)
* `host/uscr/*.pb` — user page bodies (Stage 68): standalone
  `touchy.Widget` blobs the default chrome's `widget_ref(id="page")`
  pages through via `Screen_Load`-time `ActionChangeWidgetRef`
  NEXT/PREVIOUS. Upload them with the Python API's
  `Touchy.user_screen_save(name, widget)`.
* `host/widgets/*.pb` — a serialized standalone `touchy.Widget`
  (Stage 54). Referenced by `Widget.widget_ref { string path = 1; }`
  in a screen's widget tree; the firmware reads + decodes the file
  when the referring screen is loaded and splices the decoded widget
  inline at that position. Refs may live on either drive
  (`F:host/widgets/foo.pb` for persistence, `R:host/widgets/foo.pb`
  for volatile slots). Resolution happens at `Screen_Load` time only
  — overwriting a widget file does **not** trigger a redraw today
  (deferred to Stage 55).
* anything else — written verbatim; reachable via an LVGL drive letter
  so image/font loaders can resolve it on demand. See the
  [Filesystems](#filesystems-stage-51) section below for the path
  layout and the streaming write protocol.

Commands:

* `File_Delete(path)` — Delete a single file *or* a whole subtree
  (whichever the path matches) and update the in-memory screen
  registry. Wipe the entire host-uploaded area on flash with
  `File_Delete("F:host")`.
* `File_Open_Write(path) → handle` — Begin a streaming upload to
  `path`. The firmware allocates a non-zero `uint32` handle; the
  client must reference it on every subsequent `File_Write` and on
  `File_Close`. Only one upload transaction is in flight at a time
  (system-wide). Writes go to a private temp path
  (`<path>.tmp.<handle>`) so a half-finished transfer never replaces a
  valid file.
* `File_Write(handle, data)` — Append up to 4 KiB of `data` to the
  open transaction. Multiple calls accumulate.
* `File_Close(handle, commit)` — Finish the transaction. With
  `commit = true` the firmware atomically renames the temp path over
  the destination and (if `path` looks like a screen file)
  re-registers the screen. After a successful commit the firmware
  also runs `screens_notify_file_changed(path)`, which reloads the
  active screen iff it directly or indirectly references the
  just-overwritten file (image asset, widget-ref source, or the
  screen file itself). Uploads of unrelated assets cause no visible
  redraw. With `commit = false` the temp file is
  discarded — used by the host as a clean abort path on exceptions.
* `Screen_Load(path)` — Activate a previously-uploaded screen by its
  full drive-prefixed path (e.g. `F:host/s/home.pb`). The empty
  string loads the device default (`host/s/default.pb` if present,
  else the first registered screen, or the firmware's built-in
  fallback if nothing has been uploaded).
* `Screen_Wake` — Turn backlight on.
* `Screen_Sleep_Timeout(msec)` — Auto sleep after `msec` of inactivity.
* `Run_Actions(actions)` — Run a list of `Action`s device-side, exactly
  as if a local widget had just triggered them (Stage 71). The device
  feeds each `Action` through the same runner used for widget events
  (`ActionHost` / `ActionMacro` / `ActionDevice`), so e.g. an
  `ActionDevice(ActionChangeWidgetRef(BY_PATH,
  "F:host/uscr/opendeck.pb", target_id="page"))` retargets the live
  `widget_ref(id="page")` and brings a uscr page to the front — the
  mechanism the API library's `show_user_screen` / touchy-opendeck use
  instead of `Screen_Load`. Returns a plain `Response` (`RESULT_OK` or
  error).
* `Event_Consume` — Pop an event from the device event queue.
* `Sys_Reboot_Bootloader` — Reboot into bootloader (firmware update).
* `Sys_Version_Get` — Get the protocol and firmware version info.

### Filesystems (Stage 51)

Every path the host sends to the device is **drive-prefixed**: the
first two characters are `<letter>:` and the rest is the path within
that filesystem, e.g. `F:host/s/home.pb` or
`R:host/images/avatar.bin`. The device refuses unprefixed paths
rather than silently rebasing them.

Two filesystems exist:

| Letter | Backing store | Persists across reboot? | Suggested use |
|--------|---------------|-------------------------|---------------|
| `F:`   | LittleFS partition on flash | Yes | Screens, long-lived images, fonts. |
| `R:`   | PSRAM hashmap (`RamFs`)     | No  | Frequently-changing assets — e.g. StreamDeck-style key icons. Avoids flash wear. |

By convention every host-uploaded file lives under a `host/`
subdirectory of its drive (`F:host/...` or `R:host/...`). On the
device this maps to the LittleFS POSIX prefix `/littlefs/host/...`
for `F:` and to a flat key in the PSRAM hashmap for `R:`. The
firmware's `lv_fs_drv` for `R:` makes RAM-resident files visible to
LVGL through the same path the host uses.

Image format: on the wire and on disk the firmware only understands
LVGL's native `.bin` container (a 12-byte header + raw pixel planes).
The default output format is **RGB565** (matching the 16bpp display
build), with an automatic fallback to **RGB565A8** when the source
image actually contains non-opaque pixels — the host emits a single
`WARNING` on `touchy_pad.api.lvgl_image` to flag the slow path.
Hosts do **not** need to know any of this. The Python package
(`touchy_pad.api.TouchyClient.file_save`) auto-detects any
Pillow-readable image (BMP, PNG, JPEG, GIF, WebP) by its magic bytes,
converts it to LVGL `.bin`, **and rewrites the destination path's
extension to `.bin`** before transmitting. So
`file_save("F:host/images/foo.png", png_bytes)` actually writes
`F:host/images/foo.bin` on flash, and the `Image` DSL applies the
same `.png → .bin` rewrite to the asset field — keeping host source
paths and on-flash names consistent. Already-converted `.bin` blobs
and non-image data pass through unchanged. Other host-language
bindings can either reuse the same on-the-fly conversion or upload
pre-built `.bin` files directly.

The Python `file_save(path, data)` API hides the streaming protocol:
internally it splits `data` into 4 KiB chunks, drives the
`File_Open_Write` → `File_Write*` → `File_Close(commit=True)`
sequence, and aborts with `File_Close(commit=False)` if anything
raises. Callers see one atomic operation.

Performance tip: when an image is stored on the PSRAM ramdisk (`R:`)
**and** its pixel format matches the display's native color format
(RGB565 for the current 16bpp build), the firmware aliases the image
bytes directly into LVGL via an `lv_image_dsc_t` — skipping the file
read/decode path entirely. Hot icons that swap frequently (e.g.
StreamDeck-style key art) should therefore be uploaded to `R:` to
unlock this zero-copy fast path. Mismatched formats and assets on
`F:` still work; they just go through the standard file decoder and
the device logs a single `ESP_LOGW` line explaining why mmap was
declined.

### Command responses

* Response(code) - 0 = okay, anything is some TBD error
* Sys_Version_Response(protocol_vernum, firmware_ver_num, firmware_ver_str)

### Event delivery

The firmware accumulates `LvEvent` records in an internal queue as
widgets fire. The host drains the queue by issuing `Event_Consume`
commands until it gets `RESULT_NOT_FOUND`. `TouchyClient.stream_events`
polls in a loop with a configurable interval (default 50 ms). Each
successful `EventConsumeResponse` carries an `LvEvent` directly (since
protocol v2; previously wrapped in `Event`).

`LvEvent` fields:

* `code` — mirrors `lv_event_code_t` (LV_EVENT_CLICKED, LV_EVENT_VALUE_CHANGED, ...).
* `user_data` — the widget id from the layout.
* `state` — per-widget current value oneof (`value` for sliders, `checked` for switches/checkboxes; unset for buttons).
* `host_code` *(Stage 16)* — for `ActionHost` events, the `uint32` code
  the application assigned. Use `TouchyClient.on_host_event(code, fn)`
  to dispatch on it.

### Log tunneling (Stage 64.1, protocol V4)

When the firmware is built with `CONFIG_TOUCHY_LOG_OVER_PROTO=y` (the
project default; see `firmware/main/Kconfig.projbuild`) the
`esp_log_set_vprintf()` hook intercepts every ESP-IDF log line and
enqueues a `LogRecord` onto a small ring buffer (depth 32). The host's
existing `EventConsumeCmd`-polling loop drains it: when the event queue
is empty but a log record is waiting, the response's `payload` oneof
carries `LogRecord` (tag 5) instead of `EventConsumeResponse`. Events
take priority on ties so a busy UI never starves log delivery.

`LogRecord` fields:

* `priority` — `LogPriority` enum (`TRACE`/`DEBUG`/`INFO`/`WARN`/`ERROR`).
  Unset == `TRACE`, matching the verbosity of an absent value.
* `message` — pre-formatted line, no trailing newline, capped at
  79 bytes on the wire (longer lines are silently truncated).
* `tag` — ESP_LOG TAG (capped at 15 bytes).
* `timestamp_ms` — `esp_timer_get_time() / 1000` at enqueue, truncated
  to `uint32` (wraps every ~49 days); 0 on the sim.
* `num_dropped` — records discarded since the last successful enqueue
  (ISR context, reentrant emit, or queue full); folded into the next
  surviving record so no loss is silent on the host side.

The Python client routes each record onto the `touchy_pad.device`
stdlib logger (`LOG_PRIORITY_TRACE` goes onto the noisier
`touchy_pad.device.trace` child) with the originating tag exposed via
`extra={"device_tag": ...}`. The Rust client forwards them through the
`log` crate at the matching `log::Level`, with target
`touchy_pad::device::<TAG>` for filter granularity.

To opt out at build time set `CONFIG_TOUCHY_LOG_OVER_PROTO=n`; the
host-side dispatcher still works (the device just never produces
records). `CONFIG_TOUCHY_LOG_TO_UART=y` (default) keeps a copy on the
UART console so JTAG debugging is unaffected.

### Actions (Stage 16)

Widgets that can fire (`Button.on_click`, `Slider.on_change`,
`Switch.on_change`, `Checkbox.on_change`) carry a `repeated Action`.
Each action is one of:

* **`ActionHost { uint32 code }`** — enqueue an `LvEvent` with that
  `host_code` for the host to fetch via `Event_Consume`.
* **`ActionMacro { repeated MacroStep steps }`** — replayed entirely on
  the device. A `MacroStep` is one of: `key_down` / `key_up` /
  `key_tap` (HID keyboard, with optional modifier mask), mouse-button
  bitmasks (`mouse_button_down` / `mouse_button_up` / `mouse_click`),
  `mouse_move {dx, dy, wheel}`, `set_delay_ms` (sticky inter-step
  delay, default 10 ms), or a one-shot `delay_ms`.

USB HID is a composite single-interface design with report IDs
1 = mouse, 2 = keyboard, so macros and the existing touchpad share one
endpoint pair.

The Stage-15 host-side DSL in [`touchy_pad.api.screens`](../app/src/touchy_pad/api/screens.py)
is the supported way to author layouts; see
[`docs/why-not-xml.md`](why-not-xml.md) for why we don't use XML.
Use `host_action(code)`, `macro_action(steps)`, and `action()` from
`touchy_pad.api.screens` together with the step factories in
`touchy_pad.api.macros` and the key constants in `touchy_pad.api.hid_keys`.

See https://lvgl.io/docs/open/common-widget-features/events#fields-of-lv_event_t
and https://lvgl.io/docs/open/api/core/lv_event_h for more info.

## USB descriptor layout

The device enumerates as a composite USB 2.0 full-speed device with the
following stable identifiers:

| Field        | Value      |
|--------------|------------|
| `idVendor`   | `0x4403`   |
| `idProduct`  | `0x1002`   |
| `bcdDevice`  | monotonic build number |
| Manufacturer | `Touchy-Pad` |
| Product      | `Touchy-Pad` |
| Serial       | per-device MAC-derived string |

The composite device exposes three interfaces (numbering may shift as the
firmware evolves — locate interfaces by class, not number):

1. **HID mouse** — `bInterfaceClass = 0x03`, boot protocol mouse, single
   interrupt-IN endpoint, 8-byte reports (buttons, X, Y, wheel).
2. **HID keyboard** — `bInterfaceClass = 0x03`, boot protocol keyboard,
   single interrupt-IN endpoint, standard 8-byte boot report.
3. **Touchy custom protocol** — `bInterfaceClass = 0xFF` (vendor-specific),
   `bInterfaceSubClass = 0x54` ("T"), `bInterfaceProtocol = 0x01`.

The vendor interface exposes two bulk endpoints:

| Endpoint        | Direction | Type      | `wMaxPacketSize` | Purpose                        |
|-----------------|-----------|-----------|------------------|--------------------------------|
| Command (OUT)   | host → device | Bulk      | 64 bytes (FS) / 512 bytes (HS) | framed `Command` |
| Response (IN)   | device → host | Bulk      | 64 / 512 bytes   | framed `Response`     |

Events are delivered by host polling on the same endpoint pair via the
`Event_Consume` command — see *Event delivery* above. An earlier design
allocated a dedicated interrupt-IN "event mailbox" endpoint, but the
ESP32-S3 USB-OTG controller's 5-IN-endpoint budget is already fully
claimed by EP0 + CDC notif + CDC bulk-IN + HID + vendor bulk-IN.

The host library locates this interface by scanning the active
configuration for `bInterfaceClass == 0xFF`. See
[`app/src/touchy_pad/transport.py`](../app/src/touchy_pad/transport.py).

### Wire framing

Every protobuf message — `Command`, `Response`, or `Event` — is wrapped in
a self-synchronising frame (Stage 64.3, `ProtocolVersion.CURRENT == 9`):

```
+----------+----------+--------------------+---------+
| MAGIC(2) | LEN(u16) | payload (LEN bytes)| CRC8(1) |
| A5 5A    | LE       | protobuf message   |         |
+----------+----------+--------------------+---------+
```

* **MAGIC** is the constant `0xA5 0x5A`, a sync anchor that lets a reader
  re-align to a frame boundary after garbage or a mid-stream connect.
* **LEN** is a little-endian `uint16`, so the maximum payload is 65535
  bytes. Each side rejects (and resyncs past) any frame whose length or
  CRC is invalid, so a corrupt length field cannot force a huge
  allocation.
* **CRC8** is computed (polynomial `0x07`, init `0x00`) over the two
  `LEN` bytes followed by the payload — i.e. everything between the magic
  and the CRC byte.

The frame is identical on every transport — USB bulk pair, the simulator
TCP socket, and the serial-port transport — so all three share one
encoder/decoder (`touchy_pad.transport`, `touchy_pad::transport`, and the
firmware `host_api` link abstraction). On USB, large messages still span
multiple successive transfers and the receiver concatenates them before
the decoder extracts a frame. On the serial port the frame's MAGIC lets
the host skip device boot-log noise and lock onto the first real frame.

### Physical layers

The same framing rides several physical layers, chosen by board:

* **USB vendor-bulk pair** — ESP32-S3 boards (`jc4827w543`,
  `waveshare_s3_lcd_7b`) with native USB-OTG. Also carries HID
  mouse/keyboard on separate interfaces.
* **UART0 @ 115200 8N1** — the classic-ESP32 `esp32_2432s028rv3` (CYD2USB)
  has no native USB; its CH340 bridge enumerates on the host as
  `/dev/ttyUSB*`. The protocol owns UART0 (the IDF console is moved off
  it), and device logs are tunneled as `LogRecord` frames rather than raw
  text. This board provides no HID emulation.
* **TCP socket** — the simulator (`touchy --sim`).

The host's `touchy_pad.transport_serial` (and Rust `transport_serial`
behind the `serial` feature) speak the UART layer; pass `--port
/dev/ttyUSB0` to `touchy`.

### HTTP(S) — the odd one out (no framing)

Stage lb8 adds a WiFi transport that is **not** a byte stream and so
carries a **bare, unframed** `Command`/`Response` (HTTP `Content-Length`
delimits each message — no MAGIC/LEN/CRC8). It reuses the same command
dispatcher as the framed links. See [network-api.md](network-api.md) for
the endpoint, the mutual-TLS provisioning (`touchy pref provision-mtls`),
mDNS naming, and the `--url` host selector.

### Board capabilities

`SysBoardInfoResponse` advertises what a connected board can do so the
host adapts at runtime (Stage 65, protocol V6; serial added in V7; free
memory + storage added in V8; `SetPreferencesCmd` in V9):

* `is_multitouch` — `false` on resistive single-touch panels
  (`esp32_2432s028rv3`), `true` on the GT911 capacitive boards. The sim
  trackpad only synthesises multi-finger / non-left gestures when this is
  set.
* `has_usb` — `true` when the board can emulate USB HID
  mouse/keyboard; `false` on UART-only boards.
* `serial` — a stable per-device identifier (Stage 71). On real
  hardware it is derived from the chip's factory MAC as `"t"` + 12
  lowercase hex digits (no separators, e.g. `t3c8427aa11bb`); the same
  string is exposed as the USB `iSerialNumber` descriptor so the OS and
  the vendor transport agree. The simulator reports the constant
  `tsim001`. Hosts use it as the canonical enumeration id.
* `free_heap_bytes` / `free_psram_bytes` — current free internal RAM and
  PSRAM (`uint64`; Stage 81, protocol V8). PSRAM is `0` on no-PSRAM
  boards. Filled from `heap_caps_get_free_size(MALLOC_CAP_INTERNAL` /
  `MALLOC_CAP_SPIRAM)`.
* `fs_total_bytes` / `fs_used_bytes` — LittleFS (`F:`) capacity and usage
  (`uint64`; Stage 81), from `FlashFs::usage()`.

`touchy board-info` surfaces these (plus the display size) as table rows.

### Preferences (`SetPreferencesCmd`)

Stage 82 replaced the per-setting `ScreenLoadCmd` and
`ScreenSleepTimeoutCmd` commands with a single partial-update message
(`ProtocolVersion.V9`):

```
message SetPreferencesCmd { PreferencesFile prefs = 1; }
```

Every value field on `PreferencesFile` is `optional` (proto3 presence),
so the host sets only the fields it wants changed; the device merges them
into its persisted prefs and fires the matching side effects. The host
MUST NOT set `file_version` — it is device-owned. Fields:

* `screen_timeout_ms` — backlight auto-sleep timeout (`0` = never).
  Wrapper: `screen_sleep_timeout` / CLI `pref backlight-timeout`.
* `current_screen` — activate an uploaded screen (empty = default).
  Returns `RESULT_NOT_FOUND` if the path is unknown. Wrapper:
  `screen_load` / CLI `screen load`.
* `min_log_level` — minimum `LogPriority` (as `uint32`) queued back over
  the log tunnel; lower-priority records are dropped device-side.
  Default `ERROR`. Wrapper: `set_min_log_level` / CLI `pref log-level`.
* `boot_delay_s` — early-boot `vTaskDelay` (seconds) so a debug-log
  connection can attach before subsystem bring-up. Wrapper:
  `set_boot_delay` / CLI `pref boot-delay`.

### Channel semantics

* **Command / Response** is a strict request/reply pair. The host MUST NOT
  pipeline a second `Command` before reading the previous `Response`. The
  device guarantees exactly one `Response` per `Command`, in order.
* The **Event** endpoint is non-blocking, asynchronous, and lossy in the
  sense that the device may drop the oldest queued event when the queue
  overflows. Each interrupt packet carries either a complete `Event`
  (when its serialised form fits within `wMaxPacketSize`) or an
  `event_ready` placeholder; in the latter case the host should issue an
  `EventConsume` on the command channel to drain the actual event.
* The bulk endpoints are full-duplex with respect to the event endpoint:
  events may arrive at any time, including while the host is mid-flight
  on a command/response pair.

### Permissions (Linux)

On Linux, opening the device from a non-root user requires a udev rule.
See [`bin/99-touchy-pad.rules`](../bin/99-touchy-pad.rules) (TODO: ship
this file) for a starter — typically:

```
SUBSYSTEM=="usb", ATTRS{idVendor}=="4403", ATTRS{idProduct}=="1002", MODE="0660", GROUP="plugdev"
```

Reload with `sudo udevadm control --reload-rules && sudo udevadm trigger`,
add yourself to `plugdev`, and re-plug the device.
