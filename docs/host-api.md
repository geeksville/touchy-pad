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
via a single generic pair of commands. The device inspects the file
extension after writing to decide how to post-process it:

* `screens/*.pb` — decoded as a `touchy.Screen` protobuf (see
  [Stage 15](../docs/why-not-xml.md)) and cached for `Screen_Load`.
* anything else — written verbatim; reachable via the LVGL `F:` drive
  letter so image/font loaders can resolve it on demand. The firmware
  enables `CONFIG_LV_USE_FS_POSIX` with `LV_FS_POSIX_PATH=/littlefs`,
  so a host-uploaded `images/smiley.png` is converted by the host to
  LVGL `.bin` and lands at `/littlefs/from_host/images/smiley.bin` on
  disk, resolving to `F:/from_host/images/smiley.bin` from LVGL.
  `Image` / `ImageButton` widgets store the part after `F:/from_host/`
  (e.g. `"images/smiley.png"`) in their `asset` field; the host-side
  DSL rewrites the extension to `.bin` at serialise time so the
  firmware can prepend the prefix and hand the path straight to LVGL.

  Image format: on the wire and on disk the firmware only understands
  LVGL's native `.bin` container (a 12-byte header + raw pixel planes —
  RGB565A8 by default). Hosts do **not** need to know this. The Python
  package (`touchy_pad.client.TouchyClient.file_save`) auto-detects any
  Pillow-readable image (BMP, PNG, JPEG, GIF, WebP) by its magic bytes,
  converts it to LVGL `.bin`, **and rewrites the destination path's
  extension to `.bin`** before transmitting. So
  `file_save("images/foo.png", png_bytes)` actually writes to
  `from_host/images/foo.bin` on flash, and the `Image` DSL applies the
  same `.png → .bin` rewrite to the asset field — keeping host source
  paths and on-flash names consistent. Already-converted `.bin` blobs
  and non-image data pass through unchanged. Other host-language
  bindings can either reuse the same on-the-fly conversion or upload
  pre-built `.bin` files directly.

Commands:

* `File_Reset` — Discard all files on the device filesystem (also clears
  the in-memory screen registry).
* `File_Save(path, data)` — Write `data` (raw bytes — text or binary) at
  `path`. Examples: `File_Save("screens/home.pb", screen_proto)`,
  `File_Save("img/avatar.png", png_bytes)`.
* `Screen_Load(screen_name)` — Activate a previously-uploaded screen.
* `Screen_Wake` — Turn backlight on.
* `Screen_Sleep_Timeout(msec)` — Auto sleep after `msec` of inactivity.
* `Event_Consume` — Pop an event from the device event queue.
* `Sys_Reboot_Bootloader` — Reboot into bootloader (firmware update).
* `Sys_Version_Get` — Get the protocol and firmware version info.

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
| Command (OUT)   | host → device | Bulk      | 64 bytes (FS) / 512 bytes (HS) | length-prefixed `Command` |
| Response (IN)   | device → host | Bulk      | 64 / 512 bytes   | length-prefixed `Response`     |

Events are delivered by host polling on the same endpoint pair via the
`Event_Consume` command — see *Event delivery* above. An earlier design
allocated a dedicated interrupt-IN "event mailbox" endpoint, but the
ESP32-S3 USB-OTG controller's 5-IN-endpoint budget is already fully
claimed by EP0 + CDC notif + CDC bulk-IN + HID + vendor bulk-IN.

The host library locates this interface by scanning the active
configuration for `bInterfaceClass == 0xFF`. See
[`app/src/touchy_pad/transport.py`](../app/src/touchy_pad/transport.py).

### Wire framing

Every protobuf message — `Command`, `Response`, or `Event` — is prefixed by
a single little-endian `uint32` carrying the length of the serialised
payload in bytes. The header is always 4 bytes wide and represents
payloads up to 4 GiB; in practice each side caps the frame at a small
multiple of the largest `Command` (currently ~32 KiB for `ImageSaveCmd`)
so a corrupt or malicious length field cannot force a huge allocation.
Large messages are sent as multiple successive USB transfers and the
receiver concatenates them. A USB short packet (transfer length not a
multiple of `wMaxPacketSize`, including a zero-length packet) terminates a
frame.

```
+---------------------------+---------------------------+
| u32 length (little-endian)| protobuf payload (length B)|
+---------------------------+---------------------------+
```

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
