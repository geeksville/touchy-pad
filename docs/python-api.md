# Python API guide

The official Python entry point for application code is
`touchy_pad.api`. Everything outside this package (e.g.
`touchy_pad.client`, `touchy_pad._proto`) is considered internal and
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
    pad.screen_save(s)         # uploads to /from_host/screens/home.pb
    pad.screen_load("home")
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

## Macros

The keyboard / mouse macro DSL is in `touchy_pad.api.macros`:

```python
from touchy_pad.api import macros, hid_keys, macro_action, button

go = button("paste", text="Paste!", on_press=macro_action([
    macros.key_tap(hid_keys.KEY_V, modifiers=hid_keys.MOD_LEFT_CTRL),
]))
```

## Event callbacks

`Touchy` runs a single background thread that polls the device for
events; user code registers per-host-code callbacks:

```python
def on_ping(evt):
    print("widget", evt.user_data, "fired")

with touchy_open() as pad:
    pad.on_host_event(0x100, on_ping)
    # ...other work...
```

Callbacks run on the poller thread, so:

* keep them short — long-running callbacks block USB polling;
* protect any shared state with your own lock;
* multiple callbacks can be registered for the same code; they are
  invoked in registration order.

## Lifecycle

Always prefer the context-manager form:

```python
with touchy_open() as pad:
    ...
```

If that isn't practical, call `pad.close()` explicitly to stop the
poller thread and release the USB device.

## Version compatibility

`touchy_open()` queries the device's USB protocol version on open and
raises `IncompatibleFirmwareError` if it's older than
`MINIMUM_FIRMWARE_VERSION`. Update the firmware on the device, or pin
an older `touchy-pad` release.
