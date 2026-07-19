# Python API guide

The official Python entry point for application code is
`touchy_pad.api`. Everything outside this package (e.g.
`touchy_pad._proto`, the simulator, the CLI) is considered internal and
may change without notice.

Auto-generated per-class / per-function reference docs live under
[python-api/](python-api/) (built by `just build-docs`).

## Install

```sh
pip install touchy-pad
```

You also need a working `libusb-1.0` runtime; see the
[main README](../README.md) and [`bin/99-touchy-pad.rules`](../bin/99-touchy-pad.rules)
for the Linux udev rule.

## Hello pad

```python
from touchy_pad.api import touchy_open, touchy_get_pad_ids

print("attached:", touchy_get_pad_ids())

with touchy_open() as pad:
    print("loaded:", pad.screen_load("home"))
```

`touchy_open()` opens the first connected device by default; pass a
serial number from `touchy_get_pad_ids()` to select a specific one. The
returned `Touchy` is a context manager; leaving the `with` block stops
the background event-poller thread and closes the USB transport.

## Authoring screens

The high-level **screen DSL** lives in `touchy_pad.api.screens` (and is
re-exported from `touchy_pad.api` for short imports):

```python
from touchy_pad.api import Screen, button, label, row, grid, style

s = Screen("home", layout=grid(cols=3, rows=4, gap=8))
s += label("title", text="Touchy-Pad")
s += button("go", text="Go",
            style=style(bg_color=0x1E90FF, text_color=0xFFFFFF))

with touchy_open() as pad:
    pad.screen_save(s)         # uploads to F:host/s/home.pb
    pad.screen_load("F:host/s/home.pb")
```

`Screen.to_proto()` is also exposed for advanced users who want the raw
protobuf message before upload.

## Raw protobuf

When you'd rather hand-build the message:

```python
from touchy_pad.api import protobuf, touchy_open

msg = protobuf.Screen(name="raw", version=protobuf.Screen.Version.CURRENT)
msg.active.layout_flex.flow = protobuf.LayoutFlex.ROW
msg.active.layout_flex.layout.children.add(
    label=protobuf.Label(text="raw"),
)

with touchy_open() as pad:
    pad.screen_save(msg)
```

`pad.screen_save()` accepts any of:

* a host-DSL `Screen`
* a raw `protobuf.Screen`
* a `dict` of JSON-shaped screen data (camelCase fields)
* a `pathlib.Path` or `str` pointing at a `.json` file

## Uploading arbitrary files

`pad.file_save(path, data)` writes one file to the device atomically.
Paths are **drive-prefixed** — pick the filesystem with the leading
letter:

* `F:host/...` — persistent flash (LittleFS). Use for screens, fonts,
  long-lived images. Survives reboot.
* `R:host/...` — transient PSRAM (`RamFs`). Use for assets that change
  often (e.g. StreamDeck-style key icons) — avoids wearing the flash.
  Lost on reboot.

```python
from pathlib import Path

with touchy_open() as pad:
    # 16x16 PNG → auto-converted to LVGL .bin and stored as
    # F:host/images/smiley.bin on flash.
    pad.file_save("F:host/images/smiley.png", Path("smiley.png").read_bytes())

    # A frequently-replaced key icon goes in PSRAM.
    pad.file_save("R:host/images/key_0.png", Path("key_0.png").read_bytes())
```

Behind the scenes `file_save` drives a streaming protocol
(`File_Open_Write` → `File_Write*` → `File_Close`) in ≤4 KiB USB-bulk
chunks, so arbitrarily large files upload without needing the device
to buffer the whole payload. From the caller's point of view it's a
single atomic operation — interruptions surface as exceptions and
leave no partial file behind.

`pad.file_delete(path)` removes one file or a whole subtree. The
convenience `pad.file_reset()` is shorthand for
`pad.file_delete("F:host")` and wipes the entire host-uploaded flash
area.

## Macros

The keyboard / mouse macro DSL is in `touchy_pad.api.macros`:

```python
from touchy_pad.api import macros, hid_keys, macro_action, button

go = button("paste", text="Paste!", on_press=macro_action([
    macros.key_tap(hid_keys.KEY_V, modifiers=hid_keys.MOD_LEFT_CTRL),
]))
```

## Event callbacks

The easiest way to react to a widget is to attach the callback **inline**
when you build it, with `host_action(on_event=...)`:

