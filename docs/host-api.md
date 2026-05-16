# Host API
This file is the design doc for the protocol used to talk between the device and the host PC.

## Legacy HID descriptors

Unrelated to the 'fancy' custom API, the device will also expose HID keyboard and mouse interfaces/endpoints.  These are used to provide button press and touchpad events to the PC even if our helper app is not being used.  

## Touchy Host API

We will provide a custom API on custom USB interface composed of three endpoints. 

* Command Endpoint (OUT from PC to device). A message pipe.   The host sends commands (as protocol buffers?).
* Response Endpoint (IN to the PC) reads back a response (one response per command)
* Event endpoint (IN to PC). An interrupt stream pipe.   Used to send async events from the device (user pressed button X, moved slider Y to Z etc...)

Note: the event endpoint max packet size is quite small so possibly we'll just have an event for "AsyncEventReady" and then the host will issue a "ConsumeEvent"

### Command messages

Files (screen layouts, images, fonts, or any other resource) are managed
via a single generic pair of commands. The device inspects the file
extension after writing to decide how to post-process it:

* `screens/*.pb` — decoded as a `touchy.Screen` protobuf (see
  [Stage 15](../docs/why-not-xml.md)) and cached for `Screen_Load`.
* anything else — written verbatim; reachable via the LVGL `F:` drive
  letter so image/font loaders can resolve it on demand.

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

### Event endpoint messages

* `Event_LV` — A lightly-wrapped LVGL event. The firmware populates
  `LvEvent.user_data` with the host-assigned widget id (and, where
  relevant, the `Action.event` string from the layout), so the host can
  route incoming events back to its application-level callbacks without
  the device needing to know about them. `LvEvent.code` mirrors
  `lv_event_code_t`; `extra` carries any per-event payload (e.g. a slider
  value).

The Stage-15 host-side DSL in [`touchy_pad.screens`](../app/src/touchy_pad/screens.py)
is the supported way to author layouts; see
[`docs/why-not-xml.md`](why-not-xml.md) for why we don't use XML.

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

The vendor interface has up to three endpoints:

| Endpoint        | Direction | Type      | `wMaxPacketSize` | Purpose                        |
|-----------------|-----------|-----------|------------------|--------------------------------|
| Command (OUT)   | host → device | Bulk      | 64 bytes (FS) / 512 bytes (HS) | length-prefixed `Command` |
| Response (IN)   | device → host | Bulk      | 64 / 512 bytes   | length-prefixed `Response`     |
| Event (IN)      | device → host | Interrupt | 16 bytes, `bInterval = 8 ms` | length-prefixed `Event` (optional, post-stage-13)|

The stage-13 firmware ships only the bulk command/response pair. The
interrupt-IN event endpoint is reserved by the protocol but not yet
advertised in the configuration descriptor; the host library treats it
as optional and `recv_event()` raises a `TransportError` against firmware
that lacks it.

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