```python
from touchy_pad.api import touchy_open, Screen, button, host_action

s = Screen("home")
s += button(
    "ping",
    text="Ping host",
    on_click=host_action(on_event=lambda e: print("widget", e.user_data, "fired")),
)

with touchy_open() as pad:
    pad.screen_save(s)   # callbacks are wired up automatically here
    pad.screen_load("home")
    # ...other work...
```

`host_action(on_event=...)` allocates a unique host code for you (from a
reserved range starting at `0x10000`) and remembers the callback. When the
screen or widget that references it is uploaded — via `pad.screen_save(...)`
or `pad.widget_save(...)` — the callback is registered on that `Touchy`
automatically. No separate `on_host_event` call is needed.

The callback receives the full `LvEvent`; inspect `evt.user_data` (the
widget id), `evt.value` (sliders), `evt.checked` (toggles / checkboxes),
and `evt.host_code`.

### Lower-level: explicit codes

You can still manage host codes yourself. Pass an explicit numeric `code`
to `host_action` (keep it **below** `0x10000` so it never collides with the
auto-allocated range) and dispatch it with `on_host_event`:

```python
s += button("ping", on_click=host_action(0x100))

with touchy_open() as pad:
    pad.on_host_event(0x100, lambda e: print("ping", e.user_data))
```

Callbacks run on the poller thread, so:

* keep them short — long-running callbacks block USB polling;
* protect any shared state with your own lock;
* multiple callbacks can be registered for the same code; they are
  invoked in registration order.


## Running actions device-side

`pad.run_actions(actions)` (on the low-level `TouchyClient`) asks the
device to execute a list of protobuf `Action`s exactly as if a local
widget had triggered them. The most common use is retargeting the
`widget_ref(id="page")` of the default chrome to a uploaded user-screen
body so it jumps to the front:

```python
from touchy_pad.api import protobuf

act = protobuf.Action()
act.device.change_widget_ref.behavior = (
    protobuf.ActionChangeWidgetRef.Behavior.BY_PATH
)
act.device.change_widget_ref.path = "F:host/uscr/opendeck.pb"
act.device.change_widget_ref.target_id = "page"

with touchy_open() as pad:
    pad.user_screen_save("opendeck", widget)  # body → F:host/uscr/opendeck.pb
    pad.run_actions([act])                     # bring it to the front
```

This works against both real hardware and the simulator (headless too).

## Overriding widget properties at runtime

`pad.set_property(widget_id, prop, value)` overrides a single LVGL
property on a widget by its `id`, without re-uploading the screen — e.g.
change a label's text or a box's colour on the fly:

```python
from touchy_pad.api import Color, Point

with touchy_open() as pad:
    pad.set_property("welcome", "text", "New message")   # label text
    pad.set_property("box", "bg_color", Color(0xFF8800))  # colour
    pad.set_property("box", "x", 12)                      # integer prop
    pad.set_property("welcome", "text", None)            # remove override
```

`prop` is an LVGL **property name** (a short name resolved on-device, e.g.
`"text"`, `"bg_color"`) or an `int` raw `lv_prop_id_t`. The `value` type
selects the wire payload:

| Python value            | Property kind        |
|-------------------------|----------------------|
| `bool`                  | boolean              |
| `int`                   | integer              |
| `str`                   | text (e.g. label)    |
| `Color(0xRRGGBB)`       | colour               |
| `Point(x, y)`           | point                |
| `None`                  | **remove** the override |

`Color` and `Point` (both importable from `touchy_pad.api`) are required
wrappers: a bare `int` always means an integer property, so colours and
points must be wrapped to be distinguishable.

Overrides are **session-scoped** (RAM only, never persisted) and
**sticky**: the device re-applies them whenever the target widget is
(re)built and immediately if it is already on screen — so you can even set
a property before its widget is loaded. The CLI exposes a string-only
shortcut: `touchy property set WIDGET_ID PROPERTY VALUE`. The simulator
logs a warning and ignores `set_property` (it renders with Qt, not LVGL).

## Lifecycle

```python
with touchy_open() as pad:
    ...
```

## Version compatibility

`touchy_open()` queries the device's USB protocol version on open and
raises `IncompatibleFirmwareError` if it's older than
`MINIMUM_FIRMWARE_VERSION`. Update the firmware on the device with "touchy update" if necessary.
